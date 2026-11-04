#include "WindowsStagedFilePublisher.h"

#include <windows.h>

#include <string>

namespace cceda::platform
{
namespace
{
[[nodiscard]] bool pathsAliasOnWindows(
  const std::filesystem::path& left,
  const std::filesystem::path& right) noexcept
{
  const std::wstring& leftValue = left.native();
  const std::wstring& rightValue = right.native();
  if (leftValue.size() > static_cast<std::size_t>(INT_MAX)
    || rightValue.size() > static_cast<std::size_t>(INT_MAX))
  {
    return true;
  }
  return CompareStringOrdinal(leftValue.c_str(),
      static_cast<int>(leftValue.size()), rightValue.c_str(),
      static_cast<int>(rightValue.size()), TRUE)
    == CSTR_EQUAL;
}
}

application::StagedFilePublishResult
WindowsStagedFilePublisher::publishStagedFile(
  const std::filesystem::path& stagingPath,
  const std::filesystem::path& finalPath)
{
  using application::StagedFilePublishResult;
  using application::StagedFilePublishStatus;

  if (!application::isValidStagedFilePublishRequest(stagingPath, finalPath)
    || pathsAliasOnWindows(stagingPath, finalPath))
  {
    return {StagedFilePublishStatus::InvalidRequest,
            "invalid staged-file publication request"};
  }

  const DWORD stagingAttributes = GetFileAttributesW(stagingPath.c_str());
  if (stagingAttributes == INVALID_FILE_ATTRIBUTES
    || (stagingAttributes & (FILE_ATTRIBUTE_DIRECTORY
      | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
  {
    return {StagedFilePublishStatus::PublishFailed,
            "staging file is missing or is not a regular file"};
  }

  const DWORD finalAttributes = GetFileAttributesW(finalPath.c_str());
  if (finalAttributes != INVALID_FILE_ATTRIBUTES
    && (finalAttributes & (FILE_ATTRIBUTE_DIRECTORY
      | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
  {
    return {StagedFilePublishStatus::PublishFailed,
            "final path is not a replaceable regular file"};
  }
  if (finalAttributes == INVALID_FILE_ATTRIBUTES)
  {
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
    {
      return {StagedFilePublishStatus::PublishFailed,
              "final path inspection failed with error "
                + std::to_string(error)};
    }
  }

  if (!MoveFileExW(stagingPath.c_str(), finalPath.c_str(),
      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
  {
    return {StagedFilePublishStatus::PublishFailed,
            "same-directory replacement failed with error "
              + std::to_string(GetLastError())};
  }
  return {StagedFilePublishStatus::Succeeded, {}};
}
}
