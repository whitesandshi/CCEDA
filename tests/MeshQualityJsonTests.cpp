#include "meshing/MeshQualityJson.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <functional>
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

cceda::mesh_quality::MeshQualityReport validReport()
{
  using namespace cceda::mesh_quality;
  MeshQualityReport report;
  report.meshRevision = 0;
  report.sourceGeometryRevision = 42;
  report.triangles.push_back({
    101,
    std::sqrt(3.0) / 4.0,
    1.0,
    60.0,
    60.0,
    true,
  });
  report.tetrahedra.push_back({
    201,
    std::sqrt(2.0) / 12.0,
    1.0,
    70.5287793655,
    1.0,
  });
  return report;
}

cceda::meshing::MeshQualityJsonReadResult read(
  const QByteArray& bytes,
  cceda::meshing::MeshQualityJsonLimits limits = {})
{
  QBuffer input;
  input.setData(bytes);
  input.open(QIODevice::ReadOnly);
  return cceda::meshing::readMeshQualityJson(input, limits);
}

QByteArray mutate(
  const QByteArray& bytes,
  const std::function<void(QJsonObject&)>& change)
{
  QJsonObject root = QJsonDocument::fromJson(bytes).object();
  change(root);
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}
}

int main()
{
  using namespace cceda::meshing;
  using namespace cceda::mesh_quality;

  const MeshQualityReport report = validReport();
  const MeshQualityJsonWriteResult written =
    writeMeshQualityJson(report, "job-42");
  expect(written.succeeded(), "a valid Worker quality report is serialized");
  if (!written.succeeded())
  {
    return 1;
  }


  const MeshQualityJsonReadResult parsed = read(written.bytes);
  expect(parsed.succeeded(), "canonical protocol-0.1 quality JSON is accepted");
  if (parsed.artifact)
  {
    expect(parsed.artifact->jobId == "job-42",
      "the job identity is retained");
    expect(parsed.artifact->sourceGeometryRevision == 42,
      "the source geometry revision is retained");
    expect(parsed.artifact->triangleCount == 1
        && parsed.artifact->tetrahedronCount == 1,
      "element-kind counts are retained");
    expect(parsed.artifact->numericSummaries.size() == 8,
      "all numeric metric summaries are retained");
    expect(parsed.artifact->booleanSummary.sampleCount == 1
        && parsed.artifact->booleanSummary.failedElementCount == 0,
      "the boolean metric summary is retained");
  }

  const MeshQualityJsonWriteResult writtenAgain =
    writeMeshQualityJson(report, "job-42");
  expect(written.bytes == writtenAgain.bytes,
    "quality JSON serialization is deterministic");
  expect(writeMeshQualityJson(report, ".hidden-job").error
      == MeshQualityJsonErrorCode::InvalidIdentity,
    "job identities must begin with an ASCII alphanumeric character");

  const QByteArray unknownRoot = mutate(written.bytes, [](QJsonObject& root) {
    root.insert("futureGuess", true);
  });
  expect(read(unknownRoot).error == MeshQualityJsonErrorCode::InvalidStructure,
    "unknown root fields are rejected");

  const QByteArray unsupportedSchema = mutate(
    written.bytes,
    [](QJsonObject& root) {
      QJsonObject schema = root.value("schema").toObject();
      schema.insert("major", 1);
      root.insert("schema", schema);
    });
  expect(read(unsupportedSchema).error
      == MeshQualityJsonErrorCode::UnsupportedSchema,
    "unsupported schema versions are rejected");
  const QByteArray fractionalSchema = mutate(
    written.bytes,
    [](QJsonObject& root) {
      QJsonObject schema = root.value("schema").toObject();
      schema.insert("minor", 1.5);
      root.insert("schema", schema);
    });
  expect(read(fractionalSchema).error
      == MeshQualityJsonErrorCode::UnsupportedSchema,
    "schema versions must be integral JSON numbers");


  const QByteArray duplicateSummary = mutate(
    written.bytes,
    [](QJsonObject& root) {
      QJsonArray summaries = root.value("summaries").toArray();
      summaries.replace(1, summaries.at(0));
      root.insert("summaries", summaries);
    });
  expect(read(duplicateSummary).error
      == MeshQualityJsonErrorCode::InvalidSummary,
    "duplicate metric summaries are rejected");

  const QByteArray invalidMean = mutate(written.bytes, [](QJsonObject& root) {
    QJsonArray summaries = root.value("summaries").toArray();
    QJsonObject first = summaries.at(0).toObject();
    first.insert("mean", first.value("maximum").toDouble() + 1.0);
    summaries.replace(0, first);
    root.insert("summaries", summaries);
  });
  expect(read(invalidMean).error == MeshQualityJsonErrorCode::InvalidSummary,
    "summary ordering invariants are enforced");

  const QByteArray countMismatch = mutate(
    written.bytes,
    [](QJsonObject& root) {
      QJsonObject counts = root.value("counts").toObject();
      counts.insert("triangles", 2);
      root.insert("counts", counts);
    });
  expect(read(countMismatch).error == MeshQualityJsonErrorCode::InvalidSummary,
    "summary sample counts must agree with element counts");

  const QByteArray falseValidity = mutate(
    written.bytes,
    [](QJsonObject& root) {
      root.insert("validForCommit", false);
    });
  expect(read(falseValidity).error == MeshQualityJsonErrorCode::InvalidIssue,
    "commit validity cannot contradict the issue list");

  MeshQualityJsonLimits tiny;
  tiny.maximumInputBytes = 8;
  expect(read(written.bytes, tiny).error
      == MeshQualityJsonErrorCode::InputLimitExceeded,
    "the JSON byte budget is enforced before parsing");

  QByteArray deeplyNested = "{";
  for (int depth = 0; depth < 17; ++depth)
  {
    deeplyNested += "\"x\":[";
  }
  deeplyNested += "0";
  deeplyNested += QByteArray(17, ']') + "}";
  expect(read(deeplyNested).error
      == MeshQualityJsonErrorCode::CountLimitExceeded,
    "JSON nesting depth is bounded before parsing");

  MeshQualityReport duplicateIssues = report;
  duplicateIssues.issues = {
    {MeshQualityIssueCode::InvertedTetrahedron, 201},
    {MeshQualityIssueCode::InvertedTetrahedron, 201},
  };
  expect(writeMeshQualityJson(duplicateIssues, "job-42").error
      == MeshQualityJsonErrorCode::InconsistentReport,
    "writers reject duplicate issue indexes");

  MeshQualityReport tooManyIssues = report;
  tooManyIssues.issues.push_back({
    MeshQualityIssueCode::InconsistentTriangleOrientation,
    101,
  });
  MeshQualityJsonLimits noIssues;
  noIssues.maximumIssues = 0;
  expect(writeMeshQualityJson(tooManyIssues, "job-42", noIssues).error
      == MeshQualityJsonErrorCode::InvalidLimits,
    "zero resource limits are rejected");

  MeshQualityReport largeFinite = report;
  largeFinite.triangles.front().areaSquareMillimeters = 1.0e308;
  TriangleQuality second = largeFinite.triangles.front();
  second.elementId = 102;
  largeFinite.triangles.push_back(second);
  const MeshQualityJsonWriteResult largeWritten =
    writeMeshQualityJson(largeFinite, "large-finite");
  expect(largeWritten.succeeded()
      && read(largeWritten.bytes).succeeded(),
    "finite summary values do not overflow while computing their mean");

  if (failures == 0)
  {
    std::cout << "Mesh quality JSON tests passed.\n";
  }
  return failures == 0 ? 0 : 1;
}
