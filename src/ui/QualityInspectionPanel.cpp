#include "ui/QualityInspectionPanel.h"

#include <QAbstractTableModel>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QSignalBlocker>
#include <QLabel>
#include <QSplitter>
#include <QTableView>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace cceda::ui
{
namespace
{
QString metricId(mesh_quality::MeshQualityMetric metric)
{
  const std::string_view id = mesh_quality::stableMetricId(metric);
  return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
}

QString issueId(mesh_quality::MeshQualityIssueCode code)
{
  const std::string_view id = mesh_quality::stableIssueId(code);
  return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
}

void configureTable(QTableView& table)
{
  table.setAlternatingRowColors(true);
  table.setEditTriggers(QAbstractItemView::NoEditTriggers);
  table.setSelectionBehavior(QAbstractItemView::SelectRows);
  table.setSelectionMode(QAbstractItemView::SingleSelection);
  table.setWordWrap(false);
  table.verticalHeader()->hide();
  table.verticalHeader()->setDefaultSectionSize(22);
}
}

class QualityInspectionPanel::SummaryModel final : public QAbstractTableModel
{
public:
  explicit SummaryModel(QObject* parent) : QAbstractTableModel(parent) {}

  void setQuality(std::shared_ptr<const application::AcceptedMeshQuality> quality)
  {
    beginResetModel();
    quality_ = std::move(quality);
    rows_.clear();
    if (quality_)
    {
      constexpr std::array<mesh_quality::MeshQualityMetric, 9> orderedMetrics{
        mesh_quality::MeshQualityMetric::TriangleArea,
        mesh_quality::MeshQualityMetric::TriangleAspectRatio,
        mesh_quality::MeshQualityMetric::TriangleMinimumAngle,
        mesh_quality::MeshQualityMetric::TriangleMaximumAngle,
        mesh_quality::MeshQualityMetric::TriangleNormalConsistent,
        mesh_quality::MeshQualityMetric::TetrahedronSignedVolume,
        mesh_quality::MeshQualityMetric::TetrahedronAspectRatio,
        mesh_quality::MeshQualityMetric::TetrahedronMinimumDihedralAngle,
        mesh_quality::MeshQualityMetric::TetrahedronScaledJacobian,
      };
      for (const mesh_quality::MeshQualityMetric metric : orderedMetrics)
      {
        if (metric == quality_->booleanSummary.metric)
        {
          rows_.push_back({metric, true});
          continue;
        }
        const auto found = std::find_if(
          quality_->numericSummaries.cbegin(),
          quality_->numericSummaries.cend(),
          [metric](const application::AcceptedMeshQualityNumericSummary& summary) {
            return summary.metric == metric;
          });
        if (found != quality_->numericSummaries.cend())
        {
          rows_.push_back({metric, false});
        }
      }
    }
    endResetModel();
  }

  int rowCount(const QModelIndex& parent = {}) const override
  {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
  }

  int columnCount(const QModelIndex& parent = {}) const override
  {
    return parent.isValid() ? 0 : 6;
  }

  QVariant headerData(
    int section, Qt::Orientation orientation, int role) const override
  {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    {
      return {};
    }
    constexpr std::array<const char*, 6> headers{
      "Metric", "Samples", "Minimum", "Mean", "Maximum", "Failed"};
    return section >= 0 && section < static_cast<int>(headers.size())
      ? tr(headers.at(static_cast<std::size_t>(section)))
      : QVariant{};
  }

  QVariant data(const QModelIndex& index, int role) const override
  {
    if (!quality_ || !index.isValid()
      || index.row() >= static_cast<int>(rows_.size()))
    {
      return {};
    }
    if (role == Qt::TextAlignmentRole && index.column() > 0)
    {
      return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (role != Qt::DisplayRole)
    {
      return {};
    }

    const Row& row = rows_.at(static_cast<std::size_t>(index.row()));
    if (index.column() == 0)
    {
      return metricId(row.metric);
    }
    if (row.boolean)
    {
      if (index.column() == 1)
      {
        return QVariant::fromValue<qulonglong>(quality_->booleanSummary.sampleCount);
      }
      if (index.column() == 5)
      {
        return QVariant::fromValue<qulonglong>(
          quality_->booleanSummary.failedElementCount);
      }
      return {};
    }

    const auto found = std::find_if(
      quality_->numericSummaries.cbegin(),
      quality_->numericSummaries.cend(),
      [metric = row.metric](
        const application::AcceptedMeshQualityNumericSummary& summary) {
        return summary.metric == metric;
      });
    if (found == quality_->numericSummaries.cend())
    {
      return {};
    }
    switch (index.column())
    {
      case 1: return QVariant::fromValue<qulonglong>(found->sampleCount);
      case 2: return found->minimum ? QVariant(*found->minimum) : QVariant{};
      case 3: return found->mean ? QVariant(*found->mean) : QVariant{};
      case 4: return found->maximum ? QVariant(*found->maximum) : QVariant{};
      default: return {};
    }
  }

private:
  struct Row
  {
    mesh_quality::MeshQualityMetric metric;
    bool boolean = false;
  };

  std::shared_ptr<const application::AcceptedMeshQuality> quality_;
  std::vector<Row> rows_;
};

class QualityInspectionPanel::IssueModel final : public QAbstractTableModel
{
public:
  explicit IssueModel(QObject* parent) : QAbstractTableModel(parent) {}

  void setQuality(std::shared_ptr<const application::AcceptedMeshQuality> quality)
  {
    beginResetModel();
    quality_ = std::move(quality);
    endResetModel();
  }

  int rowCount(const QModelIndex& parent = {}) const override
  {
    return !parent.isValid() && quality_
      ? static_cast<int>(quality_->issues.size())
      : 0;
  }

  int columnCount(const QModelIndex& parent = {}) const override
  {
    return parent.isValid() ? 0 : 3;
  }

  QVariant headerData(
    int section, Qt::Orientation orientation, int role) const override
  {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    {
      return {};
    }
    constexpr std::array<const char*, 3> headers{
      "Severity", "Element ID", "Issue"};
    return section >= 0 && section < static_cast<int>(headers.size())
      ? tr(headers.at(static_cast<std::size_t>(section)))
      : QVariant{};
  }

  QVariant data(const QModelIndex& index, int role) const override
  {
    if (!quality_ || !index.isValid()
      || index.row() >= static_cast<int>(quality_->issues.size()))
    {
      return {};
    }
    if (role == Qt::TextAlignmentRole && index.column() == 1)
    {
      return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (role != Qt::DisplayRole)
    {
      return {};
    }
    const mesh_quality::MeshQualityIssue& issue =
      quality_->issues.at(static_cast<std::size_t>(index.row()));
    switch (index.column())
    {
      case 0:
        return mesh_quality::isCommitBlockingIssue(issue.code)
          ? tr("Error") : tr("Warning");
      case 1:
        return QVariant::fromValue<qulonglong>(issue.elementId);
      case 2:
        return issueId(issue.code);
      default:
        return {};
    }
  }

  std::optional<std::pair<domain::MeshRevision, domain::MeshElementId>>
  issueIdentity(int row) const
  {
    if (!quality_ || row < 0
      || row >= static_cast<int>(quality_->issues.size()))
    {
      return std::nullopt;
    }
    return std::pair{
      quality_->meshRevision,
      quality_->issues.at(static_cast<std::size_t>(row)).elementId,
    };
  }

  const mesh_quality::MeshQualityElementDetail* issueDetail(int row) const
  {
    const auto identity = issueIdentity(row);
    if (!identity)
    {
      return nullptr;
    }
    const auto found = quality_->issueElementDetails.find(identity->second);
    return found == quality_->issueElementDetails.end()
      ? nullptr
      : &found->second;
  }

private:
  std::shared_ptr<const application::AcceptedMeshQuality> quality_;
};

QualityInspectionPanel::QualityInspectionPanel(QWidget* parent)
  : QWidget(parent)
  , summaryModel_(std::make_unique<SummaryModel>(this))
  , issueModel_(std::make_unique<IssueModel>(this))
{
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(6);

  status_ = new QLabel(tr("No committed mesh quality result"), this);
  status_->setObjectName("qualityInspectionStatus");
  status_->setAccessibleName(status_->text());
  layout->addWidget(status_);
  createColoringControls(layout);

  issueOnlyDisplay_ = new QCheckBox(tr("Show only issue elements"), this);
  issueOnlyDisplay_->setObjectName("qualityIssueOnlyDisplayCheckBox");
  issueOnlyDisplay_->setAccessibleName(tr("Show only accepted mesh issue elements"));
  issueOnlyDisplay_->setEnabled(false);
  connect(issueOnlyDisplay_, &QCheckBox::toggled, this, [this](bool enabled) {
    if (meshRevision_ > 0 && hasIssues_)
    {
      emit issueOnlyDisplayChanged(meshRevision_, enabled);
    }
  });
  layout->addWidget(issueOnlyDisplay_);

  auto* splitter = new QSplitter(Qt::Vertical, this);
  summaries_ = new QTableView(splitter);
  summaries_->setObjectName("qualitySummaryTable");
  summaries_->setAccessibleName(tr("Mesh quality metric summaries"));
  summaries_->setModel(summaryModel_.get());
  configureTable(*summaries_);
  summaries_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int column = 1; column < 6; ++column)
  {
    summaries_->horizontalHeader()->setSectionResizeMode(
      column, QHeaderView::ResizeToContents);
  }

  issues_ = new QTableView(splitter);
  issues_->setObjectName("qualityIssueTable");
  issues_->setAccessibleName(tr("Mesh quality element issues"));
  issues_->setModel(issueModel_.get());
  configureTable(*issues_);
  issues_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  issues_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  issues_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
  issueDetails_ = new QTableWidget(0, 2, splitter);
  issueDetails_->setObjectName("qualityIssueDetailsTable");
  issueDetails_->setAccessibleName(tr("Selected mesh quality issue properties"));
  issueDetails_->setHorizontalHeaderLabels({tr("Property"), tr("Value")});
  issueDetails_->setAlternatingRowColors(true);
  issueDetails_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  issueDetails_->setSelectionMode(QAbstractItemView::NoSelection);
  issueDetails_->setWordWrap(false);
  issueDetails_->verticalHeader()->hide();
  issueDetails_->verticalHeader()->setDefaultSectionSize(22);
  issueDetails_->horizontalHeader()->setSectionResizeMode(
    0, QHeaderView::ResizeToContents);
  issueDetails_->horizontalHeader()->setSectionResizeMode(
    1, QHeaderView::Stretch);
  issueDetails_->hide();
  connect(issues_->selectionModel(), &QItemSelectionModel::currentRowChanged,
    this, [this](const QModelIndex& current) {
      showIssueDetail(issueModel_->issueDetail(current.row()));
      const auto identity = issueModel_->issueIdentity(current.row());
      if (identity)
      {
        emit issueElementSelected(identity->first, identity->second);
      }
    });
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 1);
  splitter->setStretchFactor(2, 1);
  layout->addWidget(splitter, 1);
}

QualityInspectionPanel::~QualityInspectionPanel() = default;

void QualityInspectionPanel::showQuality(
  std::shared_ptr<const application::AcceptedMeshQuality> quality)
{
  if (!quality)
  {
    clearQuality();
    return;
  }
  summaryModel_->setQuality(quality);
  issueModel_->setQuality(quality);
  meshRevision_ = quality->meshRevision;
  coloringMetric_->setCurrentIndex(0);
  setColoringAvailable(false);
  coloringLegend_->clear();
  hasIssues_ = !quality->issues.empty();
  setIssueOnlyDisplayAvailable(false);
  showIssueDetail(nullptr);
  issues_->setVisible(!quality->issues.empty());
  status_->setText(tr(
    "Accepted mesh %1 | Geometry %2 | %3 triangles | %4 tetrahedra | %5 issues")
      .arg(quality->meshRevision)
      .arg(quality->sourceGeometryRevision)
      .arg(static_cast<qulonglong>(quality->triangleCount))
      .arg(static_cast<qulonglong>(quality->tetrahedronCount))
      .arg(static_cast<qulonglong>(quality->issues.size())));
  status_->setAccessibleName(status_->text());
}

void QualityInspectionPanel::clearQuality()
{
  summaryModel_->setQuality({});
  issueModel_->setQuality({});
  meshRevision_ = 0;
  coloringMetric_->setCurrentIndex(0);
  setColoringAvailable(false);
  coloringLegend_->clear();
  hasIssues_ = false;
  setIssueOnlyDisplayAvailable(false);
  showIssueDetail(nullptr);
  issues_->setVisible(true);
  status_->setText(tr("No committed mesh quality result"));
  status_->setAccessibleName(status_->text());
}

void QualityInspectionPanel::setIssueOnlyDisplayAvailable(bool available)
{
  const QSignalBlocker blocker(issueOnlyDisplay_);
  if (!available || !hasIssues_)
  {
    issueOnlyDisplay_->setChecked(false);
  }
  issueOnlyDisplay_->setEnabled(available && hasIssues_);
}

}
