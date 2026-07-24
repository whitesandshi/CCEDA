#pragma once

#include "geometry/IGeometryService.h"

#include <memory>

namespace cceda::geometry
{
class OccGeometryRepository;

class OccGeometryService final : public IGeometryService
{
  [[nodiscard]] std::shared_ptr<OccGeometryRepository> repository() const;
public:
  explicit OccGeometryService(std::shared_ptr<OccGeometryRepository> repository);

  domain::GeometryImportResult importStep(const std::filesystem::path& path) override;
  [[nodiscard]] domain::GeometryDocument currentDocument() const override;

private:
  std::shared_ptr<OccGeometryRepository> repository_;
};
}
