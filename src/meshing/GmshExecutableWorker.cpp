#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

#include <cstdio>
#include <optional>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace
{
constexpr int ProtocolMajor = 0;
constexpr int ProtocolMinor = 1;
constexpr qint64 MaximumJsonBytes = 64 * 1024;

struct MeshCounts
{
  qint64 nodes = 0;
  qint64 elements = 0;
  qint64 triangles = 0;
  qint64 tetrahedra = 0;
};

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
  sendMessage("error", jobId, QJsonObject{
    {"code", code},
    {"stage", stage},
    {"summaryKey", "meshing.worker_failed"},
    {"retryable", false},
    {"detail", detail},
  });
  return 1;
}

void sendProgress(const QString& jobId, const QString& stage, int percent)
{
  sendMessage("progress", jobId, QJsonObject{
    {"stage", stage},
    {"percent", percent},
    {"messageKey", QString("meshing.stage.%1").arg(stage)},
  });
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
  bool sawNodes = false;
  bool sawElements = false;
  while (!stream.atEnd())
  {
    const QString marker = stream.readLine().trimmed();
    if (marker == "$MeshFormat")
    {
      asciiMsh41 = stream.readLine().simplified() == "4.1 0 8";
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
        const int elementType = blockHeader.at(2).toInt(&typeOk);
        const qint64 elementCount = blockHeader.at(3).toLongLong(&countOk);
        if (!typeOk || !countOk || elementCount < 0)
        {
          error = "Invalid MSH 4.1 element block";
          return std::nullopt;
        }
        if (elementType == 2)
        {
          counts.triangles += elementCount;
        }
        else if (elementType == 4)
        {
          counts.tetrahedra += elementCount;
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
  if (!sawNodes || !sawElements || counts.triangles <= 0 || counts.tetrahedra <= 0)
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
      {"modes", QJsonArray{"independent-solids"}},
      {"optimization", false},
      {"quality", false},
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

  sendProgress(jobId, "initializing", 5);
  const auto request = readJsonFile(QDir(workspace).filePath("request.json"), error);
  if (!request)
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing", error);
  }
  if (!hasOnlyKeys(*request,
      {"protocol", "jobId", "sourceGeometryRevision", "geometry", "output", "settings"})
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
  if (geometryRevision <= 0
    || !hasOnlyKeys(geometry, {"path", "sha256"})
    || !hasOnlyKeys(output, {"meshPath", "resultPath"})
    || !hasOnlyKeys(settings, {"minimumSizeMm", "maximumSizeMm"})
    || (settings.contains("minimumSizeMm")
      && !settings.value("minimumSizeMm").isDouble())
    || (settings.contains("maximumSizeMm")
      && !settings.value("maximumSizeMm").isDouble()))
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing",
      "request fields are missing or unsupported");
  }

  const auto inputPath = workspacePath(workspace, geometry.value("path").toString(), error);
  if (!inputPath || !QFileInfo(*inputPath).isFile())
  {
    return fail(jobId, "MESH_INPUT_INVALID", "loading-geometry",
      inputPath ? "Input geometry does not exist" : error);
  }
  const QString actualInputHash = sha256(*inputPath, error);
  if (actualInputHash.isEmpty()
    || actualInputHash.compare(geometry.value("sha256").toString(), Qt::CaseInsensitive) != 0)
  {
    return fail(jobId, "MESH_INPUT_INVALID", "loading-geometry",
      error.isEmpty() ? "Input geometry SHA-256 mismatch" : error);
  }

  const auto meshPath = workspacePath(workspace, output.value("meshPath").toString(), error);
  const auto resultPath = workspacePath(workspace, output.value("resultPath").toString(), error);
  const auto logPath = workspacePath(workspace, "logs/gmsh.log", error);
  if (!meshPath || !resultPath || !logPath)
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing", error);
  }
  const QFileInfo meshFileInfo(*meshPath);
  if (meshFileInfo.suffix().compare("msh", Qt::CaseInsensitive) != 0)
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing",
      "Experimental mesh output must use the .msh extension");
  }
  const QString temporaryMeshPath = meshFileInfo.dir().filePath(
    meshFileInfo.completeBaseName() + ".tmp." + meshFileInfo.suffix());
  if (!QDir().mkpath(QFileInfo(*meshPath).absolutePath())
    || !QDir().mkpath(QFileInfo(*resultPath).absolutePath())
    || !QDir().mkpath(QFileInfo(*logPath).absolutePath()))
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing",
      "Cannot create controlled output directories");
  }

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
        QFile::remove(temporaryMeshPath);
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
        QFile::remove(temporaryMeshPath);
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
      QFile::remove(temporaryMeshPath);
      QFile::remove(*meshPath);
      QFile::remove(*resultPath);
      sendMessage("cancelled", jobId, QJsonObject{
        {"reason", cancellation->reason},
        {"stage", stage},
        {"forced", false},
      });
      return 0;
    };

  const double minimumSize = settings.value("minimumSizeMm").toDouble(0.0);
  const double maximumSize = settings.value("maximumSizeMm").toDouble(0.0);
  if (minimumSize < 0.0 || maximumSize < 0.0
    || (maximumSize > 0.0 && minimumSize > maximumSize))
  {
    return fail(jobId, "MESH_INPUT_INVALID", "initializing",
      "Invalid minimum/maximum mesh size");
  }

  sendProgress(jobId, "loading-geometry", 15);
  sendProgress(jobId, "generating-volume", 35);
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
  };
  if (minimumSize > 0.0)
  {
    gmshArguments << "-clmin" << QString::number(minimumSize, 'g', 17);
  }
  if (maximumSize > 0.0)
  {
    gmshArguments << "-clmax" << QString::number(maximumSize, 'g', 17);
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
  QElapsedTimer gmshDeadline;
  gmshDeadline.start();
  while (gmsh.state() != QProcess::NotRunning)
  {
    if (const auto cancelled = checkCancellation(&gmsh, "generating-volume"))
    {
      return *cancelled;
    }
    if (gmsh.waitForFinished(25))
    {
      break;
    }
    if (gmshDeadline.elapsed() >= 30'000)
    {
      gmsh.kill();
      gmsh.waitForFinished(5000);
      return fail(jobId, "MESH_WORKER_LOST", "generating-volume",
        "Gmsh exceeded the executable-adapter time limit");
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

  sendProgress(jobId, "writing-result", 85);
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
  if (const auto cancelled = checkCancellation(nullptr, "writing-result"))
  {
    return *cancelled;
  }
  if (QFileInfo::exists(*meshPath) || !QFile::rename(temporaryMeshPath, *meshPath))
  {
    QFile::remove(temporaryMeshPath);
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "writing-result",
      "Cannot publish the validated experimental mesh");
  }

  const QJsonObject result{
    {"experimental", true},
    {"format", "gmsh-msh-4.1-intermediate"},
    {"jobId", jobId},
    {"sourceGeometryRevision", geometryRevision},
    {"backendId", "gmsh"},
    {"backendVersion", backendVersion},
    {"meshPath", output.value("meshPath").toString()},
    {"meshSha256", meshHash},
    {"counts", QJsonObject{
      {"nodes", counts->nodes},
      {"elements", counts->elements},
      {"triangles", counts->triangles},
      {"tetrahedra", counts->tetrahedra},
    }},
  };
  if (!writeJsonAtomically(*resultPath, result, error))
  {
    return fail(jobId, "MESH_OUTPUT_CORRUPT", "writing-result", error);
  }
  if (const auto cancelled = checkCancellation(nullptr, "writing-result"))
  {
    return *cancelled;
  }

  sendProgress(jobId, "completed", 100);
  sendMessage("result", jobId, QJsonObject{
    {"status", "experimental-success"},
    {"sourceGeometryRevision", geometryRevision},
    {"resultPath", output.value("resultPath").toString()},
    {"nodeCount", counts->nodes},
    {"triangleCount", counts->triangles},
    {"tetrahedronCount", counts->tetrahedra},
    {"meshSha256", meshHash},
  });
  return 0;
}
