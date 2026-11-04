#include "application/MeshingJobController.h"
#include "geometry/OccGeometryRepository.h"
#include "geometry/OccGeometryService.h"
#include "geometry/OccGeometrySnapshotService.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <STEPControl_Writer.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>

#include <unordered_map>
namespace
{
using cceda::application::ExperimentalMeshingJobResult;
using cceda::application::MeshingJobController;
using cceda::application::MeshingJobCancellation;
using cceda::application::MeshingJobFailure;
using cceda::application::MeshingJobFailureKind;
using cceda::application::MeshingJobProgress;
using cceda::application::MeshingJobSubmission;
using cceda::application::MeshingRuntimeConfiguration;

struct SnapshotInput
{
  QString brep;
  QString map;
  QString brepSha256;
  QString mapSha256;
};
cceda::domain::MeshModel makeExistingWorkerMesh(
  cceda::domain::GeometryRevision sourceRevision)
{
  using namespace cceda::domain;

  MeshModel mesh;
  mesh.sourceGeometryRevision = sourceRevision;
  mesh.creationTimeUtc = "2026-07-26T00:00:00.000Z";
  mesh.backendId = "existing";
  mesh.backendVersion = "1";
  mesh.conformity = MeshConformity::NonConformal;
  mesh.nodes = {
    {1, 0.0, 0.0, 0.0, "existing/vertex-1"},
    {2, 1.0, 0.0, 0.0, "existing/vertex-2"},
    {3, 0.0, 1.0, 0.0, "existing/vertex-3"},
    {4, 0.0, 0.0, 1.0, {}},
  };
  mesh.elements = {
    {101, MeshElementType::Triangle3, {1, 2, 3, 0}, 10,
      "existing/face-1"},
    {102, MeshElementType::Tetrahedron4, {1, 2, 3, 4}, 10,
      "existing/solid-1"},
  };
  mesh.regions = {
    {10, {"existing/solid-1"}},
  };
  return mesh;
}


MeshingJobSubmission submissionFor(
  QString jobId,
  const SnapshotInput& snapshot,
  cceda::domain::MeshGenerationSettings settings = {},
  cceda::domain::MeshingResourceLimits resourceLimits = {},
  cceda::domain::MeshingRetryPolicy retryPolicy = {})
{
  return {
    std::move(jobId),
    snapshot.brep,
    snapshot.map,
    snapshot.brepSha256,
    snapshot.mapSha256,
    settings,
    resourceLimits,
    retryPolicy,
  };
}

bool require(bool condition, const char* message)
{
  if (!condition)
  {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

bool writeTwoSolidStep(const QString& path)
{
  BRep_Builder builder;
  TopoDS_Compound compound;
  builder.MakeCompound(compound);
  builder.Add(compound, BRepPrimAPI_MakeBox(
    gp_Pnt(0.0, 0.0, 0.0), 1.0, 1.0, 1.0).Solid());
  builder.Add(compound, BRepPrimAPI_MakeBox(
    gp_Pnt(3.0, 0.0, 0.0), 1.0, 1.0, 1.0).Solid());
  STEPControl_Writer writer;
  const QByteArray nativePath = QFile::encodeName(path);
  return writer.Transfer(compound, STEPControl_AsIs) == IFSelect_RetDone
    && writer.Write(nativePath.constData()) == IFSelect_RetDone;
}

SnapshotInput snapshotInput(
  const cceda::geometry::GeometrySnapshot& snapshot)
{
  return {
    QString::fromStdWString(snapshot.geometryPath.wstring()),
    QString::fromStdWString(snapshot.geometryMapPath.wstring()),
    QString::fromStdString(snapshot.geometrySha256),
    QString::fromStdString(snapshot.geometryMapSha256),
  };
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

bool successfulJobCommitsQualityValidMesh(
  const QString& worker,
  const QString& backend,
  const SnapshotInput& snapshot)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  MeshingJobController controller(project, configuration(worker, backend, jobs.path()));

  std::optional<ExperimentalMeshingJobResult> result;
  std::optional<MeshingJobFailure> failure;
  bool lateCancellationRejected = false;
  std::uint64_t lastElapsedMilliseconds = 0;
  std::uint64_t maximumWorkingSetBytes = 0;
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult& value) { result = value; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });
  QObject::connect(&controller, &MeshingJobController::progressChanged,
    &controller, [&](const MeshingJobProgress& progress) {
      lastElapsedMilliseconds = progress.elapsedMilliseconds;
      maximumWorkingSetBytes = std::max(
        maximumWorkingSetBytes, progress.workingSetBytes);
      if (progress.stage == "completed")
      {
        lateCancellationRejected =
          controller.cancel("too late", 1'000).has_value();
      }
    });

  const cceda::domain::MeshGenerationSettings submittedSettings{
    0.0, 0.0, 18, true, 2};
  const auto rejection = controller.submit(
    submissionFor("success-job", snapshot, submittedSettings));
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
    && require(result->committedMeshRevision == project.meshRevision(),
      "result should expose the committed mesh revision")
    && require(result->effectiveSettings.curvatureElementsPerCircle == 18
        && result->effectiveSettings.optimizeTetrahedra
        && result->effectiveSettings.maximumThreads == 2,
      "result should expose the validated effective meshing settings")
    && require(result->effectiveResourceLimits.maximumElapsedMilliseconds == 0
        && result->effectiveResourceLimits.maximumWorkingSetBytes == 0,
      "result should expose the disabled default resource limits")
    && require(result->elapsedMilliseconds >= lastElapsedMilliseconds
        && result->peakWorkingSetBytes >= maximumWorkingSetBytes
        && result->peakWorkingSetBytes > 0,
      "result should expose validated Worker resource telemetry")
    && require(result->quality != nullptr,
      "result should retain the accepted quality snapshot")
    && require(result->quality->meshRevision == project.meshRevision()
        && result->quality->sourceGeometryRevision == project.geometryRevision(),
      "accepted quality should bind the committed mesh and geometry revisions")
    && require(result->quality->validForCommit
        && result->quality->numericSummaries.size() == 8
        && result->quality->booleanSummary.sampleCount == result->triangleCount,
      "accepted quality should expose the canonical metric summaries")
    && require(result->quality->issueElementDetails.size()
        <= result->quality->issues.size(),
      "accepted quality details should remain bounded by unique issue elements")
    && require(QFileInfo(result->manifestPath).isFile(),
      "verified result manifest should exist")
    && require(QFileInfo(result->intermediateMeshPath).isFile(),
      "verified intermediate mesh should exist")
    && require(result->meshSha256.size() == 64, "result should expose the verified digest")
    && require(QFileInfo(result->neutralMeshPath).isFile(),
      "verified neutral mesh should exist")
    && require(result->meshBinarySha256.size() == 64,
      "result should expose the verified neutral mesh digest")
    && require(QFileInfo(result->geometryMapResultPath).isFile(),
      "verified geometry-map result should exist")
    && require(result->geometryMapResultSha256.size() == 64,
      "result should expose the verified geometry-map digest")
    && require(QFileInfo(result->qualityPath).isFile(),
      "verified quality result should exist")
    && require(result->qualitySha256.size() == 64,
      "result should expose the verified quality digest")
    && require(result->nodeCount > 0 && result->triangleCount > 0
      && result->tetrahedronCount > 0, "result should expose non-zero mesh counts")
    && require(project.currentMesh() != nullptr
        && project.meshState() == cceda::domain::MeshState::Ready,
      "quality-verified mesh should become the current ready mesh")
    && require(project.meshRevision() == 1
        && project.currentMesh()->meshRevision == project.meshRevision(),
      "successful Controller commit should assign mesh revision one")
    && require(project.currentMesh()->sourceGeometryRevision
        == project.geometryRevision(),
      "committed mesh should belong to the current geometry revision")
    && require(project.canExportMesh(),
      "quality-valid committed mesh should be exportable")
    && require(lateCancellationRejected,
      "cancellation after completed progress should be rejected")
    && require(!controller.isRunning(), "controller should return to idle after success");
}

bool multiRegionJobCommitsEverySolid(
  const QString& worker,
  const QString& backend,
  const SnapshotInput& snapshot)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  MeshingJobController controller(project, configuration(
    worker, backend, jobs.path()));

  std::optional<ExperimentalMeshingJobResult> result;
  std::optional<MeshingJobFailure> failure;
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult& value) { result = value; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });

  const auto rejection = controller.submit(
    submissionFor("multi-region-job", snapshot));
  if (!require(!rejection.has_value(), "multi-region job should be accepted")
    || !require(waitUntil([&] { return result.has_value() || failure.has_value(); }),
      "multi-region job should reach a terminal state")
    || !require(!failure.has_value(), "multi-region job should not fail")
    || !require(result.has_value(), "multi-region job should produce a result")
    || !require(project.currentMesh() != nullptr,
      "multi-region job should commit a current mesh"))
  {
    if (failure)
    {
      std::cerr << failure->code.toStdString() << ": "
                << failure->detail.toStdString() << '\n';
    }
    return false;
  }

  std::unordered_map<cceda::domain::MeshRegionId, std::string> solidByRegion;
  for (const cceda::domain::MeshRegion& region :
      project.currentMesh()->regions)
  {
    if (region.sourceGeometryIds.size() != 1
      || !solidByRegion.emplace(
        region.id, region.sourceGeometryIds.front()).second)
    {
      return false;
    }
  }
  return require(solidByRegion.size() == 2,
      "committed mesh should retain both independent-Solid regions")
    && require(std::all_of(
        project.currentMesh()->elements.begin(),
        project.currentMesh()->elements.end(),
        [&](const cceda::domain::MeshElement& element) {
          const auto expected = solidByRegion.find(element.regionId);
          return expected != solidByRegion.end()
            && element.geometryId == expected->second;
        }),
      "every committed element should retain its region Solid association");
}

