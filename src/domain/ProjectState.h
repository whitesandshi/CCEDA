#pragma once

#include "domain/MeshModel.h"
#include "domain/Revisions.h"

#include <optional>

namespace cceda::domain
{
enum class MeshState
{
  Missing,
  Ready,
  Stale,
  Invalid,
};

class ProjectState
{
public:
  [[nodiscard]] GeometryRevision geometryRevision() const noexcept;
  [[nodiscard]] MeshRevision meshRevision() const noexcept;
  [[nodiscard]] MeshState meshState() const noexcept;
  [[nodiscard]] const MeshModel* currentMesh() const noexcept;
  [[nodiscard]] bool canExportMesh() const noexcept;

  void commitImportedGeometry();
  void commitGeometryChange();
  [[nodiscard]] bool commitMesh(MeshModel workerMesh);

private:
  GeometryRevision geometryRevision_ = 0;
  MeshRevision meshRevision_ = 0;
  MeshState meshState_ = MeshState::Missing;
  std::optional<MeshModel> currentMesh_;
};
}
