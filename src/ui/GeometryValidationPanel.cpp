#include "ui/GeometryValidationPanel.h"

#include "geometry/IGeometryValidationService.h"

#include <QAbstractTableModel>
#include <QHeaderView>
#include <QLabel>
#include <QTableView>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace cceda::ui
{
class GeometryValidationPanel::ReportModel final : public QAbstractTableModel
{
public:
  using QAbstractTableModel::QAbstractTableModel;

  void setReport(domain::GeometryValidationReport report)
  {
    beginResetModel();
    report_ = std::move(report);
    endResetModel();
  }

  const domain::SolidBrepCheckResult& check(int row) const
  {
    return report_.solidChecks[static_cast<std::size_t>(row)];
  }

  int rowCount(const QModelIndex& parent) const override
  {
    return parent.isValid() ? 0 : static_cast<int>(report_.solidChecks.size());
  }

  int columnCount(const QModelIndex& parent) const override
  {
    return parent.isValid() ? 0 : 3;
  }

  QVariant headerData(int section, Qt::Orientation orientation, int role) const override
  {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    {
      return {};
    }
    switch (section)
    {
    case 0: return GeometryValidationPanel::tr("Solid ID");
    case 1: return GeometryValidationPanel::tr("B-Rep status");
    case 2: return GeometryValidationPanel::tr("Issue code");
    default: return {};
    }
  }

  QVariant data(const QModelIndex& index, int role) const override
  {
    if (!index.isValid() || (role != Qt::DisplayRole && role != Qt::ToolTipRole))
    {
      return {};
    }
    const auto& result = check(index.row());
    if (index.column() == 0)
    {
      return QString::fromStdString(result.targetId);
    }
    if (index.column() == 2)
    {
      return result.issue ? QString::fromStdString(result.issue->code) : QString{};
    }
    using Status = domain::SolidBrepCheckStatus;
    switch (result.status)
    {
    case Status::Valid: return GeometryValidationPanel::tr("Valid");
    case Status::Invalid: return GeometryValidationPanel::tr("Invalid B-Rep");
    case Status::TargetNotFound: return GeometryValidationPanel::tr("Solid shape missing");
    case Status::UnsupportedTarget: return GeometryValidationPanel::tr("Unsupported shape");
    default: return GeometryValidationPanel::tr("Check unavailable");
    }
  }

private:
  domain::GeometryValidationReport report_;
};

GeometryValidationPanel::GeometryValidationPanel(
  std::shared_ptr<const geometry::IGeometryValidationService> service, QWidget* parent)
  : QWidget(parent), service_(std::move(service))
{
  auto* layout = new QVBoxLayout(this);
  status_ = new QLabel(tr("No geometry loaded"), this);
  status_->setObjectName("geometryValidationStatus");
  status_->setWordWrap(true);
  layout->addWidget(status_);

  auto* table = new QTableView(this);
  table->setObjectName("geometryValidationTable");
  table->setAccessibleName(tr("Solid B-Rep results"));
  table->setAccessibleDescription(tr("Activate a row to locate the Solid in the model."));
  model_ = new ReportModel(table);
  table->setModel(model_);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setAlternatingRowColors(true);
  table->verticalHeader()->hide();
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  layout->addWidget(table);

  connect(table, &QTableView::activated, this, [this](const QModelIndex& index) {
    const auto& check = model_->check(index.row());
    if (check.revision == revision_)
    {
      emit solidActivated(check.revision, check.targetId);
    }
  });
  connect(&watcher_, &QFutureWatcher<domain::GeometryValidationReport>::finished,
          this, &GeometryValidationPanel::checkFinished);
}

void GeometryValidationPanel::checkGeometry(domain::GeometryRevision revision)
{
  revision_ = revision;
  model_->setReport({});
  if (revision_ == 0)
  {
    status_->setText(tr("No geometry loaded"));
  }
  else if (!service_)
  {
    status_->setText(tr("B-Rep validation is unavailable"));
  }
  else
  {
    status_->setText(tr("Checking Solid B-Reps for revision %1…").arg(revision_));
  }

  if (checking_)
  {
    // Project reopen can replace geometry with the same numeric revision.
    geometryReplaced_ = true;
    return;
  }
  startCheck();
}

void GeometryValidationPanel::startCheck()
{
  if (revision_ == 0 || !service_)
  {
    return;
  }
  checking_ = true;
  watcher_.setFuture(QtConcurrent::run([service = service_, revision = revision_] {
    try
    {
      return service->checkDocumentBrep(revision);
    }
    catch (...)
    {
      return domain::GeometryValidationReport{revision};
    }
  }));
}

void GeometryValidationPanel::checkFinished()
{
  auto report = watcher_.future().takeResult();
  checking_ = false;
  if (std::exchange(geometryReplaced_, false))
  {
    startCheck();
    return;
  }
  using ReportStatus = domain::GeometryValidationReportStatus;
  if (report.revision != revision_ || report.status == ReportStatus::RevisionMismatch)
  {
    status_->setText(tr("Geometry changed before the B-Rep check finished"));
    return;
  }
  if (report.status != ReportStatus::Completed)
  {
    status_->setText(tr("B-Rep validation is unavailable for revision %1").arg(revision_));
    return;
  }
  if (report.solidChecks.empty())
  {
    status_->setText(tr("No indexed Solids in revision %1").arg(revision_));
    return;
  }

  std::size_t valid = 0;
  std::size_t invalid = 0;
  for (const auto& check : report.solidChecks)
  {
    valid += check.status == domain::SolidBrepCheckStatus::Valid;
    invalid += check.status == domain::SolidBrepCheckStatus::Invalid;
  }
  status_->setText(tr("B-Rep revision %1 · %2 valid · %3 invalid · %4 without a result")
    .arg(revision_).arg(valid).arg(invalid).arg(report.solidChecks.size() - valid - invalid));
  model_->setReport(std::move(report));
}
}
