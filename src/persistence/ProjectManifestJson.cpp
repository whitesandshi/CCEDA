#include "persistence/ProjectManifestJson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <algorithm>
#include <cmath>

namespace cceda::persistence
{
namespace
{
ProjectManifestReadResult failure(ProjectManifestJsonError error)
{ return {{}, error}; }

bool exactKeys(const QJsonObject& object, std::initializer_list<const char*> names)
{
  if (object.size() != static_cast<qsizetype>(names.size())) { return false; }
  for (const char* name : names)
  {
    if (!object.contains(QLatin1String(name))) { return false; }
  }
  return true;
}

bool safePath(const QString& path)
{
  if (path.isEmpty() || path.size() > 1024 || path.compare("manifest.json", Qt::CaseInsensitive) == 0
    || path.startsWith('/') || path.startsWith('\\')
    || path.contains(':') || path.contains("\\") || path.startsWith("./")
    || path.endsWith('/') || path.contains("//"))
  { return false; }
  for (const auto character : path)
  {
    if (character.unicode() < 32 || character.unicode() == 127) { return false; }
  }
  const auto parts = path.split('/');
  return std::all_of(parts.cbegin(), parts.cend(), [](const QString& part) {
    return !part.isEmpty() && part != "." && part != "..";
  });
}

bool hash(const QString& value)
{
  static const QRegularExpression pattern("\\A[0-9a-f]{64}\\z");
  return pattern.match(value).hasMatch();
}

bool id(const QString& value)
{
  static const QRegularExpression pattern(
    "\\A[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\z");
  return pattern.match(value).hasMatch();
}

bool boundedString(const QString& value)
{ return !value.isEmpty() && value.size() <= 1024; }

bool utcTimestamp(const QString& value)
{
  static const QRegularExpression pattern(
    "\\A[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]{1,3})?Z\\z");
  return pattern.match(value).hasMatch() && QDateTime::fromString(value, Qt::ISODateWithMs).isValid();
}

// This sealed schema has exactly 10 root members and four per entry.
// Count before Qt collapses duplicate keys, ignoring punctuation in strings.
bool boundedStructure(const QByteArray& bytes, qsizetype& members)
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
    }
    else if (byte == '"') { quoted = true; }
    else if (byte == '{' || byte == '[')
    {
      if (++depth > 3) { return false; }
    }
    else if (byte == '}' || byte == ']') { --depth; }
    else if (byte == ':') { ++members; }
  }
  return true;
}

bool entryValid(const ProjectManifestEntry& entry)
{
  return safePath(entry.path) && boundedString(entry.role)
    && entry.uncompressedBytes <= MaximumProjectManifestEntryBytes
    && hash(entry.sha256);
}

void normalize(ProjectManifest& manifest)
{
  std::sort(manifest.entries.begin(), manifest.entries.end(),
    [](const auto& left, const auto& right) { return left.path < right.path; });
  std::sort(manifest.requiredFeatures.begin(), manifest.requiredFeatures.end());
}
}

ProjectManifestJsonError validateProjectManifest(const ProjectManifest& manifest)
{
  if (manifest.formatVersion != "0.1") { return ProjectManifestJsonError::UnsupportedVersion; }
  if (!id(manifest.projectId) || !boundedString(manifest.createdBy)
    || !utcTimestamp(manifest.createdAt) || !utcTimestamp(manifest.modifiedAt)
    || !boundedString(manifest.occtVersion) || manifest.canonicalLengthUnit != "mm")
  { return ProjectManifestJsonError::InvalidIdentity; }
  if (manifest.entries.isEmpty()
    || manifest.entries.size() > static_cast<qsizetype>(MaximumProjectManifestEntries)
    || manifest.requiredFeatures.size() > 128)
  { return ProjectManifestJsonError::InvalidLimits; }
  QSet<QString> paths;
  for (const auto& entry : manifest.entries)
  {
    if (!entryValid(entry)) { return ProjectManifestJsonError::InvalidEntry; }
    const auto key = entry.path.toCaseFolded();
    if (paths.contains(key)) { return ProjectManifestJsonError::DuplicateEntry; }
    paths.insert(key);
  }
  QSet<QString> features;
  for (const auto& feature : manifest.requiredFeatures)
  {
    if (!boundedString(feature) || features.contains(feature))
    { return ProjectManifestJsonError::InvalidStructure; }
    features.insert(feature);
  }
  return ProjectManifestJsonError::None;
}