bool resourceLimitedJobPreservesExistingMesh(
  const QString& worker,
  const SnapshotInput& snapshot)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  if (!require(project.commitMesh(
      makeExistingWorkerMesh(project.geometryRevision())),
    "resource-limit fixture should seed a current mesh"))
  {
    return false;
  }
  const auto previousMeshRevision = project.meshRevision();
  const auto previousFirstElementId = project.currentMesh()->elements.front().id;
  MeshingJobController controller(project, configuration(
    worker, QCoreApplication::applicationFilePath(), jobs.path()));

  bool succeeded = false;
  std::optional<MeshingJobFailure> failure;
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult&) { succeeded = true; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });

  const auto rejection = controller.submit(submissionFor(
    "resource-limit-job", snapshot, {}, {250, 0}, {true, 2}));
  return require(!rejection.has_value(), "resource-limited job should be accepted")
    && require(waitUntil([&] { return succeeded || failure.has_value(); }, 5'000),
      "resource-limited job should reach a terminal state")
    && require(!succeeded, "resource-limited job must not publish success")
    && require(failure.has_value(), "resource-limited job should publish failure")
    && require(failure->kind == MeshingJobFailureKind::WorkerFailure,
      "resource-limited job should remain a Worker failure at controller boundary")
    && require(failure->code == "MESH_RESOURCE_LIMIT",
      "resource-limited job should use the stable error code")
    && require(failure->attemptCount == 1,
      "resource-limit failure must not consume an automatic retry")
    && require(!QFileInfo::exists(QDir(jobs.path()).filePath(
      "resource-limit-job.attempt-2")),
      "resource-limit failure must not create a retry workspace")
    && require(project.meshState() == cceda::domain::MeshState::Ready,
      "resource failure should preserve the ready mesh state")
    && require(project.meshRevision() == previousMeshRevision,
      "resource failure should preserve the mesh revision")
    && require(project.currentMesh() != nullptr
        && project.currentMesh()->elements.front().id == previousFirstElementId,
      "resource failure should preserve the current mesh")
    && require(!QFileInfo(QDir(jobs.path()).filePath(
      "resource-limit-job/output/result.json")).exists(),
      "resource failure retained a result manifest")
    && require(!QFileInfo(QDir(jobs.path()).filePath(
      "resource-limit-job/output/result.meshbin")).exists(),
      "resource failure retained a neutral mesh")
    && require(!controller.isRunning(),
      "controller should be idle after resource failure");
}

