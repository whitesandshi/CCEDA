#include "application/MeshingJobController.h"

#include "meshing/GmshExecutableJobWorkspace.h"
#include "meshing/GmshWorkerClient.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <cmath>
#include <utility>

namespace cceda::application
{
namespace
{
constexpr qint64 MaximumManifestBytes = 64 * 1024;
constexpr qint64 HashBufferBytes = 1024 * 1024;

struct ActiveJob
{
  MeshingJobSubmission submission;
  domain::GeometryRevision sourceGeometryRevision = 0;
  QString workspace;
  std::optional<meshing::PreparedGmshExecutableJob> preparedJob;
  std::optional<meshing::GmshWorkerResult> workerResult;
};

struct ArtifactValidation
{
  bool valid = false;
  QString detail;
};

bool hasValidJobId(const QString& jobId)
{
  static const QRegularExpression pattern(
    QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
  return pattern.match(jobId).hasMatch();
}

bool hasValidSettings(const MeshingJobSubmission& submission)
{
  return std::isfinite(submission.minimumSizeMm)
    && std::isfinite(submission.maximumSizeMm)
    && submission.minimumSizeMm >= 0.0
    && submission.maximumSizeMm >= 0.0
    && (submission.maximumSizeMm == 0.0
      || submission.minimumSizeMm <= submission.maximumSizeMm);
}

MeshingJobFailure makeFailure(
  MeshingJobFailureKind kind,
  const QString& jobId,
  domain::GeometryRevision sourceGeometryRevision,
  QString code,
  QString stage,
  QString detail,
  bool retryable = false)
{
  return {
    kind,
    jobId,
    sourceGeometryRevision,
    std::move(code),
    std::move(stage),
    std::move(detail),
    retryable,
  };
}

ArtifactValidation invalidArtifact(QString detail)
{
  return {false, std::move(detail)};
}

ArtifactValidation validateArtifacts(
  const ActiveJob& active)
{
  if (!active.preparedJob || !active.workerResult)
  {
    return invalidArtifact("Controller has no prepared job or Worker result");
  }

  const auto& prepared = *active.preparedJob;
  const auto& worker = *active.workerResult;
  const QString reportedManifest =
    QDir::cleanPath(QDir(prepared.workspace).filePath(worker.resultPath));
  if (reportedManifest != QDir::cleanPath(prepared.resultPath))
  {
    return invalidArtifact("Worker reported an unexpected result manifest path");
  }

  const QFileInfo manifestInfo(prepared.resultPath);
  const QFileInfo meshInfo(prepared.meshPath);
  if (!manifestInfo.isFile() || manifestInfo.isSymbolicLink()
    || manifestInfo.size() <= 0 || manifestInfo.size() > MaximumManifestBytes
    || !meshInfo.isFile() || meshInfo.isSymbolicLink() || meshInfo.size() <= 0)
  {
    return invalidArtifact("Worker output files are missing, unsafe, empty, or oversized");
  }

  QFile manifestFile(prepared.resultPath);
  if (!manifestFile.open(QIODevice::ReadOnly))
  {
    return invalidArtifact(
      QString("Cannot read result manifest: %1").arg(manifestFile.errorString()));
  }
  QJsonParseError parseError;
  const QJsonDocument manifest =
    QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !manifest.isObject())
  {
    return invalidArtifact("Worker result manifest is not valid JSON");
  }

  const QJsonObject root = manifest.object();
  const QJsonObject counts = root.value("counts").toObject();
  if (!root.value("experimental").toBool(false)
    || root.value("format").toString() != "gmsh-msh-4.1-intermediate"
    || root.value("jobId").toString() != active.submission.jobId
    || root.value("sourceGeometryRevision").toInteger(-1)
      != static_cast<qint64>(active.sourceGeometryRevision)
    || root.value("backendId").toString() != "gmsh"
    || QDir::cleanPath(QDir(prepared.workspace).filePath(
      root.value("meshPath").toString())) != QDir::cleanPath(prepared.meshPath)
    || root.value("meshSha256").toString().compare(
      worker.meshSha256, Qt::CaseInsensitive) != 0
    || counts.value("nodes").toInteger(-1) != static_cast<qint64>(worker.nodeCount)
    || counts.value("triangles").toInteger(-1)
      != static_cast<qint64>(worker.triangleCount)
    || counts.value("tetrahedra").toInteger(-1)
      != static_cast<qint64>(worker.tetrahedronCount))
  {
    return invalidArtifact(
      "Worker result manifest does not match the submitted job or control result");
  }

  QFile meshFile(prepared.meshPath);
  if (!meshFile.open(QIODevice::ReadOnly))
  {
    return invalidArtifact(
      QString("Cannot read intermediate mesh: %1").arg(meshFile.errorString()));
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!meshFile.atEnd())
  {
    const QByteArray chunk = meshFile.read(HashBufferBytes);
    if (chunk.isEmpty() && meshFile.error() != QFileDevice::NoError)
    {
      return invalidArtifact(
        QString("Cannot hash intermediate mesh: %1").arg(meshFile.errorString()));
    }
    hash.addData(QByteArrayView(chunk));
  }
  if (meshFile.size() != meshInfo.size())
  {
    return invalidArtifact("Intermediate mesh size changed during verification");
  }
  const QString actualHash = QString::fromLatin1(hash.result().toHex());
  if (actualHash.compare(worker.meshSha256, Qt::CaseInsensitive) != 0)
  {
    return invalidArtifact("Intermediate mesh SHA-256 does not match the Worker result");
  }
  return {true, {}};
}
}

class MeshingJobController::Implementation
{
public:
  Implementation(
    MeshingJobController* owner,
    const domain::ProjectState& projectState,
    MeshingRuntimeConfiguration configuration)
    : owner_(owner)
    , projectState_(projectState)
    , configuration_(std::move(configuration))
    , workerClient_(owner)
  {
    // A QObject context makes these functor connections disconnect when the
    // controller is destroyed.
    // https://doc.qt.io/qt-6.8/qobject.html#connect-4
    QObject::connect(&preparationWatcher_,
      &QFutureWatcher<meshing::GmshJobWorkspaceResult>::finished,
      owner_,
      [this] { preparationFinished(); });
    QObject::connect(&validationWatcher_,
      &QFutureWatcher<ArtifactValidation>::finished,
      owner_,
      [this] { validationFinished(); });
    QObject::connect(&workerClient_, &meshing::GmshWorkerClient::progressChanged,
      owner_, [this](const meshing::GmshWorkerProgress& progress) {
        workerProgressed(progress);
      });
    QObject::connect(&workerClient_, &meshing::GmshWorkerClient::succeeded,
      owner_, [this](const meshing::GmshWorkerResult& result) {
        workerSucceeded(result);
      });
    QObject::connect(&workerClient_, &meshing::GmshWorkerClient::cancelled,
      owner_, [this](const meshing::GmshWorkerCancellation& cancellation) {
        workerCancelled(cancellation);
      });
    QObject::connect(&workerClient_, &meshing::GmshWorkerClient::failed,
      owner_, [this](const meshing::GmshWorkerFailure& failure) {
        workerFailed(failure);
      });
  }

