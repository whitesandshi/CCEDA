#pragma once

#include "geometry/IGeometrySnapshotService.h"

#include <memory>

namespace cceda::geometry
{
class OccGeometryRepository;

class OccGeometrySnapshotService final : public IGeometrySnapshotService
{
public:
  explicit OccGeometrySnapshotService(
    std::shared_ptr<OccGeometryRepository> repository);

  [[nodiscard]] GeometrySnapshotResult createSnapshot(
    const GeometrySnapshotRequest& request) const override;

private:
  std::shared_ptr<OccGeometryRepository> repository_;
};
}
