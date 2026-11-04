#include "MainWindow.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QStatusBar>

#include <filesystem>

namespace
{
cceda::domain::MeshExportSettings workbenchMeshExportSettings()
{
  return {
    cceda::domain::MeshExportFormatVersion{4, 1},
    cceda::domain::MeshExportEncoding::Ascii,
    "physical-groups",
  };
}

QString exportStatusCode(cceda::application::MeshExportStatus status)
{
  using cceda::application::MeshExportStatus;
  switch (status)
  {
    case MeshExportStatus::Succeeded:
      return "MESH_EXPORT_SUCCEEDED";
    case MeshExportStatus::InvalidRequest:
      return "MESH_EXPORT_INVALID_REQUEST";
    case MeshExportStatus::NoCurrentMesh:
      return "MESH_EXPORT_NO_CURRENT_MESH";
    case MeshExportStatus::UnsupportedSettings:
      return "MESH_EXPORT_UNSUPPORTED_SETTINGS";
    case MeshExportStatus::StagingWriteFailed:
      return "MESH_EXPORT_STAGING_FAILED";
    case MeshExportStatus::SnapshotChanged:
      return "MESH_EXPORT_SNAPSHOT_CHANGED";
    case MeshExportStatus::PublicationFailed:
      return "MESH_EXPORT_PUBLICATION_FAILED";
    case MeshExportStatus::StagingCleanupFailed:
      return "MESH_EXPORT_CLEANUP_FAILED";
  }
  return "MESH_EXPORT_FAILED";
}

QString exportUserMessage(cceda::application::MeshExportStatus status)
{
  using cceda::application::MeshExportStatus;
  switch (status)
  {
    case MeshExportStatus::InvalidRequest:
      return QObject::tr("The selected mesh export request is not valid.");
    case MeshExportStatus::NoCurrentMesh:
      return QObject::tr("Generate a current mesh before exporting.");
    case MeshExportStatus::UnsupportedSettings:
      return QObject::tr("This mesh export format is not supported.");
    case MeshExportStatus::StagingWriteFailed:
      return QObject::tr("The mesh file could not be written.");
    case MeshExportStatus::SnapshotChanged:
      return QObject::tr("The mesh changed during export. Export the current mesh again.");
    case MeshExportStatus::PublicationFailed:
      return QObject::tr("The destination file could not be replaced.");
    case MeshExportStatus::StagingCleanupFailed:
      return QObject::tr("Mesh export failed and its temporary file could not be removed.");
    case MeshExportStatus::Succeeded:
      break;
  }
  return QObject::tr("Mesh export failed.");
}
}

namespace cceda::ui
{
void MainWindow::exportCurrentMeshDialog()
{
  if (meshExportController_ == nullptr
    || meshExportController_->isRunning()
    || !projectState_.canExportMesh())
  {
    return;
  }

  QString baseName = QFileInfo(
    QString::fromStdString(document_.sourceName)).completeBaseName();
  if (baseName.isEmpty())
  {
    baseName = tr("mesh");
  }
  const QString suggestedPath = QDir(
    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
      .filePath(baseName + ".msh");
  const QString selectedPath = QFileDialog::getSaveFileName(this,
    tr("Export Current Mesh"), suggestedPath,
    tr("Gmsh MSH files (*.msh);;All files (*.*)"));
  if (selectedPath.isEmpty())
  {
    return;
  }

  std::optional<application::MeshExportResult> immediateResult;
  try
  {
    const std::filesystem::path finalPath = std::filesystem::absolute(
      std::filesystem::path(selectedPath.toStdWString())).lexically_normal();
    immediateResult = meshExportController_->start(
      workbenchMeshExportSettings(), finalPath);
  }
  catch (...)
  {
    immediateResult = application::MeshExportResult{
      application::MeshExportStatus::InvalidRequest,
      "could not normalize the selected export path"};
  }

  if (immediateResult)
  {
    meshExportFinished(*immediateResult);
    return;
  }

  pendingMeshExportPath_ = selectedPath;
  meshExportProgress_ = new QProgressDialog(
    tr("Exporting current mesh..."), QString(), 0, 0, this);
  meshExportProgress_->setObjectName("meshExportProgress");
  meshExportProgress_->setAccessibleName(tr("Mesh export progress"));
  meshExportProgress_->setWindowTitle(tr("CCEDA"));
  meshExportProgress_->setWindowModality(Qt::WindowModal);
  meshExportProgress_->setCancelButton(nullptr);
  meshExportProgress_->setMinimumDuration(0);
  meshExportProgress_->show();
  statusBar()->showMessage(tr("Exporting current mesh..."));
  appendLog(tr("Mesh export started: %1").arg(selectedPath));
}

void MainWindow::meshExportFinished(
  const application::MeshExportResult& result)
{
  if (meshExportProgress_ != nullptr)
  {
    meshExportProgress_->close();
    meshExportProgress_->deleteLater();
    meshExportProgress_ = nullptr;
  }
  updateMeshExportAction();

  const QString statusCode = exportStatusCode(result.status);
  if (result.succeeded())
  {
    statusBar()->showMessage(tr("Mesh exported: %1").arg(pendingMeshExportPath_));
    appendLog(tr("%1: %2").arg(statusCode, pendingMeshExportPath_));
    pendingMeshExportPath_.clear();
    return;
  }

  statusBar()->showMessage(exportUserMessage(result.status));
  appendLog(tr("%1: %2").arg(statusCode,
    QString::fromStdString(result.diagnostic)));
  QMessageBox::warning(this, tr("Mesh export failed"),
    exportUserMessage(result.status));
  pendingMeshExportPath_.clear();
}

void MainWindow::updateMeshExportAction()
{
  if (exportMeshAction_ == nullptr)
  {
    return;
  }
  exportMeshAction_->setEnabled(projectState_.canExportMesh()
    && meshExportController_ != nullptr
    && !meshExportController_->isRunning()
    && !importWatcher_.isRunning()
    && !meshingActive_);
}
}
