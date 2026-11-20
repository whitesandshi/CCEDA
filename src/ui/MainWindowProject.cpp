#include "MainWindow.h"

#include "GeometryValidationPanel.h"
#include "MeshingTaskPanel.h"
#include "QualityInspectionPanel.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>

#include <utility>

namespace cceda::ui
{
bool MainWindow::projectCommandsAvailable() const
{
  return geometryStorage_ && !projectProgress_ && !importProgress_ && !meshingActive_
    && !meshPresentationPending_ && (!meshExportController_ || !meshExportController_->isRunning());
}

void MainWindow::updateProjectActions()
{
  const bool available = projectCommandsAvailable();
  importStepAction_->setEnabled(available);
  openProjectAction_->setEnabled(available);
  saveProjectAction_->setEnabled(available);
  saveProjectAsAction_->setEnabled(available);
}

void MainWindow::setProjectDirty(bool dirty)
{
  projectDirty_ = dirty;
  QString name = !projectPath_.isEmpty() ? QFileInfo(projectPath_).fileName()
                                        : QString::fromStdString(document_.sourceName);
  if (name.isEmpty()) { name = tr("Untitled"); }
  setWindowTitle(tr("%1[*] - CCEDA Workbench").arg(name));
  setWindowModified(dirty);
}

void MainWindow::confirmProjectReplacement(std::function<void()> continuation)
{
  meshingPanel_->commitSettingsEdits();
  if (!projectDirty_)
  {
    continuation();
    return;
  }
  const auto choice = QMessageBox::warning(this, tr("Unsaved Project"),
    tr("Save changes to the current project?"),
    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
  if (choice == QMessageBox::Discard)
  {
    continuation();
  }
  else if (choice == QMessageBox::Save && startProjectSave(false))
  {
    afterProjectSave_ = std::move(continuation);
  }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
  if (projectProgress_ || importProgress_)
  {
    event->ignore();
    return;
  }
  meshingPanel_->commitSettingsEdits();
  if (closeConfirmed_ || !projectDirty_)
  {
    QMainWindow::closeEvent(event);
    return;
  }
  event->ignore();
  if (!projectCommandsAvailable())
  {
    statusBar()->showMessage(tr("Finish the active task before closing the project."));
    return;
  }
  confirmProjectReplacement([this] {
    closeConfirmed_ = true;
    QTimer::singleShot(0, this, [this] { close(); });
  });
}

void MainWindow::openProjectDialog()
{
  const auto path = QFileDialog::getOpenFileName(this, tr("Open Project"), projectPath_,
    tr("CCEDA projects (*.cceda)"));
  openProjectPath(path);
}

void MainWindow::openProjectPath(const QString& path)
{
  if (!path.isEmpty() && projectCommandsAvailable())
  {
    confirmProjectReplacement([this, path] { startProjectOpen(path); });
  }
}

void MainWindow::startProjectOpen(const QString& path)
{
  beginProjectOperation(QFileInfo(path).absoluteFilePath(), tr("Opening project..."));
  const auto storage = geometryStorage_;
  const auto absolute = pendingProjectPath_;
  projectOpenWatcher_.setFuture(QtConcurrent::run([storage, absolute] {
    try { return persistence::openProjectPackage(absolute, *storage); }
    catch (...) { return persistence::ProjectOpenResult{{}, tr("The project could not be loaded.")}; }
  }));
}

bool MainWindow::startProjectSave(bool saveAs)
{
  if (!projectCommandsAvailable()) { return false; }

  QString path = projectPath_;
  if (path.isEmpty() || saveAs)
  {
    QFileDialog dialog(this, tr("Save Project"), QString(), tr("CCEDA projects (*.cceda)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix("cceda");
    if (path.isEmpty())
    {
      QString name = QFileInfo(QString::fromStdString(document_.sourceName)).completeBaseName();
      if (name.isEmpty()) { name = tr("Untitled"); }
      path = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath(name + ".cceda");
    }
    const QFileInfo suggested(path);
    if (suggested.dir().exists()) { dialog.setDirectory(suggested.dir()); }
    dialog.selectFile(suggested.fileName());
    if (dialog.exec() != QDialog::Accepted) { return false; }
    path = dialog.selectedFiles().front();
  }
  if (QFileInfo(path).suffix().compare("cceda", Qt::CaseInsensitive) != 0)
  {
    QMessageBox::warning(this, tr("Project Save Failed"), tr("Choose a filename ending in .cceda."));
    return false;
  }

  meshingPanel_->commitSettingsEdits();
  const auto snapshot = persistence::ProjectSaveSnapshot::capture(projectState_, projectIdentity_,
    projectSource_, {meshingPanel_->mode(), meshingPanel_->settings(),
      meshingPanel_->resourceLimits(), meshingPanel_->retryPolicy()});
  if (!snapshot)
  {
    QMessageBox::warning(this, tr("Project Save Failed"), tr("Could not capture the current project."));
    return false;
  }

  beginProjectOperation(QFileInfo(path).absoluteFilePath(), tr("Saving project..."));
  const auto storage = geometryStorage_;
  const auto destination = pendingProjectPath_;
  const auto creator = QString("CCEDA/%1+%2").arg(QApplication::applicationVersion(),
    meshingConfiguration_.mainBuildId);
  projectSaveWatcher_.setFuture(QtConcurrent::run([storage, destination, snapshot, creator] {
    try { return persistence::saveProjectPackage(destination, *snapshot, *storage, creator); }
    catch (...) { return tr("The project could not be saved."); }
  }));
  return true;
}

void MainWindow::beginProjectOperation(const QString& path, const QString& label)
{
  pendingProjectPath_ = path;
  projectProgress_ = new QProgressDialog(label, QString(), 0, 0, this);
  projectProgress_->setObjectName("projectProgress");
  projectProgress_->setWindowTitle(tr("CCEDA"));
  projectProgress_->setWindowModality(Qt::WindowModal);
  projectProgress_->setCancelButton(nullptr);
  projectProgress_->setMinimumDuration(0);
  projectProgress_->show();
  generateMeshAction_->setEnabled(false);
  meshingPanel_->setGenerationBlocked(true);
  updateMeshExportAction();
  statusBar()->showMessage(label);
}

void MainWindow::endProjectOperation()
{
  projectProgress_->close();
  projectProgress_->deleteLater();
  projectProgress_ = nullptr;
  pendingProjectPath_.clear();
  meshingPanel_->setGenerationBlocked(false);
  generateMeshAction_->setEnabled(hasMeshableGeometry());
  updateMeshExportAction();
}

void MainWindow::projectSaved()
{
  const QString error = projectSaveWatcher_.result();
  const auto path = pendingProjectPath_;
  auto continuation = std::exchange(afterProjectSave_, {});
  endProjectOperation();
  if (!error.isEmpty())
  {
    appendLog(tr("Project save failed: %1").arg(error));
    statusBar()->showMessage(tr("Project save failed"));
    QMessageBox::warning(this, tr("Project Save Failed"), error);
    return;
  }

  projectPath_ = path;
  setProjectDirty(false);
  appendLog(tr("Project saved: %1").arg(path));
  statusBar()->showMessage(tr("Project saved"));
  if (continuation) { continuation(); }
}

void MainWindow::projectOpened()
{
  auto result = projectOpenWatcher_.future().takeResult();
  const auto path = pendingProjectPath_;
  if (!result.project)
  {
    endProjectOperation();
    appendLog(tr("Project open failed: %1").arg(result.error));
    statusBar()->showMessage(tr("Project open failed"));
    QMessageBox::warning(this, tr("Project Open Failed"), result.error);
    return;
  }

  auto& project = *result.project;
  domain::GeometryDocument document = project.geometry ? project.geometry->document
                                                       : domain::GeometryDocument{};
  if (!projectState_.restore(document.revision, project.lastMeshRevision, project.mesh))
  {
    endProjectOperation();
    QMessageBox::warning(this, tr("Project Open Failed"), tr("The project revisions are invalid."));
    return;
  }
  geometryStorage_->activate(std::move(project.geometry));
  document_ = std::move(document);
  projectPath_ = path;
  projectSource_ = std::move(project.source);
  projectIdentity_ = std::move(project.identity);
  meshingPanel_->applySettings(project.settings.mode, project.settings.generation,
    project.settings.resourceLimits, project.settings.retry);
  endProjectOperation();
  displayProjectGeometry();
  setProjectDirty(false);
  appendLog(tr("Project opened: %1").arg(path));
  for (const auto& log : project.missingOptionalLogs)
  {
    appendLog(tr("Optional project log missing: %1").arg(log));
  }
  statusBar()->showMessage(tr("Project opened"));
  if (project.mesh && project.quality)
  {
    meshingPanel_->showRestoredMesh(*project.mesh, *project.quality);
    displayAcceptedQuality(std::move(project.quality));
    prepareCurrentMeshPresentation();
  }
}

void MainWindow::displayProjectGeometry()
{
  qualityPanel_->clearQuality();
  meshIssueElementIds_.clear();
  properties_->setRowCount(0);
  populateModel(document_);
  {
    const QSignalBlocker blocker(meshSurfaceAction_);
    meshSurfaceAction_->setChecked(false);
  }
  meshSurfaceAction_->setEnabled(false);
  viewportController_->displayCurrentGeometry();
  geometryValidationPanel_->checkGeometry(document_.revision);
  outputTabs_->setCurrentWidget(geometryValidationPanel_);
  const bool meshable = hasMeshableGeometry();
  meshingPanel_->setGeometryAvailable(meshable);
  meshingPanel_->resetTaskResult();
  generateMeshAction_->setEnabled(meshable);
}

void MainWindow::displayAcceptedQuality(std::shared_ptr<const application::AcceptedMeshQuality> quality)
{
  meshIssueElementIds_.clear();
  if (quality)
  {
    meshIssueElementIds_.reserve(quality->issues.size());
    for (const auto& issue : quality->issues) { meshIssueElementIds_.push_back(issue.elementId); }
  }
  qualityPanel_->showQuality(std::move(quality));
  outputTabs_->setCurrentWidget(qualityPanel_);
}
}
