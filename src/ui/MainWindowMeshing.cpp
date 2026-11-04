#include "MainWindow.h"

#include "MeshingTaskPanel.h"
#include "QualityInspectionPanel.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QDir>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace cceda::ui
{
void MainWindow::startMeshing()
{
  if (meshingActive_ || importWatcher_.isRunning() || meshPresentationWatcher_.isRunning() ||
      snapshotService_ == nullptr || meshingController_ == nullptr)
  {
    return;
  }
  if (!hasMeshableGeometry())
  {
    meshingPanel_->showFailure(tr("MESH_GEOMETRY_UNAVAILABLE"),
                               tr("Import a valid, meshable STEP solid before generating a mesh."));
    return;
  }

  auto snapshotDirectory =
      std::make_unique<QTemporaryDir>(QDir(QDir::tempPath()).filePath("cceda-snapshot-XXXXXX"));
  if (!snapshotDirectory->isValid())
  {
    meshingPanel_->showFailure(tr("MESH_SNAPSHOT_DIRECTORY_FAILED"),
                               tr("Could not create a temporary geometry snapshot directory."));
    appendLog(tr("Meshing could not create a snapshot directory."));
    return;
  }

  pendingMeshJobId_ = "mesh-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  snapshotDirectory_ = std::move(snapshotDirectory);
  snapshotCancellationRequested_ = false;
  lastMeshingStage_.clear();
  meshingActive_ = true;
  updateMeshExportAction();
  generateMeshAction_->setEnabled(false);
  cancelMeshAction_->setEnabled(true);
  outputTabs_->setCurrentWidget(meshingPanel_);
  meshingPanel_->showPreparing();
  statusBar()->showMessage(tr("Preparing geometry snapshot..."));
  appendLog(tr("Meshing started: %1").arg(pendingMeshJobId_));

  const geometry::GeometrySnapshotRequest request{
      projectState_.geometryRevision(),
      std::filesystem::path(snapshotDirectory_->path().toStdWString()),
      meshingConfiguration_.maximumGeometryBytes,
      meshingPanel_->mode(),
  };
  const auto service = snapshotService_;
  snapshotWatcher_.setFuture(
      QtConcurrent::run([service, request] { return service->createSnapshot(request); }));
}

void MainWindow::snapshotFinished()
{
  const geometry::GeometrySnapshotResult result = snapshotWatcher_.result();
  if (!meshingActive_)
  {
    return;
  }
  if (snapshotCancellationRequested_)
  {
    const application::MeshingJobCancellation cancellation{
        pendingMeshJobId_,
        projectState_.geometryRevision(),
        tr("User requested cancellation"),
        "preparing-snapshot",
        false,
    };
    meshingPanel_->showCancelled(cancellation);
    statusBar()->showMessage(tr("Meshing cancelled"));
    appendLog(tr("Meshing cancelled while preparing the geometry snapshot."));
    finishMeshingTask();
    return;
  }
  if (!result.succeeded())
  {
    const QString detail = result.detail.empty() ? tr("The geometry snapshot could not be created.")
                                                 : QString::fromStdString(result.detail);
    meshingPanel_->showFailure("MESH_GEOMETRY_SNAPSHOT_FAILED", detail);
    statusBar()->showMessage(tr("Meshing failed"));
    appendLog(tr("MESH_GEOMETRY_SNAPSHOT_FAILED: %1").arg(detail));
    finishMeshingTask();
    return;
  }

  const geometry::GeometrySnapshot& snapshot = *result.snapshot;
  if (snapshot.sourceGeometryRevision != projectState_.geometryRevision())
  {
    const QString detail = tr("Geometry changed while the mesh snapshot was being prepared.");
    meshingPanel_->showFailure("MESH_GEOMETRY_STALE", detail);
    statusBar()->showMessage(tr("Meshing stopped: geometry changed"));
    appendLog(tr("MESH_GEOMETRY_STALE: %1").arg(detail));
    finishMeshingTask();
    return;
  }

  const application::MeshingJobSubmission submission{
      pendingMeshJobId_,
      QString::fromStdWString(snapshot.geometryPath.wstring()),
      QString::fromStdWString(snapshot.geometryMapPath.wstring()),
      QString::fromStdString(snapshot.geometrySha256),
      QString::fromStdString(snapshot.geometryMapSha256),
      meshingPanel_->settings(),
      meshingPanel_->resourceLimits(),
      meshingPanel_->retryPolicy(),
      meshingPanel_->mode(),
  };
  if (const auto failure = meshingController_->submit(submission))
  {
    meshingFailed(*failure);
    return;
  }
  appendLog(tr("Geometry snapshot verified and submitted to the meshing worker."));
}

void MainWindow::cancelMeshing()
{
  if (!meshingActive_)
  {
    return;
  }
  if (snapshotWatcher_.isRunning())
  {
    snapshotCancellationRequested_ = true;
    meshingPanel_->showCancelling();
    cancelMeshAction_->setEnabled(false);
    statusBar()->showMessage(tr("Cancelling meshing..."));
    appendLog(tr("Cancellation requested while preparing the geometry snapshot."));
    return;
  }
  if (meshingController_ == nullptr || !meshingController_->isRunning())
  {
    return;
  }

  if (const auto failure = meshingController_->cancel(tr("User requested cancellation"), 5'000))
  {
    appendLog(tr("%1: %2").arg(failure->code, failure->detail));
    statusBar()->showMessage(tr("Could not cancel the active meshing task"));
    cancelMeshAction_->setEnabled(meshingController_->isRunning());
  }
}

void MainWindow::meshingSucceeded(const application::ExperimentalMeshingJobResult& result)
{
  meshingPanel_->showSucceeded(result);
  qualityPanel_->showQuality(result.quality);
  meshIssueElementIds_.clear();
  if (result.quality)
  {
    meshIssueElementIds_.reserve(result.quality->issues.size());
    for (const mesh_quality::MeshQualityIssue& issue : result.quality->issues)
    {
      meshIssueElementIds_.push_back(issue.elementId);
    }
  }
  outputTabs_->setCurrentWidget(qualityPanel_);
  appendLog(tr("Mesh committed: revision %1, %2 nodes, %3 triangles, %4 tetrahedra")
                .arg(result.committedMeshRevision)
                .arg(static_cast<qulonglong>(result.nodeCount))
                .arg(static_cast<qulonglong>(result.triangleCount))
                .arg(static_cast<qulonglong>(result.tetrahedronCount)));
  finishMeshingTask();
  prepareCurrentMeshPresentation();
}

void MainWindow::prepareCurrentMeshPresentation()
{
  if (meshPresentationWatcher_.isRunning())
  {
    appendLog(tr("Mesh surface preparation is already running."));
    return;
  }

  const std::shared_ptr<const domain::MeshModel> mesh = projectState_.currentMeshHandle();
  viewportController_->clearMeshPresentation();
  {
    const QSignalBlocker blocker(meshSurfaceAction_);
    meshSurfaceAction_->setChecked(false);
  }
  meshSurfaceAction_->setEnabled(false);
  if (mesh == nullptr)
  {
    statusBar()->showMessage(tr("Mesh committed, but no current mesh is available"));
    appendLog(tr("Mesh surface preparation skipped: current mesh is unavailable."));
    return;
  }

  generateMeshAction_->setEnabled(false);
  meshingPanel_->setGenerationBlocked(true);
  statusBar()->showMessage(tr("Preparing mesh surface..."));
  appendLog(tr("Preparing mesh surface for revision %1.").arg(mesh->meshRevision));
  const auto controller = viewportController_;
  const std::vector<domain::MeshElementId> locatableElementIds =
    meshIssueElementIds_;
  meshPresentationWatcher_.setFuture(QtConcurrent::run(
    [controller, mesh, locatableElementIds] {
      try
      {
        return controller->prepareMeshPresentation(
          *mesh, locatableElementIds);
      }
      catch (...)
      {
        return visualization::PreparedMeshPresentationPtr{};
      }
    }));
}

void MainWindow::locateMeshElement(
  domain::MeshRevision meshRevision,
  domain::MeshElementId elementId)
{
  const std::shared_ptr<const domain::MeshModel> mesh =
    projectState_.currentMeshHandle();
  if (projectState_.meshState() != domain::MeshState::Ready
    || mesh == nullptr
    || mesh->meshRevision != meshRevision
    || mesh->sourceGeometryRevision != projectState_.geometryRevision())
  {
    statusBar()->showMessage(tr("Cannot locate an issue from a stale mesh"));
    return;
  }
  if (!meshSurfaceAction_->isEnabled())
  {
    statusBar()->showMessage(tr("Mesh surface is not ready for issue location"));
    return;
  }
  if (!meshSurfaceAction_->isChecked())
  {
    meshSurfaceAction_->setChecked(true);
  }
  if (!viewportController_->highlightMeshElement(meshRevision, elementId))
  {
    statusBar()->showMessage(tr("The selected mesh element is unavailable"));
    return;
  }
  statusBar()->showMessage(
      tr("Located mesh element %1").arg(static_cast<qulonglong>(elementId)));
}

void MainWindow::setIssueElementsOnly(
  domain::MeshRevision meshRevision, bool enabled)
{
  const std::shared_ptr<const domain::MeshModel> mesh =
    projectState_.currentMeshHandle();
  if (projectState_.meshState() != domain::MeshState::Ready
    || mesh == nullptr
    || mesh->meshRevision != meshRevision
    || mesh->sourceGeometryRevision != projectState_.geometryRevision()
    || !meshSurfaceAction_->isEnabled())
  {
    qualityPanel_->setIssueOnlyDisplayAvailable(false);
    statusBar()->showMessage(tr("Cannot filter issues from a stale mesh"));
    return;
  }
  if (!meshSurfaceAction_->isChecked())
  {
    meshSurfaceAction_->setChecked(true);
  }
  if (!viewportController_->setIssueElementsOnly(meshRevision, enabled))
  {
    qualityPanel_->setIssueOnlyDisplayAvailable(false);
    statusBar()->showMessage(tr("Issue-only mesh display is unavailable"));
    return;
  }
  statusBar()->showMessage(enabled
    ? tr("Showing accepted issue elements only")
    : tr("Showing the complete committed mesh surface"));
}

void MainWindow::meshPresentationFinished()
{
  meshingPanel_->setGenerationBlocked(false);
  generateMeshAction_->setEnabled(
    hasMeshableGeometry() && !meshingActive_ && !importWatcher_.isRunning());
  const visualization::PreparedMeshPresentationPtr prepared =
    meshPresentationWatcher_.result();
  if (prepared == nullptr)
  {
    statusBar()->showMessage(tr("Mesh ready; surface display unavailable"));
    appendLog(tr("Mesh surface preparation failed; the committed mesh remains valid."));
    return;
  }

  const std::shared_ptr<const domain::MeshModel> currentMesh = projectState_.currentMeshHandle();
  if (projectState_.meshState() != domain::MeshState::Ready
    || currentMesh == nullptr
    || prepared->meshRevision() != currentMesh->meshRevision
    || prepared->sourceGeometryRevision() != projectState_.geometryRevision())
  {
    appendLog(tr("Discarded a stale prepared mesh surface."));
    return;
  }

  if (!viewportController_->displayMeshPresentation(prepared))
  {
    statusBar()->showMessage(tr("Mesh ready; surface display unavailable"));
    appendLog(tr("The viewport rejected the prepared mesh surface."));
    return;
  }

  {
    const QSignalBlocker blocker(meshSurfaceAction_);
    meshSurfaceAction_->setChecked(true);
  }
  meshSurfaceAction_->setEnabled(true);
  qualityPanel_->setIssueOnlyDisplayAvailable(
    prepared->locatableElementCount() > 0);
  statusBar()->showMessage(tr("Mesh ready: revision %1").arg(prepared->meshRevision()));
  appendLog(tr("Mesh surface displayed: %1 triangles.")
                .arg(static_cast<qulonglong>(prepared->triangleCount())));
}

void MainWindow::meshingFailed(const application::MeshingJobFailure& failure)
{
  meshingPanel_->showFailure(
    failure.code, failure.detail, failure.attemptCount);
  statusBar()->showMessage(tr("Meshing failed: %1").arg(failure.code));
  appendLog(tr("%1 [%2]: %3").arg(failure.code, failure.stage, failure.detail));
  finishMeshingTask();
}

void MainWindow::meshingCancelled(const application::MeshingJobCancellation& cancellation)
{
  meshingPanel_->showCancelled(cancellation);
  statusBar()->showMessage(tr("Meshing cancelled"));
  appendLog(tr("Meshing cancelled at %1: %2").arg(cancellation.stage, cancellation.reason));
  finishMeshingTask();
}

void MainWindow::finishMeshingTask()
{
  meshingActive_ = false;
  snapshotCancellationRequested_ = false;
  generateMeshAction_->setEnabled(hasMeshableGeometry());
  cancelMeshAction_->setEnabled(false);
  snapshotDirectory_.reset();
  pendingMeshJobId_.clear();
  lastMeshingStage_.clear();
  updateMeshExportAction();
}

bool MainWindow::hasMeshableGeometry() const
{
  return std::any_of(document_.entities.begin(), document_.entities.end(),
                     [](const domain::GeometryEntity& entity) {
                       return entity.kind == domain::GeometryKind::Solid && entity.valid &&
                              entity.meshable;
                     });
}
}
