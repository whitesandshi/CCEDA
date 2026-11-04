#pragma once

#include "application/IStagedFilePublisher.h"

namespace cceda::platform
{
class WindowsStagedFilePublisher final
  : public application::IStagedFilePublisher
{
public:
  [[nodiscard]] application::StagedFilePublishResult publishStagedFile(
    const std::filesystem::path& stagingPath,
    const std::filesystem::path& finalPath) override;
};
}
