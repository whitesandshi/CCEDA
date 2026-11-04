#include "meshing/GeometryMap.h"
#include "meshing/GmshMsh41Reader.h"
#include "meshing/MeshBinaryCodec.h"
#include "meshing/MeshQualityJson.h"
#include "domain/MeshGenerationSettings.h"
#include "domain/MeshingResourceLimits.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <psapi.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace
{
constexpr int ProtocolMajor = 0;
constexpr int ProtocolMinor = 1;
constexpr qint64 MaximumJsonBytes = 64 * 1024;
// MSH 4.1 writes one node and one element block per geometric entity. Large
// CAD assemblies (for example ABC-dataset STEP models) routinely exceed the
// previous 100 000-entity budget: a 115 MB BREP produced 643 180 entity
// blocks. Keep a protective cap against absurd inputs while allowing such
// models; the reader never materializes per-block state, so memory stays
// proportional to the retained nodes and elements only.
constexpr std::uint64_t MaximumMshEntityBlocks = 2'000'000;

std::optional<std::uint32_t> boundedUnsignedInteger(
  const QJsonValue& value,
  std::uint32_t maximum)
{
  if (!value.isDouble())
  {
    return std::nullopt;
  }
  const qint64 integer = value.toInteger(-1);
  if (integer < 0 || integer > static_cast<qint64>(maximum)
    || value.toDouble() != static_cast<double>(integer))
  {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(integer);
}

std::optional<std::uint64_t> boundedUnsignedInteger64(
  const QJsonValue& value,
  std::uint64_t maximum)
{
  if (!value.isDouble())
  {
    return std::nullopt;
  }
  const qint64 integer = value.toInteger(-1);
  if (integer < 0 || static_cast<std::uint64_t>(integer) > maximum
    || value.toDouble() != static_cast<double>(integer))
  {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(integer);
}

std::optional<cceda::domain::MeshGenerationAlgorithm> meshAlgorithm(
  const QJsonValue& value)
{
  if (!value.isString())
  {
    return std::nullopt;
  }
  const QString id = value.toString();
  if (id == "delaunay")
  {
    return cceda::domain::MeshGenerationAlgorithm::Delaunay;
  }
  if (id == "hxt")
  {
    return cceda::domain::MeshGenerationAlgorithm::Hxt;
  }
  return std::nullopt;
}

QString meshAlgorithmId(cceda::domain::MeshGenerationAlgorithm algorithm)
{
  return algorithm == cceda::domain::MeshGenerationAlgorithm::Hxt
    ? "hxt" : "delaunay";
}

QString meshModeId(cceda::domain::MeshGenerationMode mode)
{
  return mode == cceda::domain::MeshGenerationMode::ConformalMultiRegion
    ? QStringLiteral("conformal-multi-region")
    : QStringLiteral("independent-solids");
}

std::vector<std::string> sourceGeometryIds(
  const cceda::meshing::GeometryMapRegion& region)
{
  const QStringList ids = region.sourceSolidGeometryIds.isEmpty()
    ? QStringList{region.solidGeometryId} : region.sourceSolidGeometryIds;
  std::vector<std::string> result;
  result.reserve(static_cast<std::size_t>(ids.size()));
  for (const QString& id : ids)
  {
    result.push_back(id.toStdString());
  }
  return result;
}

QString gmshAlgorithmId(cceda::domain::MeshGenerationAlgorithm algorithm)
{
  return algorithm == cceda::domain::MeshGenerationAlgorithm::Hxt
    ? "hxt" : "del3d";
}

struct MeshCounts
{
  qint64 nodes = 0;
  qint64 elements = 0;
  qint64 triangles = 0;
  qint64 tetrahedra = 0;
  QSet<int> triangleSurfaceEntityTags;
  QSet<int> tetrahedronVolumeEntityTags;
  QHash<int, std::array<double, 6>> volumeEntityBounds;
  QHash<int, QSet<int>> volumeEntitySurfaceTags;
};

constexpr double GeometryBoundsToleranceMm = 1.0e-6;

bool boundsEqual(
  const std::array<double, 6>& left,
  const std::array<double, 6>& right)
{
  return std::equal(left.begin(), left.end(), right.begin(),
    [](double a, double b) {
      return std::abs(a - b) <= GeometryBoundsToleranceMm;
    });
}

struct ResolvedGeometryMapMapping
{
  cceda::meshing::GmshMeshMapping meshMapping;
  QJsonArray serializedRegions;
};

std::optional<ResolvedGeometryMapMapping> resolveGeometryMapMapping(
  const cceda::meshing::GeometryMap& geometryMap,
  const MeshCounts& counts,
  QString& error)
{
  ResolvedGeometryMapMapping result;
  if (geometryMap.regions.size() == 1)
  {
    const cceda::meshing::GeometryMapRegion& region =
      geometryMap.regions.front();
    const std::string solidGeometryId = region.solidGeometryId.toStdString();
    result.meshMapping.regions.push_back({
      region.regionId,
      sourceGeometryIds(region),
    });
    QList<int> volumeTags = counts.tetrahedronVolumeEntityTags.values();
    std::sort(volumeTags.begin(), volumeTags.end());
    QJsonArray serializedVolumeTags;
    for (const int volumeTag : volumeTags)
    {
      if (!counts.volumeEntityBounds.contains(volumeTag))
      {
        error = "A meshed Gmsh volume is absent from the Entities section";
        return std::nullopt;
      }
      result.meshMapping.entities.push_back({
        3, volumeTag, region.regionId, solidGeometryId,
      });
      serializedVolumeTags.append(volumeTag);
    }
    for (const int surfaceTag : counts.triangleSurfaceEntityTags)
    {
      result.meshMapping.entities.push_back({
        2, surfaceTag, region.regionId, solidGeometryId,
      });
    }
    result.serializedRegions.append(QJsonObject{
      {"regionId", static_cast<qint64>(region.regionId)},
      {"sourceSolidGeometryId", region.solidGeometryId},
      {"sourceSolidGeometryIds", QJsonArray::fromStringList(
        region.sourceSolidGeometryIds.isEmpty()
          ? QStringList{region.solidGeometryId} : region.sourceSolidGeometryIds)},
      {"gmshVolumeEntityTags", serializedVolumeTags},
    });
    return result;
  }

  QSet<int> assignedSurfaceTags;
  QSet<int> matchedVolumeTags;
  result.meshMapping.regions.reserve(geometryMap.regions.size());
  for (const cceda::meshing::GeometryMapRegion& region : geometryMap.regions)
  {
    int matchedVolumeTag = 0;
    double closestDifference = std::numeric_limits<double>::infinity();
    int closestVolumeTag = 0;
    for (auto entity = counts.volumeEntityBounds.constBegin();
      entity != counts.volumeEntityBounds.constEnd(); ++entity)
    {
      double difference = 0.0;
      for (std::size_t component = 0; component < region.brepBoundsMm.size();
        ++component)
      {
        difference = std::max(difference,
          std::abs(entity.value()[component] - region.brepBoundsMm[component]));
      }
      if (difference < closestDifference)
      {
        closestDifference = difference;
        closestVolumeTag = entity.key();
      }
      if (!boundsEqual(entity.value(), region.brepBoundsMm))
      {
        continue;
      }
      if (matchedVolumeTag != 0)
      {
        error = "Snapshot region bounds match multiple Gmsh volume entities";
        return std::nullopt;
      }
      matchedVolumeTag = entity.key();
    }
    if (matchedVolumeTag == 0
      || !counts.tetrahedronVolumeEntityTags.contains(matchedVolumeTag)
      || matchedVolumeTags.contains(matchedVolumeTag))
    {
      error = QString(
        "Snapshot region bounds do not uniquely match a meshed Gmsh volume; "
        "closest tag %1 differs by %2 mm")
          .arg(closestVolumeTag)
          .arg(closestDifference, 0, 'g', 17);
      return std::nullopt;
    }
    const QSet<int> boundarySurfaces =
      counts.volumeEntitySurfaceTags.value(matchedVolumeTag);
    if (boundarySurfaces.isEmpty())
    {
      error = "Matched Gmsh volume has no boundary Surfaces";
      return std::nullopt;
    }

    const std::string solidGeometryId = region.solidGeometryId.toStdString();
    result.meshMapping.regions.push_back({
      region.regionId,
      sourceGeometryIds(region),
    });
    result.meshMapping.entities.push_back({
      3, matchedVolumeTag, region.regionId, solidGeometryId,
    });
    for (const int surfaceTag : boundarySurfaces)
    {
      if (!counts.triangleSurfaceEntityTags.contains(surfaceTag))
      {
        continue;
      }
      if (assignedSurfaceTags.contains(surfaceTag))
      {
        if (geometryMap.mode == "independent-solids")
        {
          error = "A meshed Gmsh Surface belongs to multiple snapshot regions";
          return std::nullopt;
        }
        continue;
      }
      assignedSurfaceTags.insert(surfaceTag);
      result.meshMapping.entities.push_back({
        2, surfaceTag, region.regionId, solidGeometryId,
      });
    }
    matchedVolumeTags.insert(matchedVolumeTag);
    result.serializedRegions.append(QJsonObject{
      {"regionId", static_cast<qint64>(region.regionId)},
      {"sourceSolidGeometryId", region.solidGeometryId},
      {"sourceSolidGeometryIds", QJsonArray::fromStringList(
        region.sourceSolidGeometryIds.isEmpty()
          ? QStringList{region.solidGeometryId} : region.sourceSolidGeometryIds)},
      {"gmshVolumeEntityTags", QJsonArray{matchedVolumeTag}},
    });
  }
  if (matchedVolumeTags != counts.tetrahedronVolumeEntityTags
    || assignedSurfaceTags != counts.triangleSurfaceEntityTags
    || matchedVolumeTags.size() != counts.volumeEntityBounds.size())
  {
    error = "Gmsh entities do not exactly cover the submitted snapshot regions";
    return std::nullopt;
  }
  return result;
}

QString utcTimestamp()
{
  return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

void sendMessage(const QString& type, const QString& jobId, const QJsonObject& payload)
{
  static quint64 messageId = 0;
  const QJsonObject message{
    {"type", type},
    {"protocol", QJsonObject{{"major", ProtocolMajor}, {"minor", ProtocolMinor}}},
    {"messageId", QString::number(++messageId)},
    {"jobId", jobId},
    {"timestampUtc", utcTimestamp()},
    {"payload", payload},
  };
  const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
  std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
  std::fflush(stdout);
}

int fail(const QString& jobId, const QString& code, const QString& stage, const QString& detail)
{
  const bool retryable = code == "MESH_SURFACE_GENERATION_FAILED"
    || code == "MESH_VOLUME_GENERATION_FAILED";
  sendMessage("error", jobId, QJsonObject{
    {"code", code},
    {"stage", stage},
    {"summaryKey", "meshing.worker_failed"},
    {"retryable", retryable},
    {"detail", detail},
  });
  return 1;
}

std::uint64_t processWorkingSetBytes(QProcess* process)
{
#ifdef Q_OS_WIN
  HANDLE handle = GetCurrentProcess();
  bool closeHandle = false;
  if (process != nullptr)
  {
    const qint64 processId = process->processId();
    if (processId <= 0)
    {
      return 0;
    }
    handle = OpenProcess(
      PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
      FALSE,
      static_cast<DWORD>(processId));
    if (handle == nullptr)
    {
      return 0;
    }
    closeHandle = true;
  }

  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  const bool succeeded = GetProcessMemoryInfo(
    handle, &counters, static_cast<DWORD>(sizeof(counters))) != FALSE;
  if (closeHandle)
  {
    CloseHandle(handle);
  }
  return succeeded
    ? static_cast<std::uint64_t>(counters.WorkingSetSize)
    : 0;
#else
  Q_UNUSED(process);
  return 0;
#endif
}

class JobTelemetry
{
public:
  JobTelemetry()
  {
    elapsed_.start();
  }

  QJsonObject progress(QProcess* backendProcess = nullptr)
  {
    const std::uint64_t workerBytes = processWorkingSetBytes(nullptr);
    const std::uint64_t backendBytes = processWorkingSetBytes(backendProcess);
    constexpr std::uint64_t MaximumJsonInteger =
      static_cast<std::uint64_t>(std::numeric_limits<qint64>::max());
    const std::uint64_t workingSetBytes =
      workerBytes > MaximumJsonInteger
        || backendBytes > MaximumJsonInteger - workerBytes
        ? MaximumJsonInteger
        : workerBytes + backendBytes;
    currentWorkingSetBytes_ = workingSetBytes;
    peakWorkingSetBytes_ = std::max(peakWorkingSetBytes_, workingSetBytes);
    return {
      {"elapsedMilliseconds", elapsed_.elapsed()},
      {"workingSetBytes", static_cast<qint64>(workingSetBytes)},
    };
  }

  QJsonObject statistics() const
  {
    return {
      {"elapsedMilliseconds", elapsed_.elapsed()},
      {"peakWorkingSetBytes", static_cast<qint64>(peakWorkingSetBytes_)},
    };
  }

  [[nodiscard]] std::uint64_t elapsedMilliseconds() const noexcept
  {
    return static_cast<std::uint64_t>(elapsed_.elapsed());
  }

  [[nodiscard]] std::uint64_t workingSetBytes() const noexcept
  {
    return currentWorkingSetBytes_;
  }

private:
  QElapsedTimer elapsed_;
  std::uint64_t currentWorkingSetBytes_ = 0;
  std::uint64_t peakWorkingSetBytes_ = 0;
};

void sendProgress(
  const QString& jobId,
  const QString& stage,
  int percent,
  JobTelemetry& telemetry,
  QProcess* backendProcess = nullptr)
{
  QJsonObject payload{
    {"stage", stage},
    {"percent", percent},
    {"messageKey", QString("meshing.stage.%1").arg(stage)},
  };
  const QJsonObject sample = telemetry.progress(backendProcess);
  payload.insert("elapsedMilliseconds", sample.value("elapsedMilliseconds"));
  payload.insert("workingSetBytes", sample.value("workingSetBytes"));
  sendMessage("progress", jobId, payload);
}

std::optional<QJsonObject> readJsonFile(const QString& path, QString& error)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
  {
    error = QString("Cannot open %1: %2").arg(path, file.errorString());
    return std::nullopt;
  }
  if (file.size() > MaximumJsonBytes)
  {
    error = QString("JSON file exceeds %1 bytes").arg(MaximumJsonBytes);
    return std::nullopt;
  }
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
  {
    error = QString("Invalid JSON object in %1: %2").arg(path, parseError.errorString());
    return std::nullopt;
  }
  return document.object();
}

enum class ControlReadStatus
{
  None,
  Message,
  Closed,
  Error,
};

struct ControlReadResult
{
  ControlReadStatus status = ControlReadStatus::None;
  QJsonObject message;
  QString error;
};

class ControlChannel
{
public:
  ControlReadResult readBlocking()
  {
    return read(true);
  }

  ControlReadResult poll()
  {
    return read(false);
  }

private:
  enum class NativeReadStatus
  {
    None,
    Data,
    Closed,
    Error,
  };

  ControlReadResult read(bool blocking)
  {
    while (true)
    {
      const qsizetype delimiter = buffer_.indexOf('\n');
      if (delimiter >= 0)
      {
        const QByteArray line = buffer_.left(delimiter);
        buffer_.remove(0, delimiter + 1);
        if (line.isEmpty() || line.size() > MaximumJsonBytes)
        {
          return {ControlReadStatus::Error, {}, "Empty or oversized control message"};
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
          return {
            ControlReadStatus::Error,
            {},
            QString("Invalid control JSON: %1").arg(parseError.errorString()),
          };
        }
        return {ControlReadStatus::Message, document.object(), {}};
      }
      if (buffer_.size() > MaximumJsonBytes)
      {
        return {ControlReadStatus::Error, {}, "Control message exceeds the byte limit"};
      }

      QString error;
      const NativeReadStatus nativeStatus = readNative(blocking, error);
      if (nativeStatus == NativeReadStatus::Data)
      {
        continue;
      }
      if (nativeStatus == NativeReadStatus::Closed)
      {
        return {ControlReadStatus::Closed, {}, "Main control channel closed"};
      }
      if (nativeStatus == NativeReadStatus::Error)
      {
        return {ControlReadStatus::Error, {}, std::move(error)};
      }
      return {};
    }
  }

  NativeReadStatus readNative(bool blocking, QString& error)
  {
    char chunk[4096];
#ifdef Q_OS_WIN
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE)
    {
      error = "Cannot access standard input";
      return NativeReadStatus::Error;
    }
    const DWORD inputType = GetFileType(input);
    if (!blocking && inputType == FILE_TYPE_DISK)
    {
      // Process-level tests can provide a sealed control file containing only
      // helloAck. Such a file has no live cancellation channel to poll.
      return NativeReadStatus::None;
    }
    DWORD available = 0;
    if (!blocking)
    {
      if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr))
      {
        const DWORD errorCode = GetLastError();
        if (errorCode == ERROR_BROKEN_PIPE)
        {
          return NativeReadStatus::Closed;
        }
        error = QString("Cannot inspect control pipe: Windows error %1")
          .arg(errorCode);
        return NativeReadStatus::Error;
      }
      if (available == 0)
      {
        return NativeReadStatus::None;
      }
    }
    const DWORD requested = blocking
      ? static_cast<DWORD>(sizeof(chunk))
      : qMin<DWORD>(available, static_cast<DWORD>(sizeof(chunk)));
    DWORD bytesRead = 0;
    if (!ReadFile(input, chunk, requested, &bytesRead, nullptr))
    {
      const DWORD errorCode = GetLastError();
      if (errorCode == ERROR_BROKEN_PIPE)
      {
        return NativeReadStatus::Closed;
      }
      error = QString("Cannot read control pipe: Windows error %1")
        .arg(errorCode);
      return NativeReadStatus::Error;
    }
    if (bytesRead == 0)
    {
      return NativeReadStatus::Closed;
    }
    buffer_.append(chunk, static_cast<qsizetype>(bytesRead));
#else
    if (!blocking)
    {
      pollfd descriptor{STDIN_FILENO, POLLIN, 0};
      const int pollResult = ::poll(&descriptor, 1, 0);
      if (pollResult < 0)
      {
        error = "Cannot inspect control pipe";
        return NativeReadStatus::Error;
      }
      if (pollResult == 0)
      {
        return NativeReadStatus::None;
      }
      if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0)
      {
        error = "Control pipe reported an error";
        return NativeReadStatus::Error;
      }
      if ((descriptor.revents & POLLIN) == 0
        && (descriptor.revents & POLLHUP) != 0)
      {
        return NativeReadStatus::Closed;
      }
    }
    const ssize_t bytesRead = ::read(STDIN_FILENO, chunk, sizeof(chunk));
    if (bytesRead < 0)
    {
      error = "Cannot read control pipe";
      return NativeReadStatus::Error;
    }
    if (bytesRead == 0)
    {
      return NativeReadStatus::Closed;
    }
    buffer_.append(chunk, static_cast<qsizetype>(bytesRead));
#endif
    return NativeReadStatus::Data;
  }

  QByteArray buffer_;
};

