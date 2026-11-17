#pragma once

#include "persistence/ProjectResourceVerification.h"

namespace cceda::persistence
{
namespace detail { struct ProjectZipState; }
class ProjectZipReader;

struct ProjectZipOptions
{
  ProjectResourceVerificationOptions resources;
  std::uint64_t maximumArchiveBytes = 9ULL * 1024 * 1024 * 1024;
  std::uint32_t maximumCompressionRatio = 2000;
};

enum class ProjectZipError
{
  None,
  InvalidPath,
  OpenFailed,
  InvalidArchive,
  LimitsExceeded,
  UnsafeEntry,
  InvalidManifest,
  ResourceVerificationFailed,
  WriteFailed,
};

struct ProjectZipResult
{
  ProjectZipError error = ProjectZipError::None;
  QString subject;
  ProjectResourceVerificationError resourceError = ProjectResourceVerificationError::None;
  [[nodiscard]] bool succeeded() const noexcept { return error == ProjectZipError::None; }
};

struct ProjectZipOpenResult
{
  std::shared_ptr<ProjectZipReader> archive;
  ProjectZipResult status;
  [[nodiscard]] bool succeeded() const noexcept { return archive && status.succeeded(); }
};

// Single-thread use. Open fully verifies the package. Resource devices retain
// the locked archive handle even after the reader is destroyed. No extraction.
class ProjectZipReader final
{
public:
  [[nodiscard]] static ProjectZipOpenResult open(
    const QString& path, const ProjectZipOptions& options = {});
  [[nodiscard]] const ProjectManifest& manifest() const noexcept;
  [[nodiscard]] const QStringList& missingOptionalLogs() const noexcept;
  [[nodiscard]] ProjectResourceOpenResult openResource(const QString& path) const;
private:
  friend ProjectZipResult writeNewProjectZip(const QString&, const ProjectManifest&,
    const OpenProjectResource&, const ProjectZipOptions&);
  [[nodiscard]] static ProjectZipOpenResult openInternal(
    const QString& path, const ProjectZipOptions& options, bool guardedWriter);
  explicit ProjectZipReader(std::shared_ptr<detail::ProjectZipState> state);
  std::shared_ptr<detail::ProjectZipState> state_;
};

// Creates an exclusively owned new .cceda staging file, never overwrites a
// destination. Manifest hashes/sizes describe immutable caller-owned resources.
// Writes manifest last, closes/reopens, and verifies before returning success.
// Removes only its own incomplete file on failure. No live-state publication.
[[nodiscard]] ProjectZipResult writeNewProjectZip(
  const QString& stagingPath, const ProjectManifest& manifest,
  const OpenProjectResource& openResource, const ProjectZipOptions& options = {});
}
