#pragma once

#include <cmath>
#include <cstdint>

namespace cceda::domain
{
inline constexpr double MaximumMeshSizeMm = 1'000'000.0;
inline constexpr std::uint32_t MaximumCurvatureElementsPerCircle = 1000;
inline constexpr std::uint32_t MaximumMeshingThreads = 256;

enum class MeshGenerationAlgorithm : std::uint8_t
{
  Delaunay = 1,
  Hxt = 2,
};

enum class MeshGenerationMode : std::uint8_t
{
  IndependentSolids = 1,
  ConformalMultiRegion = 2,
};

[[nodiscard]] inline bool isValidMeshGenerationMode(
  MeshGenerationMode mode) noexcept
{
  return mode == MeshGenerationMode::IndependentSolids
    || mode == MeshGenerationMode::ConformalMultiRegion;
}

struct MeshGenerationSettings
{
  double minimumSizeMm = 0.0;
  double maximumSizeMm = 0.0;
  std::uint32_t curvatureElementsPerCircle = 0;
  bool optimizeTetrahedra = false;
  std::uint32_t maximumThreads = 0;
  MeshGenerationAlgorithm algorithm = MeshGenerationAlgorithm::Delaunay;
};

[[nodiscard]] inline bool isValidMeshGenerationSettings(
  const MeshGenerationSettings& settings) noexcept
{
  return std::isfinite(settings.minimumSizeMm)
    && std::isfinite(settings.maximumSizeMm)
    && settings.minimumSizeMm >= 0.0
    && settings.minimumSizeMm <= MaximumMeshSizeMm
    && settings.maximumSizeMm >= 0.0
    && settings.maximumSizeMm <= MaximumMeshSizeMm
    && (settings.maximumSizeMm == 0.0
      || settings.minimumSizeMm <= settings.maximumSizeMm)
    && settings.curvatureElementsPerCircle
      <= MaximumCurvatureElementsPerCircle
    && settings.maximumThreads <= MaximumMeshingThreads
    && (settings.algorithm == MeshGenerationAlgorithm::Delaunay
      || settings.algorithm == MeshGenerationAlgorithm::Hxt);
}
}
