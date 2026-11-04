#include "domain/MeshModel.h"
#include "domain/ProjectState.h"

#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

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

  static_assert(!std::is_copy_constructible_v<ProjectState>);
  static_assert(!std::is_copy_assignable_v<ProjectState>);
  static_assert(std::is_nothrow_move_constructible_v<ProjectState>);

  ProjectState state;
  expect(state.geometryRevision() == 0, "a new project has no geometry revision");
  expect(state.meshState() == MeshState::Missing, "a new project has no mesh");
  expect(state.currentMesh() == nullptr, "a new project has no current mesh object");
  expect(state.currentMeshHandle() == nullptr, "a new project has no shared mesh snapshot");
  expect(!state.canExportMesh(), "a missing mesh cannot be exported");
  expect(!state.captureCurrentMeshForExport().has_value(),
    "a missing mesh cannot produce an export snapshot");

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
  const std::shared_ptr<const MeshModel> firstMesh = state.currentMeshHandle();
  expect(firstMesh != nullptr && firstMesh.get() == state.currentMesh(),
    "the shared mesh snapshot refers to the current committed mesh");
  expect(state.meshState() == MeshState::Ready, "a committed mesh is ready");
  expect(state.canExportMesh(), "a ready current mesh can be exported");
  const std::optional<MeshExportSnapshot> firstExportSnapshot =
    state.captureCurrentMeshForExport();
  expect(firstExportSnapshot.has_value()
      && firstExportSnapshot->mesh.get() == state.currentMesh()
      && firstExportSnapshot->geometryRevision == 1
      && firstExportSnapshot->meshRevision == 1,
    "a ready mesh produces an exact revision-bound export snapshot");
  expect(firstExportSnapshot.has_value()
      && state.isCurrentMeshExportSnapshot(*firstExportSnapshot),
    "a newly captured export snapshot is current");

  state.commitGeometryChange();
  expect(state.geometryRevision() == 2, "healing advances the geometry revision");
  expect(state.meshState() == MeshState::Stale, "geometry changes make the mesh stale");
  expect(state.currentMesh() != nullptr, "a stale mesh remains available for inspection");
  expect(!state.canExportMesh(), "a stale mesh cannot be exported");
  expect(!state.captureCurrentMeshForExport().has_value(),
    "a stale mesh cannot produce a new export snapshot");
  expect(firstExportSnapshot.has_value()
      && !state.isCurrentMeshExportSnapshot(*firstExportSnapshot)
      && firstExportSnapshot->mesh != nullptr
      && firstExportSnapshot->mesh->meshRevision == 1,
    "geometry changes invalidate export publication without destroying snapshot data");

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
  const std::shared_ptr<const MeshModel> secondMesh = state.currentMeshHandle();
  expect(secondMesh != nullptr && secondMesh != firstMesh,
    "replacing the current mesh publishes a new immutable snapshot");
  expect(firstMesh != nullptr && firstMesh->meshRevision == 1
      && firstMesh->sourceGeometryRevision == 1,
    "an earlier snapshot remains valid for an in-flight background task");
  expect(state.currentMesh() != nullptr
      && state.currentMesh()->sourceGeometryRevision == state.geometryRevision(),
    "a ready mesh belongs to the exact current geometry revision");
  const std::optional<MeshExportSnapshot> secondExportSnapshot =
    state.captureCurrentMeshForExport();
  expect(secondExportSnapshot.has_value()
      && state.isCurrentMeshExportSnapshot(*secondExportSnapshot),
    "the replacement mesh produces a current export snapshot");
  if (secondExportSnapshot)
  {
    MeshExportSnapshot wrongGeometryRevision = *secondExportSnapshot;
    ++wrongGeometryRevision.geometryRevision;
    expect(!state.isCurrentMeshExportSnapshot(wrongGeometryRevision),
      "snapshot publication rejects a mismatched captured geometry revision");

    MeshExportSnapshot wrongMeshRevision = *secondExportSnapshot;
    ++wrongMeshRevision.meshRevision;
    expect(!state.isCurrentMeshExportSnapshot(wrongMeshRevision),
      "snapshot publication rejects a mismatched captured mesh revision");
  }
  expect(firstExportSnapshot.has_value()
      && !state.isCurrentMeshExportSnapshot(*firstExportSnapshot),
    "successful remeshing keeps an earlier export snapshot non-current");

  MeshModel rejectedReplacement = makeWorkerMesh(2);
  rejectedReplacement.elements.front().nodeIds.front() = 999;
  expect(!state.commitMesh(std::move(rejectedReplacement)),
    "a malformed replacement mesh is rejected");
  expect(secondExportSnapshot.has_value()
      && state.isCurrentMeshExportSnapshot(*secondExportSnapshot),
    "a rejected replacement preserves the current export snapshot");

  if (failures == 0)
  {
    std::cout << "All ProjectState tests passed.\n";
  }
  return failures == 0 ? 0 : 1;
}