bool retryPolicyIsBoundedAndUsesFreshWorkspace(
  const QString& worker,
  const QString& backend,
  const SnapshotInput& snapshot)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  MeshingJobController controller(project, configuration(
    worker, QCoreApplication::applicationFilePath(), jobs.path()));

  qputenv("CCEDA_TEST_RETRY_BACKEND", backend.toUtf8());
  std::optional<ExperimentalMeshingJobResult> result;
  std::optional<MeshingJobFailure> failure;
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult& value) { result = value; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });

  const auto rejection = controller.submit(submissionFor(
    "retry-job", snapshot, {}, {}, {true, 2}));
  const bool finished = waitUntil(
    [&] { return result.has_value() || failure.has_value(); }, 60'000);
  qunsetenv("CCEDA_TEST_RETRY_BACKEND");
  if (!require(!rejection.has_value(), "bounded retry job should be accepted")
    || !require(finished, "bounded retry job should reach a terminal state")
    || !require(!failure.has_value(), "approved fallback should recover the job")
    || !require(result.has_value(), "fallback success should publish a result"))
  {
    if (failure)
    {
      std::cerr << failure->code.toStdString() << ": "
                << failure->detail.toStdString() << '\n';
    }
    return false;
  }

  return require(result->attemptCount == 2,
      "fallback success should report exactly two attempts")
    && require(result->effectiveSettings.algorithm
        == cceda::domain::MeshGenerationAlgorithm::Hxt,
      "attempt two should use the approved alternate algorithm")
    && require(result->workspace.endsWith("retry-job.attempt-2"),
      "fallback must publish from a fresh attempt workspace")
    && require(QFileInfo::exists(QDir(jobs.path()).filePath(
      "retry-job/logs/gmsh.log")),
      "failed attempt diagnostics should remain traceable")
    && require(QFileInfo::exists(QDir(jobs.path()).filePath(
      "retry-job.attempt-2/output/result.json")),
      "successful fallback output should remain traceable")
    && require(project.meshState() == cceda::domain::MeshState::Ready,
      "only the verified fallback mesh should become current");
}

