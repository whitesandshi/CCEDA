#include "ProjectState.h"

#include <limits>
#include <utility>

namespace cceda::domain
{
GeometryRevision ProjectState::geometryRevision() const noexcept
{
  return geometryRevision_;
}

MeshRevision ProjectState::meshRevision() const noexcept
{
  return meshRevision_;
}

MeshState ProjectState::meshState() const noexcept
{
  return meshState_;
}

bool ProjectState::isOwnerThread() const noexcept
{
  return std::this_thread::get_id() == ownerThread_;
}

const MeshModel* ProjectState::currentMesh() const noexcept
{
  return currentMesh_.get();
}

std::shared_ptr<const MeshModel> ProjectState::currentMeshHandle() const noexcept
{
  return currentMesh_;
}

bool ProjectState::canExportMesh() const noexcept
{
  return hasExportableCurrentMesh();
}

std::optional<MeshExportSnapshot>
ProjectState::captureCurrentMeshForExport() const noexcept
{
  if (!hasExportableCurrentMesh())
  {
    return std::nullopt;
  }
  return MeshExportSnapshot{currentMesh_, geometryRevision_, meshRevision_};
}

bool ProjectState::isCurrentMeshExportSnapshot(
  const MeshExportSnapshot& snapshot) const noexcept
{
  return hasExportableCurrentMesh()
    && snapshot.mesh != nullptr
    && snapshot.mesh.get() == currentMesh_.get()
    && snapshot.geometryRevision == geometryRevision_
    && snapshot.meshRevision == meshRevision_
    && snapshot.mesh->sourceGeometryRevision == snapshot.geometryRevision
    && snapshot.mesh->meshRevision == snapshot.meshRevision;
}

bool ProjectState::hasExportableCurrentMesh() const noexcept
{
  return meshState_ == MeshState::Ready
    && currentMesh_ != nullptr
    && meshRevision_ > 0
    && currentMesh_->meshRevision == meshRevision_
    && currentMesh_->sourceGeometryRevision == geometryRevision_;
}

void ProjectState::commitImportedGeometry()
{
  ++geometryRevision_;
  meshState_ = currentMesh_ ? MeshState::Stale : MeshState::Missing;
}

void ProjectState::commitGeometryChange()
{
  ++geometryRevision_;
  if (currentMesh_)
  {
    meshState_ = MeshState::Stale;
  }
}

bool ProjectState::commitMesh(MeshModel workerMesh)
{
  const MeshValidationReport report =
    validateMeshModel(workerMesh, MeshValidationMode::WorkerArtifact);
  if (!report.valid()
    || workerMesh.sourceGeometryRevision != geometryRevision_
    || meshRevision_ == std::numeric_limits<MeshRevision>::max())
  {
    return false;
  }

  const MeshRevision nextRevision = meshRevision_ + 1;
  workerMesh.meshRevision = nextRevision;
  currentMesh_ = std::make_shared<MeshModel>(std::move(workerMesh));
  meshRevision_ = nextRevision;
  meshState_ = MeshState::Ready;
  return true;
}
}
