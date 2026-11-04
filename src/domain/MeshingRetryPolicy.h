#pragma once

#include <cstdint>

namespace cceda::domain
{
inline constexpr std::uint32_t MaximumAutomaticMeshingAttempts = 2;

struct MeshingRetryPolicy
{
  bool automaticRetry = false;
  std::uint32_t maximumAttempts = MaximumAutomaticMeshingAttempts;
};

[[nodiscard]] constexpr bool isValidMeshingRetryPolicy(
  const MeshingRetryPolicy& policy) noexcept
{
  return policy.maximumAttempts >= 1
    && policy.maximumAttempts <= MaximumAutomaticMeshingAttempts;
}
}
