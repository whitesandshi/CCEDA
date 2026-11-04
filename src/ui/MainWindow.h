#pragma once

#include "application/MeshExportJobController.h"
#include "application/MeshingJobController.h"
#include "domain/GeometryDocument.h"
#include "geometry/IGeometrySnapshotService.h"
#include "visualization/IViewportController.h"

#include <QFutureWatcher>
#include <QMainWindow>

#include <memory>
#include <vector>

class QAction;
class QPlainTextEdit;
class QProgressDialog;
class QStandardItemModel;
class QTableWidget;
class QTabWidget;
class QTemporaryDir;
class QTreeView;

namespace cceda::geometry
{
class IGeometryService;
}

namespace cceda::ui
{
class OccViewportWidget;
class MeshingTaskPanel;
class QualityInspectionPanel;

class MainWindow final : public QMainWindow
{
public:
  MainWindow(std::shared_ptr<geometry::IGeometryService> geometryService,
             std::shared_ptr<geometry::IGeometrySnapshotService> snapshotService,
             std::shared_ptr<visualization::IViewportController> viewportController,
             application::MeshingRuntimeConfiguration meshingConfiguration,
             std::shared_ptr<application::IMeshExporter> meshExporter,
             std::shared_ptr<application::IStagedFilePublisher> filePublisher,
             QWidget* parent = nullptr);
  ~MainWindow() override;

  void openStepPath(const QString& path);

private:
  void createActions();
  void createDocks();
  void openStepDialog();
  void exportCurrentMeshDialog();
  void meshExportFinished(const application::MeshExportResult& result);
  void updateMeshExportAction();
  void importFinished();
  void populateModel(const domain::GeometryDocument& document);
  void startMeshing();
  void snapshotFinished();
  void cancelMeshing();
  void meshingSucceeded(const application::ExperimentalMeshingJobResult& result);
  void prepareCurrentMeshPresentation();
  void meshPresentationFinished();
  void locateMeshElement(
    domain::MeshRevision meshRevision,
    domain::MeshElementId elementId);
  void setIssueElementsOnly(
    domain::MeshRevision meshRevision, bool enabled);
  void meshingFailed(const application::MeshingJobFailure& failure);
  void meshingCancelled(const application::MeshingJobCancellation& cancellation);
  void finishMeshingTask();
  [[nodiscard]] bool hasMeshableGeometry() const;
  void showEntity(int entityIndex);
  void appendLog(const QString& message);

  std::shared_ptr<geometry::IGeometryService> geometryService_;
  std::shared_ptr<geometry::IGeometrySnapshotService> snapshotService_;
  std::shared_ptr<visualization::IViewportController> viewportController_;
  application::MeshingRuntimeConfiguration meshingConfiguration_;
  domain::ProjectState projectState_;
  domain::GeometryDocument document_;
  std::unique_ptr<application::MeshingJobController> meshingController_;
  std::unique_ptr<application::MeshExportJobController> meshExportController_;
  std::unique_ptr<QTemporaryDir> snapshotDirectory_;

  OccViewportWidget* viewport_ = nullptr;
  QTreeView* modelTree_ = nullptr;
  QStandardItemModel* model_ = nullptr;
  QTableWidget* properties_ = nullptr;
  MeshingTaskPanel* meshingPanel_ = nullptr;
  QualityInspectionPanel* qualityPanel_ = nullptr;
  QTabWidget* outputTabs_ = nullptr;
  QTableWidget* issues_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
  QProgressDialog* importProgress_ = nullptr;
  QAction* generateMeshAction_ = nullptr;
  QAction* cancelMeshAction_ = nullptr;
  QAction* meshSurfaceAction_ = nullptr;
  QAction* exportMeshAction_ = nullptr;
  QFutureWatcher<domain::GeometryImportResult> importWatcher_;
  QFutureWatcher<geometry::GeometrySnapshotResult> snapshotWatcher_;
  QFutureWatcher<visualization::PreparedMeshPresentationPtr> meshPresentationWatcher_;
  QProgressDialog* meshExportProgress_ = nullptr;
  QString pendingMeshExportPath_;
  QString pendingMeshJobId_;
  QString lastMeshingStage_;
  std::vector<domain::MeshElementId> meshIssueElementIds_;
  bool meshingActive_ = false;
  bool snapshotCancellationRequested_ = false;
};
}
