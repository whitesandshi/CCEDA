#include "application/MeshExportTransaction.h"
#include "platform/WindowsStagedFilePublisher.h"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace
{
int failures = 0;

void expect(bool condition, std::string_view message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

cceda::domain::MeshModel makeWorkerMesh(
  cceda::domain::GeometryRevision geometryRevision)
{
  using namespace cceda::domain;

  MeshModel mesh;
  mesh.sourceGeometryRevision = geometryRevision;
  mesh.creationTimeUtc = "2026-07-28T00:00:00.000Z";
  mesh.backendId = "gmsh";
  mesh.backendVersion = "4.15.2";
  mesh.conformity = MeshConformity::NonConformal;
  mesh.nodes = {
    {1, 0.0, 0.0, 0.0, "solid-1/vertex-1"},
    {2, 1.0, 0.0, 0.0, "solid-1/vertex-2"},
    {3, 0.0, 1.0, 0.0, "solid-1/vertex-3"},
    {4, 0.0, 0.0, 1.0, "solid-1/vertex-4"},
  };
  mesh.elements = {
    {101, MeshElementType::Triangle3, {1, 2, 3, 0}, 10, "solid-1/face-1"},
    {102, MeshElementType::Tetrahedron4, {1, 2, 3, 4}, 10, "solid-1"},
  };
  mesh.regions = {{10, {"solid-1"}}};
  return mesh;
}

cceda::domain::ProjectState makeReadyProject()
{
  cceda::domain::ProjectState state;
  state.commitImportedGeometry();
  expect(state.commitMesh(makeWorkerMesh(state.geometryRevision())),
    "test project accepts its valid Worker mesh");
  return state;
}

cceda::domain::MeshExportSettings supportedSettings()
{
  using namespace cceda::domain;

  MeshExportSettings settings;
  settings.formatVersion = MeshExportFormatVersion{4, 1};
  settings.encoding = MeshExportEncoding::Ascii;
  settings.regionPolicyId = "physical-groups";
  return settings;
}

void writeText(const std::filesystem::path& path, std::string_view text)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readText(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

class FileWritingExporter final : public cceda::application::IMeshExporter
{
public:
  cceda::application::MeshExportStageResult writeStagedMesh(
    const cceda::domain::MeshModel& mesh,
    const cceda::domain::MeshExportSettings&,
    const std::filesystem::path& stagingPath) noexcept override
  {
    receivedMeshRevision = mesh.meshRevision;
    receivedPath = stagingPath;
    if (status == cceda::application::MeshExportStageStatus::Succeeded
      || writeBeforeFailure || createUnownedBeforeFailure)
    {
      writeText(stagingPath, contents);
    }
    if (projectToInvalidate != nullptr)
    {
      projectToInvalidate->commitGeometryChange();
    }
    return {status, status == cceda::application::MeshExportStageStatus::Succeeded
        ? std::string{}
        : std::string{"injected staging failure"},
      status == cceda::application::MeshExportStageStatus::Succeeded
        || writeBeforeFailure};
  }

  cceda::application::MeshExportStageStatus status =
    cceda::application::MeshExportStageStatus::Succeeded;
  cceda::domain::ProjectState* projectToInvalidate = nullptr;
  bool writeBeforeFailure = false;
  bool createUnownedBeforeFailure = false;
  cceda::domain::MeshRevision receivedMeshRevision = 0;
  std::filesystem::path receivedPath;
  std::string contents = "new mesh\n";
};

struct ScopedDirectory
{
  explicit ScopedDirectory(std::filesystem::path value) : path(std::move(value))
  {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path);
  }

  ~ScopedDirectory()
  {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  std::filesystem::path path;
};
}

int main()
{
  using namespace cceda;
  using application::MeshExportStatus;

  static_assert(!std::is_copy_constructible_v<application::MeshExportTransaction>);
  static_assert(!std::is_copy_constructible_v<application::StagedMeshExport>);

  const ScopedDirectory directory(
    std::filesystem::temp_directory_path()
      / ("cceda-mesh-export-transaction-"
        + std::to_string(GetCurrentProcessId())));
  const auto pathFor = [&directory](std::string_view filename) {
    return std::filesystem::absolute(directory.path / filename).lexically_normal();
  };
  const std::filesystem::path finalPath = pathFor("model.msh");
  const domain::MeshExportSettings settings = supportedSettings();
  platform::WindowsStagedFilePublisher publisher;

  domain::ProjectState missingProject;
  expect(application::beginMeshExport(missingProject, settings, finalPath).status
      == MeshExportStatus::NoCurrentMesh,
    "a project without a ready current mesh cannot begin export");

  domain::ProjectState project = makeReadyProject();
  expect(application::beginMeshExport(project, settings, "model.msh").status
      == MeshExportStatus::InvalidRequest,
    "a relative final path is rejected before snapshot capture");
  MeshExportStatus offOwnerBeginStatus = MeshExportStatus::Succeeded;
  std::thread offOwnerBegin([&project, &settings, &finalPath,
      &offOwnerBeginStatus] {
    offOwnerBeginStatus =
      application::beginMeshExport(project, settings, finalPath).status;
  });
  offOwnerBegin.join();
  expect(offOwnerBeginStatus == MeshExportStatus::InvalidRequest,
    "beginning export away from the ProjectState owner thread is rejected");

  application::MeshExportBeginResult begin =
    application::beginMeshExport(project, settings, finalPath);
  expect(begin.succeeded(),
    "a ready current mesh and explicit request begin an export transaction");
  if (!begin.transaction)
  {
    return EXIT_FAILURE;
  }
  expect(begin.transaction->finalPath() == finalPath
      && begin.transaction->meshRevision() == project.meshRevision()
      && begin.transaction->geometryRevision() == project.geometryRevision(),
    "the transaction seals the final path and both exact revisions");

  FileWritingExporter exporter;
  writeText(finalPath, "old mesh\n");
  application::MeshExportTransaction transaction =
    std::move(*begin.transaction);
  std::optional<application::MeshExportStageOutcome> backgroundStage;
  std::thread stagingThread(
    [&backgroundStage, &exporter, transaction = std::move(transaction)]() mutable {
      backgroundStage.emplace(application::stageMeshExport(
        std::move(transaction), exporter));
    });
  stagingThread.join();
  expect(backgroundStage.has_value(),
    "staging can complete on a background thread");
  if (!backgroundStage)
  {
    return EXIT_FAILURE;
  }
  application::MeshExportStageOutcome staged = std::move(*backgroundStage);
  expect(!begin.succeeded(),
    "moving the transaction makes the begin result inactive");
  expect(staged.succeeded()
      && exporter.receivedMeshRevision == project.meshRevision()
      && exporter.receivedPath.parent_path() == finalPath.parent_path()
      && exporter.receivedPath != finalPath
      && exporter.receivedPath.extension() == finalPath.extension()
      && std::filesystem::exists(exporter.receivedPath),
    "staging writes the captured mesh to a same-directory unique path");
  if (!staged.stagedExport)
  {
    return EXIT_FAILURE;
  }
  expect(application::publishMeshExport(project,
      std::move(*staged.stagedExport), publisher).succeeded()
      && readText(finalPath) == exporter.contents
      && !std::filesystem::exists(exporter.receivedPath),
    "publication replaces an existing final file and consumes staging");
  expect(!staged.succeeded(),
    "moving the staged capability makes the stage outcome inactive");
  FileWritingExporter replayExporter;
  expect(application::stageMeshExport(
      std::move(transaction), replayExporter).status
      == MeshExportStatus::InvalidRequest,
    "a moved transaction cannot stage a second file");
  expect(application::publishMeshExport(project,
      std::move(*staged.stagedExport), publisher).status
      == MeshExportStatus::InvalidRequest,
    "a consumed staged capability cannot be replayed");

  const std::filesystem::path newFinalPath = pathFor("new-model.msh");
  domain::ProjectState newFileProject = makeReadyProject();
  auto newFileBegin = application::beginMeshExport(
    newFileProject, settings, newFinalPath);
  FileWritingExporter newFileExporter;
  auto newFileStage = application::stageMeshExport(
    std::move(*newFileBegin.transaction), newFileExporter);
  expect(application::publishMeshExport(newFileProject,
      std::move(*newFileStage.stagedExport), publisher).succeeded()
      && readText(newFinalPath) == newFileExporter.contents,
    "publication also creates a previously absent final file");

  domain::ProjectState abandonedProject = makeReadyProject();
  auto abandonedBegin = application::beginMeshExport(
    abandonedProject, settings, pathFor("abandoned.msh"));
  FileWritingExporter abandonedExporter;
  {
    auto abandonedStage = application::stageMeshExport(
      std::move(*abandonedBegin.transaction), abandonedExporter);
    expect(abandonedStage.succeeded(),
      "the abandonment fixture stages successfully");
  }
  expect(!std::filesystem::exists(abandonedExporter.receivedPath),
    "destroying an unpublished staged capability removes its owned file");

  const std::filesystem::path staleFinalPath = pathFor("stale.msh");
  writeText(staleFinalPath, "current mesh\n");
  domain::ProjectState staleProject = makeReadyProject();
  auto staleBegin = application::beginMeshExport(
    staleProject, settings, staleFinalPath);
  FileWritingExporter staleExporter;
  staleExporter.projectToInvalidate = &staleProject;
  auto staleStage = application::stageMeshExport(
    std::move(*staleBegin.transaction), staleExporter);
  expect(application::publishMeshExport(staleProject,
      std::move(*staleStage.stagedExport), publisher).status
      == MeshExportStatus::SnapshotChanged
      && readText(staleFinalPath) == "current mesh\n"
      && !std::filesystem::exists(staleExporter.receivedPath),
    "a geometry change after staging preserves final output and removes staging");

  const std::filesystem::path remeshFinalPath = pathFor("remeshed.msh");
  writeText(remeshFinalPath, "current mesh\n");
  domain::ProjectState remeshProject = makeReadyProject();
  auto remeshBegin = application::beginMeshExport(
    remeshProject, settings, remeshFinalPath);
  FileWritingExporter remeshExporter;
  auto remeshStage = application::stageMeshExport(
    std::move(*remeshBegin.transaction), remeshExporter);
  expect(remeshProject.commitMesh(makeWorkerMesh(remeshProject.geometryRevision())),
    "the remesh fixture replaces the current mesh at the same geometry revision");
  expect(application::publishMeshExport(remeshProject,
      std::move(*remeshStage.stagedExport), publisher).status
      == MeshExportStatus::SnapshotChanged
      && readText(remeshFinalPath) == "current mesh\n"
      && !std::filesystem::exists(remeshExporter.receivedPath),
    "successful remeshing invalidates staged output by mesh identity and revision");

  const std::filesystem::path rejectedFinalPath = pathFor("rejected-remesh.msh");
  domain::ProjectState rejectedProject = makeReadyProject();
  auto rejectedBegin = application::beginMeshExport(
    rejectedProject, settings, rejectedFinalPath);
  FileWritingExporter rejectedExporter;
  auto rejectedStage = application::stageMeshExport(
    std::move(*rejectedBegin.transaction), rejectedExporter);
  domain::MeshModel malformed = makeWorkerMesh(rejectedProject.geometryRevision());
  malformed.elements.front().nodeIds.front() = 999;
  expect(!rejectedProject.commitMesh(std::move(malformed)),
    "the remesh fixture rejects malformed replacement output");
  expect(application::publishMeshExport(rejectedProject,
      std::move(*rejectedStage.stagedExport), publisher).succeeded()
      && readText(rejectedFinalPath) == rejectedExporter.contents,
    "a rejected mesh commit does not invalidate staged current output");

  const std::filesystem::path failedFinalPath = pathFor("failed.msh");
  writeText(failedFinalPath, "preserved\n");
  domain::ProjectState failedProject = makeReadyProject();
  auto failedBegin = application::beginMeshExport(
    failedProject, settings, failedFinalPath);
  FileWritingExporter failedExporter;
  failedExporter.status = application::MeshExportStageStatus::WriteFailed;
  failedExporter.writeBeforeFailure = true;
  const auto failedStage = application::stageMeshExport(
    std::move(*failedBegin.transaction), failedExporter);
  expect(failedStage.status == MeshExportStatus::StagingWriteFailed
      && readText(failedFinalPath) == "preserved\n"
      && !std::filesystem::exists(failedExporter.receivedPath),
    "a partial staging failure leaves final unchanged and removes partial output");

  domain::ProjectState collisionProject = makeReadyProject();
  auto collisionBegin = application::beginMeshExport(
    collisionProject, settings, pathFor("collision.msh"));
  FileWritingExporter collisionExporter;
  collisionExporter.status = application::MeshExportStageStatus::WriteFailed;
  collisionExporter.createUnownedBeforeFailure = true;
  const auto collisionStage = application::stageMeshExport(
    std::move(*collisionBegin.transaction), collisionExporter);
  expect(collisionStage.status == MeshExportStatus::StagingWriteFailed
      && readText(collisionExporter.receivedPath) == collisionExporter.contents,
    "a failure without ownership never removes the file occupying staging");

  domain::ProjectState unsupportedProject = makeReadyProject();
  auto unsupportedBegin = application::beginMeshExport(
    unsupportedProject, settings, pathFor("unsupported.msh"));
  FileWritingExporter unsupportedExporter;
  unsupportedExporter.status =
    application::MeshExportStageStatus::UnsupportedSettings;
  const auto unsupportedStage = application::stageMeshExport(
    std::move(*unsupportedBegin.transaction), unsupportedExporter);
  expect(unsupportedStage.status == MeshExportStatus::UnsupportedSettings
      && !std::filesystem::exists(unsupportedExporter.receivedPath),
    "adapter capability rejection remains a typed transaction result");

  const std::filesystem::path lockedFinalPath = pathFor("locked.msh");
  writeText(lockedFinalPath, "locked original\n");
  domain::ProjectState lockedProject = makeReadyProject();
  auto lockedBegin = application::beginMeshExport(
    lockedProject, settings, lockedFinalPath);
  FileWritingExporter lockedExporter;
  auto lockedStage = application::stageMeshExport(
    std::move(*lockedBegin.transaction), lockedExporter);
  const HANDLE lockedFile = CreateFileW(lockedFinalPath.c_str(), GENERIC_READ,
    FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  expect(lockedFile != INVALID_HANDLE_VALUE,
    "the publication-failure fixture locks the existing final file");
  const application::MeshExportResult lockedResult =
    application::publishMeshExport(
      lockedProject, std::move(*lockedStage.stagedExport), publisher);
  if (lockedFile != INVALID_HANDLE_VALUE)
  {
    CloseHandle(lockedFile);
  }
  expect(lockedResult.status == MeshExportStatus::PublicationFailed
      && readText(lockedFinalPath) == "locked original\n"
      && !std::filesystem::exists(lockedExporter.receivedPath),
    "a failed replacement preserves the final file and removes staging");

  domain::ProjectState wrongThreadProject = makeReadyProject();
  auto wrongThreadBegin = application::beginMeshExport(
    wrongThreadProject, settings, pathFor("wrong-thread.msh"));
  FileWritingExporter wrongThreadExporter;
  auto wrongThreadStage = application::stageMeshExport(
    std::move(*wrongThreadBegin.transaction), wrongThreadExporter);
  application::MeshExportResult wrongThreadResult;
  std::thread wrongThread([&wrongThreadProject, &publisher, &wrongThreadResult,
      stagedExport = std::move(*wrongThreadStage.stagedExport)]() mutable {
    wrongThreadResult = application::publishMeshExport(wrongThreadProject,
      std::move(stagedExport), publisher);
  });
  wrongThread.join();
  expect(wrongThreadResult.status == MeshExportStatus::InvalidRequest
      && !std::filesystem::exists(wrongThreadExporter.receivedPath),
    "publication is rejected away from the ProjectState owner thread");

  const std::filesystem::path otherDirectory = directory.path / "other";
  std::filesystem::create_directory(otherDirectory);
  const std::filesystem::path invalidStage = otherDirectory / "invalid.msh";
  writeText(invalidStage, "not publishable\n");
  expect(publisher.publishStagedFile(invalidStage, finalPath).status
      == application::StagedFilePublishStatus::InvalidRequest
      && std::filesystem::exists(invalidStage),
    "the publisher rejects cross-directory paths without touching either file");

  const std::filesystem::path caseAliasStage = pathFor("Case-Alias.msh");
  const std::filesystem::path caseAliasFinal = pathFor("case-alias.msh");
  expect(publisher.publishStagedFile(caseAliasStage, caseAliasFinal).status
      == application::StagedFilePublishStatus::InvalidRequest,
    "the Windows publisher rejects case-insensitive aliases of one path");

  if (failures == 0)
  {
    std::cout << "All mesh export transaction tests passed.\n";
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
