#pragma once

#include <cstdint>

namespace cceda::domain
{
inline constexpr std::uint64_t MaximumMeshingElapsedMilliseconds =
  24ULL * 60ULL * 60ULL * 1000ULL;
inline constexpr std::uint64_t MaximumMeshingWorkingSetBytes =
  256ULL * 1024ULL * 1024ULL * 1024ULL;

struct MeshingResourceLimits
{
  // Zero disables the corresponding explicitly configured limit.
  std::uint64_t maximumElapsedMilliseconds = 0;
  std::uint64_t maximumWorkingSetBytes = 0;
};

[[nodiscard]] constexpr bool isValidMeshingResourceLimits(
  const MeshingResourceLimits& limits) noexcept
{
  return limits.maximumElapsedMilliseconds
      <= MaximumMeshingElapsedMilliseconds
    && limits.maximumWorkingSetBytes <= MaximumMeshingWorkingSetBytes;
}
}
