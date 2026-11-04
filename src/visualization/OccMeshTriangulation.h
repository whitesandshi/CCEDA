#pragma once

#include "domain/MeshModel.h"

#include <AIS_Triangulation.hxx>
#include <Poly_Triangulation.hxx>

#include <unordered_map>
#include <vector>

namespace cceda::visualization
{
[[nodiscard]] occ::handle<Poly_Triangulation> buildOccSurfaceTriangulation(
  const domain::MeshModel& mesh);

using OccElementTriangulations = std::unordered_map<
  domain::MeshElementId, occ::handle<Poly_Triangulation>>;

[[nodiscard]] occ::handle<Poly_Triangulation> buildOccElementTriangulation(
  const domain::MeshModel& mesh,
  domain::MeshElementId elementId);

[[nodiscard]] OccElementTriangulations buildOccElementTriangulations(
  const domain::MeshModel& mesh,
  const std::vector<domain::MeshElementId>& elementIds);

[[nodiscard]] occ::handle<Poly_Triangulation> buildOccIssueTriangulation(
  const OccElementTriangulations& elementTriangulations);

void configureOccSurfacePresentation(
  const occ::handle<AIS_Triangulation>& presentation);

void configureOccElementHighlight(
  const occ::handle<AIS_Triangulation>& presentation);
}
