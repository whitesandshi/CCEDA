#include "MainWindow.h"

#include "MeshingTaskPanel.h"
#include "QualityInspectionPanel.h"
#include "OccViewportWidget.h"
#include "geometry/IGeometryService.h"
#include "visualization/IViewportController.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolBar>
#include <QTreeView>

#include <filesystem>
#include <unordered_map>
#include <utility>
namespace
{
QString kindName(cceda::domain::GeometryKind kind)
{
  using cceda::domain::GeometryKind;
  switch (kind)
  {
  case GeometryKind::Assembly:
    return "Assembly";
  case GeometryKind::Part:
    return "Part";
  case GeometryKind::Solid:
    return "Solid";
  case GeometryKind::Shell:
    return "Shell";
  case GeometryKind::Face:
    return "Face";
  case GeometryKind::Wire:
    return "Wire";
  case GeometryKind::Edge:
    return "Edge";
  case GeometryKind::Vertex:
    return "Vertex";
  default:
    return "Geometry";
  }
}

QString yesNo(bool value)
{
  return value ? "Yes" : "No";
}
}

namespace cceda::ui
{
MainWindow::MainWindow(std::shared_ptr<geometry::IGeometryService> geometryService,
                       std::shared_ptr<geometry::IGeometrySnapshotService> snapshotService,
                       std::shared_ptr<visualization::IViewportController> viewportController,
                       application::MeshingRuntimeConfiguration meshingConfiguration,
                       std::shared_ptr<application::IMeshExporter> meshExporter,
                       std::shared_ptr<application::IStagedFilePublisher> filePublisher,
                       QWidget* parent)
    : QMainWindow(parent), geometryService_(std::move(geometryService)),
      snapshotService_(std::move(snapshotService)),
      viewportController_(std::move(viewportController)),
      meshingConfiguration_(std::move(meshingConfiguration))
{
  setObjectName("ccedaMainWindow");
  setWindowTitle("CCEDA Workbench");
  resize(1320, 840);

  viewport_ = new OccViewportWidget(viewportController_, this);
  setCentralWidget(viewport_);
  meshExportController_ = std::make_unique<application::MeshExportJobController>(
      projectState_, std::move(meshExporter), std::move(filePublisher), this);
  createDocks();
  createActions();
  statusBar()->showMessage("Ready");

  meshingController_ = std::make_unique<application::MeshingJobController>(
      projectState_, meshingConfiguration_, this);
  connect(&importWatcher_, &QFutureWatcher<domain::GeometryImportResult>::finished, this,
          [this] { importFinished(); });
  connect(&snapshotWatcher_, &QFutureWatcher<geometry::GeometrySnapshotResult>::finished, this,
          [this] { snapshotFinished(); });
  connect(&meshPresentationWatcher_,
          &QFutureWatcher<visualization::PreparedMeshPresentationPtr>::finished, this,
          [this] { meshPresentationFinished(); });
  connect(meshExportController_.get(),
          &application::MeshExportJobController::runningChanged, this,
          [this](bool) { updateMeshExportAction(); });
  connect(meshExportController_.get(),
          &application::MeshExportJobController::finished, this,
          [this](const application::MeshExportResult& result) {
            meshExportFinished(result);
          });
  connect(meshingController_.get(), &application::MeshingJobController::progressChanged, this,
          [this](const application::MeshingJobProgress& progress) {
            meshingPanel_->showProgress(progress);
            statusBar()->showMessage(tr("Meshing: %1").arg(progress.stage));
            if (lastMeshingStage_ != progress.stage)
            {
              lastMeshingStage_ = progress.stage;
              appendLog(tr("Meshing stage: %1").arg(progress.stage));
            }
          });
  connect(meshingController_.get(), &application::MeshingJobController::cancelling, this,
          [this](const application::MeshingJobCancellation&) {
            meshingPanel_->showCancelling();
            cancelMeshAction_->setEnabled(false);
          });
  connect(meshingController_.get(), &application::MeshingJobController::cancelled, this,
          [this](const application::MeshingJobCancellation& cancellation) {
            meshingCancelled(cancellation);
          });
  connect(meshingController_.get(), &application::MeshingJobController::experimentalSucceeded, this,
          [this](const application::ExperimentalMeshingJobResult& result) {
            meshingSucceeded(result);
          });
  connect(meshingController_.get(), &application::MeshingJobController::failed, this,
          [this](const application::MeshingJobFailure& failure) { meshingFailed(failure); });
  connect(qualityPanel_, &QualityInspectionPanel::issueElementSelected, this,
          [this](domain::MeshRevision meshRevision,
                 domain::MeshElementId elementId) {
            locateMeshElement(meshRevision, elementId);
          });
  connect(qualityPanel_, &QualityInspectionPanel::issueOnlyDisplayChanged, this,
          [this](domain::MeshRevision meshRevision, bool enabled) {
            setIssueElementsOnly(meshRevision, enabled);
          });
}

MainWindow::~MainWindow()
{
  if (meshPresentationWatcher_.isRunning())
  {
    meshPresentationWatcher_.waitForFinished();
  }
  if (snapshotWatcher_.isRunning())
  {
    snapshotCancellationRequested_ = true;
    snapshotWatcher_.waitForFinished();
  }
  if (importWatcher_.isRunning())
  {
    importWatcher_.waitForFinished();
  }
}

void MainWindow::createActions()
{
  QAction* openAction =
      new QAction(style()->standardIcon(QStyle::SP_DialogOpenButton), "Open STEP...", this);
  openAction->setShortcut(QKeySequence::Open);
  openAction->setToolTip("Open a STEP model");
  connect(openAction, &QAction::triggered, this, [this] { openStepDialog(); });

  QAction* fitAction = new QAction(style()->standardIcon(QStyle::SP_BrowserReload), "Fit", this);
  fitAction->setShortcut(Qt::Key_F);
  fitAction->setToolTip("Fit all geometry");
  connect(fitAction, &QAction::triggered, this, [this] { viewportController_->fitAll(); });

  QAction* isometricAction =
      new QAction(style()->standardIcon(QStyle::SP_ComputerIcon), "Isometric", this);
  isometricAction->setShortcut(Qt::Key_0);
  isometricAction->setToolTip("Restore isometric view");
  connect(isometricAction, &QAction::triggered, this,
          [this] { viewportController_->setIsometric(); });

  QAction* exitAction = new QAction("Exit", this);
  exportMeshAction_ = new QAction(
      style()->standardIcon(QStyle::SP_DialogSaveButton), tr("Export Mesh..."), this);
  exportMeshAction_->setObjectName("exportMeshAction");
  exportMeshAction_->setToolTip(tr("Export the current mesh as a Gmsh MSH 4.1 file"));
  exportMeshAction_->setEnabled(false);
  connect(exportMeshAction_, &QAction::triggered, this,
          [this] { exportCurrentMeshDialog(); });

  generateMeshAction_ =
      new QAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("Generate Mesh"), this);
  generateMeshAction_->setObjectName("generateMeshAction");
  generateMeshAction_->setToolTip(tr("Generate a volume mesh for the current geometry"));
  generateMeshAction_->setEnabled(false);
  connect(generateMeshAction_, &QAction::triggered, this, [this] { startMeshing(); });

