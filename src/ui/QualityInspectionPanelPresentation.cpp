#include "ui/QualityInspectionPanel.h"

#include <QTableWidget>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <optional>

namespace cceda::ui
{
namespace
{
QString metricId(mesh_quality::MeshQualityMetric metric)
{
  const std::string_view id = mesh_quality::stableMetricId(metric);
  return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
}

QString metricValue(std::optional<double> value)
{
  return value && std::isfinite(*value)
    ? QString::number(*value, 'g', 12)
    : QualityInspectionPanel::tr("Unavailable");
}

QString metricValue(double value)
{
  return std::isfinite(value)
    ? QString::number(value, 'g', 12)
    : QualityInspectionPanel::tr("Unavailable");
}
}

void QualityInspectionPanel::showIssueDetail(
  const mesh_quality::MeshQualityElementDetail* detail)
{
  issueDetails_->clearContents();
  issueDetails_->setRowCount(0);
  if (detail == nullptr)
  {
    issueDetails_->hide();
    return;
  }

  QStringList nodeIds;
  const std::size_t nodeCount = std::min(
    detail->nodeCount, detail->nodeIds.size());
  for (std::size_t index = 0; index < nodeCount; ++index)
  {
    nodeIds.push_back(QString::number(
      static_cast<qulonglong>(detail->nodeIds.at(index))));
  }

  QList<QPair<QString, QString>> values{
    {tr("Element ID"), QString::number(
      static_cast<qulonglong>(detail->elementId))},
    {tr("Type"), detail->elementType == domain::MeshElementType::Triangle3
      ? QStringLiteral("Triangle3") : QStringLiteral("Tetrahedron4")},
    {tr("Node IDs"), nodeIds.join(", ")},
    {tr("Region ID"), QString::number(
      static_cast<qulonglong>(detail->regionId))},
    {tr("Geometry"), detail->geometryId.empty()
      ? tr("None") : QString::fromStdString(detail->geometryId)},
  };

  if (detail->triangleQuality)
  {
    const mesh_quality::TriangleQuality& quality = *detail->triangleQuality;
    values.append({metricId(mesh_quality::MeshQualityMetric::TriangleArea),
      metricValue(quality.areaSquareMillimeters)});
    values.append({metricId(mesh_quality::MeshQualityMetric::TriangleAspectRatio),
      metricValue(quality.aspectRatio)});
    values.append({metricId(mesh_quality::MeshQualityMetric::TriangleMinimumAngle),
      metricValue(quality.minimumAngleDegrees)});
    values.append({metricId(mesh_quality::MeshQualityMetric::TriangleMaximumAngle),
      metricValue(quality.maximumAngleDegrees)});
    values.append({metricId(mesh_quality::MeshQualityMetric::TriangleNormalConsistent),
      quality.normalConsistent ? tr("Yes") : tr("No")});
  }
  else if (detail->tetrahedronQuality)
  {
    const mesh_quality::TetrahedronQuality& quality =
      *detail->tetrahedronQuality;
    values.append({metricId(mesh_quality::MeshQualityMetric::TetrahedronSignedVolume),
      metricValue(quality.signedVolumeCubicMillimeters)});
    values.append({metricId(mesh_quality::MeshQualityMetric::TetrahedronAspectRatio),
      metricValue(quality.aspectRatio)});
    values.append({metricId(
      mesh_quality::MeshQualityMetric::TetrahedronMinimumDihedralAngle),
      metricValue(quality.minimumDihedralAngleDegrees)});
    values.append({metricId(mesh_quality::MeshQualityMetric::TetrahedronScaledJacobian),
      metricValue(quality.scaledJacobian)});
  }

  issueDetails_->setRowCount(values.size());
  for (qsizetype row = 0; row < values.size(); ++row)
  {
    issueDetails_->setItem(
      static_cast<int>(row), 0, new QTableWidgetItem(values.at(row).first));
    issueDetails_->setItem(
      static_cast<int>(row), 1, new QTableWidgetItem(values.at(row).second));
  }
  issueDetails_->show();
}
}
