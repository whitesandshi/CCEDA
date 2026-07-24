#pragma once

#include "domain/ProjectState.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>

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
  QString geometrySnapshotStep;
  double minimumSizeMm = 0.0;
  double maximumSizeMm = 0.0;
};

struct MeshingJobProgress
{
  QString jobId;
  domain::GeometryRevision sourceGeometryRevision = 0;
  QString stage;
  std::optional<int> percent;
};

struct ExperimentalMeshingJobResult
{
  QString jobId;
  domain::GeometryRevision sourceGeometryRevision = 0;
  QString workspace;
  QString manifestPath;
  QString intermediateMeshPath;
  QString meshSha256;
  std::uint64_t nodeCount = 0;
  std::uint64_t triangleCount = 0;
  std::uint64_t tetrahedronCount = 0;
};

enum class MeshingJobFailureKind
{
  InvalidRequest,
  AlreadyRunning,
  WorkspacePreparationFailed,
  WorkerFailure,
  StaleGeometry,
  OutputValidationFailed,
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
};

// Orchestrates one experimental executable-backed job without committing it
// to ProjectState. The referenced ProjectState must outlive the controller.
class MeshingJobController final : public QObject
{
  Q_OBJECT

public:
  MeshingJobController(
    const domain::ProjectState& projectState,
    MeshingRuntimeConfiguration configuration,
    QObject* parent = nullptr);
  ~MeshingJobController() override;

  [[nodiscard]] std::optional<MeshingJobFailure> submit(
    const MeshingJobSubmission& submission);
  [[nodiscard]] bool isRunning() const noexcept;

signals:
  void progressChanged(const cceda::application::MeshingJobProgress& progress);
  void experimentalSucceeded(
    const cceda::application::ExperimentalMeshingJobResult& result);
  void failed(const cceda::application::MeshingJobFailure& failure);

private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};
}