  cancelMeshAction_ =
      new QAction(style()->standardIcon(QStyle::SP_DialogCancelButton), tr("Cancel Meshing"), this);
  cancelMeshAction_->setObjectName("cancelMeshAction");
  cancelMeshAction_->setToolTip(tr("Cancel the active meshing task"));
  cancelMeshAction_->setEnabled(false);
  connect(cancelMeshAction_, &QAction::triggered, this, [this] { cancelMeshing(); });

  meshSurfaceAction_ = new QAction(tr("Mesh Surface"), this);
  meshSurfaceAction_->setObjectName("meshSurfaceAction");
  meshSurfaceAction_->setToolTip(tr("Show or hide the committed mesh surface"));
  meshSurfaceAction_->setCheckable(true);
  meshSurfaceAction_->setEnabled(false);
  connect(meshSurfaceAction_, &QAction::toggled, this,
          [this](bool visible) { viewportController_->setMeshVisible(visible); });

  connect(exitAction, &QAction::triggered, this, &QWidget::close);

  QMenu* fileMenu = menuBar()->addMenu("File");
  fileMenu->addAction(openAction);
  fileMenu->addAction(exportMeshAction_);
  fileMenu->addSeparator();
  fileMenu->addAction(exitAction);

  QMenu* viewMenu = menuBar()->addMenu("View");
  QMenu* meshMenu = menuBar()->addMenu(tr("Mesh"));
  meshMenu->addAction(generateMeshAction_);
  meshMenu->addAction(cancelMeshAction_);
  meshMenu->addSeparator();
  meshMenu->addAction(exportMeshAction_);

  viewMenu->addAction(fitAction);
  viewMenu->addAction(isometricAction);
  viewMenu->addSeparator();
  viewMenu->addAction(meshSurfaceAction_);

