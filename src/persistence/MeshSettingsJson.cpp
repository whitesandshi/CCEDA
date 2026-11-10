#include "persistence/MeshSettingsJson.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>
#include <cmath>
#include <initializer_list>

namespace cceda::persistence
{
namespace
{
bool valid(const StoredMeshSettings& settings)
{
  return domain::isValidMeshGenerationMode(settings.mode)
    && domain::isValidMeshGenerationSettings(settings.generation)
    && domain::isValidMeshingResourceLimits(settings.resourceLimits)
    && domain::isValidMeshingRetryPolicy(settings.retry);
}

bool keys(const QJsonObject& object, std::initializer_list<const char*> names)
{
  if (object.size() != static_cast<qsizetype>(names.size())) { return false; }
  for (const char* name : names)
  {
    if (!object.contains(QLatin1String(name))) { return false; }
  }
  return true;
}

bool integer(const QJsonValue& value, double maximum)
{
  const double number = value.toDouble(-1);
  return value.isDouble() && std::isfinite(number)
    && number >= 0 && number <= maximum && std::floor(number) == number;
}

// Count all object members before Qt collapses duplicate keys. The sealed
// schema has a fixed total, so duplicates cannot be hidden by parsing.
bool boundedStructure(const QByteArray& bytes, qsizetype& memberCount)
{
  int depth = 0;
  bool quoted = false;
  bool escaped = false;
  for (const char byte : bytes)
  {
    if (quoted)
    {
      if (escaped) { escaped = false; }
      else if (byte == '\\') { escaped = true; }
      else if (byte == '"') { quoted = false; }
      continue;
    }
    if (byte == '"') { quoted = true; }
    else if (byte == '{' || byte == '[')
    {
      if (++depth > 4) { return false; }
    }
    else if (byte == '}' || byte == ']') { --depth; }
    else if (byte == ':') { ++memberCount; }
  }
  return true;
}
}

MeshSettingsWriteResult writeMeshSettingsJson(const StoredMeshSettings& settings)
{
  if (!valid(settings)) { return {{}, MeshSettingsJsonError::InvalidSettings}; }
  const auto& generation = settings.generation;
  const QJsonObject root{
    {"schema", QJsonObject{{"name", "cceda.mesh-settings"}, {"major", 0}, {"minor", 1}}},
    {"canonicalUnit", "mm"},
    {"mode", settings.mode == domain::MeshGenerationMode::IndependentSolids
      ? "independent-solids" : "conformal-multi-region"},
    {"generation", QJsonObject{
      {"minimumSizeMm", generation.minimumSizeMm},
      {"maximumSizeMm", generation.maximumSizeMm},
      {"curvatureElementsPerCircle", static_cast<qint64>(generation.curvatureElementsPerCircle)},
      {"optimizeTetrahedra", generation.optimizeTetrahedra},
      {"maximumThreads", static_cast<qint64>(generation.maximumThreads)},
      {"algorithm", generation.algorithm == domain::MeshGenerationAlgorithm::Delaunay
        ? "delaunay" : "hxt"}}},
    {"resourceLimits", QJsonObject{
      {"maximumElapsedMilliseconds", static_cast<qint64>(settings.resourceLimits.maximumElapsedMilliseconds)},
      {"maximumWorkingSetBytes", static_cast<qint64>(settings.resourceLimits.maximumWorkingSetBytes)}}},
    {"retry", QJsonObject{{"automaticRetry", settings.retry.automaticRetry},
      {"maximumAttempts", static_cast<qint64>(settings.retry.maximumAttempts)}}},
  };
  return {QJsonDocument(root).toJson(QJsonDocument::Compact), MeshSettingsJsonError::None};
}

MeshSettingsReadResult readMeshSettingsJson(const QByteArray& bytes)
{
  const auto fail = [](MeshSettingsJsonError error) { return MeshSettingsReadResult{{}, error}; };
  if (bytes.size() > MaximumMeshSettingsJsonBytes) { return fail(MeshSettingsJsonError::InputTooLarge); }
  QStringDecoder decoder(QStringDecoder::Utf8);
  const QString decoded = decoder.decode(bytes);
  (void)decoded;
  qsizetype memberCount = 0;
  if (decoder.hasError() || bytes.startsWith("\xEF\xBB\xBF") || !boundedStructure(bytes, memberCount))
  { return fail(MeshSettingsJsonError::InvalidJson); }
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(bytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
  { return fail(MeshSettingsJsonError::InvalidJson); }
  const auto root = document.object();
  const auto schema = root.value("schema").toObject();
  const auto generation = root.value("generation").toObject();
  const auto limits = root.value("resourceLimits").toObject();
  const auto retry = root.value("retry").toObject();
  if (!keys(root, {"schema", "canonicalUnit", "mode", "generation", "resourceLimits", "retry"})
    || !keys(schema, {"name", "major", "minor"})
    || !keys(generation, {"minimumSizeMm", "maximumSizeMm", "curvatureElementsPerCircle",
      "optimizeTetrahedra", "maximumThreads", "algorithm"})
    || !keys(limits, {"maximumElapsedMilliseconds", "maximumWorkingSetBytes"})
    || !keys(retry, {"automaticRetry", "maximumAttempts"})
    || memberCount != root.size() + schema.size() + generation.size() + limits.size() + retry.size())
  { return fail(MeshSettingsJsonError::InvalidStructure); }
  if (schema.value("name").toString() != "cceda.mesh-settings"
    || !integer(schema.value("major"), 0) || !integer(schema.value("minor"), 1)
    || schema.value("minor").toDouble() != 1)
  { return fail(MeshSettingsJsonError::UnsupportedSchema); }
  const QString mode = root.value("mode").toString();
  const QString algorithm = generation.value("algorithm").toString();
  if (root.value("canonicalUnit").toString() != "mm"
    || (mode != "independent-solids" && mode != "conformal-multi-region")
    || (algorithm != "delaunay" && algorithm != "hxt")
    || !generation.value("minimumSizeMm").isDouble()
    || !generation.value("maximumSizeMm").isDouble()
    || !generation.value("optimizeTetrahedra").isBool()
    || !retry.value("automaticRetry").isBool()
    || !integer(generation.value("curvatureElementsPerCircle"), domain::MaximumCurvatureElementsPerCircle)
    || !integer(generation.value("maximumThreads"), domain::MaximumMeshingThreads)
    || !integer(limits.value("maximumElapsedMilliseconds"), domain::MaximumMeshingElapsedMilliseconds)
    || !integer(limits.value("maximumWorkingSetBytes"), domain::MaximumMeshingWorkingSetBytes)
    || !integer(retry.value("maximumAttempts"), domain::MaximumAutomaticMeshingAttempts))
  { return fail(MeshSettingsJsonError::InvalidSettings); }
  StoredMeshSettings settings;
  settings.mode = mode == "independent-solids" ? domain::MeshGenerationMode::IndependentSolids
    : domain::MeshGenerationMode::ConformalMultiRegion;
  settings.generation = {generation.value("minimumSizeMm").toDouble(),
    generation.value("maximumSizeMm").toDouble(),
    static_cast<std::uint32_t>(generation.value("curvatureElementsPerCircle").toInteger()),
    generation.value("optimizeTetrahedra").toBool(),
    static_cast<std::uint32_t>(generation.value("maximumThreads").toInteger()),
    algorithm == "delaunay" ? domain::MeshGenerationAlgorithm::Delaunay : domain::MeshGenerationAlgorithm::Hxt};
  settings.resourceLimits = {
    static_cast<std::uint64_t>(limits.value("maximumElapsedMilliseconds").toInteger()),
    static_cast<std::uint64_t>(limits.value("maximumWorkingSetBytes").toInteger())};
  settings.retry = {retry.value("automaticRetry").toBool(),
    static_cast<std::uint32_t>(retry.value("maximumAttempts").toInteger())};
  if (!valid(settings)) { return fail(MeshSettingsJsonError::InvalidSettings); }
  return {settings, MeshSettingsJsonError::None};
}
}
