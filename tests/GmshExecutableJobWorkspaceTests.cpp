#include "meshing/GmshExecutableJobWorkspace.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>

namespace
{
int failures = 0;

void expect(bool condition, const char* message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool writeFile(const QString& path, const QByteArray& contents)
{
  QFile file(path);
  return file.open(QIODevice::WriteOnly)
    && file.write(contents) == contents.size();
}

QString sha256(const QByteArray& contents)
{
  return QString::fromLatin1(QCryptographicHash::hash(
    contents, QCryptographicHash::Sha256).toHex());
}

QByteArray validGeometryMap(qint64 revision)
{
  QJsonArray matrix;
  for (int row = 0; row < 4; ++row)
  {
    for (int column = 0; column < 4; ++column)
    {
      matrix.append(row == column ? 1.0 : 0.0);
    }
  }
  return QJsonDocument(QJsonObject{
    {"schema", QJsonObject{{"name", "cceda.geometry-map"},
      {"major", 0}, {"minor", 1}}},
    {"sourceGeometryRevision", revision},
    {"mode", "independent-solids"},
    {"canonicalUnit", "mm"},
    {"transformsBaked", false},
    {"regions", QJsonArray{QJsonObject{
      {"regionId", 1},
      {"brepSolidIndex", 1},
      {"sourceAssemblyGeometryIds", QJsonArray{"assembly-1"}},
      {"sourcePartGeometryId", "part-1"},
      {"sourceSolidGeometryId", "solid-1"},
      {"sourceFaceGeometryIds", QJsonArray{"face-1"}},
      {"brepLocation", QJsonObject{
        {"transformsBaked", false},
        {"matrixRowMajor", matrix},
      }},
    }}},
    {"sharedFaceCandidates", QJsonArray{}},
    {"excluded", QJsonArray{}},
  }).toJson(QJsonDocument::Compact);
}

}

int main()
{
  using namespace cceda::meshing;

  QTemporaryDir snapshotRoot;
  expect(snapshotRoot.isValid(), "snapshot test root is unavailable");
  const QByteArray brepBytes("DBRep_DrawableShape\nCASCADE Topology V3\n");
  const QByteArray mapBytes = validGeometryMap(42);
  const QString sourceBrep = QDir(snapshotRoot.path()).filePath("input.brep");
  const QString sourceMap = QDir(snapshotRoot.path()).filePath("geometry-map.json");
  expect(writeFile(sourceBrep, brepBytes), "cannot write source BREP fixture");
  expect(writeFile(sourceMap, mapBytes), "cannot write geometry-map fixture");
  GmshSnapshotJobWorkspaceRequest snapshotRequest{
    QDir(snapshotRoot.path()).filePath("job"),
    sourceBrep,
    sourceMap,
    sha256(brepBytes),
    sha256(mapBytes),
    "snapshot-job",
    42,
    0.2,
    3.0,
    16 * 1024,
  };
  const GmshJobWorkspaceResult snapshot =
    prepareGmshSnapshotJobWorkspace(snapshotRequest);
  expect(snapshot.succeeded(), "a valid BREP snapshot workspace is prepared");
  if (snapshot.job)
  {
    expect(snapshot.job->geometryPath.endsWith("geometry/input.brep"),
      "BREP uses the fixed protocol path");
    expect(snapshot.job->geometryMapPath.endsWith("geometry/geometry-map.json"),
      "geometry map uses the fixed protocol path");
    expect(snapshot.job->geometrySha256 == snapshotRequest.expectedGeometrySha256,
      "BREP digest remains bound to snapshot metadata");
    expect(snapshot.job->geometryMapSha256
        == snapshotRequest.expectedGeometryMapSha256,
      "map digest remains bound to snapshot metadata");

    QFile requestFile(snapshot.job->requestPath);
    expect(requestFile.open(QIODevice::ReadOnly),
      "snapshot request.json can be opened");
    const QJsonObject json = QJsonDocument::fromJson(requestFile.readAll()).object();
    const QJsonObject geometry = json.value("geometry").toObject();
    expect(geometry.value("mode").toString() == "independent-solids",
      "snapshot mode is explicit");
    expect(geometry.value("canonicalUnit").toString() == "mm",
      "snapshot canonical unit is explicit");
    expect(geometry.value("brep").toObject().value("path").toString()
        == "geometry/input.brep",
      "request seals the fixed BREP path");
    expect(geometry.value("map").toObject().value("sha256").toString()
        == snapshotRequest.expectedGeometryMapSha256,
      "request seals the geometry-map digest");
  }

  auto mismatchedHash = snapshotRequest;
  mismatchedHash.workspace = QDir(snapshotRoot.path()).filePath("hash-mismatch");
  mismatchedHash.expectedGeometrySha256 = QString(64, '0');
  const GmshJobWorkspaceResult hashRejected =
    prepareGmshSnapshotJobWorkspace(mismatchedHash);
  expect(hashRejected.error == GmshJobWorkspaceErrorCode::SnapshotHashMismatch,
    "a BREP digest mismatch is rejected");
  expect(!QFileInfo::exists(QDir(mismatchedHash.workspace).filePath("request.json")),
    "a digest mismatch never seals request.json");

  const QByteArray wrongRevisionMap = validGeometryMap(41);
  const QString wrongRevisionMapPath =
    QDir(snapshotRoot.path()).filePath("wrong-revision-map/geometry-map.json");
  expect(QDir().mkpath(QFileInfo(wrongRevisionMapPath).absolutePath()),
    "cannot create wrong-revision fixture directory");
  expect(writeFile(wrongRevisionMapPath, wrongRevisionMap),
    "cannot write wrong-revision geometry map");
  auto mismatchedRevision = snapshotRequest;
  mismatchedRevision.workspace = QDir(snapshotRoot.path()).filePath("revision-mismatch");
  mismatchedRevision.sourceGeometryMap = wrongRevisionMapPath;
  mismatchedRevision.expectedGeometryMapSha256 = sha256(wrongRevisionMap);
  const GmshJobWorkspaceResult revisionRejected =
    prepareGmshSnapshotJobWorkspace(mismatchedRevision);
  expect(revisionRejected.error == GmshJobWorkspaceErrorCode::GeometryMapInvalid,
    "a geometry-map revision mismatch is rejected");
  expect(!QFileInfo::exists(QDir(mismatchedRevision.workspace).filePath("request.json")),
    "a revision mismatch never seals request.json");

  auto combinedLimit = snapshotRequest;
  combinedLimit.workspace = QDir(snapshotRoot.path()).filePath("combined-limit");
  combinedLimit.maximumSnapshotBytes =
    static_cast<std::uint64_t>(brepBytes.size() + mapBytes.size() - 1);
  const GmshJobWorkspaceResult combinedRejected =
    prepareGmshSnapshotJobWorkspace(combinedLimit);
  expect(combinedRejected.error == GmshJobWorkspaceErrorCode::GeometryTooLarge,
    "the combined BREP and map byte budget is enforced");

  const QString nonEmptyWorkspace =
    QDir(snapshotRoot.path()).filePath("non-empty");
  expect(QDir().mkpath(nonEmptyWorkspace), "cannot create non-empty workspace");
  expect(writeFile(QDir(nonEmptyWorkspace).filePath("old-job.txt"), "old"),
    "cannot seed non-empty workspace");
  auto nonEmptyRequest = snapshotRequest;
  nonEmptyRequest.workspace = nonEmptyWorkspace;
  const GmshJobWorkspaceResult nonEmpty =
    prepareGmshSnapshotJobWorkspace(nonEmptyRequest);
  expect(nonEmpty.error == GmshJobWorkspaceErrorCode::WorkspaceNotEmpty,
    "a non-empty workspace is rejected");
  expect(!QFileInfo::exists(QDir(nonEmptyWorkspace).filePath("request.json")),
    "a rejected workspace is never sealed");

  auto invalidRequest = snapshotRequest;
  invalidRequest.workspace = QDir(snapshotRoot.path()).filePath("invalid-settings");
  invalidRequest.minimumSizeMm = 3.0;
  invalidRequest.maximumSizeMm = 2.0;
  const GmshJobWorkspaceResult invalid =
    prepareGmshSnapshotJobWorkspace(invalidRequest);
  expect(invalid.error == GmshJobWorkspaceErrorCode::InvalidRequest,
    "an inverted size range is rejected");

  invalidRequest.minimumSizeMm = 0.0;
  invalidRequest.maximumSizeMm = 0.0;
  invalidRequest.jobId = "invalid job\n";
  const GmshJobWorkspaceResult invalidJobId =
    prepareGmshSnapshotJobWorkspace(invalidRequest);
  expect(invalidJobId.error == GmshJobWorkspaceErrorCode::InvalidRequest,
    "a Job ID containing whitespace or control characters is rejected");

  if (failures == 0)
  {
    std::cout << "Gmsh executable job workspace tests passed.\n";
  }
  return failures == 0 ? 0 : 1;
}