bool automaticRetryIsDisabledByDefault(
  const QString& worker,
  const QString& backend,
  const SnapshotInput& snapshot)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  MeshingJobController controller(project, configuration(
    worker, QCoreApplication::applicationFilePath(), jobs.path()));

  qputenv("CCEDA_TEST_RETRY_BACKEND", backend.toUtf8());
  bool succeeded = false;
  std::optional<MeshingJobFailure> failure;
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult&) { succeeded = true; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });

  const auto rejection = controller.submit(submissionFor(
    "retry-disabled-job", snapshot));
  const bool finished = waitUntil(
    [&] { return succeeded || failure.has_value(); }, 30'000);
  qunsetenv("CCEDA_TEST_RETRY_BACKEND");
  return require(!rejection.has_value(),
      "default retry-policy job should be accepted")
    && require(finished, "default retry-policy job should terminate")
    && require(!succeeded, "disabled automatic retry must not recover")
    && require(failure.has_value()
        && failure->code == "MESH_VOLUME_GENERATION_FAILED",
      "disabled automatic retry should expose the first generation failure")
    && require(failure->attemptCount == 1,
      "disabled automatic retry should report one attempt")
    && require(!QFileInfo::exists(QDir(jobs.path()).filePath(
      "retry-disabled-job.attempt-2")),
      "disabled automatic retry must not create a fallback workspace");
}

bool concurrentSubmissionIsRejected(
  const QString& worker,
  const QString& backend,
  const SnapshotInput& snapshot)
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

  const auto first = controller.submit(submissionFor("first-job", snapshot));
  const auto second = controller.submit(submissionFor("second-job", snapshot));
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
  const SnapshotInput& snapshot)
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

  const auto rejection = controller.submit(submissionFor("stale-job", snapshot));
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
  const SnapshotInput& snapshot)
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
        QFile geometryMapResult(QDir(jobs.path()).filePath(
          "tamper-job/output/geometry-map-result.json"));
        if (geometryMapResult.open(QIODevice::Append))
        {
          tampered = geometryMapResult.write("\n ") > 0;
        }
      }
    });
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult&) { succeeded = true; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });

  const auto rejection = controller.submit(submissionFor("tamper-job", snapshot));
  return require(!rejection.has_value(), "tamper test job should be accepted")
    && require(waitUntil([&] { return succeeded || failure.has_value(); }),
      "tamper test job should reach a terminal state")
    && require(tampered, "test should modify the published Worker geometry map")
    && require(!succeeded, "modified Worker output must not be published")
    && require(failure.has_value(), "modified output should publish a failure")
    && require(failure->kind == MeshingJobFailureKind::OutputValidationFailed,
      "modified output should fail main-side artifact validation")
    && require(failure->code == "MESH_OUTPUT_CORRUPT",
      "modified output should use the protocol error code");
}

