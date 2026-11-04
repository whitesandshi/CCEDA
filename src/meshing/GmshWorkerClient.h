#pragma once

#include "domain/Revisions.h"
#include "domain/MeshGenerationSettings.h"
#include "domain/MeshingResourceLimits.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>

namespace cceda::meshing
{
struct GmshWorkerLaunchRequest
{
  QString workerExecutable;
  QString gmshExecutable;
  QString workspace;
  QString jobId;
  domain::GeometryRevision sourceGeometryRevision = 0;
  QString mainBuildId;
  domain::MeshingResourceLimits resourceLimits;
};

struct GmshWorkerProgress
{
  QString stage;
  std::optional<int> percent;
  std::uint64_t elapsedMilliseconds = 0;
  std::uint64_t workingSetBytes = 0;
};

struct GmshWorkerResult
{
  domain::GeometryRevision sourceGeometryRevision = 0;
  QString resultPath;
  QString meshSha256;
  QString meshBinarySha256;
  QString geometryMapResultSha256;
  QString qualitySha256;
  std::uint64_t nodeCount = 0;
  std::uint64_t triangleCount = 0;
  std::uint64_t tetrahedronCount = 0;
  std::uint64_t elapsedMilliseconds = 0;
  std::uint64_t peakWorkingSetBytes = 0;
  domain::MeshGenerationSettings effectiveSettings;
  domain::MeshingResourceLimits effectiveResourceLimits;
};

struct GmshWorkerCancellation
{
  QString jobId;
  QString reason;
  QString stage;
  bool forced = false;
};

enum class GmshWorkerFailureKind
{
  InvalidRequest,
  ProtocolError,
  WorkerReportedError,
  ResourceLimitExceeded,
  WorkerLost,
};

struct GmshWorkerFailure
{
  GmshWorkerFailureKind kind = GmshWorkerFailureKind::InvalidRequest;
  QString code;
  QString stage;
  QString detail;
  bool retryable = false;
  int exitCode = 0;
};

class GmshWorkerClient final : public QObject
{
  Q_OBJECT

public:
  explicit GmshWorkerClient(QObject* parent = nullptr);
  ~GmshWorkerClient() override;

  [[nodiscard]] std::optional<GmshWorkerFailure> start(
    const GmshWorkerLaunchRequest& request);
  [[nodiscard]] std::optional<GmshWorkerFailure> cancel(
    const QString& reason,
    int deadlineMilliseconds);
  [[nodiscard]] bool isRunning() const noexcept;
  [[nodiscard]] bool isCancelling() const noexcept;

signals:
  void progressChanged(const cceda::meshing::GmshWorkerProgress& progress);
  void succeeded(const cceda::meshing::GmshWorkerResult& result);
  void cancelled(const cceda::meshing::GmshWorkerCancellation& cancellation);
  void failed(const cceda::meshing::GmshWorkerFailure& failure);

private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};
}
