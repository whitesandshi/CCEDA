#include "mesh_quality/MeshQualityColoring.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
int failures = 0;

void expect(bool condition, std::string_view message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

cceda::mesh_quality::MeshQualityThresholdProfile profile()
{
  using namespace cceda::mesh_quality;
  return {
    "inspection-v1",
    {
      {MeshQualityMetric::TriangleMinimumAngle, {20.0, 10.0}},
      {MeshQualityMetric::TriangleAspectRatio, {2.0, 5.0}},
    },
  };
}

void validatesExplicitDirectionAwareThresholds()
{
  using namespace cceda::mesh_quality;
  expect(validateThresholdProfile(profile()),
    "a named profile with ordered finite thresholds should validate");
  expect(classifyMetric(MeshQualityMetric::TriangleMinimumAngle, 25.0, {},
      profile()) == MeshQualityBand::Good,
    "higher-is-better values above the warning boundary should be good");
  expect(classifyMetric(MeshQualityMetric::TriangleMinimumAngle, 15.0, {},
      profile()) == MeshQualityBand::Warning,
    "higher-is-better values between boundaries should warn");
  expect(classifyMetric(MeshQualityMetric::TriangleMinimumAngle, 5.0, {},
      profile()) == MeshQualityBand::Critical,
    "higher-is-better values below the critical boundary should be critical");
  expect(classifyMetric(MeshQualityMetric::TriangleAspectRatio, 1.5, {},
      profile()) == MeshQualityBand::Good,
    "lower-is-better values below the warning boundary should be good");
  expect(classifyMetric(MeshQualityMetric::TriangleAspectRatio, 3.0, {},
      profile()) == MeshQualityBand::Warning,
    "lower-is-better values between boundaries should warn");
  expect(classifyMetric(MeshQualityMetric::TriangleAspectRatio, 7.0, {},
      profile()) == MeshQualityBand::Critical,
    "lower-is-better values above the critical boundary should be critical");
}

void failsClosedForInvalidOrMissingPolicy()
{
  using namespace cceda::mesh_quality;
  MeshQualityThresholdProfile duplicate = profile();
  duplicate.entries.push_back(duplicate.entries.front());
  expect(!validateThresholdProfile(duplicate),
    "duplicate metric entries should invalidate a profile");

  MeshQualityThresholdProfile reversed = profile();
  reversed.entries.front().thresholds = {10.0, 20.0};
  expect(!validateThresholdProfile(reversed),
    "reversed higher-is-better boundaries should invalidate a profile");
  expect(classifyMetric(MeshQualityMetric::TriangleMaximumAngle, 60.0, {},
      profile()) == MeshQualityBand::Unavailable,
    "metrics without explicit thresholds should be unavailable");
  expect(classifyMetric(MeshQualityMetric::TriangleMinimumAngle,
      std::nullopt, {}, profile()) == MeshQualityBand::Unavailable,
    "missing numeric values should be unavailable");
  expect(classifyMetric(MeshQualityMetric::TriangleNormalConsistent,
      {}, false, profile()) == MeshQualityBand::Critical,
    "false required booleans should be critical");
  expect(classifyMetric(MeshQualityMetric::TriangleNormalConsistent,
      {}, true, profile()) == MeshQualityBand::Good,
    "true required booleans should be good");
  expect(classifyMetric(MeshQualityMetric::TetrahedronSignedVolume,
      0.1, {}, profile()) == MeshQualityBand::Good,
    "positive-required values should be good without a numeric policy");
  expect(classifyMetric(MeshQualityMetric::TetrahedronSignedVolume,
      -0.1, {}, profile()) == MeshQualityBand::Critical,
    "non-positive required values should be critical");
  expect(classifyMetric(static_cast<MeshQualityMetric>(99), 1.0, {}, profile())
      == MeshQualityBand::Unavailable,
    "unknown metric identifiers should fail closed");
}

void colorsAreStable()
{
  using namespace cceda::mesh_quality;
  expect(colorForBand(MeshQualityBand::Good).green
      > colorForBand(MeshQualityBand::Good).red,
    "good band should use a green-dominant color");
  expect(colorForBand(MeshQualityBand::Warning).red
      > colorForBand(MeshQualityBand::Warning).green,
    "warning band should use an amber color");
  expect(colorForBand(MeshQualityBand::Unavailable).red
      == colorForBand(MeshQualityBand::Unavailable).green
      && colorForBand(MeshQualityBand::Unavailable).green
        == colorForBand(MeshQualityBand::Unavailable).blue,
    "unavailable band should use a neutral color");
}
}

int main()
{
  validatesExplicitDirectionAwareThresholds();
  failsClosedForInvalidOrMissingPolicy();
  colorsAreStable();
  if (failures == 0)
  {
    std::cout << "All mesh quality coloring tests passed.\n";
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
