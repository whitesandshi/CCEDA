#pragma once

#include "application/MeshingJobController.h"
#include "visualization/IViewportController.h"

#include <QWidget>

#include <memory>

class QLabel;
class QCheckBox;
class QTableView;
class QTableWidget;
class QComboBox;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace cceda::ui
{
class QualityInspectionPanel final : public QWidget
{
  Q_OBJECT

public:
  explicit QualityInspectionPanel(QWidget* parent = nullptr);
  ~QualityInspectionPanel() override;

  void showQuality(
    std::shared_ptr<const application::AcceptedMeshQuality> quality);
  void clearQuality();
  void setIssueOnlyDisplayAvailable(bool available);
  void setColoringAvailable(bool available);
  void showAppliedColoring(bool succeeded);
  [[nodiscard]] std::optional<visualization::MeshQualityColoringRequest> coloringRequest() const;

signals:
  void coloringRequested(cceda::domain::MeshRevision meshRevision);
  void issueElementSelected(
    cceda::domain::MeshRevision meshRevision,
    cceda::domain::MeshElementId elementId);
  void issueOnlyDisplayChanged(
    cceda::domain::MeshRevision meshRevision, bool enabled);

private:
  class SummaryModel;
  class IssueModel;
  void createColoringControls(QVBoxLayout* layout);
  void updateColoringControls();

  void showIssueDetail(
    const mesh_quality::MeshQualityElementDetail* detail);

  QLabel* status_ = nullptr;
  QCheckBox* issueOnlyDisplay_ = nullptr;
  QTableView* summaries_ = nullptr;
  QTableView* issues_ = nullptr;
  QTableWidget* issueDetails_ = nullptr;
  std::unique_ptr<SummaryModel> summaryModel_;
  std::unique_ptr<IssueModel> issueModel_;
  domain::MeshRevision meshRevision_ = 0;
  bool hasIssues_ = false;
  QComboBox* coloringMetric_ = nullptr;
  QLineEdit* warningBoundary_ = nullptr;
  QLineEdit* criticalBoundary_ = nullptr;
  QPushButton* applyColoring_ = nullptr;
  QLabel* coloringLegend_ = nullptr;
  bool coloringAvailable_ = false;
};
}