  std::optional<MeshingJobFailure> submit(const MeshingJobSubmission& submission)
  {
    const domain::GeometryRevision revision = projectState_.geometryRevision();
    if (state_ != State::Idle)
    {
      return makeFailure(
        MeshingJobFailureKind::AlreadyRunning,
        submission.jobId,
        revision,
        "MESH_JOB_ALREADY_RUNNING",
        "validating",
        "This project already has an active mesh job");
    }
    if (!hasValidSubmission(submission, revision))
    {
      return makeFailure(
        MeshingJobFailureKind::InvalidRequest,
        submission.jobId,
        revision,
        "MESH_INPUT_INVALID",
        "validating",
        "Meshing paths, job identity, revision, size budget, or settings are invalid");
    }

    active_ = ActiveJob{
      submission,
      revision,
      QDir::cleanPath(QDir(configuration_.jobsRoot).filePath(submission.jobId)),
      std::nullopt,
      std::nullopt,
    };
    lastStage_ = "initializing";
    state_ = State::Preparing;
    const meshing::GmshExecutableJobWorkspaceRequest request{
      active_->workspace,
      submission.geometrySnapshotStep,
      submission.jobId,
      revision,
      submission.minimumSizeMm,
      submission.maximumSizeMm,
      configuration_.maximumGeometryBytes,
    };

    // The run overload executes the copy/hash work on the global thread pool;
    // QFutureWatcher returns completion to this object's event loop.
    // https://doc.qt.io/qt-6.8/qtconcurrentrun.html#run
    preparationWatcher_.setFuture(
      QtConcurrent::run([request] {
        return meshing::prepareGmshExecutableJobWorkspace(request);
      }));
    return std::nullopt;
  }

