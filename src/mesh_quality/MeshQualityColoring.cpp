#include "mesh_quality/MeshQualityColoring.h"

#include <cmath>

namespace cceda::mesh_quality
{
namespace
{
bool hasThresholdsForDirection(
  MeshQualityDirection direction,
  const MeshQualityThresholds& thresholds) noexcept
{
  if (!std::isfinite(thresholds.warningBoundary)
      || !std::isfinite(thresholds.criticalBoundary))
  {
    return false;
  }

  switch (direction)
  {
    case MeshQualityDirection::HigherIsBetter:
      return thresholds.criticalBoundary < thresholds.warningBoundary;
    case MeshQualityDirection::LowerIsBetter:
      return thresholds.warningBoundary < thresholds.criticalBoundary;
    case MeshQualityDirection::PositiveRequired:
    case MeshQualityDirection::TrueRequired:
      return true;
  }
  return false;
}
}

bool validateThresholdProfile(
  const MeshQualityThresholdProfile& profile) noexcept
{
  if (profile.id.empty())
  {
    return false;
  }

  for (std::size_t index = 0; index < profile.entries.size(); ++index)
  {
    const MeshQualityThresholdEntry& entry = profile.entries.at(index);
    if (stableMetricId(entry.metric).empty())
    {
      return false;
    }
    const MeshQualityDirection direction = metricDirection(entry.metric);
    if (!hasThresholdsForDirection(direction, entry.thresholds))
    {
      return false;
    }
    for (std::size_t previous = 0; previous < index; ++previous)
    {
      if (profile.entries.at(previous).metric == entry.metric)
      {
        return false;
      }
    }
  }
  return true;
}

std::optional<MeshQualityThresholds> thresholdFor(
  const MeshQualityThresholdProfile& profile,
  MeshQualityMetric metric) noexcept
{
  for (const MeshQualityThresholdEntry& entry : profile.entries)
  {
    if (entry.metric == metric)
    {
      return entry.thresholds;
    }
  }
  return std::nullopt;
}

MeshQualityBand classifyMetric(
  MeshQualityMetric metric,
  const std::optional<double>& numericValue,
  const std::optional<bool>& booleanValue,
  const MeshQualityThresholdProfile& profile) noexcept
{
  if (stableMetricId(metric).empty())
  {
    return MeshQualityBand::Unavailable;
  }
  const MeshQualityDirection direction = metricDirection(metric);
  if (direction == MeshQualityDirection::TrueRequired)
  {
    return booleanValue && *booleanValue
      ? MeshQualityBand::Good : MeshQualityBand::Critical;
  }
  if (!numericValue || !std::isfinite(*numericValue))
  {
    return MeshQualityBand::Unavailable;
  }
  if (direction == MeshQualityDirection::PositiveRequired)
  {
    return *numericValue > 0.0
      ? MeshQualityBand::Good : MeshQualityBand::Critical;
  }

  const std::optional<MeshQualityThresholds> thresholds =
    thresholdFor(profile, metric);
  if (!thresholds || !validateThresholdProfile(profile))
  {
    return MeshQualityBand::Unavailable;
  }
  if (direction == MeshQualityDirection::HigherIsBetter)
  {
    if (*numericValue >= thresholds->warningBoundary)
    {
      return MeshQualityBand::Good;
    }
    if (*numericValue >= thresholds->criticalBoundary)
    {
      return MeshQualityBand::Warning;
    }
    return MeshQualityBand::Critical;
  }

  if (*numericValue <= thresholds->warningBoundary)
  {
    return MeshQualityBand::Good;
  }
  if (*numericValue <= thresholds->criticalBoundary)
  {
    return MeshQualityBand::Warning;
  }
  return MeshQualityBand::Critical;
}

MeshQualityColor colorForBand(MeshQualityBand band) noexcept
{
  switch (band)
  {
    case MeshQualityBand::Good:
      return {46, 160, 67};
    case MeshQualityBand::Warning:
      return {235, 170, 35};
    case MeshQualityBand::Critical:
      return {204, 54, 54};
    case MeshQualityBand::Unavailable:
      return {128, 128, 128};
  }
  return {128, 128, 128};
}
}
