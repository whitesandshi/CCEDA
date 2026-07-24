#include "meshing/GmshExecutableJobWorkspace.h"

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

bool hasValidSettings(const GmshExecutableJobWorkspaceRequest& request)
{
  return std::isfinite(request.minimumSizeMm)
    && std::isfinite(request.maximumSizeMm)
    && request.minimumSizeMm >= 0.0
    && request.maximumSizeMm >= 0.0
    && (request.maximumSizeMm == 0.0
      || request.minimumSizeMm <= request.maximumSizeMm);
}

bool hasValidJobId(const QString& jobId)
{
  static const QRegularExpression pattern(
    QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
  return pattern.match(jobId).hasMatch();
}

bool isEmptyDirectory(const QString& path)
{
  const QDir directory(path);
  return directory.entryList(
    QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
    .isEmpty();
}
}

GmshJobWorkspaceResult prepareGmshExecutableJobWorkspace(
  const GmshExecutableJobWorkspaceRequest& request)
{
  const QFileInfo sourceInfo(request.sourceStep);
  const QFileInfo workspaceInfo(request.workspace);
  const bool revisionFitsJson =
    request.sourceGeometryRevision
      <= static_cast<domain::GeometryRevision>(std::numeric_limits<qint64>::max());
  if (!QDir::isAbsolutePath(request.workspace)
    || !QDir::isAbsolutePath(request.sourceStep)
    || !sourceInfo.isFile()
    || (sourceInfo.suffix().compare("step", Qt::CaseInsensitive) != 0
      && sourceInfo.suffix().compare("stp", Qt::CaseInsensitive) != 0)
    || !hasValidJobId(request.jobId)
    || request.sourceGeometryRevision == 0
    || !revisionFitsJson
    || request.maximumGeometryBytes == 0
    || !hasValidSettings(request)
    || (workspaceInfo.exists()
      && (!workspaceInfo.isDir() || workspaceInfo.isSymbolicLink())))
  {
    return fail(
      GmshJobWorkspaceErrorCode::InvalidRequest,
      "Workspace, STEP input, job identity, revision, size budget, or settings are invalid");
  }

  if (sourceInfo.size() <= 0)
  {
    return fail(
      GmshJobWorkspaceErrorCode::GeometryReadFailed,
      "Source STEP is empty");
  }
  if (static_cast<std::uint64_t>(sourceInfo.size())
    > request.maximumGeometryBytes)
  {
    return fail(
      GmshJobWorkspaceErrorCode::GeometryTooLarge,
      "Source STEP exceeds the geometry byte budget");
  }
  if (workspaceInfo.exists() && !isEmptyDirectory(request.workspace))
  {
    return fail(
      GmshJobWorkspaceErrorCode::WorkspaceNotEmpty,
      "Job workspace must be empty");
  }

  const QString workspace = QDir::cleanPath(request.workspace);
  const QDir root(workspace);
  if (!QDir().mkpath(root.filePath("geometry"))
    || !QDir().mkpath(root.filePath("output"))
    || !QDir().mkpath(root.filePath("logs")))
  {
    return fail(
      GmshJobWorkspaceErrorCode::DirectoryCreationFailed,
      "Cannot create controlled job workspace directories");
  }

  const QString geometryPath = root.filePath("geometry/input.step");
  QFile source(request.sourceStep);
  if (!source.open(QIODevice::ReadOnly))
  {
    return fail(
      GmshJobWorkspaceErrorCode::GeometryReadFailed,
      QString("Cannot open source STEP: %1").arg(source.errorString()));
  }

  // QSaveFile keeps the final path untouched until commit() atomically renames
  // its same-directory temporary file.
  // https://doc.qt.io/qt-6.8/qsavefile.html#commit
  QSaveFile geometryOutput(geometryPath);
  if (!geometryOutput.open(QIODevice::WriteOnly))
  {
    return fail(
      GmshJobWorkspaceErrorCode::GeometryWriteFailed,
      QString("Cannot create geometry snapshot: %1").arg(geometryOutput.errorString()));
  }

  QCryptographicHash hash(QCryptographicHash::Sha256);
  std::uint64_t copiedBytes = 0;
  while (!source.atEnd())
  {
    const QByteArray chunk = source.read(CopyBufferBytes);
    if (chunk.isEmpty() && source.error() != QFileDevice::NoError)
    {
      return fail(
        GmshJobWorkspaceErrorCode::GeometryReadFailed,
        QString("Cannot read source STEP: %1").arg(source.errorString()));
    }
    if (chunk.isEmpty())
    {
      break;
    }
    if (static_cast<std::uint64_t>(chunk.size())
      > request.maximumGeometryBytes - copiedBytes)
    {
      return fail(
        GmshJobWorkspaceErrorCode::GeometryTooLarge,
        "Source STEP grew beyond the geometry byte budget while copying");
    }
    if (geometryOutput.write(chunk) != chunk.size())
    {
      return fail(
        GmshJobWorkspaceErrorCode::GeometryWriteFailed,
        QString("Cannot write geometry snapshot: %1").arg(geometryOutput.errorString()));
    }
    hash.addData(QByteArrayView(chunk));
    copiedBytes += static_cast<std::uint64_t>(chunk.size());
  }

  if (copiedBytes != static_cast<std::uint64_t>(sourceInfo.size()))
  {
    return fail(
      GmshJobWorkspaceErrorCode::GeometryReadFailed,
      "Source STEP size changed while preparing the job");
  }
  if (!geometryOutput.commit())
  {
    return fail(
      GmshJobWorkspaceErrorCode::GeometryWriteFailed,
      QString("Cannot publish geometry snapshot: %1").arg(geometryOutput.errorString()));
  }

  const QString geometrySha256 = QString::fromLatin1(hash.result().toHex());
  const QString requestPath = root.filePath("request.json");
  const QJsonObject requestJson{
    {"protocol", QJsonObject{{"major", ProtocolMajor}, {"minor", ProtocolMinor}}},
    {"jobId", request.jobId},
    {"sourceGeometryRevision", static_cast<qint64>(request.sourceGeometryRevision)},
    {"geometry", QJsonObject{
      {"path", "geometry/input.step"},
      {"sha256", geometrySha256},
    }},
    {"output", QJsonObject{
      {"meshPath", "output/gmsh-intermediate.msh"},
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
    return fail(
      GmshJobWorkspaceErrorCode::RequestWriteFailed,
      QString("Cannot seal request.json: %1").arg(requestOutput.errorString()));
  }

  return {
    PreparedGmshExecutableJob{
      workspace,
      requestPath,
      geometryPath,
      geometrySha256,
      root.filePath("output/gmsh-intermediate.msh"),
      root.filePath("output/result.json"),
    },
    GmshJobWorkspaceErrorCode::None,
    {},
  };
}
}