  std::optional<MeshingJobFailure> cancel(
    const QString& reason,
    int deadlineMilliseconds)
  {
    const QString cleanReason = reason.trimmed();
    if (!active_ || state_ == State::Idle || isCancelling()
      || cleanReason.isEmpty() || cleanReason.size() > 256
      || deadlineMilliseconds <= 0 || deadlineMilliseconds > 60'000)
    {
      return makeFailure(
        MeshingJobFailureKind::InvalidRequest,
        active_ ? active_->submission.jobId : QString{},
        active_ ? active_->sourceGeometryRevision : projectState_.geometryRevision(),
        "MESH_CANCELLATION_INVALID",
        "cancelling",
        "Cancellation requires one active job, a bounded reason, and a valid deadline");
    }

    if (state_ == State::Running)
    {
      if (const auto failure =
          workerClient_.cancel(cleanReason, deadlineMilliseconds))
      {
        return makeFailure(
          MeshingJobFailureKind::InvalidRequest,
          active_->submission.jobId,
          active_->sourceGeometryRevision,
          failure->code,
          failure->stage,
          failure->detail,
          failure->retryable);
      }
    }

    cancellation_ = MeshingJobCancellation{
      active_->submission.jobId,
      active_->sourceGeometryRevision,
      cleanReason,
      lastStage_,
      false,
    };
    switch (state_)
    {
      case State::Preparing:
        state_ = State::CancellingPreparation;
        break;
      case State::Running:
        state_ = State::CancellingWorker;
        break;
      case State::Verifying:
        state_ = State::CancellingVerification;
        break;
      default:
        break;
    }
    emit owner_->cancelling(*cancellation_);
    return std::nullopt;
  }

  bool isRunning() const noexcept
  {
    return state_ != State::Idle;
  }

  bool isCancelling() const noexcept
  {
    return state_ == State::CancellingPreparation
      || state_ == State::CancellingWorker
      || state_ == State::CancellingVerification;
  }

private:
  enum class State
  {
    Idle,
    Preparing,
    Running,
    Verifying,
    CancellingPreparation,
    CancellingWorker,
    CancellingVerification,
  };

  bool hasValidSubmission(
    const MeshingJobSubmission& submission,
    domain::GeometryRevision revision) const
  {
    const QFileInfo workerInfo(configuration_.workerExecutable);
    const QFileInfo backendInfo(configuration_.backendExecutable);
    const QFileInfo snapshotInfo(submission.geometrySnapshotStep);
    return revision > 0
      && hasValidJobId(submission.jobId)
      && hasValidSettings(submission)
      && QDir::isAbsolutePath(configuration_.jobsRoot)
      && QDir::isAbsolutePath(configuration_.workerExecutable)
      && workerInfo.isFile()
      && QDir::isAbsolutePath(configuration_.backendExecutable)
      && backendInfo.isFile()
      && !configuration_.mainBuildId.isEmpty()
      && configuration_.maximumGeometryBytes > 0
      && QDir::isAbsolutePath(submission.geometrySnapshotStep)
      && snapshotInfo.isFile();
  }

  bool geometryIsCurrent() const
  {
    return active_
      && active_->sourceGeometryRevision == projectState_.geometryRevision();
  }

  MeshingJobFailure staleFailure() const
  {
    return makeFailure(
      MeshingJobFailureKind::StaleGeometry,
      active_->submission.jobId,
      active_->sourceGeometryRevision,
      "MESH_RESULT_STALE",
      "completed",
      "Geometry changed after this mesh job was submitted");
  }

  void preparationFinished()
  {
    if ((state_ != State::Preparing && state_ != State::CancellingPreparation)
      || !active_)
    {
      return;
    }
    const meshing::GmshJobWorkspaceResult result = preparationWatcher_.result();
    if (state_ == State::CancellingPreparation)
    {
      finishCancelled();
      return;
    }
    if (!result.succeeded())
    {
      finishFailure(makeFailure(
        MeshingJobFailureKind::WorkspacePreparationFailed,
        active_->submission.jobId,
        active_->sourceGeometryRevision,
        "MESH_WORKSPACE_PREPARATION_FAILED",
        "initializing",
        result.detail));
      return;
    }
    active_->preparedJob = *result.job;
    if (!geometryIsCurrent())
    {
      finishFailure(staleFailure());
      return;
    }

    state_ = State::Running;
    lastStage_ = "initializing";
    const auto startFailure = workerClient_.start({
      configuration_.workerExecutable,
      configuration_.backendExecutable,
      active_->preparedJob->workspace,
      active_->submission.jobId,
      active_->sourceGeometryRevision,
      configuration_.mainBuildId,
    });
    if (startFailure)
    {
      finishFailure(makeFailure(
        MeshingJobFailureKind::WorkerFailure,
        active_->submission.jobId,
        active_->sourceGeometryRevision,
        startFailure->code,
        startFailure->stage,
        startFailure->detail,
        startFailure->retryable));
    }
  }

  void workerProgressed(const meshing::GmshWorkerProgress& progress)
  {
    if ((state_ == State::Running || state_ == State::CancellingWorker) && active_)
    {
      lastStage_ = progress.stage;
      if (state_ == State::Running)
      {
        emit owner_->progressChanged({
          active_->submission.jobId,
          active_->sourceGeometryRevision,
          progress.stage,
          progress.percent,
        });
      }
    }
  }

  void workerSucceeded(const meshing::GmshWorkerResult& result)
  {
    if (state_ != State::Running || !active_)
    {
      return;
    }
    active_->workerResult = result;
    if (!geometryIsCurrent())
    {
      finishFailure(staleFailure());
      return;
    }

    state_ = State::Verifying;
    lastStage_ = "writing-result";
    const ActiveJob active = *active_;
    validationWatcher_.setFuture(
      QtConcurrent::run([active] { return validateArtifacts(active); }));
  }

  void workerFailed(const meshing::GmshWorkerFailure& failure)
  {
    if ((state_ != State::Running && state_ != State::CancellingWorker)
      || !active_)
    {
      return;
    }
    if (!geometryIsCurrent())
    {
      finishFailure(staleFailure());
      return;
    }
    finishFailure(makeFailure(
      MeshingJobFailureKind::WorkerFailure,
      active_->submission.jobId,
      active_->sourceGeometryRevision,
      failure.code,
      failure.stage,
      failure.detail,
      failure.retryable));
  }

  void workerCancelled(const meshing::GmshWorkerCancellation& cancellation)
  {
    if (state_ != State::CancellingWorker || !active_ || !cancellation_)
    {
      return;
    }
    cancellation_->stage = cancellation.stage;
    cancellation_->forced = cancellation.forced;
    finishCancelled();
  }

  void validationFinished()
  {
    if ((state_ != State::Verifying && state_ != State::CancellingVerification)
      || !active_
      || !active_->preparedJob || !active_->workerResult)
    {
      return;
    }
    const ArtifactValidation validation = validationWatcher_.result();
    if (state_ == State::CancellingVerification)
    {
      finishCancelled();
      return;
    }
    if (!geometryIsCurrent())
    {
      finishFailure(staleFailure());
      return;
    }
    if (!validation.valid)
    {
      finishFailure(makeFailure(
        MeshingJobFailureKind::OutputValidationFailed,
        active_->submission.jobId,
        active_->sourceGeometryRevision,
        "MESH_OUTPUT_CORRUPT",
        "writing-result",
        validation.detail));
      return;
    }

    const auto& prepared = *active_->preparedJob;
    const auto& worker = *active_->workerResult;
    ExperimentalMeshingJobResult result{
      active_->submission.jobId,
      active_->sourceGeometryRevision,
      prepared.workspace,
      prepared.resultPath,
      prepared.meshPath,
      worker.meshSha256.toLower(),
      worker.nodeCount,
      worker.triangleCount,
      worker.tetrahedronCount,
    };
    state_ = State::Idle;
    active_.reset();
    cancellation_.reset();
    emit owner_->experimentalSucceeded(result);
  }

  void finishCancelled()
  {
    if (!active_ || !cancellation_)
    {
      return;
    }
    if (active_->preparedJob)
    {
      QFile::remove(active_->preparedJob->meshPath);
      QFile::remove(active_->preparedJob->resultPath);
    }
    const MeshingJobCancellation cancellation = *cancellation_;
    state_ = State::Idle;
    active_.reset();
    cancellation_.reset();
    emit owner_->cancelled(cancellation);
  }

  void finishFailure(MeshingJobFailure failure)
  {
    state_ = State::Idle;
    active_.reset();
    cancellation_.reset();
    emit owner_->failed(failure);
  }

  MeshingJobController* owner_ = nullptr;
  const domain::ProjectState& projectState_;
  MeshingRuntimeConfiguration configuration_;
  meshing::GmshWorkerClient workerClient_;
  QFutureWatcher<meshing::GmshJobWorkspaceResult> preparationWatcher_;
  QFutureWatcher<ArtifactValidation> validationWatcher_;
  std::optional<ActiveJob> active_;
  std::optional<MeshingJobCancellation> cancellation_;
  QString lastStage_ = "initializing";
  State state_ = State::Idle;
};

MeshingJobController::MeshingJobController(
  const domain::ProjectState& projectState,
  MeshingRuntimeConfiguration configuration,
  QObject* parent)
  : QObject(parent)
  , implementation_(std::make_unique<Implementation>(
      this, projectState, std::move(configuration)))
{
}

MeshingJobController::~MeshingJobController() = default;

std::optional<MeshingJobFailure> MeshingJobController::submit(
  const MeshingJobSubmission& submission)
{
  return implementation_->submit(submission);
}

std::optional<MeshingJobFailure> MeshingJobController::cancel(
  const QString& reason,
  int deadlineMilliseconds)
{
  return implementation_->cancel(reason, deadlineMilliseconds);
}

bool MeshingJobController::isRunning() const noexcept
{
  return implementation_->isRunning();
}

bool MeshingJobController::isCancelling() const noexcept
{
  return implementation_->isCancelling();
}
}
