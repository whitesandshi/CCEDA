#pragma once

#include "geometry/IGeometryService.h"
#include "geometry/IProjectGeometryStorage.h"

#include <memory>

namespace cceda::geometry
{
class OccGeometryRepository;

class OccGeometryService final : public IGeometryService, public IProjectGeometryStorage
{
  [[nodiscard]] std::shared_ptr<OccGeometryRepository> repository() const;
public:
  explicit OccGeometryService(std::shared_ptr<OccGeometryRepository> repository);

  domain::GeometryImportResult importStep(const std::filesystem::path& path,
    const std::string& sourceName = {}) override;
  [[nodiscard]] domain::GeometryDocument currentDocument() const override;

  [[nodiscard]] std::string writerVersion() const override;
  [[nodiscard]] std::optional<domain::GeometryDocument> writeXcaf(domain::GeometryRevision revision,
    const std::filesystem::path& path) const override;
  [[nodiscard]] std::shared_ptr<PreparedProjectGeometry> prepareXcaf(
    const std::filesystem::path& path, domain::GeometryRevision revision,
    const std::string& sourceName) const override;
  void activate(std::shared_ptr<PreparedProjectGeometry> prepared) override;

private:
  std::shared_ptr<OccGeometryRepository> repository_;
};
}
