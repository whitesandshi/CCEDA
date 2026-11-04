#include "application/MeshingJobController.h"
#include "application/WorkerMeshArtifactValidator.h"

#include "meshing/GmshExecutableJobWorkspace.h"
#include "meshing/GeometryMap.h"
#include "meshing/GmshWorkerClient.h"
#include "meshing/MeshQualityJson.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QByteArrayView>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

#include <cmath>
#include <new>
#include <utility>
#include <vector>

namespace cceda::application
{
namespace
{
constexpr qint64 MaximumManifestBytes = 64 * 1024;
constexpr qint64 MaximumGeometryMapBytes = 16LL * 1024LL * 1024LL;
constexpr qint64 MaximumNeutralMeshBytes = 512LL * 1024LL * 1024LL;
constexpr qint64 MaximumQualityBytes =
  static_cast<qint64>(meshing::DefaultMeshQualityJsonMaximumBytes);
constexpr qint64 HashBufferBytes = 1024 * 1024;

struct ActiveJob
{
  MeshingJobSubmission submission;
  domain::GeometryRevision sourceGeometryRevision = 0;
  QString workspace;
  domain::MeshGenerationSettings attemptSettings;
  domain::MeshingResourceLimits attemptResourceLimits;
  std::uint32_t attemptNumber = 1;
  std::uint64_t peakWorkingSetBytes = 0;
  std::optional<meshing::PreparedGmshExecutableJob> preparedJob;
  std::optional<meshing::GmshWorkerResult> workerResult;
};

struct ArtifactValidation
{
  bool valid = false;
  QString detail;
  std::optional<domain::MeshModel> mesh;
  std::optional<meshing::MeshQualityJsonArtifact> quality;
  std::optional<mesh_quality::MeshQualityIssueDetails> issueElementDetails;
};

bool hasValidJobId(const QString& jobId)
{
  static const QRegularExpression pattern(
    QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
  return pattern.match(jobId).hasMatch();
}

bool isSha256(const QString& digest)
{
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
  return pattern.match(digest).hasMatch();
}

bool hasValidSettings(const MeshingJobSubmission& submission)
{
  return domain::isValidMeshGenerationSettings(submission.settings)
    && domain::isValidMeshGenerationMode(submission.mode);
}

QString modeId(domain::MeshGenerationMode mode)
{
  return mode == domain::MeshGenerationMode::ConformalMultiRegion
    ? QStringLiteral("conformal-multi-region")
    : QStringLiteral("independent-solids");
}

domain::MeshGenerationAlgorithm fallbackAlgorithm(
  domain::MeshGenerationAlgorithm algorithm) noexcept
{
  return algorithm == domain::MeshGenerationAlgorithm::Delaunay
    ? domain::MeshGenerationAlgorithm::Hxt
    : domain::MeshGenerationAlgorithm::Delaunay;
}

bool sameResourceLimits(
  const domain::MeshingResourceLimits& left,
  const domain::MeshingResourceLimits& right) noexcept
{
  return left.maximumElapsedMilliseconds == right.maximumElapsedMilliseconds
    && left.maximumWorkingSetBytes == right.maximumWorkingSetBytes;
}

MeshingJobFailure makeFailure(
  MeshingJobFailureKind kind,
  const QString& jobId,
  domain::GeometryRevision sourceGeometryRevision,
  QString code,
  QString stage,
  QString detail,
  bool retryable = false,
  std::uint32_t attemptCount = 0)
{
  return {
    kind,
    jobId,
    sourceGeometryRevision,
    std::move(code),
    std::move(stage),
    std::move(detail),
    retryable,
    attemptCount,
  };
}

ArtifactValidation invalidArtifact(QString detail)
{
  return {
    false,
    std::move(detail),
    std::nullopt,
    std::nullopt,
    std::nullopt,
  };
}

std::shared_ptr<AcceptedMeshQuality> acceptedQuality(
  meshing::MeshQualityJsonArtifact artifact,
  mesh_quality::MeshQualityIssueDetails issueElementDetails,
  domain::MeshRevision committedMeshRevision)
{
  auto accepted = std::make_shared<AcceptedMeshQuality>();
  accepted->meshRevision = committedMeshRevision;
  accepted->sourceGeometryRevision = artifact.sourceGeometryRevision;
  accepted->metricVersion = std::move(artifact.metricVersion);
  accepted->validForCommit = artifact.validForCommit;
  accepted->triangleCount = artifact.triangleCount;
  accepted->tetrahedronCount = artifact.tetrahedronCount;
  accepted->numericSummaries.reserve(artifact.numericSummaries.size());
  for (meshing::MeshQualityNumericSummary& summary : artifact.numericSummaries)
  {
    accepted->numericSummaries.push_back({
      summary.metric,
      summary.sampleCount,
      summary.minimum,
      summary.maximum,
      summary.mean,
    });
  }
  accepted->booleanSummary = {
    artifact.booleanSummary.metric,
    artifact.booleanSummary.sampleCount,
    artifact.booleanSummary.failedElementCount,
  };
  accepted->issues = std::move(artifact.issues);
  accepted->issueElementDetails = std::move(issueElementDetails);
  return accepted;
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

bool sameSettings(
  const domain::MeshGenerationSettings& left,
  const domain::MeshGenerationSettings& right) noexcept
{
  return left.minimumSizeMm == right.minimumSizeMm
    && left.maximumSizeMm == right.maximumSizeMm
    && left.curvatureElementsPerCircle == right.curvatureElementsPerCircle
    && left.optimizeTetrahedra == right.optimizeTetrahedra
    && left.maximumThreads == right.maximumThreads
    && left.algorithm == right.algorithm;
}

bool settingsMatch(
  const QJsonObject& object,
  const domain::MeshGenerationSettings& expected)
{
  if (!hasOnlyKeys(object, {
      "minimumSizeMm", "maximumSizeMm", "curvatureElementsPerCircle",
      "optimizeTetrahedra", "maximumThreads", "algorithm"})
    || object.size() != 6
    || !object.value("minimumSizeMm").isDouble()
    || !object.value("maximumSizeMm").isDouble()
    || !object.value("curvatureElementsPerCircle").isDouble()
    || !object.value("optimizeTetrahedra").isBool()
    || !object.value("maximumThreads").isDouble()
    || !object.value("algorithm").isString())
  {
    return false;
  }
  const qint64 curvature =
    object.value("curvatureElementsPerCircle").toInteger(-1);
  const qint64 threads = object.value("maximumThreads").toInteger(-1);
  return curvature >= 0 && threads >= 0
    && object.value("curvatureElementsPerCircle").toDouble()
      == static_cast<double>(curvature)
    && object.value("maximumThreads").toDouble()
      == static_cast<double>(threads)
    && object.value("minimumSizeMm").toDouble() == expected.minimumSizeMm
    && object.value("maximumSizeMm").toDouble() == expected.maximumSizeMm
    && static_cast<std::uint64_t>(curvature)
      == expected.curvatureElementsPerCircle
    && object.value("optimizeTetrahedra").toBool()
      == expected.optimizeTetrahedra
    && static_cast<std::uint64_t>(threads) == expected.maximumThreads
    && object.value("algorithm").toString()
      == (expected.algorithm == domain::MeshGenerationAlgorithm::Hxt
        ? "hxt" : "delaunay");
}

bool resourceLimitsMatch(
  const QJsonObject& object,
  const domain::MeshingResourceLimits& expected)
{
  if (!hasOnlyKeys(object,
      {"maximumElapsedMilliseconds", "maximumWorkingSetBytes"})
    || object.size() != 2)
  {
    return false;
  }
  const qint64 elapsed =
    object.value("maximumElapsedMilliseconds").toInteger(-1);
  const qint64 workingSet =
    object.value("maximumWorkingSetBytes").toInteger(-1);
  return elapsed >= 0 && workingSet >= 0
    && object.value("maximumElapsedMilliseconds").toDouble()
      == static_cast<double>(elapsed)
    && object.value("maximumWorkingSetBytes").toDouble()
      == static_cast<double>(workingSet)
    && static_cast<std::uint64_t>(elapsed)
      == expected.maximumElapsedMilliseconds
    && static_cast<std::uint64_t>(workingSet)
      == expected.maximumWorkingSetBytes;
}

bool statisticsMatch(
  const QJsonObject& object,
  const meshing::GmshWorkerResult& expected)
{
  if (!hasOnlyKeys(object, {"elapsedMilliseconds", "peakWorkingSetBytes"})
    || object.size() != 2)
  {
    return false;
  }
  const qint64 elapsed = object.value("elapsedMilliseconds").toInteger(-1);
  const qint64 peakWorkingSet = object.value("peakWorkingSetBytes").toInteger(-1);
  return elapsed >= 0 && peakWorkingSet >= 0
    && object.value("elapsedMilliseconds").toDouble()
      == static_cast<double>(elapsed)
    && object.value("peakWorkingSetBytes").toDouble()
      == static_cast<double>(peakWorkingSet)
    && static_cast<std::uint64_t>(elapsed) == expected.elapsedMilliseconds
    && static_cast<std::uint64_t>(peakWorkingSet)
      == expected.peakWorkingSetBytes;
}

ArtifactValidation validateArtifacts(
  const ActiveJob& active)
{
  if (!active.preparedJob || !active.workerResult)
  {
    return invalidArtifact("Controller has no prepared job or Worker result");
  }

  const auto& prepared = *active.preparedJob;
  const auto& worker = *active.workerResult;
  const QString reportedManifest =
    QDir::cleanPath(QDir(prepared.workspace).filePath(worker.resultPath));
  if (reportedManifest != QDir::cleanPath(prepared.resultPath))
  {
    return invalidArtifact("Worker reported an unexpected result manifest path");
  }

  const QFileInfo manifestInfo(prepared.resultPath);
  const QFileInfo meshInfo(prepared.meshPath);
  const QFileInfo meshBinaryInfo(prepared.meshBinaryPath);
  const QFileInfo qualityInfo(prepared.qualityPath);
  const QFileInfo mapResultInfo(prepared.geometryMapResultPath);
  if (!manifestInfo.isFile() || manifestInfo.isSymbolicLink()
    || manifestInfo.size() <= 0 || manifestInfo.size() > MaximumManifestBytes
    || !meshInfo.isFile() || meshInfo.isSymbolicLink() || meshInfo.size() <= 0
    || !meshBinaryInfo.isFile() || meshBinaryInfo.isSymbolicLink()
    || meshBinaryInfo.size() <= 0
    || meshBinaryInfo.size() > MaximumNeutralMeshBytes
    || !mapResultInfo.isFile() || mapResultInfo.isSymbolicLink()
    || mapResultInfo.size() <= 0 || mapResultInfo.size() > MaximumManifestBytes
    || !qualityInfo.isFile() || qualityInfo.isSymbolicLink()
    || qualityInfo.size() <= 0 || qualityInfo.size() > MaximumQualityBytes
    || worker.meshBinarySha256.size() != 64
    || worker.geometryMapResultSha256.size() != 64
    || worker.qualitySha256.size() != 64)
  {
    return invalidArtifact("Worker output files are missing, unsafe, empty, or oversized");
  }

  QFile manifestFile(prepared.resultPath);
  if (!manifestFile.open(QIODevice::ReadOnly))
  {
    return invalidArtifact(
      QString("Cannot read result manifest: %1").arg(manifestFile.errorString()));
  }
  const QByteArray manifestBytes = manifestFile.read(MaximumManifestBytes + 1);
  if (manifestFile.error() != QFileDevice::NoError
    || manifestBytes.size() > MaximumManifestBytes
    || !manifestFile.atEnd()
    || manifestFile.size() != manifestInfo.size())
  {
    return invalidArtifact("Worker result manifest changed or failed while reading");
  }
  QJsonParseError parseError;
  const QJsonDocument manifest =
    QJsonDocument::fromJson(manifestBytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !manifest.isObject())
  {
    return invalidArtifact("Worker result manifest is not valid JSON");
  }

  const QJsonObject root = manifest.object();
  const QJsonObject counts = root.value("counts").toObject();
  const QJsonObject effectiveSettings =
    root.value("effectiveSettings").toObject();
  const QJsonObject effectiveResourceLimits =
    root.value("effectiveResourceLimits").toObject();
  const QJsonObject statistics = root.value("statistics").toObject();
  if (!hasOnlyKeys(root, {
      "experimental", "format", "jobId", "sourceGeometryRevision",
      "backendId", "backendVersion", "meshPath", "meshSha256",
      "meshBinaryPath", "meshBinarySha256",
      "geometryMapResultPath", "geometryMapResultSha256",
      "qualityPath", "qualitySha256", "effectiveSettings",
      "effectiveResourceLimits", "statistics", "counts"})
    || !hasOnlyKeys(counts,
      {"nodes", "elements", "triangles", "tetrahedra"})
    || !root.value("experimental").toBool(false)
    || root.value("format").toString() != "gmsh-msh-4.1-intermediate"
    || root.value("jobId").toString() != active.submission.jobId
    || root.value("sourceGeometryRevision").toInteger(-1)
      != static_cast<qint64>(active.sourceGeometryRevision)
    || root.value("backendId").toString() != "gmsh"
    || !sameSettings(worker.effectiveSettings, active.attemptSettings)
    || !settingsMatch(effectiveSettings, worker.effectiveSettings)
    || !sameResourceLimits(
      worker.effectiveResourceLimits, active.attemptResourceLimits)
    || !resourceLimitsMatch(
      effectiveResourceLimits, worker.effectiveResourceLimits)
    || !statisticsMatch(statistics, worker)
    || QDir::cleanPath(QDir(prepared.workspace).filePath(
      root.value("meshPath").toString())) != QDir::cleanPath(prepared.meshPath)
    || root.value("meshSha256").toString().compare(
      worker.meshSha256, Qt::CaseInsensitive) != 0
    || QDir::cleanPath(QDir(prepared.workspace).filePath(
      root.value("meshBinaryPath").toString()))
      != QDir::cleanPath(prepared.meshBinaryPath)
    || root.value("meshBinarySha256").toString().compare(
      worker.meshBinarySha256, Qt::CaseInsensitive) != 0
    || QDir::cleanPath(QDir(prepared.workspace).filePath(
      root.value("geometryMapResultPath").toString()))
      != QDir::cleanPath(prepared.geometryMapResultPath)
    || root.value("geometryMapResultSha256").toString().compare(
      worker.geometryMapResultSha256, Qt::CaseInsensitive) != 0
    || QDir::cleanPath(QDir(prepared.workspace).filePath(
      root.value("qualityPath").toString()))
      != QDir::cleanPath(prepared.qualityPath)
    || root.value("qualitySha256").toString().compare(
      worker.qualitySha256, Qt::CaseInsensitive) != 0
    || counts.value("nodes").toInteger(-1) != static_cast<qint64>(worker.nodeCount)
    || counts.value("triangles").toInteger(-1)
      != static_cast<qint64>(worker.triangleCount)
    || counts.value("tetrahedra").toInteger(-1)
      != static_cast<qint64>(worker.tetrahedronCount))
  {
    return invalidArtifact(
      "Worker result manifest does not match the submitted job or control result");
  }

  QFile meshFile(prepared.meshPath);
  if (!meshFile.open(QIODevice::ReadOnly))
  {
    return invalidArtifact(
      QString("Cannot read intermediate mesh: %1").arg(meshFile.errorString()));
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!meshFile.atEnd())
  {
    const QByteArray chunk = meshFile.read(HashBufferBytes);
    if (chunk.isEmpty() && meshFile.error() != QFileDevice::NoError)
    {
      return invalidArtifact(
        QString("Cannot hash intermediate mesh: %1").arg(meshFile.errorString()));
    }
    hash.addData(QByteArrayView(chunk));
  }
  if (meshFile.size() != meshInfo.size())
  {
    return invalidArtifact("Intermediate mesh size changed during verification");
  }
  const QString actualHash = QString::fromLatin1(hash.result().toHex());
  if (actualHash.compare(worker.meshSha256, Qt::CaseInsensitive) != 0)
  {
    return invalidArtifact("Intermediate mesh SHA-256 does not match the Worker result");
  }

  QFile meshBinaryFile(prepared.meshBinaryPath);
  if (!meshBinaryFile.open(QIODevice::ReadOnly))
  {
    return invalidArtifact(
      QString("Cannot read neutral mesh: %1").arg(meshBinaryFile.errorString()));
  }
  QCryptographicHash meshBinaryHash(QCryptographicHash::Sha256);
  std::vector<std::uint8_t> meshBinaryBytes;
  try
  {
    meshBinaryBytes.reserve(static_cast<std::size_t>(meshBinaryInfo.size()));
    while (!meshBinaryFile.atEnd())
    {
      const QByteArray chunk = meshBinaryFile.read(HashBufferBytes);
      if (chunk.isEmpty())
      {
        return invalidArtifact(meshBinaryFile.error() == QFileDevice::NoError
          ? "Neutral mesh stopped yielding data before end of file"
          : QString("Cannot hash neutral mesh: %1")
              .arg(meshBinaryFile.errorString()));
      }
      if (static_cast<std::uint64_t>(chunk.size())
        > static_cast<std::uint64_t>(MaximumNeutralMeshBytes)
          - static_cast<std::uint64_t>(meshBinaryBytes.size()))
      {
        return invalidArtifact("Neutral mesh grew beyond the byte budget while reading");
      }
      meshBinaryHash.addData(QByteArrayView(chunk));
      const auto* begin = reinterpret_cast<const std::uint8_t*>(chunk.constData());
      meshBinaryBytes.insert(
        meshBinaryBytes.end(), begin, begin + chunk.size());
    }
  }
  catch (const std::bad_alloc&)
  {
    return invalidArtifact("Neutral mesh exceeds available validation memory");
  }
  if (meshBinaryFile.size() != meshBinaryInfo.size()
    || meshBinaryBytes.size()
      != static_cast<std::size_t>(meshBinaryInfo.size()))
  {
    return invalidArtifact("Neutral mesh size changed during verification");
  }
  const QString actualMeshBinaryHash =
    QString::fromLatin1(meshBinaryHash.result().toHex());
  if (actualMeshBinaryHash.compare(
    worker.meshBinarySha256, Qt::CaseInsensitive) != 0)
  {
    return invalidArtifact(
      "Neutral mesh SHA-256 does not match the Worker result");
  }

  QFile qualityFile(prepared.qualityPath);
  if (!qualityFile.open(QIODevice::ReadOnly))
  {
    return invalidArtifact(
      QString("Cannot read quality result: %1").arg(qualityFile.errorString()));
  }
  const QByteArray qualityBytes = qualityFile.read(MaximumQualityBytes + 1);
  if (qualityFile.error() != QFileDevice::NoError
    || qualityBytes.size() > MaximumQualityBytes
    || !qualityFile.atEnd()
    || qualityFile.size() != qualityInfo.size())
  {
    return invalidArtifact("Quality result changed or failed while reading");
  }
  const QString actualQualityHash = QString::fromLatin1(
    QCryptographicHash::hash(
      qualityBytes, QCryptographicHash::Sha256).toHex());
  if (actualQualityHash.compare(
    worker.qualitySha256, Qt::CaseInsensitive) != 0)
  {
    return invalidArtifact(
      "Quality result SHA-256 does not match the Worker result");
  }

  QBuffer qualityBuffer;
  qualityBuffer.setData(qualityBytes);
  if (!qualityBuffer.open(QIODevice::ReadOnly))
  {
    return invalidArtifact("Cannot parse the verified quality result");
  }
  meshing::MeshQualityJsonReadResult quality =
    meshing::readMeshQualityJson(qualityBuffer);
  if (!quality.succeeded()
    || quality.artifact->jobId != active.submission.jobId
    || quality.artifact->sourceGeometryRevision
      != active.sourceGeometryRevision
    || quality.artifact->triangleCount != worker.triangleCount
    || quality.artifact->tetrahedronCount != worker.tetrahedronCount
    || !quality.artifact->validForCommit)
  {
    return invalidArtifact(quality.succeeded()
      ? "Quality result identity, counts, or commit validity are invalid"
      : QString("Quality result is invalid: %1").arg(quality.detail));
  }

  QFile mapResultFile(prepared.geometryMapResultPath);
  if (!mapResultFile.open(QIODevice::ReadOnly))
  {
    return invalidArtifact(
      QString("Cannot read geometry-map result: %1").arg(mapResultFile.errorString()));
  }
  const QByteArray mapResultBytes = mapResultFile.read(MaximumManifestBytes + 1);
  if (mapResultFile.error() != QFileDevice::NoError
    || mapResultBytes.size() > MaximumManifestBytes
    || !mapResultFile.atEnd()
    || mapResultFile.size() != mapResultInfo.size())
  {
    return invalidArtifact("Geometry-map result changed or failed while reading");
  }
  const QString actualMapHash = QString::fromLatin1(
    QCryptographicHash::hash(mapResultBytes, QCryptographicHash::Sha256).toHex());
  if (actualMapHash.compare(
    worker.geometryMapResultSha256, Qt::CaseInsensitive) != 0)
  {
    return invalidArtifact(
      "Geometry-map result SHA-256 does not match the Worker result");
  }

  QJsonParseError mapParseError;
  const QJsonDocument mapResultDocument =
    QJsonDocument::fromJson(mapResultBytes, &mapParseError);
  if (mapParseError.error != QJsonParseError::NoError
    || !mapResultDocument.isObject())
  {
    return invalidArtifact("Geometry-map result is not valid JSON");
  }
  const QJsonObject mapRoot = mapResultDocument.object();
  const QJsonObject mapSchema = mapRoot.value("schema").toObject();
  const QJsonArray mappedRegions = mapRoot.value("regions").toArray();
  if (!hasOnlyKeys(mapRoot,
      {"schema", "jobId", "sourceGeometryRevision", "mode", "regions"})
    || !hasOnlyKeys(mapSchema, {"name", "major", "minor"})
    || mapSchema.value("name").toString() != "cceda.geometry-map-result"
    || mapSchema.value("major").toInt(-1) != 0
    || mapSchema.value("minor").toInt(-1) != 1
    || mapRoot.value("jobId").toString() != active.submission.jobId
    || mapRoot.value("sourceGeometryRevision").toInteger(-1)
      != static_cast<qint64>(active.sourceGeometryRevision)
    || mapRoot.value("mode").toString() != modeId(active.submission.mode)
    || mappedRegions.isEmpty())
  {
    return invalidArtifact("Geometry-map result identity or schema is invalid");
  }

  const QFileInfo inputMapInfo(prepared.geometryMapPath);
  QFile inputMapFile(prepared.geometryMapPath);
  if (!inputMapInfo.isFile() || inputMapInfo.isSymbolicLink()
    || inputMapInfo.size() <= 0 || inputMapInfo.size() > MaximumGeometryMapBytes
    || !inputMapFile.open(QIODevice::ReadOnly))
  {
    return invalidArtifact("Submitted geometry map is missing, unsafe, or oversized");
  }
  const QByteArray inputMapBytes = inputMapFile.read(MaximumGeometryMapBytes + 1);
  if (inputMapFile.error() != QFileDevice::NoError
    || inputMapBytes.size() > MaximumGeometryMapBytes
    || !inputMapFile.atEnd() || inputMapFile.size() != inputMapInfo.size())
  {
    return invalidArtifact("Submitted geometry map changed or failed while reading");
  }
  const QString actualInputMapHash = QString::fromLatin1(
    QCryptographicHash::hash(inputMapBytes, QCryptographicHash::Sha256).toHex());
  if (actualInputMapHash.compare(
    prepared.geometryMapSha256, Qt::CaseInsensitive) != 0)
  {
    return invalidArtifact("Submitted geometry map SHA-256 changed after sealing");
  }

  QBuffer inputMapBuffer;
  inputMapBuffer.setData(inputMapBytes);
  if (!inputMapBuffer.open(QIODevice::ReadOnly))
  {
    return invalidArtifact("Cannot parse the verified submitted geometry map");
  }
  const meshing::GeometryMapReadLimits inputMapLimits{
    static_cast<std::uint64_t>(MaximumGeometryMapBytes),
    100'000,
    2'000'000,
    100'000,
    100'000,
    4096,
  };
  const auto inputMap = meshing::readGeometryMap(inputMapBuffer, inputMapLimits);
  if (!inputMap.succeeded()
    || mappedRegions.size()
      != static_cast<qsizetype>(inputMap.map->regions.size()))
  {
    return invalidArtifact(
      "Geometry-map result does not cover every submitted snapshot region");
  }
  std::vector<WorkerMeshArtifactRegionExpectation> expectedRegions;
  expectedRegions.reserve(inputMap.map->regions.size());
  QSet<qint64> uniqueRegionIds;
  QSet<int> uniqueVolumeTags;
  for (const QJsonValue& value : mappedRegions)
  {
    const QJsonObject mappedRegion = value.toObject();
    const qint64 regionId = mappedRegion.value("regionId").toInteger(-1);
    const auto expected = std::find_if(
      inputMap.map->regions.begin(), inputMap.map->regions.end(),
      [regionId](const meshing::GeometryMapRegion& region) {
        return region.regionId == static_cast<std::uint64_t>(regionId);
      });
    const QJsonArray volumeTags =
      mappedRegion.value("gmshVolumeEntityTags").toArray();
    if (!value.isObject()
      || !hasOnlyKeys(mappedRegion,
        {"regionId", "sourceSolidGeometryId", "sourceSolidGeometryIds",
          "gmshVolumeEntityTags"})
      || !mappedRegion.value("regionId").isDouble()
      || mappedRegion.value("regionId").toDouble()
        != static_cast<double>(regionId)
      || regionId <= 0 || uniqueRegionIds.contains(regionId)
      || expected == inputMap.map->regions.end()
      || mappedRegion.value("sourceSolidGeometryId").toString()
        != expected->solidGeometryId
      || volumeTags.isEmpty())
    {
      return invalidArtifact(
        "Geometry-map result does not match the submitted snapshot regions");
    }
    const QJsonArray sourceIds = mappedRegion.value("sourceSolidGeometryIds").toArray();
    if (active.submission.mode == domain::MeshGenerationMode::ConformalMultiRegion)
    {
      if (sourceIds.isEmpty()
        || sourceIds.size() != expected->sourceSolidGeometryIds.size())
      {
        return invalidArtifact(
          "Conformal geometry-map result is missing source Solid mappings");
      }
      for (const QString& sourceId : expected->sourceSolidGeometryIds)
      {
        bool found = false;
        for (const QJsonValue& value : sourceIds)
        {
          if (value.toString() == sourceId)
          {
            found = true;
            break;
          }
        }
        if (!found)
        {
          return invalidArtifact(
            "Conformal geometry-map result source Solid mapping changed");
        }
      }
    }
    uniqueRegionIds.insert(regionId);
    for (const QJsonValue& tagValue : volumeTags)
    {
      const int tag = tagValue.toInt(-1);
      if (!tagValue.isDouble()
        || tagValue.toDouble() != static_cast<double>(tag)
        || tag <= 0 || uniqueVolumeTags.contains(tag))
      {
        return invalidArtifact(
          "Geometry-map result contains invalid Gmsh volume tags");
      }
      uniqueVolumeTags.insert(tag);
    }
    expectedRegions.push_back({
      expected->regionId,
      expected->solidGeometryId.toStdString(),
    });
  }

  meshing::MeshBinaryLimits meshLimits;
  meshLimits.maximumFileBytes =
    static_cast<std::uint64_t>(MaximumNeutralMeshBytes);
  WorkerMeshArtifactValidationResult meshValidation =
    validateWorkerMeshArtifact(
      meshBinaryBytes,
      {
        active.sourceGeometryRevision,
        root.value("backendId").toString().toStdString(),
        root.value("backendVersion").toString().toStdString(),
        worker.nodeCount,
        worker.triangleCount,
        worker.tetrahedronCount,
        expectedRegions,
      },
      meshLimits);
  if (!meshValidation.succeeded())
  {
    return invalidArtifact(QString::fromStdString(meshValidation.detail));
  }
  const mesh_quality::MeshQualityEvaluationResult recomputed =
    mesh_quality::evaluateMeshQuality(
      *meshValidation.mesh,
      domain::MeshValidationMode::WorkerArtifact);
  if (!recomputed.succeeded() || !recomputed.report->validForCommit())
  {
    return invalidArtifact(
      "Main-process quality evaluation rejected the neutral mesh");
  }
  const meshing::MeshQualityJsonWriteResult canonicalQuality =
    meshing::writeMeshQualityJson(
      *recomputed.report, active.submission.jobId);
  if (!canonicalQuality.succeeded()
    || canonicalQuality.bytes != qualityBytes)
  {
    return invalidArtifact(canonicalQuality.succeeded()
      ? "Quality result does not match main-process evaluation"
      : QString("Cannot canonicalize recomputed quality: %1")
          .arg(canonicalQuality.detail));
  }
  std::optional<mesh_quality::MeshQualityIssueDetails> issueElementDetails =
    mesh_quality::buildMeshQualityIssueDetails(
      *meshValidation.mesh, *recomputed.report);
  if (!issueElementDetails)
  {
    return invalidArtifact(
      "Cannot bind quality issue details to the verified neutral mesh");
  }

  return {
    true,
    {},
    std::move(*meshValidation.mesh),
    std::move(*quality.artifact),
    std::move(*issueElementDetails),
  };
}
}

class MeshingJobController::Implementation
{
public:
  Implementation(
    MeshingJobController* owner,
    domain::ProjectState& projectState,
    MeshingRuntimeConfiguration configuration)
    : owner_(owner)
    , projectState_(projectState)
    , configuration_(std::move(configuration))
    , workerClient_(owner)
  {
    // A QObject context makes these functor connections disconnect when the
    // controller is destroyed.
    // https://doc.qt.io/qt-6.8/qobject.html#connect-4
    QObject::connect(&preparationWatcher_,
      &QFutureWatcher<meshing::GmshJobWorkspaceResult>::finished,
      owner_,
      [this] { preparationFinished(); });
    QObject::connect(&validationWatcher_,
      &QFutureWatcher<ArtifactValidation>::finished,
      owner_,
      [this] { validationFinished(); });
    QObject::connect(&workerClient_, &meshing::GmshWorkerClient::progressChanged,
      owner_, [this](const meshing::GmshWorkerProgress& progress) {
        workerProgressed(progress);
      });
    QObject::connect(&workerClient_, &meshing::GmshWorkerClient::succeeded,
      owner_, [this](const meshing::GmshWorkerResult& result) {
        workerSucceeded(result);
      });
    QObject::connect(&workerClient_, &meshing::GmshWorkerClient::cancelled,
      owner_, [this](const meshing::GmshWorkerCancellation& cancellation) {
        workerCancelled(cancellation);
      });
    QObject::connect(&workerClient_, &meshing::GmshWorkerClient::failed,
      owner_, [this](const meshing::GmshWorkerFailure& failure) {
        workerFailed(failure);
      });
  }

  std::optional<MeshingJobFailure> submit(const MeshingJobSubmission& submission)
  {
    const domain::GeometryRevision revision = projectState_.geometryRevision();
    if (state_ != State::Idle)
    {
      return makeFailure(
        MeshingJobFailureKind::AlreadyRunning,
        submission.jobId,
        revision,
        "MESH_JOB_ALREADY_RUNNING",
        "validating",
        "This project already has an active mesh job");
    }
    if (!hasValidSubmission(submission, revision))
    {
      return makeFailure(
        MeshingJobFailureKind::InvalidRequest,
        submission.jobId,
        revision,
        "MESH_INPUT_INVALID",
        "validating",
        "Meshing paths, job identity, revision, size budget, settings, or resource limits are invalid");
    }

    active_ = ActiveJob{submission, revision};
    jobTimer_.start();
    lastStage_ = "initializing";
    startPreparation();
    return std::nullopt;
  }

  std::optional<MeshingJobFailure> cancel(
    const QString& reason,
    int deadlineMilliseconds)
  {
    const QString cleanReason = reason.trimmed();
    if (!active_ || state_ == State::Idle || isCancelling()
      || cleanReason.isEmpty() || cleanReason.size() > 256
      || deadlineMilliseconds <= 0 || deadlineMilliseconds > 60'000)
    {
      return makeFailure(
        MeshingJobFailureKind::InvalidRequest,
        active_ ? active_->submission.jobId : QString{},
        active_ ? active_->sourceGeometryRevision : projectState_.geometryRevision(),
        "MESH_CANCELLATION_INVALID",
        "cancelling",
        "Cancellation requires one active job, a bounded reason, and a valid deadline");
    }

    if (state_ == State::Running)
    {
      if (const auto failure =
          workerClient_.cancel(cleanReason, deadlineMilliseconds))
      {
        return makeFailure(
          MeshingJobFailureKind::InvalidRequest,
          active_->submission.jobId,
          active_->sourceGeometryRevision,
          failure->code,
          failure->stage,
          failure->detail,
          failure->retryable);
      }
    }

    cancellation_ = MeshingJobCancellation{
      active_->submission.jobId,
      active_->sourceGeometryRevision,
      cleanReason,
      lastStage_,
      false,
    };
    switch (state_)
    {
      case State::Preparing:
        state_ = State::CancellingPreparation;
        break;
      case State::Running:
        state_ = State::CancellingWorker;
        break;
      case State::Verifying:
        state_ = State::CancellingVerification;
        break;
      default:
        break;
    }
    emit owner_->cancelling(*cancellation_);
    return std::nullopt;
  }

  bool isRunning() const noexcept
  {
    return state_ != State::Idle;
  }

  bool isCancelling() const noexcept
  {
    return state_ == State::CancellingPreparation
      || state_ == State::CancellingWorker
      || state_ == State::CancellingVerification;
  }

private:
  enum class State
  {
    Idle,
    Preparing,
    Running,
    Verifying,
    CancellingPreparation,
    CancellingWorker,
    CancellingVerification,
  };

  bool hasValidSubmission(
    const MeshingJobSubmission& submission,
    domain::GeometryRevision revision) const
  {
    const QFileInfo workerInfo(configuration_.workerExecutable);
    const QFileInfo backendInfo(configuration_.backendExecutable);
    const QFileInfo brepInfo(submission.geometrySnapshotBrep);
    const QFileInfo mapInfo(submission.geometrySnapshotMap);
    return revision > 0
      && hasValidJobId(submission.jobId)
      && hasValidSettings(submission)
      && domain::isValidMeshingResourceLimits(submission.resourceLimits)
      && domain::isValidMeshingRetryPolicy(submission.retryPolicy)
      && QDir::isAbsolutePath(configuration_.jobsRoot)
      && QDir::isAbsolutePath(configuration_.workerExecutable)
      && workerInfo.isFile()
      && QDir::isAbsolutePath(configuration_.backendExecutable)
      && backendInfo.isFile()
      && !configuration_.mainBuildId.isEmpty()
      && configuration_.maximumGeometryBytes > 0
      && QDir::isAbsolutePath(submission.geometrySnapshotBrep)
      && QDir::isAbsolutePath(submission.geometrySnapshotMap)
      && brepInfo.isFile() && !brepInfo.isSymbolicLink()
      && mapInfo.isFile() && !mapInfo.isSymbolicLink()
      && isSha256(submission.geometrySnapshotSha256)
      && isSha256(submission.geometrySnapshotMapSha256);
  }

  bool geometryIsCurrent() const
  {
    return active_
      && active_->sourceGeometryRevision == projectState_.geometryRevision();
  }

  MeshingJobFailure staleFailure() const
  {
    return makeFailure(
      MeshingJobFailureKind::StaleGeometry,
      active_->submission.jobId,
      active_->sourceGeometryRevision,
      "MESH_RESULT_STALE",
      "completed",
      "Geometry changed after this mesh job was submitted");
  }

  std::uint32_t attemptLimit() const noexcept
  {
    return active_ && active_->submission.retryPolicy.automaticRetry
      ? active_->submission.retryPolicy.maximumAttempts : 1;
  }

  void startPreparation()
  {
    if (!active_)
    {
      return;
    }

    active_->attemptSettings = active_->submission.settings;
    if (active_->attemptNumber > 1)
    {
      active_->attemptSettings.algorithm = fallbackAlgorithm(
        active_->submission.settings.algorithm);
    }
    active_->attemptResourceLimits = active_->submission.resourceLimits;
    if (active_->attemptResourceLimits.maximumElapsedMilliseconds > 0)
    {
      const std::uint64_t elapsed = static_cast<std::uint64_t>(
        std::max<qint64>(0, jobTimer_.elapsed()));
      if (elapsed >= active_->attemptResourceLimits.maximumElapsedMilliseconds)
      {
        finishFailure(makeFailure(
          MeshingJobFailureKind::WorkerFailure,
          active_->submission.jobId,
          active_->sourceGeometryRevision,
          "MESH_RESOURCE_LIMIT",
          "initializing",
          "The total meshing time budget was exhausted before the next attempt",
          false,
          active_->attemptNumber));
        return;
      }
      active_->attemptResourceLimits.maximumElapsedMilliseconds -= elapsed;
    }

    const QString workspaceName = active_->attemptNumber == 1
      ? active_->submission.jobId
      : QString("%1.attempt-%2")
          .arg(active_->submission.jobId)
          .arg(active_->attemptNumber);
    active_->workspace = QDir::cleanPath(
      QDir(configuration_.jobsRoot).filePath(workspaceName));
    active_->preparedJob.reset();
    active_->workerResult.reset();
    state_ = State::Preparing;
    const meshing::GmshSnapshotJobWorkspaceRequest request{
      active_->workspace,
      active_->submission.geometrySnapshotBrep,
      active_->submission.geometrySnapshotMap,
      active_->submission.geometrySnapshotSha256,
      active_->submission.geometrySnapshotMapSha256,
      active_->submission.jobId,
      active_->sourceGeometryRevision,
      active_->attemptSettings,
      active_->attemptResourceLimits,
      configuration_.maximumGeometryBytes,
      active_->submission.mode,
    };
    preparationWatcher_.setFuture(
      QtConcurrent::run([request] {
        return meshing::prepareGmshSnapshotJobWorkspace(request);
      }));
  }

  void preparationFinished()
  {
    if ((state_ != State::Preparing && state_ != State::CancellingPreparation)
      || !active_)
    {
      return;
    }
    const meshing::GmshJobWorkspaceResult result = preparationWatcher_.result();
    if (state_ == State::CancellingPreparation)
    {
      finishCancelled();
      return;
    }
    if (!result.succeeded())
    {
      finishFailure(makeFailure(
        MeshingJobFailureKind::WorkspacePreparationFailed,
        active_->submission.jobId,
        active_->sourceGeometryRevision,
        "MESH_WORKSPACE_PREPARATION_FAILED",
        "initializing",
        result.detail));
      return;
    }
    active_->preparedJob = *result.job;
    if (!geometryIsCurrent())
    {
      finishFailure(staleFailure());
      return;
    }

    state_ = State::Running;
    lastStage_ = "initializing";
    const auto startFailure = workerClient_.start({
      configuration_.workerExecutable,
      configuration_.backendExecutable,
      active_->preparedJob->workspace,
      active_->submission.jobId,
      active_->sourceGeometryRevision,
      configuration_.mainBuildId,
      active_->attemptResourceLimits,
    });
    if (startFailure)
    {
      finishFailure(makeFailure(
        MeshingJobFailureKind::WorkerFailure,
        active_->submission.jobId,
        active_->sourceGeometryRevision,
        startFailure->code,
        startFailure->stage,
        startFailure->detail,
        startFailure->retryable));
    }
  }

  void workerProgressed(const meshing::GmshWorkerProgress& progress)
  {
    if ((state_ == State::Running || state_ == State::CancellingWorker) && active_)
    {
      lastStage_ = progress.stage;
      active_->peakWorkingSetBytes = std::max(
        active_->peakWorkingSetBytes, progress.workingSetBytes);
      if (state_ == State::Running)
      {
        emit owner_->progressChanged({
          active_->submission.jobId,
          active_->sourceGeometryRevision,
          progress.stage,
          progress.percent,
          static_cast<std::uint64_t>(std::max<qint64>(0, jobTimer_.elapsed())),
          progress.workingSetBytes,
          active_->attemptNumber,
          attemptLimit(),
        });
      }
    }
  }

  void workerSucceeded(const meshing::GmshWorkerResult& result)
  {
    if (state_ != State::Running || !active_)
    {
      return;
    }
    active_->workerResult = result;
    active_->peakWorkingSetBytes = std::max(
      active_->peakWorkingSetBytes, result.peakWorkingSetBytes);
    if (!geometryIsCurrent())
    {
      finishFailure(staleFailure());
      return;
    }

    state_ = State::Verifying;
    lastStage_ = "writing-result";
    const ActiveJob active = *active_;
    validationWatcher_.setFuture(
      QtConcurrent::run([active] { return validateArtifacts(active); }));
  }

  void workerFailed(const meshing::GmshWorkerFailure& failure)
  {
    if ((state_ != State::Running && state_ != State::CancellingWorker)
      || !active_)
    {
      return;
    }
    if (!geometryIsCurrent())
    {
      finishFailure(staleFailure());
      return;
    }
    if (state_ == State::Running
      && failure.retryable
      && active_->attemptNumber < attemptLimit())
    {
      removePublishedArtifacts();
      ++active_->attemptNumber;
      lastStage_ = "retrying";
      emit owner_->progressChanged({
        active_->submission.jobId,
        active_->sourceGeometryRevision,
        lastStage_,
        std::nullopt,
        static_cast<std::uint64_t>(std::max<qint64>(0, jobTimer_.elapsed())),
        active_->peakWorkingSetBytes,
        active_->attemptNumber,
        attemptLimit(),
      });
      startPreparation();
      return;
    }
    finishFailure(makeFailure(
      MeshingJobFailureKind::WorkerFailure,
      active_->submission.jobId,
      active_->sourceGeometryRevision,
      failure.code,
      failure.stage,
      failure.detail,
      failure.retryable,
      active_->attemptNumber));
  }

  void workerCancelled(const meshing::GmshWorkerCancellation& cancellation)
  {
    if (state_ != State::CancellingWorker || !active_ || !cancellation_)
    {
      return;
    }
    cancellation_->stage = cancellation.stage;
    cancellation_->forced = cancellation.forced;
    finishCancelled();
  }

  void validationFinished()
  {
    if ((state_ != State::Verifying && state_ != State::CancellingVerification)
      || !active_
      || !active_->preparedJob || !active_->workerResult)
    {
      return;
    }
    ArtifactValidation validation = validationWatcher_.future().takeResult();
    if (state_ == State::CancellingVerification)
    {
      finishCancelled();
      return;
    }
    if (!geometryIsCurrent())
    {
      finishFailure(staleFailure());
      return;
    }
    if (!validation.valid)
    {
      finishFailure(makeFailure(
        MeshingJobFailureKind::OutputValidationFailed,
        active_->submission.jobId,
        active_->sourceGeometryRevision,
        "MESH_OUTPUT_CORRUPT",
        "writing-result",
        validation.detail));
      return;
    }

    if (!validation.mesh || !validation.quality
      || !validation.issueElementDetails)
    {
      finishFailure(makeFailure(
        MeshingJobFailureKind::OutputValidationFailed,
        active_->submission.jobId,
        active_->sourceGeometryRevision,
        "MESH_OUTPUT_CORRUPT",
        "writing-result",
        "Verified output did not retain mesh and quality data"));
      return;
    }
    std::shared_ptr<AcceptedMeshQuality> quality =
      acceptedQuality(
        std::move(*validation.quality),
        std::move(*validation.issueElementDetails),
        0);

    lastStage_ = "committing";
    if (!projectState_.commitMesh(std::move(*validation.mesh)))
    {
      finishFailure(makeFailure(
        MeshingJobFailureKind::ProjectCommitFailed,
        active_->submission.jobId,
        active_->sourceGeometryRevision,
        "MESH_COMMIT_REJECTED",
        "committing",
        "Project state rejected the verified Worker mesh"));
      return;
    }
    const auto& prepared = *active_->preparedJob;
    const auto& worker = *active_->workerResult;
    const domain::MeshRevision committedMeshRevision =
      projectState_.meshRevision();
    quality->meshRevision = committedMeshRevision;
    ExperimentalMeshingJobResult result{
      active_->submission.jobId,
      active_->sourceGeometryRevision,
      prepared.workspace,
      prepared.resultPath,
      prepared.meshPath,
      worker.meshSha256.toLower(),
      prepared.meshBinaryPath,
      worker.meshBinarySha256.toLower(),
      prepared.geometryMapResultPath,
      worker.geometryMapResultSha256.toLower(),
      prepared.qualityPath,
      worker.qualitySha256.toLower(),
      worker.nodeCount,
      worker.triangleCount,
      worker.tetrahedronCount,
      static_cast<std::uint64_t>(std::max<qint64>(0, jobTimer_.elapsed())),
      active_->peakWorkingSetBytes,
      committedMeshRevision,
      worker.effectiveSettings,
      worker.effectiveResourceLimits,
      quality,
      active_->attemptNumber,
    };
    state_ = State::Idle;
    active_.reset();
    cancellation_.reset();
    emit owner_->experimentalSucceeded(result);
  }

  void finishCancelled()
  {
    if (!active_ || !cancellation_)
    {
      return;
    }
    removePublishedArtifacts();
    const MeshingJobCancellation cancellation = *cancellation_;
    state_ = State::Idle;
    active_.reset();
    cancellation_.reset();
    emit owner_->cancelled(cancellation);
  }

  void finishFailure(MeshingJobFailure failure)
  {
    if (active_ && failure.attemptCount == 0)
    {
      failure.attemptCount = active_->attemptNumber;
    }
    removePublishedArtifacts();
    state_ = State::Idle;
    active_.reset();
    cancellation_.reset();
    emit owner_->failed(failure);
  }

  void removePublishedArtifacts()
  {
    if (!active_ || !active_->preparedJob)
    {
      return;
    }
    QFile::remove(active_->preparedJob->meshPath);
    QFile::remove(active_->preparedJob->meshBinaryPath);
    QFile::remove(active_->preparedJob->geometryMapResultPath);
    QFile::remove(active_->preparedJob->qualityPath);
    QFile::remove(active_->preparedJob->resultPath);
  }

  MeshingJobController* owner_ = nullptr;
  domain::ProjectState& projectState_;
  MeshingRuntimeConfiguration configuration_;
  meshing::GmshWorkerClient workerClient_;
  QFutureWatcher<meshing::GmshJobWorkspaceResult> preparationWatcher_;
  QFutureWatcher<ArtifactValidation> validationWatcher_;
  QElapsedTimer jobTimer_;
  std::optional<ActiveJob> active_;
  std::optional<MeshingJobCancellation> cancellation_;
  QString lastStage_ = "initializing";
  State state_ = State::Idle;
};

MeshingJobController::MeshingJobController(
  domain::ProjectState& projectState,
  MeshingRuntimeConfiguration configuration,
  QObject* parent)
  : QObject(parent)
  , implementation_(std::make_unique<Implementation>(
      this, projectState, std::move(configuration)))
{
}

MeshingJobController::~MeshingJobController() = default;

std::optional<MeshingJobFailure> MeshingJobController::submit(
  const MeshingJobSubmission& submission)
{
  return implementation_->submit(submission);
}

std::optional<MeshingJobFailure> MeshingJobController::cancel(
  const QString& reason,
  int deadlineMilliseconds)
{
  return implementation_->cancel(reason, deadlineMilliseconds);
}

bool MeshingJobController::isRunning() const noexcept
{
  return implementation_->isRunning();
}

bool MeshingJobController::isCancelling() const noexcept
{
  return implementation_->isCancelling();
}
}
