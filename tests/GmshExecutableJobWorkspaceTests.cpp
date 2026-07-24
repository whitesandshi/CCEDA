#include "meshing/GmshExecutableJobWorkspace.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
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

cceda::meshing::GmshExecutableJobWorkspaceRequest requestFor(
  const QString& workspace,
  const QString& sourceStep)
{
  return {
    workspace,
    sourceStep,
    "workspace-test-job",
    42,
    0.1,
    2.5,
    1024,
  };
}
}

int main()
{
  using namespace cceda::meshing;

  QTemporaryDir validRoot;
  const QString sourceStep = QDir(validRoot.path()).filePath("source.step");
  expect(validRoot.isValid(), "valid test root is unavailable");
  expect(writeFile(sourceStep, "STEP-test-payload"), "cannot write source STEP fixture");

  const QString workspace = QDir(validRoot.path()).filePath("job");
  const GmshJobWorkspaceResult prepared =
    prepareGmshExecutableJobWorkspace(requestFor(workspace, sourceStep));
  expect(prepared.succeeded(), "a valid executable-adapter workspace is prepared");
  expect(prepared.job.has_value(), "successful preparation returns workspace metadata");
  if (prepared.job)
  {
    expect(QFileInfo::exists(prepared.job->requestPath), "request.json is published last");
    expect(QFileInfo::exists(prepared.job->geometryPath), "geometry snapshot is published");
    expect(QFileInfo::exists(QDir(workspace).filePath("output")),
      "controlled output directory is created");
    expect(QFileInfo::exists(QDir(workspace).filePath("logs")),
      "controlled log directory is created");

    QFile copied(prepared.job->geometryPath);
    expect(copied.open(QIODevice::ReadOnly), "copied geometry can be opened");
    const QByteArray copiedBytes = copied.readAll();
    expect(copiedBytes == "STEP-test-payload", "geometry bytes are copied exactly");
    expect(prepared.job->geometrySha256
        == QString::fromLatin1(QCryptographicHash::hash(
          copiedBytes, QCryptographicHash::Sha256).toHex()),
      "prepared geometry SHA-256 matches copied bytes");

    QFile requestFile(prepared.job->requestPath);
    expect(requestFile.open(QIODevice::ReadOnly), "request.json can be opened");
    QJsonParseError parseError;
    const QJsonDocument document =
      QJsonDocument::fromJson(requestFile.readAll(), &parseError);
    expect(parseError.error == QJsonParseError::NoError && document.isObject(),
      "request.json is valid JSON");
    const QJsonObject json = document.object();
    expect(json.value("jobId").toString() == "workspace-test-job",
      "job identity is serialized");
    expect(json.value("sourceGeometryRevision").toInteger() == 42,
      "geometry revision is serialized");
    expect(json.value("geometry").toObject().value("path").toString()
        == "geometry/input.step",
      "geometry path is fixed inside the workspace");
    expect(json.value("geometry").toObject().value("sha256").toString()
        == prepared.job->geometrySha256,
      "request seals the copied geometry hash");
    expect(json.value("output").toObject().value("meshPath").toString()
        == "output/gmsh-intermediate.msh",
      "experimental mesh path is fixed");
    expect(json.value("settings").toObject().value("minimumSizeMm").toDouble()
        == 0.1,
      "minimum size is serialized");
  }

  QTemporaryDir nonEmptyRoot;
  const QString nonEmptySource = QDir(nonEmptyRoot.path()).filePath("source.step");
  const QString nonEmptyWorkspace = QDir(nonEmptyRoot.path()).filePath("job");
  expect(writeFile(nonEmptySource, "STEP"), "cannot write non-empty fixture");
  expect(QDir().mkpath(nonEmptyWorkspace), "cannot create non-empty workspace");
  expect(writeFile(QDir(nonEmptyWorkspace).filePath("old-job.txt"), "old"),
    "cannot seed non-empty workspace");
  const GmshJobWorkspaceResult nonEmpty =
    prepareGmshExecutableJobWorkspace(requestFor(nonEmptyWorkspace, nonEmptySource));
  expect(nonEmpty.error == GmshJobWorkspaceErrorCode::WorkspaceNotEmpty,
    "a non-empty workspace is rejected");
  expect(!QFileInfo::exists(QDir(nonEmptyWorkspace).filePath("request.json")),
    "a rejected workspace is never sealed");

  QTemporaryDir limitedRoot;
  const QString limitedSource = QDir(limitedRoot.path()).filePath("source.step");
  expect(writeFile(limitedSource, "oversized"), "cannot write size-limit fixture");
  auto limitedRequest = requestFor(
    QDir(limitedRoot.path()).filePath("job"), limitedSource);
  limitedRequest.maximumGeometryBytes = 4;
  const GmshJobWorkspaceResult limited =
    prepareGmshExecutableJobWorkspace(limitedRequest);
  expect(limited.error == GmshJobWorkspaceErrorCode::GeometryTooLarge,
    "geometry larger than the caller budget is rejected");
  expect(!QFileInfo::exists(QDir(limitedRequest.workspace).filePath("request.json")),
    "oversized geometry never publishes a request");

  QTemporaryDir invalidRoot;
  const QString invalidSource = QDir(invalidRoot.path()).filePath("source.step");
  expect(writeFile(invalidSource, "STEP"), "cannot write invalid-settings fixture");
  auto invalidRequest = requestFor(
    QDir(invalidRoot.path()).filePath("job"), invalidSource);
  invalidRequest.minimumSizeMm = 3.0;
  invalidRequest.maximumSizeMm = 2.0;
  const GmshJobWorkspaceResult invalid =
    prepareGmshExecutableJobWorkspace(invalidRequest);
  expect(invalid.error == GmshJobWorkspaceErrorCode::InvalidRequest,
    "an inverted size range is rejected");

  invalidRequest.minimumSizeMm = 0.0;
  invalidRequest.maximumSizeMm = 0.0;
  invalidRequest.jobId = "invalid job\n";
  const GmshJobWorkspaceResult invalidJobId =
    prepareGmshExecutableJobWorkspace(invalidRequest);
  expect(invalidJobId.error == GmshJobWorkspaceErrorCode::InvalidRequest,
    "a Job ID containing whitespace or control characters is rejected");

  if (failures == 0)
  {
    std::cout << "Gmsh executable job workspace tests passed.\n";
  }
  return failures == 0 ? 0 : 1;
}
