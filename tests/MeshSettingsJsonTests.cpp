#include "persistence/MeshSettingsJson.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <limits>

namespace
{
int failures = 0;
void expect(bool condition, const char* message)
{
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
}

int main()
{
  using namespace cceda;
  using namespace persistence;
  StoredMeshSettings settings;
  settings.mode = domain::MeshGenerationMode::ConformalMultiRegion;
  settings.generation = {0.125, 12.5, 32, true, 8, domain::MeshGenerationAlgorithm::Hxt};
  settings.resourceLimits = {86'400'000, 274'877'906'944};
  settings.retry = {true, 2};
  const auto written = writeMeshSettingsJson(settings);
  expect(written.succeeded(), "configured settings serialize");
  const auto read = readMeshSettingsJson(written.bytes);
  expect(read.succeeded(), "configured settings deserialize");
  if (read.settings)
  {
    const auto& restored = *read.settings;
    expect(restored.mode == settings.mode && restored.generation.algorithm == settings.generation.algorithm,
      "mode and algorithm are preserved");
    expect(restored.generation.minimumSizeMm == 0.125 && restored.generation.maximumSizeMm == 12.5
      && restored.generation.curvatureElementsPerCircle == 32 && restored.generation.optimizeTetrahedra
      && restored.generation.maximumThreads == 8, "all generation fields are preserved");
    expect(restored.resourceLimits.maximumElapsedMilliseconds == 86'400'000
      && restored.resourceLimits.maximumWorkingSetBytes == 274'877'906'944
      && restored.retry.automaticRetry && restored.retry.maximumAttempts == 2,
      "large integer limits and retry policy are preserved");
    expect(writeMeshSettingsJson(restored).bytes == written.bytes, "roundtrip serialization is deterministic");
  }
  const auto defaultBytes = writeMeshSettingsJson({});
  const auto defaults = readMeshSettingsJson(defaultBytes.bytes);
  expect(defaults.succeeded() && defaults.settings->resourceLimits.maximumWorkingSetBytes == 0
    && defaults.settings->generation.maximumSizeMm == 0 && !defaults.settings->retry.automaticRetry
    && defaults.settings->mode == domain::MeshGenerationMode::IndependentSolids
    && defaults.settings->generation.algorithm == domain::MeshGenerationAlgorithm::Delaunay,
    "explicit zero and disabled values survive roundtrip");

  const auto root = QJsonDocument::fromJson(written.bytes).object();
  for (const auto& name : root.keys())
  {
    auto missing = root;
    missing.remove(name);
    expect(!readMeshSettingsJson(QJsonDocument(missing).toJson()).succeeded(), "missing root fields are rejected");
  }
  for (const auto& section : {"generation", "resourceLimits", "retry", "schema"})
  {
    const auto nested = root.value(section).toObject();
    for (const auto& name : nested.keys())
    {
      auto missing = nested;
      missing.remove(name);
      auto changed = root;
      changed.insert(section, missing);
      expect(!readMeshSettingsJson(QJsonDocument(changed).toJson()).succeeded(), "missing nested fields are rejected");
    }
  }
  const auto rejects = [&](const char* section, const char* field, QJsonValue value) {
    auto changed = root;
    auto nested = root.value(section).toObject();
    nested.insert(field, value);
    changed.insert(section, nested);
    const auto result = readMeshSettingsJson(QJsonDocument(changed).toJson());
    expect(!result.succeeded() && !result.settings, "invalid stored field fails without partial settings");
  };
  rejects("generation", "maximumThreads", 1.5);
  rejects("generation", "maximumThreads", -1);
  rejects("generation", "maximumThreads", 257);
  rejects("generation", "maximumThreads", "8");
  rejects("generation", "maximumThreads", true);
  rejects("generation", "optimizeTetrahedra", 1);
  rejects("generation", "minimumSizeMm", 13);
  rejects("generation", "maximumSizeMm", 1e7);
  rejects("generation", "algorithm", "unknown");
  rejects("generation", "extra", true);
  rejects("resourceLimits", "maximumWorkingSetBytes", 274877906945.0);
  rejects("resourceLimits", "maximumElapsedMilliseconds", 86'400'001);
  rejects("retry", "maximumAttempts", 0);
  rejects("retry", "maximumAttempts", 3);
  rejects("retry", "automaticRetry", "false");
  rejects("schema", "major", 1);
  rejects("schema", "minor", 2);
  rejects("schema", "minor", 1.5);
  rejects("schema", "name", "wrong");
  for (const auto& pair : {std::pair{"mode", "other"}, std::pair{"canonicalUnit", "m"}})
  {
    auto changed = root;
    changed.insert(pair.first, pair.second);
    expect(!readMeshSettingsJson(QJsonDocument(changed).toJson()).succeeded(), "unknown mode or units are rejected");
  }
  QByteArray duplicate = written.bytes;
  duplicate.insert(1, "\"mode\":\"independent-solids\",");
  expect(!readMeshSettingsJson(duplicate).succeeded(), "duplicate members cannot silently overwrite values");
  duplicate = written.bytes;
  duplicate.replace("\"maximumThreads\":8", "\"maximumThreads\":1,\"maximumThreads\":8");
  expect(!readMeshSettingsJson(duplicate).succeeded(), "nested duplicates are rejected");
  expect(!readMeshSettingsJson("{}").succeeded(), "empty objects cannot load implicit defaults");
  expect(!readMeshSettingsJson("[]").succeeded(), "array roots are rejected");
  expect(!readMeshSettingsJson(written.bytes + "x").succeeded(), "trailing garbage is rejected");
  expect(!readMeshSettingsJson(QByteArray("\xEF\xBB\xBF") + written.bytes).succeeded(), "BOM is rejected");
  expect(!readMeshSettingsJson(QByteArray(1, char(0xff)) + written.bytes).succeeded(), "invalid UTF8 is rejected");
  expect(!readMeshSettingsJson("[[[[[[]]]]]]").succeeded(), "excessive nesting is rejected");
  expect(readMeshSettingsJson(QByteArray(MaximumMeshSettingsJsonBytes + 1, ' ')).error
    == MeshSettingsJsonError::InputTooLarge, "byte budget is enforced before parsing");
  auto invalid = settings;
  invalid.generation.minimumSizeMm = std::numeric_limits<double>::quiet_NaN();
  expect(!writeMeshSettingsJson(invalid).succeeded(), "writer rejects non-finite settings");
  invalid = settings;
  invalid.mode = static_cast<domain::MeshGenerationMode>(99);
  expect(!writeMeshSettingsJson(invalid).succeeded(), "writer rejects unknown modes");
  invalid = settings;
  invalid.retry.maximumAttempts = 3;
  expect(!writeMeshSettingsJson(invalid).succeeded(), "writer reuses domain retry validation");
  return failures == 0 ? 0 : 1;
}