bool modifiedNeutralMeshIsRejected(
  const QString& worker,
  const QString& backend,
  const SnapshotInput& snapshot)
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
        QFile neutralMesh(QDir(jobs.path()).filePath(
          "meshbin-tamper-job/output/result.meshbin"));
        if (neutralMesh.open(QIODevice::Append))
        {
          tampered = neutralMesh.write("x") == 1;
        }
      }
    });
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult&) { succeeded = true; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });

  const auto rejection = controller.submit(
    submissionFor("meshbin-tamper-job", snapshot));
  return require(!rejection.has_value(), "meshbin tamper job should be accepted")
    && require(waitUntil([&] { return succeeded || failure.has_value(); }),
      "meshbin tamper job should reach a terminal state")
    && require(tampered, "test should modify the published neutral mesh")
    && require(!succeeded, "modified neutral mesh must not be published")
    && require(failure.has_value(), "modified neutral mesh should publish a failure")
    && require(failure->kind == MeshingJobFailureKind::OutputValidationFailed,
      "modified neutral mesh should fail main-side artifact validation")
    && require(failure->code == "MESH_OUTPUT_CORRUPT",
      "modified neutral mesh should use the protocol error code");
}

bool modifiedQualityIsRejected(
  const QString& worker,
  const QString& backend,
  const SnapshotInput& snapshot)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  if (!require(project.commitMesh(
      makeExistingWorkerMesh(project.geometryRevision())),
    "quality tamper fixture should seed a current mesh"))
  {
    return false;
  }
  const auto previousMeshRevision = project.meshRevision();
  const auto previousFirstElementId = project.currentMesh()->elements.front().id;
  MeshingJobController controller(project, configuration(worker, backend, jobs.path()));

  bool tampered = false;
  bool succeeded = false;
  std::optional<MeshingJobFailure> failure;
  QObject::connect(&controller, &MeshingJobController::progressChanged,
    &controller, [&](const MeshingJobProgress& progress) {
      if (!tampered && progress.stage == "completed")
      {
        QFile quality(QDir(jobs.path()).filePath(
          "quality-tamper-job/output/quality.json"));
        if (quality.open(QIODevice::Append))
        {
          tampered = quality.write("\n ") > 0;
        }
      }
    });
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult&) { succeeded = true; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });

  const auto rejection = controller.submit(
    submissionFor("quality-tamper-job", snapshot));
  return require(!rejection.has_value(), "quality tamper job should be accepted")
    && require(waitUntil([&] { return succeeded || failure.has_value(); }),
      "quality tamper job should reach a terminal state")
    && require(tampered, "test should modify the published quality artifact")
    && require(!succeeded, "modified quality output must not be published")
    && require(failure.has_value(), "modified quality output should publish a failure")
    && require(failure->kind == MeshingJobFailureKind::OutputValidationFailed,
      "modified quality output should fail main-side artifact validation")
    && require(failure->code == "MESH_OUTPUT_CORRUPT",
      "modified quality output should use the protocol error code")
    && require(project.meshState() == cceda::domain::MeshState::Ready,
      "rejected quality output should preserve the ready mesh state")
    && require(project.meshRevision() == previousMeshRevision,
      "rejected quality output should preserve the mesh revision")
    && require(project.currentMesh() != nullptr
        && project.currentMesh()->elements.front().id == previousFirstElementId,
      "rejected quality output should preserve the current mesh");
}