  QToolBar* toolbar = addToolBar("Main");
  toolbar->setObjectName("mainToolbar");
  toolbar->setMovable(false);
  toolbar->addAction(openAction);
  toolbar->addSeparator();
  toolbar->addAction(fitAction);
  toolbar->addAction(isometricAction);
  toolbar->addSeparator();
  toolbar->addAction(generateMeshAction_);
  toolbar->addAction(cancelMeshAction_);
  toolbar->addSeparator();
  toolbar->addAction(exportMeshAction_);

  connect(meshingPanel_, &MeshingTaskPanel::generateRequested, this, [this] { startMeshing(); });
  connect(meshingPanel_, &MeshingTaskPanel::cancelRequested, this, [this] { cancelMeshing(); });
}

void MainWindow::createDocks()
{
  QDockWidget* modelDock = new QDockWidget("Model", this);
  modelDock->setObjectName("modelDock");
  modelDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
  modelTree_ = new QTreeView(modelDock);
  modelTree_->setAccessibleName("Assembly and geometry tree");
  modelTree_->setAlternatingRowColors(true);
  modelTree_->setUniformRowHeights(true);
  modelTree_->setSelectionBehavior(QAbstractItemView::SelectRows);
  model_ = new QStandardItemModel(modelTree_);
  model_->setHorizontalHeaderLabels({"Name", "Type", "Topology"});
  modelTree_->setModel(model_);
  modelTree_->header()->setStretchLastSection(false);
  modelTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  modelTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  modelTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  modelDock->setWidget(modelTree_);
  addDockWidget(Qt::LeftDockWidgetArea, modelDock);

  connect(modelTree_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
          [this](const QModelIndex& current) {
            if (current.isValid())
            {
              showEntity(current.siblingAtColumn(0).data(Qt::UserRole).toInt());
            }
          });

  QDockWidget* propertiesDock = new QDockWidget("Properties", this);
  propertiesDock->setObjectName("propertiesDock");
  propertiesDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
  properties_ = new QTableWidget(0, 2, propertiesDock);
  properties_->setAccessibleName("Selected geometry properties");
  properties_->setHorizontalHeaderLabels({"Property", "Value"});
  properties_->verticalHeader()->hide();
  properties_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  properties_->setSelectionMode(QAbstractItemView::NoSelection);
  properties_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  properties_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  propertiesDock->setWidget(properties_);
  addDockWidget(Qt::RightDockWidgetArea, propertiesDock);

  QDockWidget* outputDock = new QDockWidget("Output", this);
  outputDock->setObjectName("outputDock");
  outputDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  outputTabs_ = new QTabWidget(outputDock);
  issues_ = new QTableWidget(0, 2, outputTabs_);
  issues_->setHorizontalHeaderLabels({"Severity", "Message"});
  issues_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  issues_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  issues_->verticalHeader()->hide();
  issues_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  meshingPanel_ = new MeshingTaskPanel(outputTabs_);
  qualityPanel_ = new QualityInspectionPanel(outputTabs_);
  log_ = new QPlainTextEdit(outputTabs_);
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(2000);
  outputTabs_->addTab(issues_, "Issues");
  outputTabs_->addTab(meshingPanel_, "Tasks");
  outputTabs_->addTab(qualityPanel_, "Quality");
  outputTabs_->addTab(log_, "Log");
  outputDock->setWidget(outputTabs_);
  addDockWidget(Qt::BottomDockWidgetArea, outputDock);
  resizeDocks({modelDock, propertiesDock}, {300, 300}, Qt::Horizontal);
  resizeDocks({outputDock}, {180}, Qt::Vertical);
}

void MainWindow::openStepDialog()
{
  const QString path = QFileDialog::getOpenFileName(this, "Open STEP", QString(),
                                                    "STEP files (*.step *.stp);;All files (*.*)");
  if (!path.isEmpty())
  {
    openStepPath(path);
  }
}

void MainWindow::openStepPath(const QString& path)
{
  if (path.isEmpty() || importWatcher_.isRunning() || meshingActive_)
  {
    return;
  }

  generateMeshAction_->setEnabled(false);
  meshingPanel_->setGeometryAvailable(false);
  importProgress_ = new QProgressDialog("Importing STEP model...", QString(), 0, 0, this);
  importProgress_->setWindowTitle("CCEDA");
  importProgress_->setWindowModality(Qt::WindowModal);
  importProgress_->setCancelButton(nullptr);
  importProgress_->setMinimumDuration(0);
  importProgress_->show();
  statusBar()->showMessage("Importing STEP model...");
  appendLog("Import started: " + path);

  const auto service = geometryService_;
  const std::filesystem::path nativePath(path.toStdWString());
  importWatcher_.setFuture(
      QtConcurrent::run([service, nativePath] { return service->importStep(nativePath); }));
  updateMeshExportAction();
}