ProjectManifestWriteResult writeProjectManifestJson(const ProjectManifest& input)
{
  const auto error = validateProjectManifest(input);
  if (error != ProjectManifestJsonError::None) { return {{}, error}; }
  auto manifest = input;
  normalize(manifest);
  QJsonArray entries;
  for (const auto& entry : manifest.entries)
  {
    entries.append(QJsonObject{{"path", entry.path}, {"role", entry.role},
      {"uncompressedBytes", static_cast<qint64>(entry.uncompressedBytes)},
      {"sha256", entry.sha256}});
  }
  QJsonArray features;
  for (const auto& feature : manifest.requiredFeatures) { features.append(feature); }
  const QJsonObject root{
    {"format", "cceda-project"}, {"formatVersion", manifest.formatVersion},
    {"projectId", manifest.projectId}, {"createdBy", manifest.createdBy},
    {"createdAt", manifest.createdAt}, {"modifiedAt", manifest.modifiedAt},
    {"occtVersion", manifest.occtVersion}, {"canonicalLengthUnit", manifest.canonicalLengthUnit},
    {"entries", entries}, {"requiredFeatures", features},
  };
  const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
  if (bytes.size() > MaximumProjectManifestBytes) { return {{}, ProjectManifestJsonError::InputTooLarge}; }
  return {bytes, ProjectManifestJsonError::None};
}

ProjectManifestReadResult readProjectManifestJson(const QByteArray& bytes)
{
  if (bytes.size() > MaximumProjectManifestBytes) { return failure(ProjectManifestJsonError::InputTooLarge); }
  QStringDecoder decoder(QStringDecoder::Utf8);
  const QString decoded = decoder.decode(bytes);
  (void)decoded;
  qsizetype members = 0;
  if (decoder.hasError() || bytes.startsWith("\xEF\xBB\xBF") || !boundedStructure(bytes, members))
  { return failure(ProjectManifestJsonError::InvalidJson); }
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(bytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
  { return failure(ProjectManifestJsonError::InvalidJson); }
  const auto root = document.object();
  if (!exactKeys(root, {"format", "formatVersion", "projectId", "createdBy", "createdAt",
      "modifiedAt", "occtVersion", "canonicalLengthUnit", "entries", "requiredFeatures"}))
  { return failure(ProjectManifestJsonError::InvalidStructure); }
  if (root.value("format").toString() != "cceda-project" || root.value("formatVersion").toString() != "0.1")
  { return failure(ProjectManifestJsonError::UnsupportedVersion); }
  if (!root.value("entries").isArray() || !root.value("requiredFeatures").isArray())
  { return failure(ProjectManifestJsonError::InvalidStructure); }
  ProjectManifest result;
  result.projectId = root.value("projectId").toString();
  result.formatVersion = root.value("formatVersion").toString();
  result.createdBy = root.value("createdBy").toString();
  result.createdAt = root.value("createdAt").toString();
  result.modifiedAt = root.value("modifiedAt").toString();
  result.occtVersion = root.value("occtVersion").toString();
  result.canonicalLengthUnit = root.value("canonicalLengthUnit").toString();
  const auto entries = root.value("entries").toArray();
  if (members != 10 + 4 * entries.size())
  { return failure(ProjectManifestJsonError::InvalidStructure); }
  if (entries.size() > static_cast<qsizetype>(MaximumProjectManifestEntries))
  { return failure(ProjectManifestJsonError::InvalidLimits); }
  for (const auto& value : entries)
  {
    if (!value.isObject()) { return failure(ProjectManifestJsonError::InvalidEntry); }
    const auto object = value.toObject();
    if (!exactKeys(object, {"path", "role", "uncompressedBytes", "sha256"})
      || !object.value("path").isString() || !object.value("role").isString()
      || !object.value("uncompressedBytes").isDouble() || !object.value("sha256").isString())
    { return failure(ProjectManifestJsonError::InvalidEntry); }
    const double size = object.value("uncompressedBytes").toDouble(-1);
    if (size < 0 || size > MaximumProjectManifestEntryBytes || std::floor(size) != size)
    { return failure(ProjectManifestJsonError::InvalidEntry); }
    result.entries.push_back({object.value("path").toString(), object.value("role").toString(),
      static_cast<std::uint64_t>(size), object.value("sha256").toString()});
  }
  for (const auto& value : root.value("requiredFeatures").toArray())
  {
    if (!value.isString() || value.toString().isEmpty()) { return failure(ProjectManifestJsonError::InvalidStructure); }
    result.requiredFeatures.push_back(value.toString());
  }
  const auto error = validateProjectManifest(result);
  if (error != ProjectManifestJsonError::None) { return failure(error); }
  normalize(result);
  return {result, ProjectManifestJsonError::None};
}
}
