#pragma once

#include "mesh_quality/MeshQuality.h"

#include <QByteArray>
#include <QIODevice>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace cceda::meshing
{
inline constexpr int MeshQualityJsonSchemaMajor = 0;
inline constexpr int MeshQualityJsonSchemaMinor = 1;
inline constexpr std::uint64_t DefaultMeshQualityJsonMaximumBytes =
  16ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t DefaultMeshQualityJsonMaximumIssues = 100000;

inline constexpr std::uint64_t DefaultMeshQualityJsonMaximumNestingDepth = 16;
struct MeshQualityJsonLimits
{
  std::uint64_t maximumInputBytes = DefaultMeshQualityJsonMaximumBytes;
  std::uint64_t maximumIssues = DefaultMeshQualityJsonMaximumIssues;
  std::uint64_t maximumNestingDepth =
    DefaultMeshQualityJsonMaximumNestingDepth;
};

struct MeshQualityNumericSummary
{
  mesh_quality::MeshQualityMetric metric =
    mesh_quality::MeshQualityMetric::TriangleArea;
  std::uint64_t sampleCount = 0;
  std::optional<double> minimum;
  std::optional<double> maximum;
  std::optional<double> mean;
};

struct MeshQualityBooleanSummary
{
  mesh_quality::MeshQualityMetric metric =
    mesh_quality::MeshQualityMetric::TriangleNormalConsistent;
  std::uint64_t sampleCount = 0;
  std::uint64_t failedElementCount = 0;
};

struct MeshQualityJsonArtifact
{
  QString jobId;
  domain::MeshRevision meshRevision = 0;
  domain::GeometryRevision sourceGeometryRevision = 0;
  QString metricVersion;
  bool validForCommit = false;
  std::uint64_t triangleCount = 0;
  std::uint64_t tetrahedronCount = 0;
  std::vector<MeshQualityNumericSummary> numericSummaries;
  MeshQualityBooleanSummary booleanSummary;
  std::vector<mesh_quality::MeshQualityIssue> issues;
};

enum class MeshQualityJsonErrorCode
{
  None,
  InvalidLimits,
  InputLimitExceeded,
  ReadFailed,
  InvalidJson,
  InvalidStructure,
  UnsupportedSchema,
  InvalidIdentity,
  CountLimitExceeded,
  InvalidSummary,
  InvalidIssue,
  InconsistentReport,
};

struct MeshQualityJsonReadResult
{
  std::optional<MeshQualityJsonArtifact> artifact;
  MeshQualityJsonErrorCode error = MeshQualityJsonErrorCode::None;
  QString detail;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return error == MeshQualityJsonErrorCode::None && artifact.has_value();
  }
};

struct MeshQualityJsonWriteResult
{
  QByteArray bytes;
  MeshQualityJsonErrorCode error = MeshQualityJsonErrorCode::None;
  QString detail;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return error == MeshQualityJsonErrorCode::None && !bytes.isEmpty();
  }
};

[[nodiscard]] MeshQualityJsonWriteResult writeMeshQualityJson(
  const mesh_quality::MeshQualityReport& report,
  const QString& jobId,
  const MeshQualityJsonLimits& limits = {});

[[nodiscard]] MeshQualityJsonReadResult readMeshQualityJson(
  QIODevice& input,
  const MeshQualityJsonLimits& limits = {});
}
