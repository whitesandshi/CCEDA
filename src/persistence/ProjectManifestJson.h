#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>

namespace cceda::persistence
{
struct ProjectManifestEntry
{
  QString path;
  QString role;
  std::uint64_t uncompressedBytes = 0;
  QString sha256;
};

struct ProjectManifest
{
  QString projectId;
  QString formatVersion = "0.1";
  QString createdBy;
  QString createdAt;
  QString modifiedAt;
  QString occtVersion;
  QString canonicalLengthUnit = "mm";
  QVector<ProjectManifestEntry> entries;
  QVector<QString> requiredFeatures;
};

inline constexpr qsizetype MaximumProjectManifestBytes = 2 * 1024 * 1024;
inline constexpr std::uint64_t MaximumProjectManifestEntries = 10000;
inline constexpr std::uint64_t MaximumProjectManifestEntryBytes =
  512ULL * 1024ULL * 1024ULL * 1024ULL;

enum class ProjectManifestJsonError
{
  None,
  InputTooLarge,
  InvalidJson,
  InvalidStructure,
  UnsupportedVersion,
  InvalidIdentity,
  InvalidEntry,
  DuplicateEntry,
  InvalidLimits,
};

struct ProjectManifestWriteResult
{
  QByteArray bytes;
  ProjectManifestJsonError error = ProjectManifestJsonError::None;
  [[nodiscard]] bool succeeded() const noexcept
  { return error == ProjectManifestJsonError::None && !bytes.isEmpty(); }
};

struct ProjectManifestReadResult
{
  std::optional<ProjectManifest> manifest;
  ProjectManifestJsonError error = ProjectManifestJsonError::None;
  [[nodiscard]] bool succeeded() const noexcept
  { return error == ProjectManifestJsonError::None && manifest.has_value(); }
};

[[nodiscard]] ProjectManifestWriteResult writeProjectManifestJson(
  const ProjectManifest& manifest);
[[nodiscard]] ProjectManifestJsonError validateProjectManifest(
  const ProjectManifest& manifest);
[[nodiscard]] ProjectManifestReadResult readProjectManifestJson(
  const QByteArray& bytes);
}
