#include "ui/QualityInspectionPanel.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>

namespace cceda::ui
{
void QualityInspectionPanel::createColoringControls(QVBoxLayout* layout)
{
  auto* form = new QFormLayout;
  coloringMetric_ = new QComboBox(this);
  coloringMetric_->setObjectName("qualityColorMetric");
  coloringMetric_->setAccessibleName(tr("Surface quality coloring"));
  coloringMetric_->addItem(tr("Uniform surface"), -1);
  using Metric = mesh_quality::MeshQualityMetric;
  for (const auto metric : {Metric::TriangleArea, Metric::TriangleAspectRatio,
         Metric::TriangleMinimumAngle, Metric::TriangleMaximumAngle,
         Metric::TriangleNormalConsistent})
  {
    const auto id = mesh_quality::stableMetricId(metric);
    coloringMetric_->addItem(QString::fromUtf8(id.data(), id.size()), static_cast<int>(metric));
  }
  warningBoundary_ = new QLineEdit(this);
  warningBoundary_->setObjectName("qualityWarningBoundary");
  criticalBoundary_ = new QLineEdit(this);
  criticalBoundary_->setObjectName("qualityCriticalBoundary");
  warningBoundary_->setPlaceholderText(tr("Enter boundary (decimal point)"));
  criticalBoundary_->setPlaceholderText(tr("Enter boundary (decimal point)"));
  form->addRow(tr("Surface metric"), coloringMetric_);
  form->addRow(tr("Warning boundary"), warningBoundary_);
  form->addRow(tr("Critical boundary"), criticalBoundary_);
  layout->addLayout(form);
  applyColoring_ = new QPushButton(tr("Apply surface colors"), this);
  applyColoring_->setObjectName("applyQualityColors");
  layout->addWidget(applyColoring_);
  coloringLegend_ = new QLabel(this);
  coloringLegend_->setObjectName("qualityColorLegend");
  coloringLegend_->setWordWrap(true);
  layout->addWidget(coloringLegend_);
  connect(coloringMetric_, &QComboBox::currentIndexChanged, this, [this] {
    warningBoundary_->clear();
    criticalBoundary_->clear();
    updateColoringControls();
  });
  connect(warningBoundary_, &QLineEdit::textChanged, this, [this] { updateColoringControls(); });
  connect(criticalBoundary_, &QLineEdit::textChanged, this, [this] { updateColoringControls(); });
  connect(applyColoring_, &QPushButton::clicked, this, [this] {
    if (applyColoring_->isEnabled())
    {
      emit coloringRequested(meshRevision_);
    }
  });
  updateColoringControls();
}

std::optional<visualization::MeshQualityColoringRequest>
QualityInspectionPanel::coloringRequest() const
{
  const int selected = coloringMetric_->currentData().toInt();
  if (selected < 0)
  {
    return std::nullopt;
  }
  visualization::MeshQualityColoringRequest request;
  request.metric = static_cast<mesh_quality::MeshQualityMetric>(selected);
  request.profile.id = "workbench-explicit/1";
  if (request.metric != mesh_quality::MeshQualityMetric::TriangleNormalConsistent)
  {
    bool warningOk = false;
    bool criticalOk = false;
    const double warning = warningBoundary_->text().toDouble(&warningOk);
    const double critical = criticalBoundary_->text().toDouble(&criticalOk);
    if (!warningOk || !criticalOk || !std::isfinite(warning) || !std::isfinite(critical))
    {
      return std::nullopt;
    }
    request.profile.entries.push_back({request.metric, {warning, critical}});
  }
  return mesh_quality::validateThresholdProfile(request.profile)
    ? std::optional<visualization::MeshQualityColoringRequest>(request) : std::nullopt;
}

void QualityInspectionPanel::setColoringAvailable(bool available)
{
  coloringAvailable_ = available && meshRevision_ > 0;
  updateColoringControls();
}

void QualityInspectionPanel::updateColoringControls()
{
  const int selected = coloringMetric_->currentData().toInt();
  const bool numeric = selected >= 0
    && selected != static_cast<int>(mesh_quality::MeshQualityMetric::TriangleNormalConsistent);
  coloringMetric_->setEnabled(coloringAvailable_);
  warningBoundary_->setEnabled(coloringAvailable_ && numeric);
  criticalBoundary_->setEnabled(coloringAvailable_ && numeric);
  applyColoring_->setEnabled(coloringAvailable_ && (selected < 0 || coloringRequest().has_value()));
  const bool higher = numeric && mesh_quality::metricDirection(
    static_cast<mesh_quality::MeshQualityMetric>(selected)) == mesh_quality::MeshQualityDirection::HigherIsBetter;
  applyColoring_->setToolTip(numeric
    ? (higher ? tr("Critical boundary must be less than warning boundary")
              : tr("Warning boundary must be less than critical boundary"))
    : QString{});
}

void QualityInspectionPanel::showAppliedColoring(bool succeeded)
{
  if (!succeeded)
  {
    coloringLegend_->setText(tr("Surface display unavailable. Apply to retry."));
    return;
  }
  const auto request = coloringRequest();
  if (!request)
  {
    coloringLegend_->clear();
    return;
  }
  if (request->metric == mesh_quality::MeshQualityMetric::TriangleNormalConsistent)
  {
    coloringLegend_->setText(tr("Applied normal consistency — Green: consistent | Red: inconsistent | Gray: unavailable"));
    return;
  }
  const auto thresholds = request->profile.entries.front().thresholds;
  const bool higher = mesh_quality::metricDirection(request->metric)
    == mesh_quality::MeshQualityDirection::HigherIsBetter;
  coloringLegend_->setText(tr("Applied %1 — Green: %2 %3 | Amber: between boundaries | Red: %4 %5 | Gray: unavailable")
    .arg(coloringMetric_->currentText(), higher ? ">=" : "<=")
    .arg(thresholds.warningBoundary).arg(higher ? "<" : ">")
    .arg(thresholds.criticalBoundary));
}
}
