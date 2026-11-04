#pragma once

#include "application/IMeshExporter.h"

namespace cceda::meshing
{
class GmshMsh41MeshExporter final : public application::IMeshExporter
{
public:
  [[nodiscard]] application::MeshExportStageResult writeStagedMesh(
    const domain::MeshModel& mesh,
    const domain::MeshExportSettings& settings,
    const std::filesystem::path& stagingPath) noexcept override;
};
}