void MainWindow::importFinished()
{
  if (importProgress_ != nullptr)
  {
    importProgress_->close();
    importProgress_->deleteLater();
    importProgress_ = nullptr;
  }

  const domain::GeometryImportResult result = importWatcher_.result();
  if (!result.success)
  {
    statusBar()->showMessage("STEP import failed");
    appendLog(QString::fromStdString(result.errorCode + ": " + result.message));
    QMessageBox::critical(this, "STEP import failed", QString::fromStdString(result.message));
    const bool meshable = hasMeshableGeometry();
    meshingPanel_->setGeometryAvailable(meshable);
    generateMeshAction_->setEnabled(meshable);
    updateMeshExportAction();
    return;
  }

  document_ = result.document;
  projectState_.commitImportedGeometry();
  qualityPanel_->clearQuality();
  meshIssueElementIds_.clear();
  populateModel(document_);
  const bool meshable = hasMeshableGeometry();
  meshSurfaceAction_->setChecked(false);
  meshSurfaceAction_->setEnabled(false);
  viewportController_->displayCurrentGeometry();
  setWindowTitle(QString::fromStdString(document_.sourceName) + " - CCEDA Workbench");
  statusBar()->showMessage(
      QString("Loaded %1 solid(s), %2 face(s)").arg(document_.solidCount).arg(document_.faceCount));
  appendLog(QString("Import completed: %1 entities, geometry revision %2")
                .arg(document_.entities.size())
                .arg(document_.revision));
  meshingPanel_->setGeometryAvailable(meshable);
  generateMeshAction_->setEnabled(meshable);
  updateMeshExportAction();
}

void MainWindow::populateModel(const domain::GeometryDocument& document)
{
  model_->clear();
  model_->setHorizontalHeaderLabels({"Name", "Type", "Topology"});
  std::unordered_map<std::string, QStandardItem*> itemsById;

  for (std::size_t index = 0; index < document.entities.size(); ++index)
  {
    const domain::GeometryEntity& entity = document.entities[index];
    auto* nameItem = new QStandardItem(QString::fromUtf8(entity.name.c_str()));
    nameItem->setEditable(false);
    nameItem->setData(static_cast<int>(index), Qt::UserRole);
    auto* typeItem = new QStandardItem(kindName(entity.kind));
    typeItem->setEditable(false);
    auto* topologyItem =
        new QStandardItem(QString("%1 F / %2 E").arg(entity.faceCount).arg(entity.edgeCount));
    topologyItem->setEditable(false);
    QList<QStandardItem*> row{nameItem, typeItem, topologyItem};

    const auto parent = itemsById.find(entity.parentId);
    if (!entity.parentId.empty() && parent != itemsById.end())
    {
      parent->second->appendRow(row);
    }
    else
    {
      model_->appendRow(row);
    }
    itemsById.emplace(entity.id, nameItem);
  }
  modelTree_->expandToDepth(1);
}

void MainWindow::showEntity(int entityIndex)
{
  if (entityIndex < 0 || static_cast<std::size_t>(entityIndex) >= document_.entities.size())
  {
    return;
  }

  const domain::GeometryEntity& entity = document_.entities[static_cast<std::size_t>(entityIndex)];
  const QList<QPair<QString, QString>> values{
      {"Name", QString::fromUtf8(entity.name.c_str())},
      {"Type", kindName(entity.kind)},
      {"Solids", QString::number(entity.solidCount)},
      {"Faces", QString::number(entity.faceCount)},
      {"Edges", QString::number(entity.edgeCount)},
      {"Vertices", QString::number(entity.vertexCount)},
      {"Volume (mm^3)", QString::number(entity.volume, 'g', 10)},
      {"Valid", yesNo(entity.valid)},
      {"Meshable", yesNo(entity.meshable)},
      {"Geometry ID", QString::fromStdString(entity.id)},
  };

  properties_->setRowCount(values.size());
  for (int row = 0; row < values.size(); ++row)
  {
    properties_->setItem(row, 0, new QTableWidgetItem(values[row].first));
    properties_->setItem(row, 1, new QTableWidgetItem(values[row].second));
  }
  viewportController_->highlight(entity.id);
}

void MainWindow::appendLog(const QString& message)
{
  log_->appendPlainText(message);
}
}