QString sha256(const QString& path, QString& error)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
  {
    error = QString("Cannot hash %1: %2").arg(path, file.errorString());
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file))
  {
    error = QString("Cannot read %1 while hashing").arg(path);
    return {};
  }
  return QString::fromLatin1(hash.result().toHex());
}

std::optional<QString> workspacePath(
  const QString& workspace,
  const QString& relativePath,
  QString& error)
{
  if (relativePath.isEmpty() || QDir::isAbsolutePath(relativePath))
  {
    error = QString("Expected a relative workspace path: %1").arg(relativePath);
    return std::nullopt;
  }
  const QString cleanRelative = QDir::cleanPath(relativePath);
  if (cleanRelative == ".." || cleanRelative.startsWith("../") || cleanRelative.startsWith("..\\"))
  {
    error = QString("Path escapes workspace: %1").arg(relativePath);
    return std::nullopt;
  }
  const QString absolute =
    QDir::fromNativeSeparators(QDir::cleanPath(QDir(workspace).absoluteFilePath(cleanRelative)));
  const QString prefix =
    QDir::fromNativeSeparators(QDir::cleanPath(workspace)) + '/';
  if (!absolute.startsWith(prefix, Qt::CaseInsensitive))
  {
    error = QString("Path escapes workspace: %1").arg(relativePath);
    return std::nullopt;
  }
  return absolute;
}

