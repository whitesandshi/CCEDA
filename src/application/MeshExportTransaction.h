#pragma once

#include "application/IMeshExporter.h"
#include "application/IStagedFilePublisher.h"
#include "domain/ProjectState.h"

#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace cceda::application
{
enum class MeshExportStatus
{
  Succeeded,
  InvalidRequest,
  NoCurrentMesh,
  UnsupportedSettings,
  StagingWriteFailed,
  SnapshotChanged,
  PublicationFailed,
  StagingCleanupFailed,
};

struct MeshExportResult
{
  MeshExportStatus status = MeshExportStatus::InvalidRequest;
  std::string diagnostic;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return status == MeshExportStatus::Succeeded;
  }
};

struct MeshExportBeginResult;
struct MeshExportStageOutcome;
class StagedMeshExport;

class MeshExportTransaction
{
public:
  MeshExportTransaction(const MeshExportTransaction&) = delete;
  MeshExportTransaction& operator=(const MeshExportTransaction&) = delete;
  MeshExportTransaction(MeshExportTransaction&&) noexcept = default;
  MeshExportTransaction& operator=(MeshExportTransaction&&) noexcept = default;

  [[nodiscard]] const std::filesystem::path& finalPath() const noexcept;
  [[nodiscard]] domain::GeometryRevision geometryRevision() const noexcept;
  [[nodiscard]] domain::MeshRevision meshRevision() const noexcept;
  [[nodiscard]] bool active() const noexcept;

private:
  friend MeshExportBeginResult beginMeshExport(
    const domain::ProjectState& project,
    const domain::MeshExportSettings& settings,
    const std::filesystem::path& finalPath);
  friend MeshExportStageOutcome stageMeshExport(
    MeshExportTransaction transaction,
    IMeshExporter& exporter) noexcept;
  friend MeshExportResult publishMeshExport(
    const domain::ProjectState& project,
    StagedMeshExport stagedExport,
    IStagedFilePublisher& publisher) noexcept;
  friend class StagedMeshExport;

  MeshExportTransaction(
    domain::MeshExportSnapshot snapshot,
    domain::MeshExportSettings settings,
    std::filesystem::path stagingPath,
    std::filesystem::path finalPath,
    std::thread::id ownerThread);

  domain::MeshExportSnapshot snapshot_;
  domain::MeshExportSettings settings_;
  std::filesystem::path stagingPath_;
  std::filesystem::path finalPath_;
  std::thread::id ownerThread_;
};

class StagedMeshExport
{
public:
  StagedMeshExport(const StagedMeshExport&) = delete;
  StagedMeshExport& operator=(const StagedMeshExport&) = delete;
  StagedMeshExport(StagedMeshExport&& other) noexcept;
  StagedMeshExport& operator=(StagedMeshExport&& other) = delete;
  ~StagedMeshExport();

  [[nodiscard]] bool active() const noexcept;

private:
  friend MeshExportStageOutcome stageMeshExport(
    MeshExportTransaction transaction,
    IMeshExporter& exporter) noexcept;
  friend MeshExportResult publishMeshExport(
    const domain::ProjectState& project,
    StagedMeshExport stagedExport,
    IStagedFilePublisher& publisher) noexcept;

  explicit StagedMeshExport(MeshExportTransaction transaction);
  [[nodiscard]] bool cleanup() noexcept;
  void release() noexcept;

  std::optional<MeshExportTransaction> transaction_;
};

struct MeshExportBeginResult
{
  MeshExportStatus status = MeshExportStatus::InvalidRequest;
  std::optional<MeshExportTransaction> transaction;
  std::string diagnostic;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return status == MeshExportStatus::Succeeded
      && transaction.has_value()
      && transaction->active();
  }
};

struct MeshExportStageOutcome
{
  MeshExportStatus status = MeshExportStatus::StagingWriteFailed;
  std::optional<StagedMeshExport> stagedExport;
  std::string diagnostic;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return status == MeshExportStatus::Succeeded
      && stagedExport.has_value()
      && stagedExport->active();
  }
};

[[nodiscard]] MeshExportBeginResult beginMeshExport(
  const domain::ProjectState& project,
  const domain::MeshExportSettings& settings,
  const std::filesystem::path& finalPath);

[[nodiscard]] MeshExportStageOutcome stageMeshExport(
  MeshExportTransaction transaction,
  IMeshExporter& exporter) noexcept;

[[nodiscard]] MeshExportResult publishMeshExport(
  const domain::ProjectState& project,
  StagedMeshExport stagedExport,
  IStagedFilePublisher& publisher) noexcept;
}
