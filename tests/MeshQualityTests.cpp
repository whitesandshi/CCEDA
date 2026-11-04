#include "mesh_quality/MeshQuality.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
int failures = 0;

void expect(bool condition, std::string_view message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool closeTo(double actual, double expected, double tolerance = 1.0e-10)
{
  return std::abs(actual - expected) <= tolerance;
}

cceda::domain::MeshModel makeReferenceMesh()
{
  using namespace cceda::domain;
  const double rootThree = std::sqrt(3.0);
  const double rootTwoThirds = std::sqrt(2.0 / 3.0);

  MeshModel mesh;
  mesh.sourceGeometryRevision = 1;
  mesh.creationTimeUtc = "2026-07-25T00:00:00.000Z";
  mesh.backendId = "test";
  mesh.backendVersion = "1";
  mesh.conformity = MeshConformity::NonConformal;
  mesh.nodes = {
    {1, 0.0, 0.0, 0.0, {}},
    {2, 1.0, 0.0, 0.0, {}},
    {3, 0.5, rootThree / 2.0, 0.0, {}},
    {10, 0.0, 0.0, 0.0, {}},
    {11, 1.0, 0.0, 0.0, {}},
    {12, 0.5, rootThree / 2.0, 0.0, {}},
    {13, 0.5, rootThree / 6.0, rootTwoThirds, {}},
  };
  mesh.elements = {
    {101, MeshElementType::Triangle3, {1, 2, 3, 0}, 7, "solid-1"},
    {201, MeshElementType::Tetrahedron4, {10, 11, 12, 13}, 7, "solid-1"},
  };
  mesh.regions = {{7, {"solid-1"}}};
  return mesh;
}

const cceda::mesh_quality::MeshQualityIssue* findIssue(
  const cceda::mesh_quality::MeshQualityReport& report,
  cceda::domain::MeshElementId elementId,
  cceda::mesh_quality::MeshQualityIssueCode code)
{
  const auto it = std::find_if(report.issues.begin(), report.issues.end(),
    [=](const cceda::mesh_quality::MeshQualityIssue& issue) {
      return issue.elementId == elementId && issue.code == code;
    });
  return it == report.issues.end() ? nullptr : &*it;
}

void metricContractIsStable()
{
  using namespace cceda::mesh_quality;
  expect(MeshQualityMetricVersion == "cceda.mesh-quality/0.1",
    "quality metric version should be stable");
  expect(stableMetricId(MeshQualityMetric::TriangleArea)
      == "triangle.area.mm2",
    "triangle area ID should be stable");
  expect(stableMetricId(MeshQualityMetric::TetrahedronScaledJacobian)
      == "tetrahedron.scaled-jacobian",
    "tetrahedron scaled-Jacobian ID should be stable");
  expect(metricDirection(MeshQualityMetric::TriangleAspectRatio)
      == MeshQualityDirection::LowerIsBetter,
    "triangle aspect ratio should be lower-is-better");
  expect(metricDirection(MeshQualityMetric::TetrahedronSignedVolume)
      == MeshQualityDirection::PositiveRequired,
    "tetrahedron signed volume should require a positive value");
  expect(stableIssueId(MeshQualityIssueCode::NonManifoldSurfaceEdge)
      == "non-manifold-surface-edge",
    "quality issue IDs should be stable for persistence");
}

void referenceElementsProduceIdealMetrics()
{
  using namespace cceda::mesh_quality;
  const auto result = evaluateMeshQuality(
    makeReferenceMesh(), cceda::domain::MeshValidationMode::WorkerArtifact);
  expect(result.succeeded(), "reference mesh should evaluate");
  if (!result.report)
  {
    return;
  }

  const MeshQualityReport& report = *result.report;
  expect(report.validForCommit(), "reference mesh should be commit-valid");
  expect(report.triangles.size() == 1, "one triangle metric should be returned");
  expect(report.tetrahedra.size() == 1,
    "one tetrahedron metric should be returned");
  if (report.triangles.size() == 1)
  {
    const TriangleQuality& triangle = report.triangles.front();
    expect(closeTo(triangle.areaSquareMillimeters, std::sqrt(3.0) / 4.0),
      "equilateral triangle area should match");
    expect(triangle.aspectRatio && closeTo(*triangle.aspectRatio, 1.0),
      "equilateral triangle aspect ratio should be one");
    expect(triangle.minimumAngleDegrees
        && closeTo(*triangle.minimumAngleDegrees, 60.0),
      "equilateral triangle minimum angle should be 60 degrees");
    expect(triangle.maximumAngleDegrees
        && closeTo(*triangle.maximumAngleDegrees, 60.0),
      "equilateral triangle maximum angle should be 60 degrees");
    expect(triangle.normalConsistent,
      "an isolated triangle should have neutral normal consistency");
  }
  if (report.tetrahedra.size() == 1)
  {
    const TetrahedronQuality& tetrahedron = report.tetrahedra.front();
    expect(closeTo(tetrahedron.signedVolumeCubicMillimeters,
        std::sqrt(2.0) / 12.0),
      "regular tetrahedron signed volume should match");
    expect(tetrahedron.aspectRatio && closeTo(*tetrahedron.aspectRatio, 1.0),
      "regular tetrahedron aspect ratio should be one");
    expect(tetrahedron.minimumDihedralAngleDegrees
        && closeTo(*tetrahedron.minimumDihedralAngleDegrees,
          70.52877936550931),
      "regular tetrahedron minimum dihedral angle should match");
    expect(tetrahedron.scaledJacobian
        && closeTo(*tetrahedron.scaledJacobian, 1.0),
      "regular tetrahedron scaled Jacobian should be one");
  }
}

void sharedTriangleOrientationIsDetected()
{
  using namespace cceda;
  domain::MeshModel mesh = makeReferenceMesh();
  mesh.nodes.push_back({4, 0.5, -1.0, 0.0, {}});
  mesh.elements.insert(mesh.elements.begin() + 1,
    {102, domain::MeshElementType::Triangle3, {2, 1, 4, 0}, 7, "solid-1"});

  const auto consistent = mesh_quality::evaluateMeshQuality(
    mesh, domain::MeshValidationMode::WorkerArtifact);
  expect(consistent.succeeded(), "consistent adjacent triangles should evaluate");
  expect(consistent.report && consistent.report->issues.empty(),
    "opposite shared-edge directions should be consistent");

  mesh.elements.at(1).nodeIds = {1, 2, 4, 0};
  const auto inconsistent = mesh_quality::evaluateMeshQuality(
    mesh, domain::MeshValidationMode::WorkerArtifact);
  expect(inconsistent.succeeded(), "inconsistent adjacent triangles should evaluate");
  if (!inconsistent.report)
  {
    return;
  }
  expect(findIssue(*inconsistent.report, 101,
      mesh_quality::MeshQualityIssueCode::InconsistentTriangleOrientation),
    "first triangle should be indexed as orientation-inconsistent");
  expect(findIssue(*inconsistent.report, 102,
      mesh_quality::MeshQualityIssueCode::InconsistentTriangleOrientation),
    "second triangle should be indexed as orientation-inconsistent");
  expect(inconsistent.report->validForCommit(),
    "orientation warnings alone should not block commit");

  const auto details = mesh_quality::buildMeshQualityIssueDetails(
    mesh, *inconsistent.report);
  expect(details && details->size() == 2,
    "quality inspection should retain one direct detail record per issue element");
  if (details && details->count(101) == 1)
  {
    const mesh_quality::MeshQualityElementDetail& detail = details->at(101);
    expect(detail.elementType == domain::MeshElementType::Triangle3
        && detail.nodeCount == 3
        && detail.nodeIds.at(0) == 1
        && detail.nodeIds.at(1) == 2
        && detail.nodeIds.at(2) == 3,
      "issue detail should retain exact element type and ordered node IDs");
    expect(detail.regionId == 7 && detail.geometryId == "solid-1"
        && detail.triangleQuality
        && !detail.triangleQuality->normalConsistent
        && !detail.tetrahedronQuality,
      "issue detail should retain association and applicable metric values");
  }

  mesh_quality::MeshQualityReport mismatched = *inconsistent.report;
  ++mismatched.sourceGeometryRevision;
  expect(!mesh_quality::buildMeshQualityIssueDetails(mesh, mismatched),
    "issue details should reject a report from another geometry revision");
}

void nonManifoldSurfaceEdgesBlockCommit()
{
  using namespace cceda;
  domain::MeshModel mesh = makeReferenceMesh();
  mesh.nodes.push_back({4, 0.5, -1.0, 0.0, {}});
  mesh.nodes.push_back({5, 0.5, 0.0, 1.0, {}});
  mesh.elements.insert(mesh.elements.begin() + 1, {
    {102, domain::MeshElementType::Triangle3, {2, 1, 4, 0}, 7, "solid-1"},
    {103, domain::MeshElementType::Triangle3, {1, 2, 5, 0}, 7, "solid-1"},
  });

  const auto result = mesh_quality::evaluateMeshQuality(
    mesh, domain::MeshValidationMode::WorkerArtifact);
  expect(result.succeeded(), "non-manifold triangle mesh should evaluate");
  if (!result.report)
  {
    return;
  }
  expect(!result.report->validForCommit(),
    "a non-manifold surface edge should block commit");
  for (const domain::MeshElementId elementId : {101ULL, 102ULL, 103ULL})
  {
    expect(findIssue(*result.report, elementId,
        mesh_quality::MeshQualityIssueCode::NonManifoldSurfaceEdge),
      "every triangle on a non-manifold edge should be indexed");
  }
}

void invertedAndDegenerateElementsBlockCommit()
{
  using namespace cceda;
  domain::MeshModel inverted = makeReferenceMesh();
  std::swap(inverted.elements.back().nodeIds.at(1),
    inverted.elements.back().nodeIds.at(2));
  const auto invertedResult = mesh_quality::evaluateMeshQuality(
    inverted, domain::MeshValidationMode::WorkerArtifact);
  expect(invertedResult.succeeded(), "inverted tetrahedron should evaluate");
  if (invertedResult.report)
  {
    const auto& tetrahedron = invertedResult.report->tetrahedra.front();
    expect(tetrahedron.signedVolumeCubicMillimeters < 0.0,
      "inverted tetrahedron should have negative signed volume");
    expect(tetrahedron.scaledJacobian && *tetrahedron.scaledJacobian < 0.0,
      "inverted tetrahedron should have negative scaled Jacobian");
    expect(findIssue(*invertedResult.report, 201,
        mesh_quality::MeshQualityIssueCode::InvertedTetrahedron),
      "inverted tetrahedron should have an exact issue index");
    expect(!invertedResult.report->validForCommit(),
      "inverted tetrahedron should block commit");
    const auto details = mesh_quality::buildMeshQualityIssueDetails(
      inverted, *invertedResult.report);
    expect(details && details->size() == 1 && details->count(201) == 1,
      "tetrahedron issues should produce one direct detail record");
    if (details && details->count(201) == 1)
    {
      const mesh_quality::MeshQualityElementDetail& detail = details->at(201);
      expect(detail.elementType == domain::MeshElementType::Tetrahedron4
          && detail.nodeCount == 4
          && detail.tetrahedronQuality
          && detail.tetrahedronQuality->signedVolumeCubicMillimeters < 0.0
          && !detail.triangleQuality,
        "tetrahedron detail should retain its type, nodes, and metric values");
    }
  }

  domain::MeshModel degenerate = makeReferenceMesh();
  degenerate.nodes.at(2) = {3, 2.0, 0.0, 0.0, {}};
  const auto degenerateResult = mesh_quality::evaluateMeshQuality(
    degenerate, domain::MeshValidationMode::WorkerArtifact);
  expect(degenerateResult.succeeded(), "zero-area triangle should evaluate");
  if (degenerateResult.report)
  {
    expect(findIssue(*degenerateResult.report, 101,
        mesh_quality::MeshQualityIssueCode::DegenerateTriangle),
      "zero-area triangle should have an exact issue index");
    expect(!degenerateResult.report->validForCommit(),
      "zero-area triangle should block commit");
  }

  domain::MeshModel flatTetrahedron = makeReferenceMesh();
  flatTetrahedron.nodes.back().z = 0.0;
  const auto flatResult = mesh_quality::evaluateMeshQuality(
    flatTetrahedron, domain::MeshValidationMode::WorkerArtifact);
  expect(flatResult.succeeded(), "zero-volume tetrahedron should evaluate");
  if (flatResult.report)
  {
    expect(findIssue(*flatResult.report, 201,
        mesh_quality::MeshQualityIssueCode::DegenerateTetrahedron),
      "zero-volume tetrahedron should have an exact issue index");
    expect(!flatResult.report->validForCommit(),
      "zero-volume tetrahedron should block commit");
  }

  domain::MeshModel overflow = makeReferenceMesh();
  overflow.nodes.at(1).x = 1.0e308;
  const auto overflowResult = mesh_quality::evaluateMeshQuality(
    overflow, domain::MeshValidationMode::WorkerArtifact);
  expect(overflowResult.succeeded(),
    "finite coordinates with overflowing metrics should evaluate");
  if (overflowResult.report)
  {
    expect(findIssue(*overflowResult.report, 101,
        mesh_quality::MeshQualityIssueCode::NonFiniteMetric),
      "overflowing metric should have an exact non-finite issue");
    expect(!overflowResult.report->validForCommit(),
      "a non-finite metric should block commit");
  }
}

void structurallyInvalidMeshIsRejected()
{
  cceda::domain::MeshModel mesh = makeReferenceMesh();
  mesh.elements.front().nodeIds.front() = 999;
  const auto result = cceda::mesh_quality::evaluateMeshQuality(
    mesh, cceda::domain::MeshValidationMode::WorkerArtifact);
  expect(!result.succeeded(), "structurally invalid mesh should be rejected");
  expect(result.code
      == cceda::mesh_quality::MeshQualityEvaluationCode::InvalidMesh,
    "structurally invalid mesh should use InvalidMesh");
  expect(!result.meshValidationIssues.empty(),
    "structural rejection should retain domain validation issues");
}
}

int main()
{
  metricContractIsStable();
  referenceElementsProduceIdealMetrics();
  sharedTriangleOrientationIsDetected();
  nonManifoldSurfaceEdgesBlockCommit();
  invertedAndDegenerateElementsBlockCommit();
  structurallyInvalidMeshIsRejected();
  if (failures == 0)
  {
    std::cout << "All mesh quality tests passed.\n";
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
