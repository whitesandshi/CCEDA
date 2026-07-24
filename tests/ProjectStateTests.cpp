#include "domain/MeshModel.h"
#include "domain/ProjectState.h"

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

cceda::domain::MeshModel makeWorkerMesh(cceda::domain::GeometryRevision sourceRevision)
{
  using namespace cceda::domain;

  MeshModel mesh;
  mesh.sourceGeometryRevision = sourceRevision;
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
}

int main()
{
  using namespace cceda::domain;

  ProjectState state;
  expect(state.geometryRevision() == 0, "a new project has no geometry revision");
  expect(state.meshState() == MeshState::Missing, "a new project has no mesh");
  expect(state.currentMesh() == nullptr, "a new project has no current mesh object");
  expect(!state.canExportMesh(), "a missing mesh cannot be exported");

  state.commitImportedGeometry();
  expect(state.geometryRevision() == 1, "import creates geometry revision one");

  MeshModel malformed = makeWorkerMesh(1);
  malformed.elements.front().nodeIds.front() = 999;
  expect(!state.commitMesh(malformed), "a structurally invalid Worker mesh is rejected");
  expect(state.currentMesh() == nullptr, "a rejected first mesh does not change the project");

  MeshModel workerMesh = makeWorkerMesh(1);
  expect(state.commitMesh(workerMesh), "a valid Worker mesh for current geometry is committed");
  expect(state.meshRevision() == 1, "the aggregate assigns mesh revision one");
  expect(state.currentMesh() != nullptr, "the committed neutral mesh is retained");
  expect(state.currentMesh() != nullptr && state.currentMesh()->meshRevision == 1,
    "the committed mesh header receives the assigned revision");
  expect(state.meshState() == MeshState::Ready, "a committed mesh is ready");
  expect(state.canExportMesh(), "a ready current mesh can be exported");

  state.commitGeometryChange();
  expect(state.geometryRevision() == 2, "healing advances the geometry revision");
  expect(state.meshState() == MeshState::Stale, "geometry changes make the mesh stale");
  expect(state.currentMesh() != nullptr, "a stale mesh remains available for inspection");
  expect(!state.canExportMesh(), "a stale mesh cannot be exported");

  const MeshModel staleSnapshot = *state.currentMesh();
  expect(!state.commitMesh(makeWorkerMesh(1)),
    "a Worker mesh from an old geometry revision is rejected");
  expect(state.meshState() == MeshState::Stale,
    "a rejected mesh preserves the stale state");
  expect(state.currentMesh() != nullptr
      && state.currentMesh()->meshRevision == staleSnapshot.meshRevision,
    "a rejected mesh preserves the previous current mesh");

  MeshModel alreadyCommitted = makeWorkerMesh(2);
  alreadyCommitted.meshRevision = 99;
  expect(!state.commitMesh(alreadyCommitted),
    "the project accepts only uncommitted Worker artifacts");

  expect(state.commitMesh(makeWorkerMesh(2)),
    "a valid mesh from the new geometry revision is accepted");
  expect(state.meshRevision() == 2, "mesh revisions advance monotonically");
  expect(state.currentMesh() != nullptr
      && state.currentMesh()->sourceGeometryRevision == state.geometryRevision(),
    "a ready mesh belongs to the exact current geometry revision");

  if (failures == 0)
  {
    std::cout << "All ProjectState tests passed.\n";
  }
  return failures == 0 ? 0 : 1;
}