QString gmshVersion(const QString& executable, const QString& workspace, QString& error)
{
  const QString logPath = QDir(workspace).filePath("logs/gmsh-version.log");
  if (!QDir().mkpath(QFileInfo(logPath).absolutePath()))
  {
    error = "Cannot create Gmsh version log directory";
    return {};
  }
  QProcess process;
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.setStandardInputFile(QProcess::nullDevice());
  process.setStandardOutputFile(logPath, QIODevice::Truncate);
  process.start(executable, {"-version"}, QIODevice::ReadOnly);
  if (!process.waitForStarted(5000) || !process.waitForFinished(5000))
  {
    error = QString("Cannot execute Gmsh: %1").arg(process.errorString());
    return {};
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
  {
    error = QString("Gmsh version probe failed with exit code %1").arg(process.exitCode());
    return {};
  }
  QFile log(logPath);
  if (!log.open(QIODevice::ReadOnly))
  {
    error = QString("Cannot read Gmsh version log: %1").arg(log.errorString());
    return {};
  }
  return QString::fromLocal8Bit(log.readAll()).trimmed();
}

std::optional<MeshCounts> readMeshCounts(const QString& path, QString& error)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    error = QString("Cannot inspect mesh %1: %2").arg(path, file.errorString());
    return std::nullopt;
  }

  QTextStream stream(&file);
  MeshCounts counts;
  bool asciiMsh41 = false;
  bool sawEntities = false;
  bool sawNodes = false;
  bool sawElements = false;
  while (!stream.atEnd())
  {
    const QString marker = stream.readLine().trimmed();
    if (marker == "$MeshFormat")
    {
      asciiMsh41 = stream.readLine().simplified() == "4.1 0 8";
    }
    else if (marker == "$Entities")
    {
      const QStringList header =
        stream.readLine().simplified().split(' ', Qt::SkipEmptyParts);
      bool pointsOk = false;
      bool curvesOk = false;
      bool surfacesOk = false;
      bool volumesOk = false;
      const qint64 pointCount = header.value(0).toLongLong(&pointsOk);
      const qint64 curveCount = header.value(1).toLongLong(&curvesOk);
      const qint64 surfaceCount = header.value(2).toLongLong(&surfacesOk);
      const qint64 volumeCount = header.value(3).toLongLong(&volumesOk);
      if (header.size() != 4 || !pointsOk || !curvesOk || !surfacesOk
        || !volumesOk || pointCount < 0 || curveCount < 0
        || surfaceCount < 0 || volumeCount <= 0
        || pointCount > 2'000'000 || curveCount > 2'000'000
        || surfaceCount > 2'000'000 || volumeCount > 100'000)
      {
        error = "Invalid MSH 4.1 entity header";
        return std::nullopt;
      }
      for (qint64 index = 0;
        index < pointCount + curveCount + surfaceCount; ++index)
      {
        if (stream.readLine().isNull())
        {
          error = "Truncated MSH 4.1 entity block";
          return std::nullopt;
        }
      }
      for (qint64 index = 0; index < volumeCount; ++index)
      {
        const QStringList values =
          stream.readLine().simplified().split(' ', Qt::SkipEmptyParts);
        bool tagOk = false;
        const int tag = values.value(0).toInt(&tagOk);
        std::array<double, 6> bounds{};
        bool boundsOk = values.size() >= 9;
        for (qsizetype component = 0;
          boundsOk && component < static_cast<qsizetype>(bounds.size());
          ++component)
        {
          bool componentOk = false;
          bounds[static_cast<std::size_t>(component)] =
            values.value(component + 1).toDouble(&componentOk);
          boundsOk = componentOk
            && std::isfinite(bounds[static_cast<std::size_t>(component)]);
        }
        bool physicalCountOk = false;
        const int physicalCount =
          values.value(7).toInt(&physicalCountOk);
        const qsizetype boundaryCountIndex =
          8 + static_cast<qsizetype>(physicalCount);
        bool boundaryCountOk = false;
        const int boundaryCount =
          physicalCountOk && physicalCount >= 0
            ? values.value(boundaryCountIndex).toInt(&boundaryCountOk)
            : -1;
        if (!tagOk || tag <= 0 || !boundsOk
          || bounds[0] > bounds[3] || bounds[1] > bounds[4]
          || bounds[2] > bounds[5]
          || !physicalCountOk || physicalCount < 0
          || !boundaryCountOk || boundaryCount <= 0
          || values.size() != boundaryCountIndex + 1 + boundaryCount
          || counts.volumeEntityBounds.contains(tag))
        {
          error = "Invalid MSH 4.1 volume entity";
          return std::nullopt;
        }
        QSet<int> surfaceTags;
        for (int boundary = 0; boundary < boundaryCount; ++boundary)
        {
          bool surfaceTagOk = false;
          const int orientedTag =
            values.at(boundaryCountIndex + 1 + boundary)
              .toInt(&surfaceTagOk);
          if (!surfaceTagOk || orientedTag == 0
            || orientedTag == std::numeric_limits<int>::min())
          {
            error = "Invalid MSH 4.1 volume boundary Surface";
            return std::nullopt;
          }
          surfaceTags.insert(std::abs(orientedTag));
        }
        if (surfaceTags.size() != boundaryCount)
        {
          error = "Duplicate MSH 4.1 volume boundary Surface";
          return std::nullopt;
        }
        counts.volumeEntityBounds.insert(tag, bounds);
        counts.volumeEntitySurfaceTags.insert(tag, surfaceTags);
      }
      sawEntities = true;
    }
    else if (marker == "$Nodes")
    {
      const QStringList header = stream.readLine().simplified().split(' ');
      if (header.size() != 4)
      {
        error = "Invalid MSH 4.1 node header";
        return std::nullopt;
      }
      bool ok = false;
      counts.nodes = header.at(1).toLongLong(&ok);
      if (!ok || counts.nodes <= 0)
      {
        error = "MSH node count is invalid or empty";
        return std::nullopt;
      }
      sawNodes = true;
    }
    else if (marker == "$Elements")
    {
      const QStringList header = stream.readLine().simplified().split(' ');
      if (header.size() != 4)
      {
        error = "Invalid MSH 4.1 element header";
        return std::nullopt;
      }
      bool blocksOk = false;
      bool elementsOk = false;
      const qint64 blockCount = header.at(0).toLongLong(&blocksOk);
      counts.elements = header.at(1).toLongLong(&elementsOk);
      if (!blocksOk || !elementsOk || blockCount <= 0 || counts.elements <= 0)
      {
        error = "MSH element counts are invalid or empty";
        return std::nullopt;
      }
      for (qint64 block = 0; block < blockCount; ++block)
      {
        const QStringList blockHeader = stream.readLine().simplified().split(' ');
        if (blockHeader.size() != 4)
        {
          error = "Invalid MSH 4.1 element block header";
          return std::nullopt;
        }
        bool typeOk = false;
        bool countOk = false;
        bool dimensionOk = false;
        bool entityTagOk = false;
        const int entityDimension = blockHeader.at(0).toInt(&dimensionOk);
        const int entityTag = blockHeader.at(1).toInt(&entityTagOk);
        const int elementType = blockHeader.at(2).toInt(&typeOk);
        const qint64 elementCount = blockHeader.at(3).toLongLong(&countOk);
        if (!dimensionOk || !entityTagOk || !typeOk || !countOk
          || entityDimension < 0 || entityDimension > 3
          || entityTag <= 0 || elementCount < 0)
        {
          error = "Invalid MSH 4.1 element block";
          return std::nullopt;
        }
        if (elementType == 2)
        {
          counts.triangles += elementCount;
          if (entityDimension == 2 && elementCount > 0)
          {
            counts.triangleSurfaceEntityTags.insert(entityTag);
          }
        }
        else if (elementType == 4)
        {
          counts.tetrahedra += elementCount;
          if (entityDimension == 3 && elementCount > 0)
          {
            counts.tetrahedronVolumeEntityTags.insert(entityTag);
          }
        }
        for (qint64 element = 0; element < elementCount; ++element)
        {
          if (stream.readLine().isNull())
          {
            error = "Truncated MSH 4.1 element block";
            return std::nullopt;
          }
        }
      }
      sawElements = true;
    }
  }

  if (!asciiMsh41)
  {
    error = "Expected ASCII MSH 4.1 intermediate output";
    return std::nullopt;
  }
  if (!sawEntities || !sawNodes || !sawElements
    || counts.triangles <= 0 || counts.tetrahedra <= 0
    || counts.tetrahedronVolumeEntityTags.isEmpty())
  {
    error = "Mesh does not contain nodes, Triangle3, and Tetrahedron4 elements";
    return std::nullopt;
  }
  return counts;
}

