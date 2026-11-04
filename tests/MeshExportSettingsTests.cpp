#include "domain/MeshExportSettings.h"

#include <cstdlib>
#include <iostream>
#include <string>
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
}

int main()
{
  using namespace cceda::domain;

  MeshExportSettings settings;
  expect(!isValidMeshExportSettings(settings),
    "export settings have no implicit defaults");

  settings.formatVersion = MeshExportFormatVersion{4, 1};
  settings.regionPolicyId = "physical-groups";
  expect(!isValidMeshExportSettings(settings),
    "an explicit encoding is required");

  settings.encoding = MeshExportEncoding::Ascii;
  expect(isValidMeshExportSettings(settings),
    "explicit MSH 4.1 ASCII settings are structurally valid");
  settings.encoding = MeshExportEncoding::Binary;
  expect(isValidMeshExportSettings(settings),
    "explicit binary settings are structurally valid");

  settings.formatVersion = MeshExportFormatVersion{0, 1};
  expect(!isValidMeshExportSettings(settings),
    "a zero format major version is rejected");
  settings.formatVersion.reset();
  expect(!isValidMeshExportSettings(settings),
    "a missing format version is rejected");

  settings.formatVersion = MeshExportFormatVersion{4, 1};
  settings.encoding = static_cast<MeshExportEncoding>(255);
  expect(!isValidMeshExportSettings(settings),
    "a forged encoding enum is rejected");
  settings.encoding = MeshExportEncoding::Ascii;

  settings.formatVersion = MeshExportFormatVersion{99, 99};
  settings.regionPolicyId = std::string(MaximumMeshExportPolicyIdLength, 'a');
  expect(isValidMeshExportSettings(settings),
    "structural validation leaves supported versions and policies to the exporter");

  settings.formatVersion = MeshExportFormatVersion{4, 1};
  for (const std::string_view invalidPolicy : {
         "", "Physical-Groups", "physical groups", "physical/groups",
         ".physical", "physical-", "physical..groups"})
  {
    settings.regionPolicyId = invalidPolicy;
    expect(!isValidMeshExportSettings(settings),
      "an unsafe or non-canonical region policy ID is rejected");
  }

  settings.regionPolicyId = std::string(MaximumMeshExportPolicyIdLength + 1, 'a');
  expect(!isValidMeshExportSettings(settings),
    "an oversized region policy ID is rejected");
  settings.regionPolicyId = "region-ids.v1";
  expect(isValidMeshExportSettings(settings),
    "a bounded canonical region policy ID is structurally valid");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
