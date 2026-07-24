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

const MeshModel* ProjectState::currentMesh() const noexcept
{
  return currentMesh_ ? &*currentMesh_ : nullptr;
}

bool ProjectState::canExportMesh() const noexcept
{
  return meshState_ == MeshState::Ready
    && currentMesh_.has_value()
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
  currentMesh_ = std::move(workerMesh);
  meshRevision_ = nextRevision;
  meshState_ = MeshState::Ready;
  return true;
}
}
