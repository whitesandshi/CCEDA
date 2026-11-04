#include "application/WorkerMeshArtifactValidator.h"
#include "meshing/MeshBinaryCodec.h"

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

cceda::domain::MeshModel makeWorkerMesh()
{
  using namespace cceda::domain;
  MeshModel mesh;
  mesh.sourceGeometryRevision = 7;
  mesh.creationTimeUtc = "2026-07-25T00:00:00.000Z";
  mesh.backendId = "gmsh";
  mesh.backendVersion = "4.15.2";
  mesh.conformity = MeshConformity::NonConformal;
  mesh.nodes = {
    {1, 0.0, 0.0, 0.0, "solid-7"},
    {2, 1.0, 0.0, 0.0, "solid-7"},
    {3, 0.0, 1.0, 0.0, "solid-7"},
    {4, 0.0, 0.0, 1.0, {}},
  };
  mesh.elements = {
    {11, MeshElementType::Triangle3, {1, 2, 3, 0}, 3, "solid-7"},
    {12, MeshElementType::Tetrahedron4, {1, 2, 3, 4}, 3, "solid-7"},
  };
  mesh.regions = {{3, {"solid-7"}}};
  return mesh;
}

cceda::domain::MeshModel makeMultiRegionWorkerMesh()
{
  using namespace cceda::domain;
  MeshModel mesh = makeWorkerMesh();
  mesh.nodes.insert(mesh.nodes.end(), {
    {5, 2.0, 0.0, 0.0, "solid-8"},
    {6, 3.0, 0.0, 0.0, "solid-8"},
    {7, 2.0, 1.0, 0.0, "solid-8"},
    {8, 2.0, 0.0, 1.0, {}},
  });
  mesh.elements.insert(mesh.elements.end(), {
    {21, MeshElementType::Triangle3, {5, 6, 7, 0}, 4, "solid-8"},
    {22, MeshElementType::Tetrahedron4, {5, 6, 7, 8}, 4, "solid-8"},
  });
  mesh.regions.push_back({4, {"solid-8"}});
  return mesh;
}

cceda::application::WorkerMeshArtifactExpectation expectation()
{
  return {
    7,
    "gmsh",
    "4.15.2",
    4,
    1,
    1,
    {{3, "solid-7"}},
  };
}

cceda::application::WorkerMeshArtifactExpectation multiRegionExpectation()
{
  return {
    7,
    "gmsh",
    "4.15.2",
    8,
    2,
    2,
    {{3, "solid-7"}, {4, "solid-8"}},
  };
}
}

int main()
{
  using namespace cceda;
  const auto encoded = meshing::writeMeshBinary(
    makeWorkerMesh(), domain::MeshValidationMode::WorkerArtifact);
  expect(encoded.succeeded(), "test Worker mesh should encode");

  const auto valid = application::validateWorkerMeshArtifact(
    encoded.bytes, expectation(), {});
  expect(valid.succeeded(), "valid Worker mesh artifact should pass policy validation");
  expect(valid.mesh && valid.mesh->meshRevision == 0,
    "validated artifact should retain Worker mesh revision zero");

  const auto multiRegionEncoded = meshing::writeMeshBinary(
    makeMultiRegionWorkerMesh(), domain::MeshValidationMode::WorkerArtifact);
  expect(multiRegionEncoded.succeeded(),
    "test multi-region Worker mesh should encode");
  expect(application::validateWorkerMeshArtifact(
      multiRegionEncoded.bytes, multiRegionExpectation(), {}).succeeded(),
    "a complete independent-Solid region mapping should pass validation");

  auto missingRegion = multiRegionExpectation();
  missingRegion.regions.pop_back();
  expect(!application::validateWorkerMeshArtifact(
      multiRegionEncoded.bytes, missingRegion, {}).succeeded(),
    "every neutral mesh region must have a submitted mapping expectation");

  auto duplicateRegion = multiRegionExpectation();
  duplicateRegion.regions.back().regionId = duplicateRegion.regions.front().regionId;
  expect(application::validateWorkerMeshArtifact(
      multiRegionEncoded.bytes, duplicateRegion, {}).code
      == application::WorkerMeshArtifactValidationCode::InvalidExpectation,
    "duplicate expected region IDs must be rejected at the policy boundary");

  auto crossedRegionAssociation = makeMultiRegionWorkerMesh();
  crossedRegionAssociation.elements.front().geometryId = "solid-8";
  const auto crossedRegionBytes = meshing::writeMeshBinary(
    crossedRegionAssociation, domain::MeshValidationMode::WorkerArtifact);
  expect(crossedRegionBytes.succeeded(),
    "crossed-region association fixture should encode");
  expect(!application::validateWorkerMeshArtifact(
      crossedRegionBytes.bytes, multiRegionExpectation(), {}).succeeded(),
    "each element must reference the Solid assigned to its own region");

  auto corrupt = encoded.bytes;
  corrupt.back() ^= 0x01U;
  expect(!application::validateWorkerMeshArtifact(
      corrupt, expectation(), {}).succeeded(),
    "checksum-corrupt Worker mesh artifact should be rejected");

  auto wrongCounts = expectation();
  ++wrongCounts.nodeCount;
  expect(!application::validateWorkerMeshArtifact(
      encoded.bytes, wrongCounts, {}).succeeded(),
    "Worker mesh counts must match the control result");

  auto foreignAssociation = makeWorkerMesh();
  foreignAssociation.elements.front().geometryId = "foreign-solid";
  const auto foreignBytes = meshing::writeMeshBinary(
    foreignAssociation, domain::MeshValidationMode::WorkerArtifact);
  expect(foreignBytes.succeeded(), "foreign-association fixture should encode");
  expect(!application::validateWorkerMeshArtifact(
      foreignBytes.bytes, expectation(), {}).succeeded(),
    "Worker mesh geometry associations must stay inside the mapped Solid");

  auto foreignNodeAssociation = makeWorkerMesh();
  foreignNodeAssociation.nodes.front().geometryId = "foreign-face";
  const auto foreignNodeBytes = meshing::writeMeshBinary(
    foreignNodeAssociation, domain::MeshValidationMode::WorkerArtifact);
  expect(foreignNodeBytes.succeeded(), "foreign-node fixture should encode");
  expect(!application::validateWorkerMeshArtifact(
      foreignNodeBytes.bytes, expectation(), {}).succeeded(),
    "non-empty node associations must stay inside the mapped Solid");

  if (failures == 0)
  {
    std::cout << "Worker mesh artifact validator tests passed.\n";
  }
  return failures == 0 ? 0 : 1;
}
