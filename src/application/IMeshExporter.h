#pragma once

#include "domain/MeshExportSettings.h"
#include "domain/MeshModel.h"

#include <filesystem>
#include <string>

namespace cceda::application
{
enum class MeshExportStageStatus
{
  Succeeded,
  InvalidRequest,
  UnsupportedSettings,
  WriteFailed,
};

struct MeshExportStageResult
{
  MeshExportStageStatus status = MeshExportStageStatus::WriteFailed;
  std::string diagnostic;
  bool stagingFileOwned = false;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return status == MeshExportStageStatus::Succeeded && stagingFileOwned;
  }
};

[[nodiscard]] inline bool isValidMeshExportStageRequest(
  const domain::MeshModel& mesh,
  const domain::MeshExportSettings& settings,
  const std::filesystem::path& stagingPath)
{
  return domain::isValidMeshExportSettings(settings)
    && !stagingPath.empty()
    && stagingPath.is_absolute()
    && stagingPath.has_parent_path()
    && stagingPath.has_filename()
    && stagingPath == stagingPath.lexically_normal()
    && domain::validateMeshModel(
      mesh, domain::MeshValidationMode::CommittedMesh).valid();
}

class IMeshExporter
{
public:
  virtual ~IMeshExporter() = default;

  [[nodiscard]] virtual MeshExportStageResult writeStagedMesh(
    const domain::MeshModel& mesh,
    const domain::MeshExportSettings& settings,
    const std::filesystem::path& stagingPath) noexcept = 0;
};
}
