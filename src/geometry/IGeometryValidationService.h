#pragma once

#include "domain/GeometryValidation.h"

namespace cceda::geometry
{
class IGeometryValidationService
{
public:
  virtual ~IGeometryValidationService() = default;

  [[nodiscard]] virtual domain::SolidBrepCheckResult checkSolidBrep(
    domain::GeometryRevision revision, const std::string& targetId) const = 0;

  // Completed reports contain one outcome per indexed Solid in document order.
  // Compare the report revision with the current geometry before presenting it.
  [[nodiscard]] virtual domain::GeometryValidationReport checkDocumentBrep(
    domain::GeometryRevision revision) const = 0;
};
}
