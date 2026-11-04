#include "MeshExportJobController.h"

#include <QtConcurrent/QtConcurrentRun>

#include <exception>
#include <utility>

namespace cceda::application
{
MeshExportJobController::MeshExportJobController(
  domain::ProjectState& project,
  std::shared_ptr<IMeshExporter> exporter,
  std::shared_ptr<IStagedFilePublisher> publisher,
  QObject* parent)
  : QObject(parent),
    project_(project),
    exporter_(std::move(exporter)),
    publisher_(std::move(publisher))
{
  connect(&stagingWatcher_,
    &QFutureWatcher<std::shared_ptr<MeshExportStageOutcome>>::finished,
    this, &MeshExportJobController::stagingFinished);
}

MeshExportJobController::~MeshExportJobController()
{
  if (stagingWatcher_.isRunning())
  {
    stagingWatcher_.waitForFinished();
  }
}

bool MeshExportJobController::isRunning() const noexcept
{
  return running_;
}

std::optional<MeshExportResult> MeshExportJobController::start(
  const domain::MeshExportSettings& settings,
  const std::filesystem::path& finalPath)
{
  if (running_ || exporter_ == nullptr || publisher_ == nullptr)
  {
    return MeshExportResult{MeshExportStatus::InvalidRequest,
      "mesh export is already running or is not configured"};
  }

  MeshExportBeginResult begin = beginMeshExport(project_, settings, finalPath);
  if (!begin.succeeded())
  {
    return MeshExportResult{begin.status, std::move(begin.diagnostic)};
  }

  running_ = true;
  emit runningChanged(true);
  try
  {
    const std::shared_ptr<IMeshExporter> exporter = exporter_;
    stagingWatcher_.setFuture(QtConcurrent::run(
      [exporter, transaction = std::move(*begin.transaction)]() mutable {
        return std::make_shared<MeshExportStageOutcome>(stageMeshExport(
          std::move(transaction), *exporter));
      }));
  }
  catch (const std::exception& exception)
  {
    running_ = false;
    emit runningChanged(false);
    return MeshExportResult{MeshExportStatus::StagingWriteFailed,
      exception.what()};
  }
  catch (...)
  {
    running_ = false;
    emit runningChanged(false);
    return MeshExportResult{MeshExportStatus::StagingWriteFailed,
      "could not schedule mesh export staging"};
  }
  return std::nullopt;
}

void MeshExportJobController::stagingFinished()
{
  try
  {
    const std::shared_ptr<MeshExportStageOutcome> outcome =
      stagingWatcher_.result();
    if (outcome == nullptr || !outcome->succeeded())
    {
      complete(outcome == nullptr
          ? MeshExportResult{MeshExportStatus::StagingWriteFailed,
              "mesh export staging returned no result"}
          : MeshExportResult{outcome->status, std::move(outcome->diagnostic)});
      return;
    }

    complete(publishMeshExport(project_,
      std::move(*outcome->stagedExport), *publisher_));
  }
  catch (const std::exception& exception)
  {
    complete({MeshExportStatus::StagingWriteFailed, exception.what()});
  }
  catch (...)
  {
    complete({MeshExportStatus::StagingWriteFailed,
      "mesh export staging failed"});
  }
}

void MeshExportJobController::complete(MeshExportResult result)
{
  running_ = false;
  emit runningChanged(false);
  emit finished(result);
}
}
