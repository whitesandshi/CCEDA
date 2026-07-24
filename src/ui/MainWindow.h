#pragma once

#include "domain/GeometryDocument.h"

#include <QFutureWatcher>
#include <QMainWindow>

#include <memory>

class QPlainTextEdit;
class QProgressDialog;
class QStandardItemModel;
class QTableWidget;
class QTreeView;

namespace cceda::geometry
{
class IGeometryService;
}
namespace cceda::visualization
{
class IViewportController;
}

namespace cceda::ui
{
class OccViewportWidget;

class MainWindow final : public QMainWindow
{
public:
  MainWindow(std::shared_ptr<geometry::IGeometryService> geometryService,
             std::shared_ptr<visualization::IViewportController> viewportController,
             QWidget* parent = nullptr);
  ~MainWindow() override;

  void openStepPath(const QString& path);

private:
  void createActions();
  void createDocks();
  void openStepDialog();
  void importFinished();
  void populateModel(const domain::GeometryDocument& document);
  void showEntity(int entityIndex);
  void appendLog(const QString& message);

  std::shared_ptr<geometry::IGeometryService> geometryService_;
  std::shared_ptr<visualization::IViewportController> viewportController_;
  domain::ProjectState projectState_;
  domain::GeometryDocument document_;

  OccViewportWidget* viewport_ = nullptr;
  QTreeView* modelTree_ = nullptr;
  QStandardItemModel* model_ = nullptr;
  QTableWidget* properties_ = nullptr;
  QTableWidget* issues_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
  QProgressDialog* importProgress_ = nullptr;
  QFutureWatcher<domain::GeometryImportResult> importWatcher_;
};
}