bool modifiedSealedInputMapIsRejected(
  const QString& worker,
  const QString& backend,
  const SnapshotInput& snapshot)
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
        QFile inputMap(QDir(jobs.path()).filePath(
          "input-map-tamper-job/geometry/geometry-map.json"));
        if (inputMap.open(QIODevice::Append))
        {
          tampered = inputMap.write("\n ") > 0;
        }
      }
    });
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult&) { succeeded = true; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });

  const auto rejection = controller.submit(
    submissionFor("input-map-tamper-job", snapshot));
  return require(!rejection.has_value(), "input-map tamper job should be accepted")
    && require(waitUntil([&] { return succeeded || failure.has_value(); }),
      "input-map tamper job should reach a terminal state")
    && require(tampered, "test should modify the sealed input geometry map")
    && require(!succeeded, "modified input geometry map must not be trusted")
    && require(failure.has_value(), "modified input map should publish a failure")
    && require(failure->kind == MeshingJobFailureKind::OutputValidationFailed,
      "modified input map should fail artifact validation")
    && require(failure->code == "MESH_OUTPUT_CORRUPT",
      "modified input map should use the protocol error code");
}

bool cancelledJobDoesNotPublishOrMutateProject(
  const QString& worker,
  const SnapshotInput& snapshot)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  const auto revision = project.geometryRevision();
  MeshingJobController controller(
    project,
    configuration(worker, QCoreApplication::applicationFilePath(), jobs.path()));

  bool cancelAccepted = false;
  bool succeeded = false;
  std::optional<MeshingJobFailure> failure;
  std::optional<MeshingJobCancellation> cancelling;
  std::optional<MeshingJobCancellation> cancelled;
  QObject::connect(&controller, &MeshingJobController::progressChanged,
    &controller, [&](const MeshingJobProgress& progress) {
      if (!cancelAccepted && progress.stage == "generating-volume")
      {
        cancelAccepted = !controller.cancel("user requested", 2'000).has_value();
      }
    });
  QObject::connect(&controller, &MeshingJobController::cancelling,
    &controller, [&](const MeshingJobCancellation& value) { cancelling = value; });
  QObject::connect(&controller, &MeshingJobController::cancelled,
    &controller, [&](const MeshingJobCancellation& value) { cancelled = value; });
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult&) { succeeded = true; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });

  const auto rejection = controller.submit(submissionFor("cancel-job", snapshot));
  return require(!rejection.has_value(), "cancel test job should be accepted")
    && require(waitUntil([&] {
      return succeeded || failure.has_value() || cancelled.has_value();
    }), "cancel test job should reach a terminal state")
    && require(cancelAccepted, "active controller cancellation should be accepted")
    && require(cancelling.has_value(), "controller should publish Cancelling")
    && require(cancelled.has_value(), "controller should publish Cancelled")
    && require(!cancelled->forced, "responsive Worker cancellation should be cooperative")
    && require(cancelled->reason == "user requested",
      "controller cancellation reason should round-trip")
    && require(cancelled->sourceGeometryRevision == revision,
      "controller cancellation should retain the submitted revision")
    && require(!succeeded, "cancelled controller job must not publish success")
    && require(!failure.has_value(), "cancelled controller job must not publish failure")
    && require(project.currentMesh() == nullptr,
      "cancelled job must not create a current mesh")
    && require(!QFileInfo(QDir(jobs.path()).filePath(
      "cancel-job/output/quality.json")).exists(),
      "cancelled controller job retained quality output")
    && require(project.geometryRevision() == revision,
      "cancelled job must not change geometry")
    && require(!QFileInfo(QDir(jobs.path()).filePath(
      "cancel-job/output/gmsh-intermediate.msh")).exists(),
      "cancelled controller job retained formal mesh output")
    && require(!controller.isRunning(), "controller should be idle after cancellation")
    && require(!controller.isCancelling(),
      "controller should leave Cancelling after its terminal state");
}

