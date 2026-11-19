#pragma once
#include "mesh_quality/MeshQuality.h"

#include <QString>
#include <memory>

namespace cceda::meshing { struct MeshQualityJsonArtifact; }

namespace cceda::application
{
struct AcceptedMeshQualityNumericSummary
{
  mesh_quality::MeshQualityMetric metric =
    mesh_quality::MeshQualityMetric::TriangleArea;
  std::uint64_t sampleCount = 0;
  std::optional<double> minimum;
  std::optional<double> maximum;
  std::optional<double> mean;
};

struct AcceptedMeshQualityBooleanSummary
{
  mesh_quality::MeshQualityMetric metric =
    mesh_quality::MeshQualityMetric::TriangleNormalConsistent;
  std::uint64_t sampleCount = 0;
  std::uint64_t failedElementCount = 0;
};

// A bounded, immutable view of the quality result accepted for one exact
// committed mesh revision. Full per-element metric arrays remain
// evaluator-local; only unique issue element details are retained.
struct AcceptedMeshQuality
{
  domain::MeshRevision meshRevision = 0;
  domain::GeometryRevision sourceGeometryRevision = 0;
  QString metricVersion;
  bool validForCommit = false;
  std::uint64_t triangleCount = 0;
  std::uint64_t tetrahedronCount = 0;
  std::vector<AcceptedMeshQualityNumericSummary> numericSummaries;
  AcceptedMeshQualityBooleanSummary booleanSummary;
  std::vector<mesh_quality::MeshQualityIssue> issues;
  mesh_quality::MeshQualityIssueDetails issueElementDetails;
};

[[nodiscard]] std::shared_ptr<AcceptedMeshQuality> makeAcceptedMeshQuality(
  meshing::MeshQualityJsonArtifact artifact,
  mesh_quality::MeshQualityIssueDetails issueElementDetails,
  domain::MeshRevision committedMeshRevision);
}
