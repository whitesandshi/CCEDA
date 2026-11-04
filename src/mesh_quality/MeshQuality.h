#pragma once

#include "domain/MeshModel.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cceda::mesh_quality
{
inline constexpr std::string_view MeshQualityMetricVersion =
  "cceda.mesh-quality/0.1";

enum class MeshQualityMetric
{
  TriangleArea,
  TriangleAspectRatio,
  TriangleMinimumAngle,
  TriangleMaximumAngle,
  TriangleNormalConsistent,
  TetrahedronSignedVolume,
  TetrahedronAspectRatio,
  TetrahedronMinimumDihedralAngle,
  TetrahedronScaledJacobian,
};

enum class MeshQualityDirection
{
  HigherIsBetter,
  LowerIsBetter,
  PositiveRequired,
  TrueRequired,
};

[[nodiscard]] std::string_view stableMetricId(
  MeshQualityMetric metric) noexcept;
[[nodiscard]] MeshQualityDirection metricDirection(
  MeshQualityMetric metric) noexcept;

struct TriangleQuality
{
  domain::MeshElementId elementId = 0;
  double areaSquareMillimeters = 0.0;
  std::optional<double> aspectRatio;
  std::optional<double> minimumAngleDegrees;
  std::optional<double> maximumAngleDegrees;
  bool normalConsistent = true;
};

struct TetrahedronQuality
{
  domain::MeshElementId elementId = 0;
  double signedVolumeCubicMillimeters = 0.0;
  std::optional<double> aspectRatio;
  std::optional<double> minimumDihedralAngleDegrees;
  std::optional<double> scaledJacobian;
};

enum class MeshQualityIssueCode
{
  DegenerateTriangle,
  InconsistentTriangleOrientation,
  NonManifoldSurfaceEdge,
  DegenerateTetrahedron,
  InvertedTetrahedron,
  NonFiniteMetric,
};

[[nodiscard]] std::string_view stableIssueId(
  MeshQualityIssueCode code) noexcept;
[[nodiscard]] bool isCommitBlockingIssue(
  MeshQualityIssueCode code) noexcept;

struct MeshQualityIssue
{
  MeshQualityIssueCode code = MeshQualityIssueCode::NonFiniteMetric;
  domain::MeshElementId elementId = 0;
};

struct MeshQualityReport
{
  domain::MeshRevision meshRevision = 0;
  domain::GeometryRevision sourceGeometryRevision = 0;
  std::vector<TriangleQuality> triangles;
  std::vector<TetrahedronQuality> tetrahedra;
  std::vector<MeshQualityIssue> issues;

  [[nodiscard]] bool validForCommit() const noexcept;
};

struct MeshQualityElementDetail
{
  domain::MeshElementId elementId = 0;
  domain::MeshElementType elementType = domain::MeshElementType::Triangle3;
  std::array<domain::MeshNodeId, 4> nodeIds{};
  std::size_t nodeCount = 0;
  domain::MeshRegionId regionId = 0;
  std::string geometryId;
  std::optional<TriangleQuality> triangleQuality;
  std::optional<TetrahedronQuality> tetrahedronQuality;
};

using MeshQualityIssueDetails = std::unordered_map<
  domain::MeshElementId, MeshQualityElementDetail>;

enum class MeshQualityEvaluationCode
{
  None,
  InvalidMesh,
};

struct MeshQualityEvaluationResult
{
  std::optional<MeshQualityReport> report;
  MeshQualityEvaluationCode code = MeshQualityEvaluationCode::None;
  std::vector<domain::MeshValidationIssue> meshValidationIssues;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return code == MeshQualityEvaluationCode::None && report.has_value();
  }
};

[[nodiscard]] MeshQualityEvaluationResult evaluateMeshQuality(
  const domain::MeshModel& mesh,
  domain::MeshValidationMode validationMode);

// Builds a direct lookup bounded by unique issue element IDs. A mismatch means
// the report was not produced from the supplied mesh and fails closed.
[[nodiscard]] std::optional<MeshQualityIssueDetails>
buildMeshQualityIssueDetails(
  const domain::MeshModel& mesh,
  const MeshQualityReport& report);
}
