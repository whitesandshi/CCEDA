#include "geometry/OccGeometryValidationService.h"

#include "geometry/OccGeometryRepository.h"

#include <BRepCheck_Analyzer.hxx>
#include <Standard_Failure.hxx>
#include <QUuid>

#include <utility>

namespace cceda::geometry
{
namespace
{
domain::SolidBrepCheckResult checkShape(
  domain::GeometryRevision revision, const std::string& targetId,
  const TopoDS_Shape& shape)
{
  using domain::SolidBrepCheckStatus;
  domain::SolidBrepCheckResult result{revision, targetId};
  if (shape.IsNull())
  {
    return result;
  }
  if (shape.ShapeType() != TopAbs_SOLID)
  {
    result.status = SolidBrepCheckStatus::UnsupportedTarget;
    return result;
  }

  try
  {
    if (BRepCheck_Analyzer(shape).IsValid())
    {
      result.status = SolidBrepCheckStatus::Valid;
      return result;
    }
  }
  catch (const Standard_Failure&)
  {
    return result;
  }

  result.status = SolidBrepCheckStatus::Invalid;
  result.issue = domain::GeometryIssue{
    QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
    "GEO_INVALID_BREP", domain::GeometryIssueCategory::Validity,
    domain::GeometryIssueSeverity::Error, "geometry.validation.invalidBrep",
    domain::GeometryIssueRepairability::None};
  return result;
}
}

OccGeometryValidationService::OccGeometryValidationService(
  std::shared_ptr<const OccGeometryRepository> repository)
  : repository_(std::move(repository))
{
}

domain::SolidBrepCheckResult OccGeometryValidationService::checkSolidBrep(
  domain::GeometryRevision revision, const std::string& targetId) const
{
  using domain::SolidBrepCheckStatus;
  if (revision == 0 || targetId.empty())
  {
    return {revision, targetId, SolidBrepCheckStatus::InvalidRequest};
  }
  if (!repository_)
  {
    return {revision, targetId};
  }

  const auto target = repository_->lookupTopologyShape(revision, targetId);
  if (target.status == OccShapeLookupStatus::RevisionMismatch)
  {
    return {revision, targetId, SolidBrepCheckStatus::RevisionMismatch};
  }
  if (target.status == OccShapeLookupStatus::NotFound)
  {
    return {revision, targetId, SolidBrepCheckStatus::TargetNotFound};
  }
  return checkShape(revision, targetId, target.shape);
}

domain::GeometryValidationReport OccGeometryValidationService::checkDocumentBrep(
  domain::GeometryRevision revision) const
{
  using domain::GeometryValidationReportStatus;
  domain::GeometryValidationReport report{revision};
  if (revision == 0)
  {
    report.status = GeometryValidationReportStatus::InvalidRequest;
    return report;
  }
  if (!repository_)
  {
    return report;
  }

  const auto snapshot = repository_->topologySnapshot(revision);
  if (snapshot.status == OccShapeLookupStatus::RevisionMismatch)
  {
    report.status = GeometryValidationReportStatus::RevisionMismatch;
    return report;
  }
  if (snapshot.status != OccShapeLookupStatus::Found)
  {
    return report;
  }

  for (const auto& entity : snapshot.document.entities)
  {
    if (entity.kind != domain::GeometryKind::Solid)
    {
      continue;
    }
    const auto shape = snapshot.topologyShapes.find(entity.id);
    if (shape == snapshot.topologyShapes.end())
    {
      report.solidChecks.push_back({revision, entity.id, domain::SolidBrepCheckStatus::TargetNotFound});
      continue;
    }
    report.solidChecks.push_back(checkShape(revision, entity.id, shape->second));
  }
  report.status = GeometryValidationReportStatus::Completed;
  return report;
}
}
