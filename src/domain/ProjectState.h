#pragma once

#include "domain/MeshModel.h"
#include "domain/Revisions.h"

#include <memory>
#include <optional>
#include <thread>

namespace cceda::domain
{
enum class MeshState
{
  Missing,
  Ready,
  Stale,
  Invalid,
};

struct MeshExportSnapshot
{
  std::shared_ptr<const MeshModel> mesh;
  GeometryRevision geometryRevision = 0;
  MeshRevision meshRevision = 0;
};

class ProjectState
{
public:
  ProjectState() = default;
  ProjectState(const ProjectState&) = delete;
  ProjectState& operator=(const ProjectState&) = delete;
  ProjectState(ProjectState&&) noexcept = default;
  ProjectState& operator=(ProjectState&&) noexcept = default;

  [[nodiscard]] GeometryRevision geometryRevision() const noexcept;
  [[nodiscard]] MeshRevision meshRevision() const noexcept;
  [[nodiscard]] MeshState meshState() const noexcept;
  [[nodiscard]] bool isOwnerThread() const noexcept;
  [[nodiscard]] const MeshModel* currentMesh() const noexcept;
  [[nodiscard]] std::shared_ptr<const MeshModel> currentMeshHandle() const noexcept;
  [[nodiscard]] bool canExportMesh() const noexcept;
  [[nodiscard]] std::optional<MeshExportSnapshot>
    captureCurrentMeshForExport() const noexcept;
  [[nodiscard]] bool isCurrentMeshExportSnapshot(
    const MeshExportSnapshot& snapshot) const noexcept;

  void commitImportedGeometry();
  void commitGeometryChange();
  [[nodiscard]] bool commitMesh(MeshModel workerMesh);

private:
  [[nodiscard]] bool hasExportableCurrentMesh() const noexcept;

  GeometryRevision geometryRevision_ = 0;
  MeshRevision meshRevision_ = 0;
  MeshState meshState_ = MeshState::Missing;
  std::shared_ptr<MeshModel> currentMesh_;
  std::thread::id ownerThread_ = std::this_thread::get_id();
};
}