bool writeJsonAtomically(const QString& path, const QJsonObject& object, QString& error)
{
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
  {
    error = QString("Cannot write %1: %2").arg(path, file.errorString());
    return false;
  }
  if (file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) < 0 || !file.commit())
  {
    error = QString("Cannot commit %1: %2").arg(path, file.errorString());
    return false;
  }
  return true;
}

bool writeByteArrayAtomically(
  const QString& path,
  const QByteArray& bytes,
  QString& error)
{
  if (bytes.isEmpty())
  {
    error = "Artifact payload is empty";
    return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
  {
    error = QString("Cannot write %1: %2").arg(path, file.errorString());
    return false;
  }
  if (file.write(bytes) != bytes.size() || !file.commit())
  {
    error = QString("Cannot commit %1: %2").arg(path, file.errorString());
    return false;
  }
  return true;
}


bool writeBinaryAtomically(
  const QString& path,
  const std::vector<std::uint8_t>& bytes,
  QString& error)
{
  if (bytes.empty()
    || bytes.size() > static_cast<std::size_t>((std::numeric_limits<qint64>::max)()))
  {
    error = "Neutral mesh payload size is invalid";
    return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
  {
    error = QString("Cannot write %1: %2").arg(path, file.errorString());
    return false;
  }
  const qint64 byteCount = static_cast<qint64>(bytes.size());
  if (file.write(reinterpret_cast<const char*>(bytes.data()), byteCount)
      != byteCount
    || !file.commit())
  {
    error = QString("Cannot commit %1: %2").arg(path, file.errorString());
    return false;
  }
  return true;
}

bool hasOnlyKeys(const QJsonObject& object, const QSet<QString>& allowed)
{
  for (auto it = object.constBegin(); it != object.constEnd(); ++it)
  {
    if (!allowed.contains(it.key()))
    {
      return false;
    }
  }
  return true;
}

struct CancellationRequest
{
  QString reason;
  QString deadlineUtc;
};

std::optional<CancellationRequest> parseCancellation(
  const QJsonObject& message,
  const QString& jobId,
  QString& error)
{
  const QJsonObject protocol = message.value("protocol").toObject();
  const QJsonObject payload = message.value("payload").toObject();
  const QString reason = payload.value("reason").toString();
  const QString deadlineUtc = payload.value("deadlineUtc").toString();
  if (!hasOnlyKeys(message,
      {"type", "protocol", "messageId", "jobId", "timestampUtc", "payload"})
    || !hasOnlyKeys(protocol, {"major", "minor"})
    || message.value("type").toString() != "cancel"
    || protocol.value("major").toInt(-1) != ProtocolMajor
    || protocol.value("minor").toInt(-1) < ProtocolMinor
    || message.value("messageId").toString().isEmpty()
    || message.value("jobId").toString() != jobId
    || !QDateTime::fromString(
      message.value("timestampUtc").toString(), Qt::ISODateWithMs).isValid()
    || !hasOnlyKeys(payload, {"reason", "deadlineUtc"})
    || reason.trimmed() != reason || reason.isEmpty() || reason.size() > 256
    || !QDateTime::fromString(deadlineUtc, Qt::ISODateWithMs).isValid())
  {
    error = "Cancellation message does not match the active job or protocol";
    return std::nullopt;
  }
  return CancellationRequest{reason, deadlineUtc};
}
} // namespace

int main(int argc, char* argv[])
{
  QCoreApplication application(argc, argv);
  QCoreApplication::setApplicationName("cceda_gmsh_worker");
  QCoreApplication::setApplicationVersion("0.1-executable-spike");

  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption workspaceOption("workspace", "Absolute job workspace.", "path");
  const QCommandLineOption gmshOption("gmsh", "Absolute Gmsh executable.", "path");
  parser.addOption(workspaceOption);
  parser.addOption(gmshOption);
  parser.process(application);

  const QString workspace = QDir::cleanPath(parser.value(workspaceOption));
  const QString gmshExecutable = QDir::cleanPath(parser.value(gmshOption));
  if (!QDir::isAbsolutePath(workspace) || !QFileInfo(workspace).isDir()
    || !QDir::isAbsolutePath(gmshExecutable) || !QFileInfo(gmshExecutable).isFile())
  {
    return fail({}, "MESH_INPUT_INVALID", "initializing",
      "--workspace and --gmsh must name existing absolute paths");
  }

  QString error;
  const QString backendVersion = gmshVersion(gmshExecutable, workspace, error);
  if (backendVersion.isEmpty())
  {
    return fail({}, "MESH_WORKER_LOST", "initializing", error);
  }
  sendMessage("hello", {}, QJsonObject{
    {"workerVersion", QCoreApplication::applicationVersion()},
    {"buildId", "gmsh-executable-adapter"},
    {"backendId", "gmsh"},
    {"backendVersion", backendVersion},
    {"capabilities", QJsonObject{
      {"dimensions", QJsonArray{3}},
      {"elementTypes", QJsonArray{"Triangle3", "Tetrahedron4"}},
      {"modes", QJsonArray{"independent-solids", "conformal-multi-region"}},
      {"optimization", true},
      {"quality", true},
    }},
  });

  ControlChannel controlChannel;
  const ControlReadResult helloAckRead = controlChannel.readBlocking();
  if (helloAckRead.status != ControlReadStatus::Message)
  {
    return fail({}, "MESH_PROTOCOL_INCOMPATIBLE", "initializing",
      helloAckRead.error.isEmpty() ? "Missing helloAck" : helloAckRead.error);
  }
  const QJsonObject helloAck = helloAckRead.message;
  const QString jobId = helloAck.value("jobId").toString();
  const QJsonObject ackProtocol = helloAck.value("protocol").toObject();
  const QString acknowledgedWorkspace =
    QDir::cleanPath(helloAck.value("payload").toObject().value("workspace").toString());
  if (helloAck.value("type").toString() != "helloAck"
    || ackProtocol.value("major").toInt(-1) != ProtocolMajor
    || jobId.isEmpty()
    || acknowledgedWorkspace.compare(workspace, Qt::CaseInsensitive) != 0)
  {
    return fail(jobId, "MESH_PROTOCOL_INCOMPATIBLE", "initializing",
      "helloAck does not match Worker protocol or workspace");
  }

  JobTelemetry telemetry;
  sendProgress(jobId, "initializing", 5, telemetry);
  const auto request = readJsonFile(QDir(workspace).filePath("request.json"), error);
  if (!request)
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing", error);
  }
  if (!hasOnlyKeys(*request,
      {"protocol", "jobId", "sourceGeometryRevision", "geometry", "output",
        "settings", "resourceLimits"})
    || request->value("jobId").toString() != jobId
    || request->value("protocol").toObject().value("major").toInt(-1) != ProtocolMajor)
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing",
      "request contains unknown fields or mismatched identity");
  }

  const qint64 geometryRevision = request->value("sourceGeometryRevision").toInteger(-1);
  const QJsonObject geometry = request->value("geometry").toObject();
  const QJsonObject output = request->value("output").toObject();
  const QJsonObject settings = request->value("settings").toObject();
  const QJsonObject resourceLimits =
    request->value("resourceLimits").toObject();
  const auto curvatureElements = boundedUnsignedInteger(
    settings.value("curvatureElementsPerCircle"),
    cceda::domain::MaximumCurvatureElementsPerCircle);
  const auto maximumThreads = boundedUnsignedInteger(
    settings.value("maximumThreads"),
    cceda::domain::MaximumMeshingThreads);
  const auto algorithm = meshAlgorithm(settings.value("algorithm"));
  const auto maximumElapsedMilliseconds = boundedUnsignedInteger64(
    resourceLimits.value("maximumElapsedMilliseconds"),
    cceda::domain::MaximumMeshingElapsedMilliseconds);
  const auto maximumWorkingSetBytes = boundedUnsignedInteger64(
    resourceLimits.value("maximumWorkingSetBytes"),
    cceda::domain::MaximumMeshingWorkingSetBytes);
  const QJsonObject geometrySchema = geometry.value("schema").toObject();
  const QJsonObject brep = geometry.value("brep").toObject();
  const QJsonObject geometryMapArtifact = geometry.value("map").toObject();
  if (geometryRevision <= 0
    || !hasOnlyKeys(geometry,
      {"schema", "mode", "canonicalUnit", "brep", "map"})
    || !hasOnlyKeys(geometrySchema, {"major", "minor"})
    || geometrySchema.value("major").toInt(-1)
      != cceda::meshing::GeometryMapSchemaMajor
    || geometrySchema.value("minor").toInt(-1)
      != cceda::meshing::GeometryMapSchemaMinor
    || (geometry.value("mode").toString() != "independent-solids"
      && geometry.value("mode").toString() != "conformal-multi-region")
    || geometry.value("canonicalUnit").toString() != "mm"
    || !hasOnlyKeys(brep, {"path", "sha256"})
    || !hasOnlyKeys(geometryMapArtifact, {"path", "sha256"})
    || !hasOnlyKeys(output,
      {"meshPath", "meshBinaryPath", "geometryMapResultPath",
        "qualityPath", "resultPath"})
    || !hasOnlyKeys(settings, {
      "minimumSizeMm", "maximumSizeMm", "curvatureElementsPerCircle",
      "optimizeTetrahedra", "maximumThreads", "algorithm"})
    || !settings.value("minimumSizeMm").isDouble()
    || !settings.value("maximumSizeMm").isDouble()
    || !curvatureElements || !maximumThreads || !algorithm
    || !settings.value("optimizeTetrahedra").isBool()
    || !hasOnlyKeys(resourceLimits,
      {"maximumElapsedMilliseconds", "maximumWorkingSetBytes"})
    || !maximumElapsedMilliseconds || !maximumWorkingSetBytes)
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing",
      "request fields are missing or unsupported");
  }

  const QString inputRelativePath = brep.value("path").toString();
  const QString expectedInputHash = brep.value("sha256").toString();
  if (inputRelativePath != "geometry/input.brep"
    || geometryMapArtifact.value("path").toString()
      != "geometry/geometry-map.json"
    || output.value("meshPath").toString()
      != "output/gmsh-intermediate.msh"
    || output.value("meshBinaryPath").toString()
      != "output/result.meshbin"
    || output.value("qualityPath").toString()
      != "output/quality.json"
    || output.value("geometryMapResultPath").toString()
      != "output/geometry-map-result.json"
    || output.value("resultPath").toString() != "output/result.json")
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing",
      "Snapshot request paths do not match the protocol workspace layout");
  }

  const auto inputPath = workspacePath(workspace, inputRelativePath, error);
  if (!inputPath || !QFileInfo(*inputPath).isFile())
  {
    return fail(jobId, "MESH_INPUT_INVALID", "loading-geometry",
      inputPath ? "Input geometry does not exist" : error);
  }
  const QString actualInputHash = sha256(*inputPath, error);
  if (actualInputHash.isEmpty()
    || actualInputHash.compare(expectedInputHash, Qt::CaseInsensitive) != 0)
  {
    return fail(jobId, "MESH_INPUT_INVALID", "loading-geometry",
      error.isEmpty() ? "Input geometry SHA-256 mismatch" : error);
  }

  const auto mapPath = workspacePath(
    workspace, geometryMapArtifact.value("path").toString(), error);
  if (!mapPath || !QFileInfo(*mapPath).isFile())
  {
    return fail(jobId, "MESH_INPUT_INVALID", "loading-geometry",
      mapPath ? "Geometry map does not exist" : error);
  }
  const QString actualMapHash = sha256(*mapPath, error);
  if (actualMapHash.isEmpty()
    || actualMapHash.compare(
      geometryMapArtifact.value("sha256").toString(), Qt::CaseInsensitive) != 0)
  {
    return fail(jobId, "MESH_INPUT_INVALID", "loading-geometry",
      error.isEmpty() ? "Geometry map SHA-256 mismatch" : error);
  }

  QFile mapFile(*mapPath);
  if (!mapFile.open(QIODevice::ReadOnly))
  {
    return fail(jobId, "MESH_INPUT_INVALID", "loading-geometry",
      QString("Cannot open geometry map: %1").arg(mapFile.errorString()));
  }
  const cceda::meshing::GeometryMapReadLimits mapLimits{
    16ULL * 1024ULL * 1024ULL,
    100'000,
    2'000'000,
    100'000,
    100'000,
    4096,
  };
  const auto parsed = cceda::meshing::readGeometryMap(mapFile, mapLimits);
  if (!parsed.succeeded()
    || parsed.map->sourceGeometryRevision
      != static_cast<std::uint64_t>(geometryRevision)
    || parsed.map->mode != geometry.value("mode").toString())
  {
    return fail(jobId, "MESH_INPUT_INVALID", "loading-geometry",
      parsed.succeeded()
        ? "Geometry map revision does not match request.json"
        : QString("Geometry map is invalid: %1").arg(parsed.detail));
  }
  sendProgress(jobId, "preparing-topology", 25, telemetry);
  const cceda::meshing::GeometryMap parsedGeometryMap = *parsed.map;
  const auto resolvedGeometryMapResultPath = workspacePath(
    workspace, output.value("geometryMapResultPath").toString(), error);
  if (!resolvedGeometryMapResultPath)
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing", error);
  }
  const QString geometryMapResultPath = *resolvedGeometryMapResultPath;

  const auto meshPath = workspacePath(workspace, output.value("meshPath").toString(), error);
  const auto meshBinaryPath = workspacePath(
    workspace, output.value("meshBinaryPath").toString(), error);
  const auto qualityPath = workspacePath(
    workspace, output.value("qualityPath").toString(), error);
  const auto resultPath = workspacePath(workspace, output.value("resultPath").toString(), error);
  const auto logPath = workspacePath(workspace, "logs/gmsh.log", error);
  if (!meshPath || !meshBinaryPath || !qualityPath || !resultPath || !logPath)
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing", error);
  }
  const QFileInfo meshFileInfo(*meshPath);
  if (meshFileInfo.suffix().compare("msh", Qt::CaseInsensitive) != 0)
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing",
      "Experimental mesh output must use the .msh extension");
  }
  if (QFileInfo(*meshBinaryPath).suffix().compare("meshbin", Qt::CaseInsensitive) != 0)
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing",
      "Neutral mesh output must use the .meshbin extension");
  }
  const QString temporaryMeshPath = meshFileInfo.dir().filePath(
    meshFileInfo.completeBaseName() + ".tmp." + meshFileInfo.suffix());
  if (!QDir().mkpath(QFileInfo(*meshPath).absolutePath())
    || !QDir().mkpath(QFileInfo(*meshBinaryPath).absolutePath())
    || !QDir().mkpath(QFileInfo(*qualityPath).absolutePath())
    || !QDir().mkpath(QFileInfo(*resultPath).absolutePath())
    || !QDir().mkpath(QFileInfo(geometryMapResultPath).absolutePath())
    || !QDir().mkpath(QFileInfo(*logPath).absolutePath()))
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing",
      "Cannot create controlled output directories");
  }

  const auto removePublishedArtifacts = [&] {
    QFile::remove(temporaryMeshPath);
    QFile::remove(*meshPath);
    QFile::remove(*meshBinaryPath);
    QFile::remove(geometryMapResultPath);
    QFile::remove(*qualityPath);
    QFile::remove(*resultPath);
  };


  auto checkCancellation =
    [&](QProcess* backendProcess, const QString& stage) -> std::optional<int> {
      const ControlReadResult control = controlChannel.poll();
      if (control.status == ControlReadStatus::None)
      {
        return std::nullopt;
      }
      if (control.status == ControlReadStatus::Closed
        || control.status == ControlReadStatus::Error)
      {
        if (backendProcess != nullptr
          && backendProcess->state() != QProcess::NotRunning)
        {
          backendProcess->kill();
          backendProcess->waitForFinished(5000);
        }
        removePublishedArtifacts();
        return fail(jobId, "MESH_WORKER_LOST", stage, control.error);
      }

      QString cancellationError;
      const auto cancellation =
        parseCancellation(control.message, jobId, cancellationError);
      if (!cancellation)
      {
        if (backendProcess != nullptr
          && backendProcess->state() != QProcess::NotRunning)
        {
          backendProcess->kill();
          backendProcess->waitForFinished(5000);
        }
        removePublishedArtifacts();
        return fail(jobId, "MESH_PROTOCOL_INCOMPATIBLE", stage, cancellationError);
      }

      sendMessage("cancelAck", jobId, QJsonObject{
        {"reason", cancellation->reason},
        {"deadlineUtc", cancellation->deadlineUtc},
      });
      if (backendProcess != nullptr
        && backendProcess->state() != QProcess::NotRunning)
      {
        backendProcess->kill();
        backendProcess->waitForFinished(5000);
      }
      removePublishedArtifacts();
      sendMessage("cancelled", jobId, QJsonObject{
        {"reason", cancellation->reason},
        {"stage", stage},
        {"forced", false},
      });
      return 0;
    };

  const cceda::domain::MeshGenerationSettings effectiveSettings{
    settings.value("minimumSizeMm").toDouble(),
    settings.value("maximumSizeMm").toDouble(),
    *curvatureElements,
    settings.value("optimizeTetrahedra").toBool(),
    *maximumThreads,
    *algorithm,
  };
  const cceda::domain::MeshingResourceLimits effectiveResourceLimits{
    *maximumElapsedMilliseconds,
    *maximumWorkingSetBytes,
  };
  if (!cceda::domain::isValidMeshGenerationSettings(effectiveSettings)
    || !cceda::domain::isValidMeshingResourceLimits(effectiveResourceLimits))
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing",
      "Meshing settings or resource limits are outside protocol-0.1 limits");
  }

  const auto checkResourceLimits =
    [&](QProcess* backendProcess, const QString& stage) -> std::optional<int> {
      const bool elapsedExceeded =
        effectiveResourceLimits.maximumElapsedMilliseconds > 0
        && telemetry.elapsedMilliseconds()
          >= effectiveResourceLimits.maximumElapsedMilliseconds;
      const bool workingSetExceeded =
        effectiveResourceLimits.maximumWorkingSetBytes > 0
        && telemetry.workingSetBytes()
          >= effectiveResourceLimits.maximumWorkingSetBytes;
      if (!elapsedExceeded && !workingSetExceeded)
      {
        return std::nullopt;
      }
      if (backendProcess != nullptr
        && backendProcess->state() != QProcess::NotRunning)
      {
        backendProcess->kill();
        backendProcess->waitForFinished(5000);
      }
      removePublishedArtifacts();
      return fail(jobId, "MESH_RESOURCE_LIMIT", stage,
        elapsedExceeded
          ? QString("Meshing exceeded the configured %1 ms wall-clock limit")
              .arg(effectiveResourceLimits.maximumElapsedMilliseconds)
          : QString("Meshing exceeded the configured %1 byte working-set limit")
              .arg(effectiveResourceLimits.maximumWorkingSetBytes));
    };

  sendProgress(jobId, "loading-geometry", 15, telemetry);
  if (const auto limited = checkResourceLimits(nullptr, "loading-geometry"))
  {
    return *limited;
  }
  sendProgress(jobId, "generating-volume", 35, telemetry);
  if (const auto limited = checkResourceLimits(nullptr, "generating-volume"))
  {
    return *limited;
  }
  if (const auto cancelled = checkCancellation(nullptr, "generating-volume"))
  {
    return *cancelled;
  }
  QFile::remove(temporaryMeshPath);
  QStringList gmshArguments{
    *inputPath,
    "-3",
    "-format", "msh4",
    "-o", temporaryMeshPath,
    "-nopopup",
    "-v", "4",
    "-algo", gmshAlgorithmId(effectiveSettings.algorithm),
  };
  if (effectiveSettings.minimumSizeMm > 0.0)
  {
    gmshArguments << "-clmin"
      << QString::number(effectiveSettings.minimumSizeMm, 'g', 17);
  }
  if (effectiveSettings.maximumSizeMm > 0.0)
  {
    gmshArguments << "-clmax"
      << QString::number(effectiveSettings.maximumSizeMm, 'g', 17);
  }
  if (effectiveSettings.curvatureElementsPerCircle > 0)
  {
    gmshArguments << "-clcurv"
      << QString::number(effectiveSettings.curvatureElementsPerCircle);
  }
  if (effectiveSettings.optimizeTetrahedra)
  {
    gmshArguments << "-optimize";
  }
  if (effectiveSettings.maximumThreads > 0)
  {
    gmshArguments << "-nt"
      << QString::number(effectiveSettings.maximumThreads);
  }

  QProcess gmsh;
  gmsh.setWorkingDirectory(workspace);
  gmsh.setProcessChannelMode(QProcess::MergedChannels);
  gmsh.setStandardInputFile(QProcess::nullDevice());
  gmsh.setStandardOutputFile(*logPath, QIODevice::Truncate);
  gmsh.start(gmshExecutable, gmshArguments, QIODevice::ReadOnly);
  if (!gmsh.waitForStarted(5000))
  {
    gmsh.kill();
    gmsh.waitForFinished(5000);
    return fail(jobId, "MESH_WORKER_LOST", "generating-volume",
      QString("Gmsh did not start: %1").arg(gmsh.errorString()));
  }
  sendProgress(jobId, "generating-volume", 35, telemetry, &gmsh);
  if (const auto limited = checkResourceLimits(&gmsh, "generating-volume"))
  {
    return *limited;
  }
  QElapsedTimer telemetryUpdate;
  telemetryUpdate.start();
  while (gmsh.state() != QProcess::NotRunning)
  {
    if (const auto cancelled = checkCancellation(&gmsh, "generating-volume"))
    {
      return *cancelled;
    }
    if (const auto limited = checkResourceLimits(&gmsh, "generating-volume"))
    {
      return *limited;
    }
    if (gmsh.waitForFinished(25))
    {
      break;
    }
    if (telemetryUpdate.elapsed() >= 250)
    {
      sendProgress(jobId, "generating-volume", 35, telemetry, &gmsh);
      telemetryUpdate.restart();
      if (const auto limited =
          checkResourceLimits(&gmsh, "generating-volume"))
      {
        return *limited;
      }
    }
  }
  if (const auto cancelled = checkCancellation(&gmsh, "generating-volume"))
  {
    return *cancelled;
  }
  if (gmsh.exitStatus() != QProcess::NormalExit || gmsh.exitCode() != 0
    || !QFileInfo(temporaryMeshPath).isFile())
  {
    QFile::remove(temporaryMeshPath);
    return fail(jobId, "MESH_VOLUME_GENERATION_FAILED", "generating-volume",
      QString("Gmsh exit code %1; see logs/gmsh.log").arg(gmsh.exitCode()));
  }

  sendProgress(jobId, "converting-mesh", 70, telemetry);
  if (const auto limited = checkResourceLimits(nullptr, "converting-mesh"))
  {
    return *limited;
  }
  const auto counts = readMeshCounts(temporaryMeshPath, error);
  if (!counts)
  {
    QFile::remove(temporaryMeshPath);
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "writing-result", error);
  }
  const QString meshHash = sha256(temporaryMeshPath, error);
  if (meshHash.isEmpty())
  {
    QFile::remove(temporaryMeshPath);
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "writing-result", error);
  }

  const auto resolvedMapping =
    resolveGeometryMapMapping(parsedGeometryMap, *counts, error);
  if (!resolvedMapping)
  {
    QFile::remove(temporaryMeshPath);
    return fail(jobId, "MESH_TOPOLOGY_PREPARATION_FAILED",
      "preparing-topology", error);
  }

  const QByteArray nativeMeshPath = QFile::encodeName(temporaryMeshPath);
  std::ifstream meshInput(nativeMeshPath.constData(), std::ios::binary);
  if (!meshInput.is_open())
  {
    QFile::remove(temporaryMeshPath);
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "writing-result",
      "Cannot open validated Gmsh output for neutral conversion");
  }
  const cceda::meshing::GmshMsh41ReadResult converted =
    cceda::meshing::readGmshMsh41(
      meshInput,
      {
        static_cast<cceda::domain::GeometryRevision>(geometryRevision),
        utcTimestamp().toStdString(),
        backendVersion.toStdString(),
        parsedGeometryMap.mode == "conformal-multi-region"
          ? cceda::domain::MeshConformity::Conformal
          : cceda::domain::MeshConformity::NonConformal,
      },
      resolvedMapping->meshMapping,
      {
        512ULL * 1024ULL * 1024ULL,
        64ULL * 1024ULL,
        10'000'000,
        40'000'000,
        MaximumMshEntityBlocks,
      });
  meshInput.close();
  if (!converted.succeeded())
  {
    QFile::remove(temporaryMeshPath);
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "writing-result",
      QString("Cannot convert Gmsh output at line %1: %2")
        .arg(converted.line)
        .arg(QString::fromStdString(converted.detail)));
  }
  sendProgress(jobId, "evaluating-quality", 80, telemetry);
  if (const auto limited = checkResourceLimits(nullptr, "evaluating-quality"))
  {
    return *limited;
  }
  const cceda::mesh_quality::MeshQualityEvaluationResult evaluated =
    cceda::mesh_quality::evaluateMeshQuality(
      *converted.mesh,
      cceda::domain::MeshValidationMode::WorkerArtifact);
  if (!evaluated.succeeded() || !evaluated.report->validForCommit())
  {
    QFile::remove(temporaryMeshPath);
    return fail(jobId, "MESH_QUALITY_INVALID", "evaluating-quality",
      evaluated.succeeded()
        ? "Neutral mesh contains commit-blocking quality issues"
        : "Neutral mesh quality evaluation rejected the mesh structure");
  }
  const cceda::meshing::MeshQualityJsonWriteResult qualityJson =
    cceda::meshing::writeMeshQualityJson(*evaluated.report, jobId);
  if (!qualityJson.succeeded()
      || !writeByteArrayAtomically(*qualityPath, qualityJson.bytes, error))
  {
    QFile::remove(temporaryMeshPath);
    QFile::remove(*qualityPath);
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "evaluating-quality",
      qualityJson.succeeded() ? error : qualityJson.detail);
  }
  const QString qualityHash = sha256(*qualityPath, error);
  if (qualityHash.isEmpty())
  {
    QFile::remove(temporaryMeshPath);
    QFile::remove(*qualityPath);
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "evaluating-quality", error);
  }
  if (const auto cancelled =
      checkCancellation(nullptr, "evaluating-quality"))
  {
    return *cancelled;
  }
  sendProgress(jobId, "writing-result", 85, telemetry);
  if (const auto limited = checkResourceLimits(nullptr, "writing-result"))
  {
    return *limited;
  }

  const cceda::meshing::MeshBinaryWriteResult encoded =
    cceda::meshing::writeMeshBinary(
      *converted.mesh,
      cceda::domain::MeshValidationMode::WorkerArtifact);
  if (!encoded.succeeded()
    || !writeBinaryAtomically(*meshBinaryPath, encoded.bytes, error))
  {
    QFile::remove(temporaryMeshPath);
    QFile::remove(*meshBinaryPath);
    QFile::remove(*qualityPath);
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "writing-result",
      encoded.succeeded()
        ? error
        : QString("Cannot encode neutral mesh: %1")
            .arg(QString::fromStdString(encoded.detail)));
  }
  const QString meshBinaryHash = sha256(*meshBinaryPath, error);
  if (meshBinaryHash.isEmpty())
  {
    QFile::remove(temporaryMeshPath);
    QFile::remove(*meshBinaryPath);
    QFile::remove(*qualityPath);
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "writing-result", error);
  }
  if (const auto cancelled = checkCancellation(nullptr, "writing-result"))
  {
    return *cancelled;
  }
  if (QFileInfo::exists(*meshPath) || !QFile::rename(temporaryMeshPath, *meshPath))
  {
    QFile::remove(temporaryMeshPath);
    QFile::remove(*meshBinaryPath);
    QFile::remove(*qualityPath);
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "writing-result",
      "Cannot publish the validated experimental mesh");
  }

  const QJsonObject geometryMapResult{
    {"schema", QJsonObject{
      {"name", "cceda.geometry-map-result"},
      {"major", 0},
      {"minor", 1},
    }},
    {"jobId", jobId},
    {"sourceGeometryRevision", geometryRevision},
    {"mode", parsedGeometryMap.mode},
    {"regions", resolvedMapping->serializedRegions},
  };
  if (!writeJsonAtomically(geometryMapResultPath, geometryMapResult, error))
  {
    QFile::remove(*meshBinaryPath);
    QFile::remove(*qualityPath);
    QFile::remove(*meshPath);
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "writing-result", error);
  }
  const QString geometryMapResultHash = sha256(geometryMapResultPath, error);
  if (geometryMapResultHash.isEmpty())
  {
    QFile::remove(geometryMapResultPath);
    QFile::remove(*qualityPath);
    QFile::remove(*meshBinaryPath);
    QFile::remove(*meshPath);
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "writing-result", error);
  }

  const QJsonObject effectiveSettingsJson{
    {"minimumSizeMm", effectiveSettings.minimumSizeMm},
    {"maximumSizeMm", effectiveSettings.maximumSizeMm},
    {"curvatureElementsPerCircle",
      static_cast<qint64>(effectiveSettings.curvatureElementsPerCircle)},
    {"optimizeTetrahedra", effectiveSettings.optimizeTetrahedra},
    {"maximumThreads", static_cast<qint64>(effectiveSettings.maximumThreads)},
    {"algorithm", meshAlgorithmId(effectiveSettings.algorithm)},
  };
  const QJsonObject effectiveResourceLimitsJson{
    {"maximumElapsedMilliseconds",
      static_cast<qint64>(effectiveResourceLimits.maximumElapsedMilliseconds)},
    {"maximumWorkingSetBytes",
      static_cast<qint64>(effectiveResourceLimits.maximumWorkingSetBytes)},
  };
  sendProgress(jobId, "completed", 100, telemetry);
  if (const auto limited = checkResourceLimits(nullptr, "completed"))
  {
    return *limited;
  }
  const QJsonObject statistics = telemetry.statistics();
  QJsonObject result{
    {"experimental", true},
    {"format", "gmsh-msh-4.1-intermediate"},
    {"jobId", jobId},
    {"sourceGeometryRevision", geometryRevision},
    {"backendId", "gmsh"},
    {"backendVersion", backendVersion},
    {"meshPath", output.value("meshPath").toString()},
    {"meshSha256", meshHash},
    {"meshBinaryPath", output.value("meshBinaryPath").toString()},
    {"meshBinarySha256", meshBinaryHash},
    {"qualityPath", output.value("qualityPath").toString()},
    {"qualitySha256", qualityHash},
    {"effectiveSettings", effectiveSettingsJson},
    {"effectiveResourceLimits", effectiveResourceLimitsJson},
    {"statistics", statistics},
    {"counts", QJsonObject{
      {"nodes", counts->nodes},
      {"elements", counts->elements},
      {"triangles", counts->triangles},
      {"tetrahedra", counts->tetrahedra},
    }},
  };
  result.insert("geometryMapResultPath",
    output.value("geometryMapResultPath").toString());
  result.insert("geometryMapResultSha256", geometryMapResultHash);
  if (!writeJsonAtomically(*resultPath, result, error))
  {
    QFile::remove(geometryMapResultPath);
    QFile::remove(*qualityPath);
    QFile::remove(*meshBinaryPath);
    QFile::remove(*meshPath);
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "writing-result", error);
  }
  if (const auto cancelled = checkCancellation(nullptr, "writing-result"))
  {
    return *cancelled;
  }

  QJsonObject successPayload{
    {"status", "experimental-success"},
    {"sourceGeometryRevision", geometryRevision},
    {"resultPath", output.value("resultPath").toString()},
    {"nodeCount", counts->nodes},
    {"triangleCount", counts->triangles},
    {"tetrahedronCount", counts->tetrahedra},
    {"meshSha256", meshHash},
    {"meshBinarySha256", meshBinaryHash},
    {"qualitySha256", qualityHash},
    {"effectiveSettings", effectiveSettingsJson},
    {"effectiveResourceLimits", effectiveResourceLimitsJson},
    {"statistics", statistics},
  };
  if (!geometryMapResultHash.isEmpty())
  {
    successPayload.insert("geometryMapResultSha256", geometryMapResultHash);
  }
  sendMessage("result", jobId, successPayload);
  return 0;
}
