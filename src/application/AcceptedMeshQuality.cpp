#include "application/AcceptedMeshQuality.h"
#include "meshing/MeshQualityJson.h"

#include <utility>

namespace cceda::application
{
std::shared_ptr<AcceptedMeshQuality> makeAcceptedMeshQuality(
  meshing::MeshQualityJsonArtifact artifact,
  mesh_quality::MeshQualityIssueDetails issueElementDetails,
  domain::MeshRevision committedMeshRevision)
{
  auto accepted = std::make_shared<AcceptedMeshQuality>();
  accepted->meshRevision = committedMeshRevision;
  accepted->sourceGeometryRevision = artifact.sourceGeometryRevision;
  accepted->metricVersion = std::move(artifact.metricVersion);
  accepted->validForCommit = artifact.validForCommit;
  accepted->triangleCount = artifact.triangleCount;
  accepted->tetrahedronCount = artifact.tetrahedronCount;
  accepted->numericSummaries.reserve(artifact.numericSummaries.size());
  for (meshing::MeshQualityNumericSummary& summary : artifact.numericSummaries)
  {
    accepted->numericSummaries.push_back({
      summary.metric,
      summary.sampleCount,
      summary.minimum,
      summary.maximum,
      summary.mean,
    });
  }
  accepted->booleanSummary = {
    artifact.booleanSummary.metric,
    artifact.booleanSummary.sampleCount,
    artifact.booleanSummary.failedElementCount,
  };
  accepted->issues = std::move(artifact.issues);
  accepted->issueElementDetails = std::move(issueElementDetails);
  return accepted;
}
}
