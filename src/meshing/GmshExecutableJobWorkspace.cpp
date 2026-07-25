#include "meshing/GmshExecutableJobWorkspace.h"
#include "meshing/GeometryMap.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <cmath>
#include <limits>

namespace cceda::meshing
{
namespace
{
constexpr int ProtocolMajor = 0;
constexpr int ProtocolMinor = 1;
constexpr qint64 CopyBufferBytes = 1024 * 1024;

GmshJobWorkspaceResult fail(
  GmshJobWorkspaceErrorCode error,
  QString detail)
{
  return {
    std::nullopt,
    error,
    std::move(detail),
  };
}

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

bool isEmptyDirectory(const QString& path)
{
  const QDir directory(path);
  return directory.entryList(
    QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
    .isEmpty();
}

struct CopiedArtifact
{
  QString path;
  QString sha256;
  std::uint64_t bytes = 0;
};

struct ArtifactCopyResult
{
  std::optional<CopiedArtifact> artifact;
  GmshJobWorkspaceErrorCode error = GmshJobWorkspaceErrorCode::None;
  QString detail;
};

ArtifactCopyResult copyArtifact(
  const QString& sourcePath,
  const QString& destinationPath,
  const QString& expectedSha256,
  std::uint64_t maximumBytes,
  GmshJobWorkspaceErrorCode readError,
  GmshJobWorkspaceErrorCode writeError,
  QStringView label)
{
  const QFileInfo sourceInfo(sourcePath);
  if (sourceInfo.size() <= 0)
  {
    return {std::nullopt, readError, QString("Source %1 is empty").arg(label)};
  }
  if (static_cast<std::uint64_t>(sourceInfo.size()) > maximumBytes)
  {
    return {std::nullopt, GmshJobWorkspaceErrorCode::GeometryTooLarge,
      QString("Source %1 exceeds the snapshot byte budget").arg(label)};
  }

  QFile source(sourcePath);
  if (!source.open(QIODevice::ReadOnly))
  {
    return {std::nullopt, readError,
      QString("Cannot open source %1: %2").arg(label, source.errorString())};
  }
  QSaveFile output(destinationPath);
  if (!output.open(QIODevice::WriteOnly))
  {
    return {std::nullopt, writeError,
      QString("Cannot create %1 snapshot: %2").arg(label, output.errorString())};
  }

  QCryptographicHash hash(QCryptographicHash::Sha256);
  std::uint64_t copiedBytes = 0;
  while (!source.atEnd())
  {
    const QByteArray chunk = source.read(CopyBufferBytes);
    if (chunk.isEmpty() && source.error() != QFileDevice::NoError)
    {
      return {std::nullopt, readError,
        QString("Cannot read source %1: %2").arg(label, source.errorString())};
    }
    if (chunk.isEmpty())
    {
      break;
    }
    if (static_cast<std::uint64_t>(chunk.size()) > maximumBytes - copiedBytes)
    {
      return {std::nullopt, GmshJobWorkspaceErrorCode::GeometryTooLarge,
        QString("Source %1 grew beyond the snapshot byte budget").arg(label)};
    }
    if (output.write(chunk) != chunk.size())
    {
      return {std::nullopt, writeError,
        QString("Cannot write %1 snapshot: %2").arg(label, output.errorString())};
    }
    hash.addData(QByteArrayView(chunk));
    copiedBytes += static_cast<std::uint64_t>(chunk.size());
  }
  if (copiedBytes != static_cast<std::uint64_t>(sourceInfo.size()))
  {
    return {std::nullopt, readError,
      QString("Source %1 size changed while preparing the job").arg(label)};
  }

  const QString actualSha256 = QString::fromLatin1(hash.result().toHex());
  if (actualSha256 != expectedSha256)
  {
    return {std::nullopt, GmshJobWorkspaceErrorCode::SnapshotHashMismatch,
      QString("Source %1 SHA-256 does not match snapshot metadata").arg(label)};
  }
  if (!output.commit())
  {
    return {std::nullopt, writeError,
      QString("Cannot publish %1 snapshot: %2").arg(label, output.errorString())};
  }
  return {CopiedArtifact{destinationPath, actualSha256, copiedBytes},
    GmshJobWorkspaceErrorCode::None, {}};
}
}

GmshJobWorkspaceResult prepareGmshSnapshotJobWorkspace(
  const GmshSnapshotJobWorkspaceRequest& request)
{
  const QFileInfo brepInfo(request.sourceBrep);
  const QFileInfo mapInfo(request.sourceGeometryMap);
  const QFileInfo workspaceInfo(request.workspace);
  const bool revisionFitsJson = request.sourceGeometryRevision
    <= static_cast<domain::GeometryRevision>(std::numeric_limits<qint64>::max());
  if (!QDir::isAbsolutePath(request.workspace)
    || !QDir::isAbsolutePath(request.sourceBrep)
    || !QDir::isAbsolutePath(request.sourceGeometryMap)
    || !brepInfo.isFile() || brepInfo.isSymbolicLink()
    || brepInfo.suffix().compare("brep", Qt::CaseInsensitive) != 0
    || !mapInfo.isFile() || mapInfo.isSymbolicLink()
    || mapInfo.fileName() != "geometry-map.json"
    || !isSha256(request.expectedGeometrySha256)
    || !isSha256(request.expectedGeometryMapSha256)
    || !hasValidJobId(request.jobId)
    || request.sourceGeometryRevision == 0
    || !revisionFitsJson
    || request.maximumSnapshotBytes == 0
    || !std::isfinite(request.minimumSizeMm)
    || !std::isfinite(request.maximumSizeMm)
    || request.minimumSizeMm < 0.0
    || request.maximumSizeMm < 0.0
    || (request.maximumSizeMm != 0.0
      && request.minimumSizeMm > request.maximumSizeMm)
    || (workspaceInfo.exists()
      && (!workspaceInfo.isDir() || workspaceInfo.isSymbolicLink())))
  {
    return fail(GmshJobWorkspaceErrorCode::InvalidRequest,
      "Workspace, snapshot inputs, identity, revision, budget, or settings are invalid");
  }
  if (workspaceInfo.exists() && !isEmptyDirectory(request.workspace))
  {
    return fail(GmshJobWorkspaceErrorCode::WorkspaceNotEmpty,
      "Job workspace must be empty");
  }
  if (brepInfo.size() <= 0 || mapInfo.size() <= 0)
  {
    return fail(GmshJobWorkspaceErrorCode::GeometryReadFailed,
      "Snapshot artifacts must be non-empty");
  }
  const auto brepBytes = static_cast<std::uint64_t>(brepInfo.size());
  const auto mapBytes = static_cast<std::uint64_t>(mapInfo.size());
  if (brepBytes > request.maximumSnapshotBytes
    || mapBytes > request.maximumSnapshotBytes - brepBytes)
  {
    return fail(GmshJobWorkspaceErrorCode::GeometryTooLarge,
      "Source snapshot exceeds the combined byte budget");
  }

  const QString workspace = QDir::cleanPath(request.workspace);
  const QDir root(workspace);
  if (!QDir().mkpath(root.filePath("geometry"))
    || !QDir().mkpath(root.filePath("output"))
    || !QDir().mkpath(root.filePath("logs")))
  {
    return fail(GmshJobWorkspaceErrorCode::DirectoryCreationFailed,
      "Cannot create controlled job workspace directories");
  }

  const ArtifactCopyResult brep = copyArtifact(
    request.sourceBrep,
    root.filePath("geometry/input.brep"),
    request.expectedGeometrySha256,
    request.maximumSnapshotBytes,
    GmshJobWorkspaceErrorCode::GeometryReadFailed,
    GmshJobWorkspaceErrorCode::GeometryWriteFailed,
    u"BREP");
  if (!brep.artifact)
  {
    return fail(brep.error, brep.detail);
  }
  const ArtifactCopyResult map = copyArtifact(
    request.sourceGeometryMap,
    root.filePath("geometry/geometry-map.json"),
    request.expectedGeometryMapSha256,
    request.maximumSnapshotBytes - brep.artifact->bytes,
    GmshJobWorkspaceErrorCode::GeometryMapReadFailed,
    GmshJobWorkspaceErrorCode::GeometryMapWriteFailed,
    u"geometry map");
  if (!map.artifact)
  {
    return fail(map.error, map.detail);
  }

  GeometryMapReadLimits mapLimits;
  mapLimits.maximumInputBytes = map.artifact->bytes;
  mapLimits.maximumRegions = map.artifact->bytes;
  mapLimits.maximumFaceReferences = map.artifact->bytes;
  mapLimits.maximumSharedFaceCandidates = map.artifact->bytes;
  mapLimits.maximumExclusions = map.artifact->bytes;
  mapLimits.maximumIdentityCharacters = map.artifact->bytes;
  QFile copiedMap(map.artifact->path);
  if (!copiedMap.open(QIODevice::ReadOnly))
  {
    return fail(GmshJobWorkspaceErrorCode::GeometryMapReadFailed,
      QString("Cannot reopen copied geometry map: %1").arg(copiedMap.errorString()));
  }
  const GeometryMapReadResult parsed = readGeometryMap(copiedMap, mapLimits);
  if (!parsed.succeeded()
    || parsed.map->sourceGeometryRevision != request.sourceGeometryRevision)
  {
    return fail(GmshJobWorkspaceErrorCode::GeometryMapInvalid,
      parsed.succeeded()
        ? "Geometry map revision does not match the submitted revision"
        : QString("Geometry map is invalid: %1").arg(parsed.detail));
  }

  const QString requestPath = root.filePath("request.json");
  const QJsonObject requestJson{
    {"protocol", QJsonObject{{"major", ProtocolMajor}, {"minor", ProtocolMinor}}},
    {"jobId", request.jobId},
    {"sourceGeometryRevision", static_cast<qint64>(request.sourceGeometryRevision)},
    {"geometry", QJsonObject{
      {"schema", QJsonObject{{"major", GeometryMapSchemaMajor},
        {"minor", GeometryMapSchemaMinor}}},
      {"mode", "independent-solids"},
      {"canonicalUnit", "mm"},
      {"brep", QJsonObject{{"path", "geometry/input.brep"},
        {"sha256", brep.artifact->sha256}}},
      {"map", QJsonObject{{"path", "geometry/geometry-map.json"},
        {"sha256", map.artifact->sha256}}},
    }},
    {"output", QJsonObject{
      {"meshPath", "output/gmsh-intermediate.msh"},
      {"meshBinaryPath", "output/result.meshbin"},
      {"geometryMapResultPath", "output/geometry-map-result.json"},
      {"resultPath", "output/result.json"},
    }},
    {"settings", QJsonObject{
      {"minimumSizeMm", request.minimumSizeMm},
      {"maximumSizeMm", request.maximumSizeMm},
    }},
  };
  const QByteArray requestBytes =
    QJsonDocument(requestJson).toJson(QJsonDocument::Indented);
  QSaveFile requestOutput(requestPath);
  if (!requestOutput.open(QIODevice::WriteOnly)
    || requestOutput.write(requestBytes) != requestBytes.size()
    || !requestOutput.commit())
  {
    return fail(GmshJobWorkspaceErrorCode::RequestWriteFailed,
      QString("Cannot seal request.json: %1").arg(requestOutput.errorString()));
  }

  return {
    PreparedGmshExecutableJob{
      workspace,
      requestPath,
      brep.artifact->path,
      brep.artifact->sha256,
      map.artifact->path,
      map.artifact->sha256,
      root.filePath("output/gmsh-intermediate.msh"),
      root.filePath("output/result.meshbin"),
      root.filePath("output/geometry-map-result.json"),
      root.filePath("output/result.json"),
    },
    GmshJobWorkspaceErrorCode::None,
    {},
  };
}

}
