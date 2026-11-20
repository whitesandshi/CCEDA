#pragma once

#include "geometry/IGeometryValidationService.h"

#include <memory>

namespace cceda::geometry
{
class OccGeometryRepository;

class OccGeometryValidationService final : public IGeometryValidationService
{
public:
  explicit OccGeometryValidationService(std::shared_ptr<const OccGeometryRepository> repository);

  [[nodiscard]] domain::SolidBrepCheckResult checkSolidBrep(
    domain::GeometryRevision revision, const std::string& targetId) const override;

  [[nodiscard]] domain::GeometryValidationReport checkDocumentBrep(
    domain::GeometryRevision revision) const override;

private:
  std::shared_ptr<const OccGeometryRepository> repository_;
};
}
