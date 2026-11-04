#pragma once

#include <filesystem>
#include <string>

namespace cceda::application
{
enum class StagedFilePublishStatus
{
  Succeeded,
  InvalidRequest,
  PublishFailed,
};

struct StagedFilePublishResult
{
  StagedFilePublishStatus status = StagedFilePublishStatus::PublishFailed;
  std::string diagnostic;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return status == StagedFilePublishStatus::Succeeded;
  }
};

[[nodiscard]] inline bool isValidStagedFilePublishRequest(
  const std::filesystem::path& stagingPath,
  const std::filesystem::path& finalPath)
{
  const auto isValidFilePath = [](const std::filesystem::path& path) {
    return !path.empty()
      && path.is_absolute()
      && path.has_parent_path()
      && path.has_filename()
      && path == path.lexically_normal();
  };

  return isValidFilePath(stagingPath)
    && isValidFilePath(finalPath)
    && stagingPath != finalPath
    && stagingPath.parent_path() == finalPath.parent_path();
}

class IStagedFilePublisher
{
public:
  virtual ~IStagedFilePublisher() = default;

  // Non-success and exceptions must leave both paths unchanged. This method is
  // called on the project-owner thread and must not re-enter project mutation.
  [[nodiscard]] virtual StagedFilePublishResult publishStagedFile(
    const std::filesystem::path& stagingPath,
    const std::filesystem::path& finalPath) = 0;
};
}
