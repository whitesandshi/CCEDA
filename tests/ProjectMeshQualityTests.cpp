#include "persistence/ProjectMeshQuality.h"
#include "meshing/MeshBinaryCodec.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

cceda::domain::MeshModel meshFixture()
{
  using namespace cceda::domain;
  MeshModel mesh;
  mesh.meshRevision = 7;
  mesh.sourceGeometryRevision = 3;
  mesh.creationTimeUtc = "2026-11-10T00:00:00.000Z";
  mesh.backendId = "test";
  mesh.backendVersion = "1";
  mesh.conformity = MeshConformity::NonConformal;
  mesh.nodes = {{10, 0, 0, 0, {}}, {20, 1, 0, 0, {}},
    {30, 0, 1, 0, {}}, {40, 0, 0, 1, {}}};
  mesh.elements = {{101, MeshElementType::Triangle3, {10, 20, 30, 0}, 1, "solid"},
    {201, MeshElementType::Tetrahedron4, {10, 20, 30, 40}, 1, "solid"}};
  mesh.regions = {{1, {"solid"}}};
  return mesh;
}
}

int main()
{
  using namespace cceda;
  using namespace persistence;
  const auto mesh = meshFixture();
  const auto stored = writeProjectMeshQuality(mesh);
  expect(stored.succeeded(), "committed quality can be stored");
  if (!stored.succeeded()) { return 1; }
  const auto root = QJsonDocument::fromJson(stored.bytes).object();
  expect(root.value("schema").toObject().value("name").toString() == "cceda.mesh-quality-project"
    && !root.contains("jobId") && root.value("meshRevision").toInteger() == 7,
    "project entry records committed identity with independent schema");
  const auto restored = readProjectMeshQuality(stored.bytes, mesh);
  expect(restored.succeeded() && restored.report->meshRevision == 7
    && restored.report->sourceGeometryRevision == 3
    && restored.report->triangles.size() == 1 && restored.report->tetrahedra.size() == 1,
    "project quality returns independently recomputed revision-bound data");
  expect(writeProjectMeshQuality(mesh).bytes == stored.bytes, "project encoding is deterministic");

  const auto binary = meshing::writeMeshBinary(mesh, domain::MeshValidationMode::CommittedMesh);
  const auto decoded = meshing::readMeshBinary(binary.bytes, domain::MeshValidationMode::CommittedMesh);
  expect(decoded.succeeded() && readProjectMeshQuality(stored.bytes, *decoded.mesh).succeeded(),
    "paired meshbin and quality entries roundtrip together");
  auto changed = mesh;
  ++changed.meshRevision;
  expect(!readProjectMeshQuality(stored.bytes, changed).succeeded(), "another mesh revision is rejected");
  changed = mesh;
  ++changed.sourceGeometryRevision;
  expect(!readProjectMeshQuality(stored.bytes, changed).succeeded(), "another geometry revision is rejected");
  changed = mesh;
  changed.nodes[1].x = 2;
  expect(!readProjectMeshQuality(stored.bytes, changed).succeeded(), "changed geometry metrics are detected");
  changed = mesh;
  changed.meshRevision = 0;
  expect(writeProjectMeshQuality(changed).error == ProjectMeshQualityError::InvalidMesh
    && !readProjectMeshQuality(stored.bytes, changed).report,
    "worker meshes cannot be restored as committed quality");
  changed = mesh;
  std::swap(changed.elements[1].nodeIds[0], changed.elements[1].nodeIds[1]);
  expect(writeProjectMeshQuality(changed).error == ProjectMeshQualityError::InvalidQuality,
    "inverted tetrahedra cannot become stored accepted quality");

  changed = mesh;
  changed.nodes.push_back({50, 0, -1, 0, {}});
  changed.elements.push_back({102, domain::MeshElementType::Triangle3, {10, 20, 50, 0}, 1, "solid"});
  const auto warningBytes = writeProjectMeshQuality(changed);
  const auto warnings = readProjectMeshQuality(warningBytes.bytes, changed);
  expect(warningBytes.succeeded() && warnings.succeeded() && warnings.report->issues.size() == 2,
    "accepted orientation warnings and their exact IDs survive persistence");
  if (warnings.report)
  {
    const auto details = mesh_quality::buildMeshQualityIssueDetails(changed, *warnings.report);
    expect(details && details->count(101) == 1 && details->count(102) == 1,
      "verified restored quality can rebuild bounded issue details");
  }
  auto tampered = root;
  auto summaries = root.value("summaries").toArray();
  auto first = summaries[0].toObject();
  first.insert("mean", 999);
  summaries[0] = first;
  tampered.insert("summaries", summaries);
  expect(!readProjectMeshQuality(QJsonDocument(tampered).toJson(QJsonDocument::Compact), mesh).report,
    "tampered summaries return no report");
  tampered = root;
  tampered.insert("validForCommit", false);
  expect(!readProjectMeshQuality(QJsonDocument(tampered).toJson(QJsonDocument::Compact), mesh).succeeded(),
    "stored validity cannot override recomputation");
  QByteArray duplicate = stored.bytes;
  duplicate.insert(1, "\"meshRevision\":7,");
  expect(!readProjectMeshQuality(duplicate, mesh).succeeded(), "duplicate fields are rejected");
  expect(!readProjectMeshQuality(stored.bytes + " ", mesh).succeeded(), "noncanonical bytes are rejected");
  expect(!readProjectMeshQuality("{}", mesh).succeeded(), "incomplete quality is rejected");
  tampered = root;
  auto schema = root.value("schema").toObject();
  schema.insert("minor", 2);
  tampered.insert("schema", schema);
  expect(!readProjectMeshQuality(QJsonDocument(tampered).toJson(QJsonDocument::Compact), mesh).succeeded(),
    "unknown versions are rejected");

  meshing::MeshQualityJsonLimits limits;
  limits.maximumInputBytes = 1;
  expect(readProjectMeshQuality(stored.bytes, mesh, limits).error == ProjectMeshQualityError::InputTooLarge,
    "input byte budget is enforced before evaluation");
  expect(!writeProjectMeshQuality(mesh, limits).succeeded(), "writer enforces encoded byte budget");
  limits = {};
  limits.maximumIssues = 1;
  expect(!writeProjectMeshQuality(changed, limits).succeeded(), "issue budget is enforced");
  limits.maximumIssues = 0;
  expect(!readProjectMeshQuality(stored.bytes, mesh, limits).succeeded(), "invalid limits are rejected");

  QBuffer workerInput;
  workerInput.setData(stored.bytes);
  workerInput.open(QIODevice::ReadOnly);
  expect(!meshing::readMeshQualityJson(workerInput).succeeded(), "worker reader rejects project entries");
  auto workerReport = *restored.report;
  workerReport.meshRevision = 0;
  const auto workerBytes = meshing::writeMeshQualityJson(workerReport, "job-1");
  expect(workerBytes.succeeded() && !readProjectMeshQuality(workerBytes.bytes, mesh).succeeded(),
    "project reader rejects worker artifacts");
  return failures == 0 ? 0 : 1;
}
