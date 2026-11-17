#pragma once

#include "persistence/ProjectManifestJson.h"

#include <QIODevice>
#include <QStringList>
#include <functional>
#include <memory>

namespace cceda::persistence
{
enum class ProjectResourceOpenError { None, Missing, ReadFailed };

struct ProjectResourceOpenResult
{
  std::unique_ptr<QIODevice> device;
  ProjectResourceOpenError error = ProjectResourceOpenError::None;
};

// The container adapter supplies fresh, open, binary streams of uncompressed
// bytes from one immutable package snapshot. Missing and I/O failure differ.
// It owns archive path/link/duplicate checks; this callback must not resolve
// package names directly against an untrusted filesystem directory.
using OpenProjectResource = std::function<ProjectResourceOpenResult(const QString&)>;

struct ProjectResourceVerificationOptions
{
  QStringList supportedFeatures;
  std::uint64_t maximumTotalBytes = 8ULL * 1024 * 1024 * 1024;
};

enum class ProjectResourceVerificationError
{
  None,
  InvalidManifest,
  UnsupportedFeature,
  MissingDeclaration,
  IncompleteResourcePair,
  BudgetExceeded,
  MissingResource,
  ReadFailed,
  SizeMismatch,
  HashMismatch,
};

struct ProjectResourceVerificationResult
{
  ProjectResourceVerificationError error = ProjectResourceVerificationError::None;
  QString subject; // Entry path or unsupported feature ID on failure.
  QStringList missingOptionalLogs; // Populated only on complete success.
  [[nodiscard]] bool succeeded() const noexcept
  { return error == ProjectResourceVerificationError::None; }
};

// Verifies declared resources only. Project identity, geometry/mesh revisions,
// payload decoding, and final application to live state remain caller duties.
[[nodiscard]] ProjectResourceVerificationResult verifyProjectResources(
  const ProjectManifest& manifest, const OpenProjectResource& openResource,
  const ProjectResourceVerificationOptions& options = {});
}
