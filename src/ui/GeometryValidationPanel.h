#pragma once

#include "domain/GeometryValidation.h"

#include <QFutureWatcher>
#include <QWidget>

#include <memory>

class QLabel;

namespace cceda::geometry
{
class IGeometryValidationService;
}

namespace cceda::ui
{
class GeometryValidationPanel final : public QWidget
{
  Q_OBJECT

public:
  explicit GeometryValidationPanel(
    std::shared_ptr<const geometry::IGeometryValidationService> service,
    QWidget* parent = nullptr);

  void checkGeometry(domain::GeometryRevision revision);

signals:
  void solidActivated(cceda::domain::GeometryRevision revision, const std::string& targetId);

private:
  class ReportModel;
  void startCheck();
  void checkFinished();

  std::shared_ptr<const geometry::IGeometryValidationService> service_;
  QFutureWatcher<domain::GeometryValidationReport> watcher_;
  QLabel* status_ = nullptr;
  ReportModel* model_ = nullptr;
  domain::GeometryRevision revision_ = 0;
  bool checking_ = false;
  bool geometryReplaced_ = false;
};
}
