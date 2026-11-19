#pragma once

#include "domain/GeometryDocument.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace cceda::geometry
{
struct PreparedProjectGeometry
{
  virtual ~PreparedProjectGeometry() = default;
  domain::GeometryDocument document;
};

class IProjectGeometryStorage
{
public:
  virtual ~IProjectGeometryStorage() = default;
  [[nodiscard]] virtual std::string writerVersion() const = 0;
  [[nodiscard]] virtual std::optional<domain::GeometryDocument> writeXcaf(domain::GeometryRevision revision,
    const std::filesystem::path& path) const = 0;
  [[nodiscard]] virtual std::shared_ptr<PreparedProjectGeometry> prepareXcaf(
    const std::filesystem::path& path, domain::GeometryRevision revision,
    const std::string& sourceName) const = 0;

  // Publish only a successfully prepared value from this adapter, or clear the
  // repository for an empty project. The workbench serializes state changes.
  virtual void activate(std::shared_ptr<PreparedProjectGeometry> prepared) = 0;
};
}
