#include "domain/MeshModel.h"

#include <cmath>
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

bool hasIssue(
  const cceda::domain::MeshValidationReport& report,
  cceda::domain::MeshValidationCode code)
{
  for (const auto& issue : report.issues)
  {
    if (issue.code == code)
    {
      return true;
    }
  }
  return false;
}

cceda::domain::MeshModel makeValidWorkerMesh()
{
  using namespace cceda::domain;

  MeshModel mesh;
  mesh.meshRevision = 0;
  mesh.sourceGeometryRevision = 7;
  mesh.creationTimeUtc = "2026-07-23T00:00:00.000Z";
  mesh.backendId = "gmsh";
  mesh.backendVersion = "4.15.2";
  mesh.conformity = MeshConformity::NonConformal;
  mesh.nodes = {
    {1, 0.0, 0.0, 0.0, "solid-1/vertex-1"},
    {2, 1.0, 0.0, 0.0, "solid-1/vertex-2"},
    {3, 0.0, 1.0, 0.0, "solid-1/vertex-3"},
    {4, 0.0, 0.0, 1.0, {}},
  };
  mesh.elements = {
    {101, MeshElementType::Triangle3, {1, 2, 3, 0}, 10, "solid-1/face-1"},
    {102, MeshElementType::Tetrahedron4, {1, 2, 3, 4}, 10, "solid-1"},
  };
  mesh.regions = {
    {10, {"solid-1"}},
  };
  return mesh;
}
} // namespace

int main()
{
  using namespace cceda::domain;

  const MeshModel validWorkerMesh = makeValidWorkerMesh();
  const MeshValidationReport workerReport =
    validateMeshModel(validWorkerMesh, MeshValidationMode::WorkerArtifact);
  expect(workerReport.valid(), "a complete uncommitted Worker mesh is valid");
  expect(workerReport.nodeCount == 4, "validation reports the node count");
  expect(workerReport.triangleCount == 1, "validation reports the Triangle3 count");
  expect(workerReport.tetrahedronCount == 1, "validation reports the Tetrahedron4 count");
  expect(workerReport.regionCount == 1, "validation reports the region count");

  MeshModel committed = validWorkerMesh;
  committed.meshRevision = 3;
  expect(validateMeshModel(committed, MeshValidationMode::CommittedMesh).valid(),
    "a positive mesh revision is valid for a committed mesh");
  expect(hasIssue(validateMeshModel(validWorkerMesh, MeshValidationMode::CommittedMesh),
      MeshValidationCode::CommittedMeshRevisionMissing),
    "a committed mesh rejects MeshRevision zero");
  expect(hasIssue(validateMeshModel(committed, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::WorkerMeshRevisionNotZero),
    "a Worker artifact rejects a positive MeshRevision");

  MeshModel missingSource = validWorkerMesh;
  missingSource.sourceGeometryRevision = 0;
  expect(hasIssue(validateMeshModel(missingSource, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::SourceGeometryRevisionMissing),
    "a mesh requires its exact source geometry revision");

  MeshModel duplicateNode = validWorkerMesh;
  duplicateNode.nodes.at(1).id = duplicateNode.nodes.at(0).id;
  expect(hasIssue(validateMeshModel(duplicateNode, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::DuplicateNodeId),
    "duplicate node IDs are rejected");

  MeshModel nonFiniteNode = validWorkerMesh;
  nonFiniteNode.nodes.at(0).x = std::nan("");
  expect(hasIssue(validateMeshModel(nonFiniteNode, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::NonFiniteNodeCoordinate),
    "non-finite node coordinates are rejected");

  MeshModel duplicateElement = validWorkerMesh;
  duplicateElement.elements.at(1).id = duplicateElement.elements.at(0).id;
  expect(hasIssue(validateMeshModel(duplicateElement, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::DuplicateElementId),
    "duplicate element IDs are rejected");

  MeshModel missingTriangles = validWorkerMesh;
  missingTriangles.elements.erase(missingTriangles.elements.begin());
  expect(hasIssue(validateMeshModel(missingTriangles, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::TriangleSetEmpty),
    "a first-release neutral mesh requires Triangle3 surface elements");

  MeshModel missingTetrahedra = validWorkerMesh;
  missingTetrahedra.elements.pop_back();
  expect(hasIssue(validateMeshModel(missingTetrahedra, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::TetrahedronSetEmpty),
    "a first-release neutral mesh requires Tetrahedron4 volume elements");

  MeshModel unsupportedElement = validWorkerMesh;
  unsupportedElement.elements.at(0).type = static_cast<MeshElementType>(99);
  expect(hasIssue(validateMeshModel(
      unsupportedElement, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::UnsupportedElementType),
    "unknown element enum values are rejected");

  MeshModel missingNode = validWorkerMesh;
  missingNode.elements.at(1).nodeIds.at(3) = 999;
  expect(hasIssue(validateMeshModel(missingNode, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::ElementNodeMissing),
    "an element cannot reference a missing node");

  MeshModel degenerateElement = validWorkerMesh;
  degenerateElement.elements.at(1).nodeIds.at(3) = 1;
  expect(hasIssue(validateMeshModel(degenerateElement, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::ElementNodeRepeated),
    "an element cannot repeat a node");

  MeshModel invalidTriangle = validWorkerMesh;
  invalidTriangle.elements.at(0).nodeIds.at(3) = 4;
  expect(hasIssue(validateMeshModel(invalidTriangle, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::TriangleFourthNodeNotZero),
    "Triangle3 reserves its fourth node slot as zero");

  MeshModel unknownRegion = validWorkerMesh;
  unknownRegion.elements.at(1).regionId = 999;
  expect(hasIssue(validateMeshModel(unknownRegion, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::ElementRegionMissing),
    "an element cannot reference a missing region");

  MeshModel missingElementGeometry = validWorkerMesh;
  missingElementGeometry.elements.at(1).geometryId.clear();
  expect(hasIssue(validateMeshModel(
      missingElementGeometry, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::ElementGeometryAssociationMissing),
    "every element retains a geometry association");

  MeshModel duplicateRegion = validWorkerMesh;
  duplicateRegion.regions.push_back(duplicateRegion.regions.front());
  expect(hasIssue(validateMeshModel(duplicateRegion, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::DuplicateRegionId),
    "duplicate region IDs are rejected");

  MeshModel missingRegionMapping = validWorkerMesh;
  missingRegionMapping.regions.front().sourceGeometryIds.clear();
  expect(hasIssue(validateMeshModel(
      missingRegionMapping, MeshValidationMode::WorkerArtifact),
      MeshValidationCode::RegionGeometryMappingMissing),
    "every region maps back to source geometry");

  if (failures == 0)
  {
    std::cout << "All MeshModel tests passed.\n";
  }
  return failures == 0 ? 0 : 1;
}
