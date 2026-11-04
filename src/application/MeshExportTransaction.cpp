#include "MeshExportTransaction.h"

#include <QUuid>

#include <exception>
#include <system_error>
#include <utility>

namespace cceda::application
{
namespace
{
[[nodiscard]] bool isValidFinalPath(const std::filesystem::path& path)
{
  return !path.empty()
    && path.is_absolute()
    && path.has_parent_path()
    && path.has_filename()
    && path == path.lexically_normal();
}

[[nodiscard]] std::filesystem::path makeStagingPath(
  const std::filesystem::path& finalPath)
{
  const std::filesystem::path filename = finalPath.filename();
  const std::wstring token = QUuid::createUuid()
    .toString(QUuid::Id128)
    .toStdWString();
  const std::wstring stagingName = L"."
    + filename.stem().wstring()
    + L".cceda-stage-"
    + token
    + filename.extension().wstring();
  return (finalPath.parent_path() / stagingName).lexically_normal();
}

[[nodiscard]] bool removeOwnedStagingFile(
  const std::filesystem::path& stagingPath) noexcept
{
  std::error_code error;
  const std::filesystem::file_status status =
    std::filesystem::symlink_status(stagingPath, error);
  if (error == std::errc::no_such_file_or_directory
    || status.type() == std::filesystem::file_type::not_found)
  {
    return true;
  }
  if (error || status.type() != std::filesystem::file_type::regular)
  {
    return false;
  }
  return std::filesystem::remove(stagingPath, error) && !error;
}

[[nodiscard]] MeshExportStatus mapStageFailure(
  MeshExportStageStatus status) noexcept
{
  switch (status)
  {
    case MeshExportStageStatus::InvalidRequest:
      return MeshExportStatus::InvalidRequest;
    case MeshExportStageStatus::UnsupportedSettings:
      return MeshExportStatus::UnsupportedSettings;
    case MeshExportStageStatus::WriteFailed:
      return MeshExportStatus::StagingWriteFailed;
    case MeshExportStageStatus::Succeeded:
      return MeshExportStatus::Succeeded;
  }
  return MeshExportStatus::InvalidRequest;
}
}

MeshExportTransaction::MeshExportTransaction(
  domain::MeshExportSnapshot snapshot,
  domain::MeshExportSettings settings,
  std::filesystem::path stagingPath,
  std::filesystem::path finalPath,
  std::thread::id ownerThread)
  : snapshot_(std::move(snapshot)),
    settings_(std::move(settings)),
    stagingPath_(std::move(stagingPath)),
    finalPath_(std::move(finalPath)),
    ownerThread_(ownerThread)
{
}

const std::filesystem::path& MeshExportTransaction::finalPath() const noexcept
{
  return finalPath_;
}

domain::GeometryRevision MeshExportTransaction::geometryRevision() const noexcept
{
  return snapshot_.geometryRevision;
}

domain::MeshRevision MeshExportTransaction::meshRevision() const noexcept
{
  return snapshot_.meshRevision;
}

bool MeshExportTransaction::active() const noexcept
{
  return snapshot_.mesh != nullptr;
}

StagedMeshExport::StagedMeshExport(MeshExportTransaction transaction)
  : transaction_(std::move(transaction))
{
}

StagedMeshExport::StagedMeshExport(StagedMeshExport&& other) noexcept
  : transaction_(std::move(other.transaction_))
{
  other.transaction_.reset();
}

StagedMeshExport::~StagedMeshExport()
{
  (void)cleanup();
}

bool StagedMeshExport::active() const noexcept
{
  return transaction_.has_value();
}

bool StagedMeshExport::cleanup() noexcept
{
  if (!transaction_)
  {
    return true;
  }
  if (!removeOwnedStagingFile(transaction_->stagingPath_))
  {
    return false;
  }
  transaction_.reset();
  return true;
}

void StagedMeshExport::release() noexcept
{
  transaction_.reset();
}

MeshExportBeginResult beginMeshExport(
  const domain::ProjectState& project,
  const domain::MeshExportSettings& settings,
  const std::filesystem::path& finalPath)
{
  if (!domain::isValidMeshExportSettings(settings)
    || !isValidFinalPath(finalPath)
    || !project.isOwnerThread())
  {
    return {MeshExportStatus::InvalidRequest, std::nullopt,
            "invalid mesh export request"};
  }

  const std::optional<domain::MeshExportSnapshot> snapshot =
    project.captureCurrentMeshForExport();
  if (!snapshot)
  {
    return {MeshExportStatus::NoCurrentMesh, std::nullopt,
            "no current mesh is available for export"};
  }

  try
  {
    std::filesystem::path stagingPath = makeStagingPath(finalPath);
    if (!isValidStagedFilePublishRequest(stagingPath, finalPath))
    {
      return {MeshExportStatus::InvalidRequest, std::nullopt,
              "could not create a valid staging path"};
    }
    return {MeshExportStatus::Succeeded,
      MeshExportTransaction(*snapshot, settings, std::move(stagingPath),
        finalPath, std::this_thread::get_id()), {}};
  }
  catch (const std::exception& exception)
  {
    return {MeshExportStatus::InvalidRequest, std::nullopt, exception.what()};
  }
  catch (...)
  {
    return {MeshExportStatus::InvalidRequest, std::nullopt,
            "staging path generation failed"};
  }
}

MeshExportStageOutcome stageMeshExport(
  MeshExportTransaction transaction,
  IMeshExporter& exporter) noexcept
{
  MeshExportStageResult result;
  try
  {
    if (transaction.snapshot_.mesh == nullptr
      || !domain::isValidMeshExportSettings(transaction.settings_)
      || !isValidStagedFilePublishRequest(
        transaction.stagingPath_, transaction.finalPath_))
    {
      return {MeshExportStatus::InvalidRequest, std::nullopt,
              "inactive mesh export transaction"};
    }
    result = exporter.writeStagedMesh(*transaction.snapshot_.mesh,
      transaction.settings_, transaction.stagingPath_);
  }
  catch (const std::exception& exception)
  {
    result = {MeshExportStageStatus::WriteFailed, exception.what()};
  }
  catch (...)
  {
    result = {MeshExportStageStatus::WriteFailed, "mesh exporter threw"};
  }

  MeshExportStatus status = mapStageFailure(result.status);
  if (result.succeeded())
  {
    return {status, StagedMeshExport(std::move(transaction)), {}};
  }
  if (status == MeshExportStatus::Succeeded)
  {
    status = MeshExportStatus::StagingWriteFailed;
  }
  if (result.stagingFileOwned
    && !removeOwnedStagingFile(transaction.stagingPath_))
  {
    return {MeshExportStatus::StagingCleanupFailed, std::nullopt,
            "staging failed and cleanup failed"};
  }
  return {status, std::nullopt, std::move(result.diagnostic)};
}

MeshExportResult publishMeshExport(
  const domain::ProjectState& project,
  StagedMeshExport stagedExport,
  IStagedFilePublisher& publisher) noexcept
{
  if (!stagedExport.transaction_)
  {
    return {MeshExportStatus::InvalidRequest, "inactive staged export"};
  }

  MeshExportTransaction& transaction = *stagedExport.transaction_;
  if (std::this_thread::get_id() != transaction.ownerThread_)
  {
    if (!stagedExport.cleanup())
    {
      return {MeshExportStatus::StagingCleanupFailed,
              "wrong publication thread and staging cleanup failed"};
    }
    return {MeshExportStatus::InvalidRequest,
            "publication must run on the project owner thread"};
  }

  if (!project.isCurrentMeshExportSnapshot(transaction.snapshot_))
  {
    if (!stagedExport.cleanup())
    {
      return {MeshExportStatus::StagingCleanupFailed,
              "mesh changed and staging cleanup failed"};
    }
    return {MeshExportStatus::SnapshotChanged,
            "mesh changed before export publication"};
  }

  try
  {
    const StagedFilePublishResult publication = publisher.publishStagedFile(
      transaction.stagingPath_, transaction.finalPath_);
    if (publication.succeeded())
    {
      stagedExport.release();
      return {MeshExportStatus::Succeeded, {}};
    }
    if (!stagedExport.cleanup())
    {
      return {MeshExportStatus::StagingCleanupFailed,
              "publication and staging cleanup failed"};
    }
    return {publication.status == StagedFilePublishStatus::InvalidRequest
        ? MeshExportStatus::InvalidRequest
        : MeshExportStatus::PublicationFailed,
      publication.diagnostic};
  }
  catch (const std::exception& exception)
  {
    if (!stagedExport.cleanup())
    {
      return {MeshExportStatus::StagingCleanupFailed,
              "publisher threw and staging cleanup failed"};
    }
    return {MeshExportStatus::PublicationFailed, exception.what()};
  }
  catch (...)
  {
    if (!stagedExport.cleanup())
    {
      return {MeshExportStatus::StagingCleanupFailed,
              "publisher threw and staging cleanup failed"};
    }
    return {MeshExportStatus::PublicationFailed, "file publisher threw"};
  }
}
}
