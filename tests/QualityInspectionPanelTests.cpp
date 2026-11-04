#include "ui/QualityInspectionPanel.h"

#include <QApplication>
#include <QAbstractItemModel>
#include <QCheckBox>
#include <QLabel>
#include <QTableView>
#include <QTableWidget>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>

namespace
{
int failures = 0;

void expect(bool condition, std::string_view message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
}

int main(int argc, char* argv[])
{
  QApplication application(argc, argv);
  cceda::ui::QualityInspectionPanel panel;

  auto* status = panel.findChild<QLabel*>("qualityInspectionStatus");
  auto* summaries = panel.findChild<QTableView*>("qualitySummaryTable");
  auto* issues = panel.findChild<QTableView*>("qualityIssueTable");
  auto* details = panel.findChild<QTableWidget*>("qualityIssueDetailsTable");
  auto* issueOnly = panel.findChild<QCheckBox*>("qualityIssueOnlyDisplayCheckBox");
  expect(status && summaries && issues && details && issueOnly,
    "quality inspection exposes status, summary, issue, detail, and issue-only views");
  if (!status || !summaries || !issues || !details || !issueOnly)
  {
    return EXIT_FAILURE;
  }
  expect(summaries->model()->rowCount() == 0 && issues->model()->rowCount() == 0,
    "quality inspection starts empty");
  expect(!issueOnly->isEnabled() && !issueOnly->isChecked(),
    "issue-only display starts disabled and clear");

  auto quality = std::make_shared<cceda::application::AcceptedMeshQuality>();
  quality->meshRevision = 7;
  quality->sourceGeometryRevision = 3;
  quality->metricVersion = QStringLiteral("cceda.mesh-quality/0.1");
  quality->validForCommit = true;
  quality->triangleCount = 12;
  quality->tetrahedronCount = 4;
  quality->numericSummaries.push_back({
    cceda::mesh_quality::MeshQualityMetric::TriangleArea,
    12,
    0.25,
    4.0,
    1.5,
  });
  quality->booleanSummary = {
    cceda::mesh_quality::MeshQualityMetric::TriangleNormalConsistent,
    12,
    2,
  };
  quality->issues.push_back({
    cceda::mesh_quality::MeshQualityIssueCode::InconsistentTriangleOrientation,
    42,
  });
  quality->issues.push_back({
    cceda::mesh_quality::MeshQualityIssueCode::InvertedTetrahedron,
    84,
  });
  cceda::mesh_quality::MeshQualityElementDetail detail;
  detail.elementId = 42;
  detail.elementType = cceda::domain::MeshElementType::Triangle3;
  detail.nodeIds = {10, 20, 30, 0};
  detail.nodeCount = 3;
  detail.regionId = 5;
  detail.geometryId = "solid-1/face-2";
  detail.triangleQuality = cceda::mesh_quality::TriangleQuality{
    42, 0.75, 1.5, 30.0, 120.0, false};
  quality->issueElementDetails.emplace(42, std::move(detail));
  cceda::mesh_quality::MeshQualityElementDetail tetrahedronDetail;
  tetrahedronDetail.elementId = 84;
  tetrahedronDetail.elementType = cceda::domain::MeshElementType::Tetrahedron4;
  tetrahedronDetail.nodeIds = {10, 20, 30, 40};
  tetrahedronDetail.nodeCount = 4;
  tetrahedronDetail.regionId = 6;
  tetrahedronDetail.geometryId = "solid-2";
  tetrahedronDetail.tetrahedronQuality = cceda::mesh_quality::TetrahedronQuality{
    84, 0.125, 1.2, 65.0, 0.8};
  quality->issueElementDetails.emplace(84, std::move(tetrahedronDetail));

  panel.showQuality(quality);
  expect(status->text().contains("7") && status->text().contains("3")
      && status->text().contains("accepted", Qt::CaseInsensitive),
    "quality status identifies the committed mesh and geometry revisions");
  expect(summaries->model()->rowCount() == 2,
    "numeric and boolean quality summaries are both exposed");
  expect(summaries->model()->index(0, 0).data().toString()
      == "triangle.area.mm2",
    "summary rows expose stable metric identifiers");
  expect(summaries->model()->index(0, 1).data().toULongLong() == 12
      && summaries->model()->index(0, 2).data().toDouble() == 0.25
      && summaries->model()->index(0, 4).data().toDouble() == 4.0,
    "numeric summary cells preserve typed values");
  expect(summaries->model()->index(1, 5).data().toULongLong() == 2,
    "boolean summary exposes failed element count");
  expect(issues->model()->rowCount() == 2
      && issues->model()->index(0, 1).data().toULongLong() == 42
      && issues->model()->index(0, 2).data().toString()
        == "inconsistent-triangle-orientation",
    "issue rows preserve exact element IDs and stable issue codes");
  expect(!issues->isHidden(),
    "the issue table remains visible when accepted issues exist");
  expect(!issueOnly->isEnabled() && !issueOnly->isChecked(),
    "issue-only display waits for the matching mesh presentation");
  panel.setIssueOnlyDisplayAvailable(true);
  expect(issueOnly->isEnabled() && !issueOnly->isChecked(),
    "a matching mesh presentation enables the issue-only display control");

  std::optional<cceda::domain::MeshRevision> selectedMeshRevision;
  std::optional<cceda::domain::MeshElementId> selectedElementId;
  QObject::connect(&panel,
    &cceda::ui::QualityInspectionPanel::issueElementSelected,
    &panel,
    [&](cceda::domain::MeshRevision meshRevision,
        cceda::domain::MeshElementId elementId) {
      selectedMeshRevision = meshRevision;
      selectedElementId = elementId;
    });
  std::optional<cceda::domain::MeshRevision> issueOnlyMeshRevision;
  std::optional<bool> issueOnlyEnabled;
  QObject::connect(&panel,
    &cceda::ui::QualityInspectionPanel::issueOnlyDisplayChanged,
    &panel,
    [&](cceda::domain::MeshRevision meshRevision, bool enabled) {
      issueOnlyMeshRevision = meshRevision;
      issueOnlyEnabled = enabled;
    });
  issueOnly->setChecked(true);
  expect(issueOnlyMeshRevision == 7 && issueOnlyEnabled == true,
    "issue-only display changes publish the accepted mesh revision");
  issues->setCurrentIndex(issues->model()->index(0, 0));
  expect(selectedMeshRevision == 7 && selectedElementId == 42,
    "selecting an issue publishes its accepted mesh revision and exact element ID");
  expect(!details->isHidden() && details->rowCount() == 10,
    "selecting an issue exposes metadata and every applicable triangle metric");
  expect(details->item(0, 1)->text() == "42"
      && details->item(1, 1)->text() == "Triangle3"
      && details->item(2, 1)->text() == "10, 20, 30"
      && details->item(4, 1)->text() == "solid-1/face-2",
    "selected issue details preserve identity, type, nodes, and geometry association");
  expect(details->item(9, 0)->text() == "triangle.normal-consistent"
      && details->item(9, 1)->text() == "No",
    "selected issue details expose stable metric IDs and typed values");
  issues->setCurrentIndex(issues->model()->index(1, 0));
  expect(details->rowCount() == 9
      && details->item(1, 1)->text() == "Tetrahedron4"
      && details->item(2, 1)->text() == "10, 20, 30, 40"
      && details->item(8, 0)->text() == "tetrahedron.scaled-jacobian"
      && details->item(8, 1)->text() == "0.8",
    "tetrahedron selection exposes its four nodes and all applicable metrics");

  auto cleanQuality =
    std::make_shared<cceda::application::AcceptedMeshQuality>(*quality);
  cleanQuality->issues.clear();
  panel.showQuality(cleanQuality);
  expect(issues->isHidden(),
    "an empty issue table does not consume quality summary space");
  expect(!issueOnly->isEnabled() && !issueOnly->isChecked(),
    "replacing quality without issues disables and clears issue-only display");

  panel.clearQuality();
  expect(summaries->model()->rowCount() == 0 && issues->model()->rowCount() == 0,
    "clearing quality removes stale inspection data");
  expect(details->isHidden() && details->rowCount() == 0,
    "clearing quality removes the selected issue detail view");
  expect(!issueOnly->isEnabled() && !issueOnly->isChecked(),
    "clearing quality leaves issue-only display disabled and clear");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
