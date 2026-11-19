#pragma once

#include "domain/GeometryDocument.h"

#include <filesystem>
#include <memory>

namespace cceda::geometry
{
class IGeometryService
{
public:
  virtual ~IGeometryService() = default;
  virtual domain::GeometryImportResult importStep(const std::filesystem::path& path,
    const std::string& sourceName = {}) = 0;
  [[nodiscard]] virtual domain::GeometryDocument currentDocument() const = 0;
};

std::shared_ptr<IGeometryService> createOccGeometryService();
}
