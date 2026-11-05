#pragma once

#include "mesh_quality/MeshQuality.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cceda::mesh_quality
{
// Thresholds are explicit policy input. The evaluator deliberately has no
// built-in warning values, so a color view cannot silently change quality
// semantics across releases.
struct MeshQualityThresholds
{
  double warningBoundary = 0.0;
  double criticalBoundary = 0.0;
};

struct MeshQualityThresholdEntry
{
  MeshQualityMetric metric = MeshQualityMetric::TriangleArea;
  MeshQualityThresholds thresholds;
};

struct MeshQualityThresholdProfile
{
  std::string id;
  std::vector<MeshQualityThresholdEntry> entries;
};

enum class MeshQualityBand
{
  Good,
  Warning,
  Critical,
  Unavailable,
};

struct MeshQualityColor
{
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
};

[[nodiscard]] bool validateThresholdProfile(
  const MeshQualityThresholdProfile& profile) noexcept;

[[nodiscard]] std::optional<MeshQualityThresholds> thresholdFor(
  const MeshQualityThresholdProfile& profile,
  MeshQualityMetric metric) noexcept;

// Numeric metrics use the supplied direction-aware thresholds. Boolean
// metrics use true=Good and false=Critical. Positive-required metrics use a
// strictly positive finite value as Good. Missing or non-finite values are
// Unavailable so the caller can render them distinctly.
[[nodiscard]] MeshQualityBand classifyMetric(
  MeshQualityMetric metric,
  const std::optional<double>& numericValue,
  const std::optional<bool>& booleanValue,
  const MeshQualityThresholdProfile& profile) noexcept;

[[nodiscard]] MeshQualityColor colorForBand(MeshQualityBand band) noexcept;
}
