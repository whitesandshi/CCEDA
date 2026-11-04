#pragma once

#include "domain/ProjectState.h"
#include "domain/MeshGenerationSettings.h"
#include "domain/MeshingResourceLimits.h"
#include "domain/MeshingRetryPolicy.h"
#include "mesh_quality/MeshQuality.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace cceda::application
{
struct MeshingRuntimeConfiguration
{
  QString workerExecutable;
  QString backendExecutable;
  QString jobsRoot;
  QString mainBuildId;
  std::uint64_t maximumGeometryBytes = 0;
};

struct MeshingJobSubmission
{
  QString jobId;
  QString geometrySnapshotBrep;
  QString geometrySnapshotMap;
  QString geometrySnapshotSha256;
  QString geometrySnapshotMapSha256;
  domain::MeshGenerationSettings settings;
  domain::MeshingResourceLimits resourceLimits;
  domain::MeshingRetryPolicy retryPolicy;
  domain::MeshGenerationMode mode = domain::MeshGenerationMode::IndependentSolids;
};

struct MeshingJobProgress
{
  QString jobId;
  domain::GeometryRevision sourceGeometryRevision = 0;
  QString stage;
  std::optional<int> percent;
  std::uint64_t elapsedMilliseconds = 0;
  std::uint64_t workingSetBytes = 0;
  std::uint32_t attemptNumber = 1;
  std::uint32_t maximumAttempts = 1;
};

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

struct ExperimentalMeshingJobResult
{
  QString jobId;
  domain::GeometryRevision sourceGeometryRevision = 0;
  QString workspace;
  QString manifestPath;
  QString intermediateMeshPath;
  QString meshSha256;
  QString neutralMeshPath;
  QString meshBinarySha256;
  QString geometryMapResultPath;
  QString geometryMapResultSha256;
  QString qualityPath;
  QString qualitySha256;
  std::uint64_t nodeCount = 0;
  std::uint64_t triangleCount = 0;
  std::uint64_t tetrahedronCount = 0;
  std::uint64_t elapsedMilliseconds = 0;
  std::uint64_t peakWorkingSetBytes = 0;
  domain::MeshRevision committedMeshRevision = 0;
  domain::MeshGenerationSettings effectiveSettings;
  domain::MeshingResourceLimits effectiveResourceLimits;
  std::shared_ptr<const AcceptedMeshQuality> quality;
  std::uint32_t attemptCount = 1;
};

struct MeshingJobCancellation
{
  QString jobId;
  domain::GeometryRevision sourceGeometryRevision = 0;
  QString reason;
  QString stage;
  bool forced = false;
};

enum class MeshingJobFailureKind
{
  InvalidRequest,
  AlreadyRunning,
  WorkspacePreparationFailed,
  WorkerFailure,
  StaleGeometry,
  OutputValidationFailed,
  ProjectCommitFailed,
};

struct MeshingJobFailure
{
  MeshingJobFailureKind kind = MeshingJobFailureKind::InvalidRequest;
  QString jobId;
  domain::GeometryRevision sourceGeometryRevision = 0;
  QString code;
  QString stage;
  QString detail;
  bool retryable = false;
  std::uint32_t attemptCount = 0;
};

// Orchestrates one experimental executable-backed job and commits a verified
// mesh to ProjectState. The referenced ProjectState must outlive the controller.
class MeshingJobController final : public QObject
{
  Q_OBJECT

public:
  MeshingJobController(
    domain::ProjectState& projectState,
    MeshingRuntimeConfiguration configuration,
    QObject* parent = nullptr);
  ~MeshingJobController() override;

  [[nodiscard]] std::optional<MeshingJobFailure> submit(
    const MeshingJobSubmission& submission);
  // deadlineMilliseconds bounds the wait for Worker cooperation (1..60000).
  [[nodiscard]] std::optional<MeshingJobFailure> cancel(
    const QString& reason,
    int deadlineMilliseconds);
  [[nodiscard]] bool isRunning() const noexcept;
  [[nodiscard]] bool isCancelling() const noexcept;

signals:
  void progressChanged(const cceda::application::MeshingJobProgress& progress);
  void experimentalSucceeded(
    const cceda::application::ExperimentalMeshingJobResult& result);
  void cancelling(
    const cceda::application::MeshingJobCancellation& cancellation);
  void cancelled(
    const cceda::application::MeshingJobCancellation& cancellation);
  void failed(const cceda::application::MeshingJobFailure& failure);

private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};
}