bool preparationCanBeCancelledBeforeWorkerLaunch(
  const QString& worker,
  const QString& backend,
  const SnapshotInput& snapshot)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  MeshingJobController controller(project, configuration(worker, backend, jobs.path()));

  bool succeeded = false;
  std::optional<MeshingJobFailure> failure;
  std::optional<MeshingJobCancellation> cancelled;
  QObject::connect(&controller, &MeshingJobController::experimentalSucceeded,
    &controller, [&](const ExperimentalMeshingJobResult&) { succeeded = true; });
  QObject::connect(&controller, &MeshingJobController::failed,
    &controller, [&](const MeshingJobFailure& value) { failure = value; });
  QObject::connect(&controller, &MeshingJobController::cancelled,
    &controller, [&](const MeshingJobCancellation& value) { cancelled = value; });

  const auto rejection = controller.submit(submissionFor("prepare-cancel", snapshot));
  const auto cancelRejection = controller.cancel("cancel before launch", 1'000);
  return require(!rejection.has_value(), "preparation-cancel job should be accepted")
    && require(!cancelRejection.has_value(),
      "cancellation during preparation should be accepted")
    && require(waitUntil([&] {
      return succeeded || failure.has_value() || cancelled.has_value();
    }), "preparation cancellation should reach a terminal state")
    && require(cancelled.has_value(), "preparation cancellation should publish Cancelled")
    && require(cancelled->stage == "initializing",
      "preparation cancellation should report the initializing stage")
    && require(!succeeded, "preparation cancellation must not publish success")
    && require(!failure.has_value(), "preparation cancellation must not publish failure")
    && require(!QFileInfo(QDir(jobs.path()).filePath(
      "prepare-cancel/output/gmsh-intermediate.msh")).exists(),
      "preparation cancellation should not launch the Worker")
    && require(!controller.isRunning(),
      "controller should be idle after preparation cancellation");
}

bool invalidSubmissionIsRejectedBeforeWorkStarts(
  const QString& worker,
  const QString& backend,
  const SnapshotInput& snapshot)
{
  QTemporaryDir jobs;
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  MeshingJobController controller(project, configuration(worker, backend, jobs.path()));

  const auto idleCancellation = controller.cancel("nothing active", 1'000);
  const auto rejection = controller.submit(submissionFor("../escape", snapshot));
  auto invalidRetryPolicy = submissionFor("invalid-retry-policy", snapshot);
  invalidRetryPolicy.retryPolicy.maximumAttempts = 3;
  const auto retryPolicyRejection = controller.submit(invalidRetryPolicy);
  return require(idleCancellation.has_value(),
      "idle controller cancellation should be rejected")
    && require(idleCancellation->kind == MeshingJobFailureKind::InvalidRequest,
      "idle cancellation should have invalid-request kind")
    && require(rejection.has_value(), "unsafe job ID should be rejected")
    && require(rejection->kind == MeshingJobFailureKind::InvalidRequest,
      "unsafe job ID should have invalid-request kind")
    && require(retryPolicyRejection.has_value()
        && retryPolicyRejection->kind == MeshingJobFailureKind::InvalidRequest,
      "an out-of-range retry attempt count should be rejected")
    && require(!controller.isRunning(), "rejected job should not start work")
    && require(!QFileInfo(QDir(jobs.path()).filePath("../escape")).exists(),
      "rejected job should not create a workspace outside the jobs root");
}
}

