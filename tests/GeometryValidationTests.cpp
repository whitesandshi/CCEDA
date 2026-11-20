#include "geometry/OccGeometryRepository.h"
#include "geometry/OccGeometryValidationService.h"

#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>

#include <iostream>
#include <iterator>

int main()
{
  using namespace cceda::geometry;
  using namespace cceda::domain;

  const TopoDS_Shape box = BRepPrimAPI_MakeBox(1.0, 2.0, 3.0).Shape();
  const TopoDS_Shape face = TopExp_Explorer(box, TopAbs_FACE).Current();
  BRep_Builder builder;
  TopoDS_Shell openShell;
  builder.MakeShell(openShell);
  builder.Add(openShell, face);
  TopoDS_Solid openSolid;
  builder.MakeSolid(openSolid);
  builder.Add(openSolid, openShell);

  auto repository = std::make_shared<OccGeometryRepository>();
  GeometryDocument document;
  constexpr GeometryRevision revision = 7;
  document.revision = revision;
  for (const auto* id : {"open-solid", "box", "null-shape", "missing-solid", "wrong-shape"})
  {
    GeometryEntity entity;
    entity.id = id;
    entity.kind = GeometryKind::Solid;
    document.entities.push_back(entity);
  }
  GeometryEntity indexedFace;
  indexedFace.id = "face";
  indexedFace.kind = GeometryKind::Face;
  document.entities.push_back(indexedFace);
  repository->replace({}, document, {}, {{"box", box}, {"open-solid", openSolid},
    {"face", face}, {"null-shape", {}}, {"wrong-shape", face}});
  const OccGeometryValidationService adapter(repository);
  const IGeometryValidationService& service = adapter;

  const auto invalid = service.checkSolidBrep(revision, "open-solid");
  if (invalid.revision != revision || invalid.targetId != "open-solid" ||
      invalid.status != SolidBrepCheckStatus::Invalid || !invalid.issue ||
      invalid.issue->id.empty() || invalid.issue->code != "GEO_INVALID_BREP" ||
      invalid.issue->category != GeometryIssueCategory::Validity ||
      invalid.issue->severity != GeometryIssueSeverity::Error ||
      invalid.issue->summaryKey.empty() ||
      invalid.issue->repairability != GeometryIssueRepairability::None)
  {
    std::cerr << "A Solid with an open shell must produce a scoped error\n";
    return 1;
  }

  const auto expectNoIssue = [&](GeometryRevision requestedRevision, const std::string& targetId,
                                 SolidBrepCheckStatus expected)
  {
    const auto result = service.checkSolidBrep(requestedRevision, targetId);
    if (result.revision == requestedRevision && result.targetId == targetId &&
        result.status == expected && !result.issue)
    {
      return true;
    }
    std::cerr << "Unexpected check outcome for target: " << targetId << '\n';
    return false;
  };
  if (!expectNoIssue(revision, "box", SolidBrepCheckStatus::Valid) ||
      !expectNoIssue(0, "box", SolidBrepCheckStatus::InvalidRequest) ||
      !expectNoIssue(revision, "", SolidBrepCheckStatus::InvalidRequest) ||
      !expectNoIssue(revision, "unknown", SolidBrepCheckStatus::TargetNotFound) ||
      !expectNoIssue(revision, "face", SolidBrepCheckStatus::UnsupportedTarget) ||
      !expectNoIssue(revision, "null-shape", SolidBrepCheckStatus::CheckUnavailable))
  {
    return 1;
  }

  const auto report = service.checkDocumentBrep(revision);
  const SolidBrepCheckStatus expectedChecks[] = {
    SolidBrepCheckStatus::Invalid, SolidBrepCheckStatus::Valid,
    SolidBrepCheckStatus::CheckUnavailable, SolidBrepCheckStatus::TargetNotFound,
    SolidBrepCheckStatus::UnsupportedTarget,
  };
  if (report.revision != revision || report.status != GeometryValidationReportStatus::Completed ||
      report.solidChecks.size() != std::size(expectedChecks))
  {
    std::cerr << "A report must cover every indexed Solid at the requested revision\n";
    return 1;
  }
  for (std::size_t i = 0; i < report.solidChecks.size(); ++i)
  {
    const auto& check = report.solidChecks[i];
    if (check.revision != revision || check.targetId != document.entities[i].id ||
        check.status != expectedChecks[i] ||
        check.issue.has_value() != (check.status == SolidBrepCheckStatus::Invalid) ||
        (check.issue && check.issue->code != "GEO_INVALID_BREP"))
    {
      std::cerr << "The report must preserve each Solid outcome in document order\n";
      return 1;
    }
  }

  const auto original = repository->lookupTopologyShape(revision, "open-solid");
  if (original.status != OccShapeLookupStatus::Found || !original.shape.IsEqual(openSolid))
  {
    std::cerr << "Validation must leave the original shape and revision in place\n";
    return 1;
  }

  document.revision = revision + 1;
  document.entities = {indexedFace};
  repository->replace({}, document, {}, {{"open-solid", box}});
  const auto expectEmptyReport = [](const IGeometryValidationService& validation,
                                    GeometryRevision requestedRevision,
                                    GeometryValidationReportStatus expected)
  {
    const auto result = validation.checkDocumentBrep(requestedRevision);
    return result.revision == requestedRevision && result.status == expected &&
      result.solidChecks.empty();
  };
  const OccGeometryValidationService unavailable(nullptr);
  if (!expectEmptyReport(service, revision, GeometryValidationReportStatus::RevisionMismatch) ||
      !expectEmptyReport(service, 0, GeometryValidationReportStatus::InvalidRequest) ||
      !expectEmptyReport(service, document.revision, GeometryValidationReportStatus::Completed) ||
      !expectEmptyReport(unavailable, revision, GeometryValidationReportStatus::CheckUnavailable))
  {
    std::cerr << "Empty and rejected reports must preserve their distinct outcomes\n";
    return 1;
  }
  return expectNoIssue(revision, "open-solid", SolidBrepCheckStatus::RevisionMismatch) &&
    expectNoIssue(document.revision, "open-solid", SolidBrepCheckStatus::Valid) ? 0 : 1;
}
