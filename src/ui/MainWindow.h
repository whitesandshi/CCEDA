#pragma once

#include "application/MeshExportJobController.h"
#include "application/MeshingJobController.h"
#include "domain/GeometryDocument.h"
#include "geometry/IGeometrySnapshotService.h"
#include "persistence/ProjectPackage.h"
#include "visualization/IViewportController.h"

#include <QFutureWatcher>
#include <QMainWindow>

#include <functional>
#include <memory>
#include <vector>

class QAction;
class QCloseEvent;
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
class IGeometryValidationService;
}

namespace cceda::ui
{
class OccViewportWidget;
class GeometryValidationPanel;
class MeshingTaskPanel;
class QualityInspectionPanel;

class MainWindow final : public QMainWindow
{
public:
  MainWindow(std::shared_ptr<geometry::IGeometryService> geometryService,
             std::shared_ptr<geometry::IProjectGeometryStorage> geometryStorage,
             std::shared_ptr<geometry::IGeometrySnapshotService> snapshotService,
             std::shared_ptr<const geometry::IGeometryValidationService> validationService,
             std::shared_ptr<visualization::IViewportController> viewportController,
             application::MeshingRuntimeConfiguration meshingConfiguration,
             std::shared_ptr<application::IMeshExporter> meshExporter,
             std::shared_ptr<application::IStagedFilePublisher> filePublisher,
             QWidget* parent = nullptr);
  ~MainWindow() override;

  void openStepPath(const QString& path);
  void openProjectPath(const QString& path);

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void createActions();
  void createDocks(std::shared_ptr<const geometry::IGeometryValidationService> validationService);
  void openStepDialog();
  void startStepImport(const QString& path);
  void openProjectDialog();
  void confirmProjectReplacement(std::function<void()> continuation);
  void startProjectOpen(const QString& path);
  bool startProjectSave(bool saveAs);
  void projectOpened();
  void projectSaved();
  void beginProjectOperation(const QString& path, const QString& label);
  void endProjectOperation();
  [[nodiscard]] bool projectCommandsAvailable() const;
  void updateProjectActions();
  void setProjectDirty(bool dirty);
  void displayProjectGeometry();
  void displayAcceptedQuality(std::shared_ptr<const application::AcceptedMeshQuality> quality);
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
  void locateSolid(domain::GeometryRevision revision, const std::string& targetId);
  void appendLog(const QString& message);

  std::shared_ptr<geometry::IGeometryService> geometryService_;
  std::shared_ptr<geometry::IProjectGeometryStorage> geometryStorage_;
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
  GeometryValidationPanel* geometryValidationPanel_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
  QProgressDialog* importProgress_ = nullptr;
  QAction* generateMeshAction_ = nullptr;
  QAction* cancelMeshAction_ = nullptr;
  QAction* meshSurfaceAction_ = nullptr;
  QAction* exportMeshAction_ = nullptr;
  QAction* importStepAction_ = nullptr;
  QAction* openProjectAction_ = nullptr;
  QAction* saveProjectAction_ = nullptr;
  QAction* saveProjectAsAction_ = nullptr;
  QFutureWatcher<persistence::ProjectImportResult> importWatcher_;
  QFutureWatcher<persistence::ProjectOpenResult> projectOpenWatcher_;
  QFutureWatcher<QString> projectSaveWatcher_;
  QFutureWatcher<geometry::GeometrySnapshotResult> snapshotWatcher_;
  QFutureWatcher<visualization::PreparedMeshPresentationPtr> meshPresentationWatcher_;
  QProgressDialog* meshExportProgress_ = nullptr;
  QProgressDialog* projectProgress_ = nullptr;
  persistence::ProjectIdentity projectIdentity_;
  persistence::ProjectSource projectSource_;
  QString projectPath_;
  QString pendingProjectPath_;
  std::function<void()> afterProjectSave_;
  QString pendingMeshExportPath_;
  QString pendingMeshJobId_;
  QString lastMeshingStage_;
  std::vector<domain::MeshElementId> meshIssueElementIds_;
  bool meshingActive_ = false;
  bool meshPresentationPending_ = false;
  bool projectDirty_ = false;
  bool closeConfirmed_ = false;
  bool snapshotCancellationRequested_ = false;
};
}