int main(int argc, char* argv[])
{
  QCoreApplication application(argc, argv);
  if (argc == 2
    && (QString::fromLocal8Bit(argv[1]) == "-version"
      || QString::fromLocal8Bit(argv[1]) == "--version"))
  {
    std::fputs("4.13.1\n", stdout);
    return 0;
  }
  if (application.arguments().contains("-3"))
  {
    const QByteArray retryBackend = qgetenv("CCEDA_TEST_RETRY_BACKEND");
    if (!retryBackend.isEmpty())
    {
      const QStringList arguments = application.arguments().mid(1);
      const qsizetype algorithmIndex = arguments.indexOf("-algo");
      if (algorithmIndex >= 0 && algorithmIndex + 1 < arguments.size()
        && arguments[algorithmIndex + 1] == "del3d")
      {
        std::fputs("simulated Delaunay generation failure\n", stderr);
        return 1;
      }
      QProcess backendProcess;
      backendProcess.setProcessChannelMode(QProcess::ForwardedChannels);
      backendProcess.start(QString::fromUtf8(retryBackend), arguments);
      if (!backendProcess.waitForStarted(5'000)
        || !backendProcess.waitForFinished(60'000))
      {
        backendProcess.kill();
        backendProcess.waitForFinished(5'000);
        return 1;
      }
      return backendProcess.exitStatus() == QProcess::NormalExit
        ? backendProcess.exitCode() : 1;
    }
    QThread::msleep(10'000);
    return 1;
  }
  if (argc != 4)
  {
    std::cerr << "Usage: MeshingJobControllerTests <worker> <gmsh> <step>\n";
    return EXIT_FAILURE;
  }

  const QString worker = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
  const QString backend = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
  const QString step = QFileInfo(QString::fromLocal8Bit(argv[3])).absoluteFilePath();
  const auto repository =
    std::make_shared<cceda::geometry::OccGeometryRepository>();
  cceda::geometry::OccGeometryService geometryService(repository);
  cceda::geometry::OccGeometrySnapshotService snapshotService(repository);
  const auto imported = geometryService.importStep(
    std::filesystem::path(step.toStdWString()));
  QTemporaryDir snapshotRoot(QDir::current().filePath(
    "cceda-controller-source-XXXXXX"));
  if (!imported.success || !snapshotRoot.isValid())
  {
    std::cerr << "Cannot create Controller source snapshot fixture\n";
    return EXIT_FAILURE;
  }
  const auto created = snapshotService.createSnapshot({
    imported.document.revision,
    std::filesystem::path(snapshotRoot.path().toStdWString()) / "geometry",
    32ULL * 1024ULL * 1024ULL,
  });
  if (!created.succeeded())
  {
    std::cerr << "Cannot create Controller snapshot: " << created.detail << '\n';
    return EXIT_FAILURE;
  }
  const SnapshotInput snapshot = snapshotInput(*created.snapshot);

  QTemporaryDir multiSource(QDir::current().filePath(
    "cceda-controller-multi-source-XXXXXX"));
  QTemporaryDir multiSnapshotRoot(QDir::current().filePath(
    "cceda-controller-multi-snapshot-XXXXXX"));
  const QString multiStep =
    QDir(multiSource.path()).filePath("two-solids.step");
  const auto multiRepository =
    std::make_shared<cceda::geometry::OccGeometryRepository>();
  cceda::geometry::OccGeometryService multiGeometryService(multiRepository);
  cceda::geometry::OccGeometrySnapshotService multiSnapshotService(
    multiRepository);
  if (!multiSource.isValid() || !multiSnapshotRoot.isValid()
    || !writeTwoSolidStep(multiStep))
  {
    std::cerr << "Cannot create Controller multi-region STEP fixture\n";
    return EXIT_FAILURE;
  }
  const auto multiImported = multiGeometryService.importStep(
    std::filesystem::path(multiStep.toStdWString()));
  const auto multiCreated = multiSnapshotService.createSnapshot({
    multiImported.document.revision,
    std::filesystem::path(multiSnapshotRoot.path().toStdWString()) / "geometry",
    32ULL * 1024ULL * 1024ULL,
  });
  if (!multiImported.success || !multiCreated.succeeded()
    || multiCreated.snapshot->regions.size() != 2)
  {
    std::cerr << "Cannot create Controller multi-region snapshot fixture\n";
    return EXIT_FAILURE;
  }
  const SnapshotInput multiSnapshot = snapshotInput(*multiCreated.snapshot);

  const bool ok = successfulJobCommitsQualityValidMesh(
      worker, backend, snapshot)
    && multiRegionJobCommitsEverySolid(worker, backend, multiSnapshot)
    && resourceLimitedJobPreservesExistingMesh(worker, snapshot)
    && automaticRetryIsDisabledByDefault(worker, backend, snapshot)
    && retryPolicyIsBoundedAndUsesFreshWorkspace(worker, backend, snapshot)
    && concurrentSubmissionIsRejected(worker, backend, snapshot)
    && geometryChangeMakesInFlightResultStale(worker, backend, snapshot)
    && modifiedWorkerOutputIsRejected(worker, backend, snapshot)
    && modifiedNeutralMeshIsRejected(worker, backend, snapshot)
    && modifiedQualityIsRejected(worker, backend, snapshot)
    && modifiedSealedInputMapIsRejected(worker, backend, snapshot)
    && cancelledJobDoesNotPublishOrMutateProject(worker, snapshot)
    && preparationCanBeCancelledBeforeWorkerLaunch(worker, backend, snapshot)
    && invalidSubmissionIsRejectedBeforeWorkStarts(worker, backend, snapshot);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
