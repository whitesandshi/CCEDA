#include "application/MeshingJobController.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>

namespace
{
using cceda::application::ExperimentalMeshingJobResult;
using cceda::application::MeshingJobController;
using cceda::application::MeshingJobFailure;
using cceda::application::MeshingJobFailureKind;
using cceda::application::MeshingJobProgress;
using cceda::application::MeshingJobSubmission;
using cceda::application::MeshingRuntimeConfiguration;

bool require(bool condition, const char* message)
{
  if (!condition)
  {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 30000)
{
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeoutMs)
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(5);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  return predicate();
}

MeshingRuntimeConfiguration configuration(
  const QString& worker,
  const QString& backend,
  const QString& jobsRoot)
{
  return {
    worker,
    backend,
    jobsRoot,
    "controller-tests",
    32ULL * 1024ULL * 1024ULL,
  };
}

bool successfulJobProducesVerifiedExperimentalResult(
  const QString& worker,
  const QString& backend,
  const QString& step)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  MeshingJobController controller(project, configuration(worker, backend, jobs.path()));

  std::optional<ExperimentalMeshingJobResult> result;
  std::optional<MeshingJobFailure> failure;
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult& value) { result = value; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });

  const auto rejection = controller.submit({"success-job", step, 0.0, 0.0});
  if (!require(!rejection.has_value(), "valid job should be accepted")
    || !require(waitUntil([&] { return result.has_value() || failure.has_value(); }),
      "accepted job should reach a terminal state")
    || !require(!failure.has_value(), "valid job should not fail")
    || !require(result.has_value(), "valid job should produce a result"))
  {
    if (failure)
    {
      std::cerr << failure->code.toStdString() << ": "
                << failure->detail.toStdString() << '\n';
    }
    return false;
  }

  return require(result->jobId == "success-job", "result should retain the job ID")
    && require(result->sourceGeometryRevision == project.geometryRevision(),
      "result should retain the captured geometry revision")
    && require(QFileInfo(result->manifestPath).isFile(),
      "verified result manifest should exist")
    && require(QFileInfo(result->intermediateMeshPath).isFile(),
      "verified intermediate mesh should exist")
    && require(result->meshSha256.size() == 64, "result should expose the verified digest")
    && require(result->nodeCount > 0 && result->triangleCount > 0
      && result->tetrahedronCount > 0, "result should expose non-zero mesh counts")
    && require(!controller.isRunning(), "controller should return to idle after success");
}

bool concurrentSubmissionIsRejected(
  const QString& worker,
  const QString& backend,
  const QString& step)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  MeshingJobController controller(project, configuration(worker, backend, jobs.path()));

  bool terminal = false;
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult&) { terminal = true; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure&) { terminal = true; });

  const auto first = controller.submit({"first-job", step, 0.0, 0.0});
  const auto second = controller.submit({"second-job", step, 0.0, 0.0});
  return require(!first.has_value(), "first job should be accepted")
    && require(second.has_value(), "second active job should be rejected")
    && require(second->kind == MeshingJobFailureKind::AlreadyRunning,
      "concurrent rejection should have a stable kind")
    && require(waitUntil([&] { return terminal; }),
      "first job should still complete after concurrent rejection");
}

bool geometryChangeMakesInFlightResultStale(
  const QString& worker,
  const QString& backend,
  const QString& step)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  const auto submittedRevision = project.geometryRevision();
  MeshingJobController controller(project, configuration(worker, backend, jobs.path()));

  std::optional<MeshingJobFailure> failure;
  bool succeeded = false;
  bool revisionChanged = false;
  QObject::connect(&controller, &MeshingJobController::progressChanged,
    &controller, [&](const MeshingJobProgress&) {
      if (!revisionChanged)
      {
        revisionChanged = true;
        project.commitGeometryChange();
      }
    });
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult&) { succeeded = true; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });

  const auto rejection = controller.submit({"stale-job", step, 0.0, 0.0});
  return require(!rejection.has_value(), "stale test job should be accepted")
    && require(waitUntil([&] { return succeeded || failure.has_value(); }),
      "stale test job should reach a terminal state")
    && require(revisionChanged, "test should change revision while the job is active")
    && require(!succeeded, "stale result must never be published as success")
    && require(failure.has_value(), "stale result should publish a failure")
    && require(failure->kind == MeshingJobFailureKind::StaleGeometry,
      "stale result should have a stable failure kind")
    && require(failure->sourceGeometryRevision == submittedRevision,
      "stale failure should identify the submitted revision")
    && require(!controller.isRunning(), "controller should return to idle after stale result");
}

bool modifiedWorkerOutputIsRejected(
  const QString& worker,
  const QString& backend,
  const QString& step)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  MeshingJobController controller(project, configuration(worker, backend, jobs.path()));

  bool tampered = false;
  bool succeeded = false;
  std::optional<MeshingJobFailure> failure;
  QObject::connect(&controller, &MeshingJobController::progressChanged,
    &controller, [&](const MeshingJobProgress& progress) {
      if (!tampered && progress.stage == "completed")
      {
        QFile mesh(QDir(jobs.path()).filePath(
          "tamper-job/output/gmsh-intermediate.msh"));
        if (mesh.open(QIODevice::Append))
        {
          tampered = mesh.write("\n# tampered\n") > 0;
        }
      }
    });
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult&) { succeeded = true; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });

  const auto rejection = controller.submit({"tamper-job", step, 0.0, 0.0});
  return require(!rejection.has_value(), "tamper test job should be accepted")
    && require(waitUntil([&] { return succeeded || failure.has_value(); }),
      "tamper test job should reach a terminal state")
    && require(tampered, "test should modify the published Worker mesh")
    && require(!succeeded, "modified Worker output must not be published")
    && require(failure.has_value(), "modified output should publish a failure")
    && require(failure->kind == MeshingJobFailureKind::OutputValidationFailed,
      "modified output should fail main-side artifact validation")
    && require(failure->code == "MESH_OUTPUT_CORRUPT",
      "modified output should use the protocol error code");
}

bool invalidSubmissionIsRejectedBeforeWorkStarts(
  const QString& worker,
  const QString& backend,
  const QString& step)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  MeshingJobController controller(project, configuration(worker, backend, jobs.path()));

  const auto rejection = controller.submit({"../escape", step, 0.0, 0.0});
  return require(rejection.has_value(), "unsafe job ID should be rejected")
    && require(rejection->kind == MeshingJobFailureKind::InvalidRequest,
      "unsafe job ID should have invalid-request kind")
    && require(!controller.isRunning(), "rejected job should not start work")
    && require(!QFileInfo(QDir(jobs.path()).filePath("../escape")).exists(),
      "rejected job should not create a workspace outside the jobs root");
}
}

int main(int argc, char* argv[])
{
  QCoreApplication application(argc, argv);
  if (argc != 4)
  {
    std::cerr << "Usage: MeshingJobControllerTests <worker> <gmsh> <step>\n";
    return EXIT_FAILURE;
  }

  const QString worker = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
  const QString backend = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
  const QString step = QFileInfo(QString::fromLocal8Bit(argv[3])).absoluteFilePath();
  const bool ok = successfulJobProducesVerifiedExperimentalResult(worker, backend, step)
    && concurrentSubmissionIsRejected(worker, backend, step)
    && geometryChangeMakesInFlightResultStale(worker, backend, step)
    && modifiedWorkerOutputIsRejected(worker, backend, step)
    && invalidSubmissionIsRejectedBeforeWorkStarts(worker, backend, step);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
