#include "application/MeshExportJobController.h"
#include "platform/WindowsStagedFilePublisher.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSemaphore>
#include <QThread>
#include <QTimer>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
    {101, MeshElementType::Triangle3, {1, 2, 3, 0}, 10,
      "solid-1/face-1"},
    {102, MeshElementType::Tetrahedron4, {1, 2, 3, 4}, 10, "solid-1"},
  };
  mesh.regions = {{10, {"solid-1"}}};
  return mesh;
}

cceda::domain::ProjectState makeReadyProject()
{
  cceda::domain::ProjectState project;
  project.commitImportedGeometry();
  expect(project.commitMesh(makeWorkerMesh(project.geometryRevision())),
    "the controller fixture commits a valid mesh");
  return project;
}

cceda::domain::MeshExportSettings supportedSettings()
{
  return {
    cceda::domain::MeshExportFormatVersion{4, 1},
    cceda::domain::MeshExportEncoding::Ascii,
    "physical-groups",
  };
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

class BlockingExporter final : public cceda::application::IMeshExporter
{
public:
  cceda::application::MeshExportStageResult writeStagedMesh(
    const cceda::domain::MeshModel&,
    const cceda::domain::MeshExportSettings&,
    const std::filesystem::path& stagingPath) noexcept override
  {
    receivedPath = stagingPath;
    entered.release();
    if (block)
    {
      proceed.acquire();
    }
    writeText(stagingPath, contents);
    return {cceda::application::MeshExportStageStatus::Succeeded, {}, true};
  }

  QSemaphore entered;
  QSemaphore proceed;
  bool block = false;
  std::filesystem::path receivedPath;
  std::string contents = "exported mesh\n";
};

std::optional<cceda::application::MeshExportResult> waitForResult(
  cceda::application::MeshExportJobController& controller)
{
  std::optional<cceda::application::MeshExportResult> result;
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(&controller,
    &cceda::application::MeshExportJobController::finished, &loop,
    [&result, &loop](const cceda::application::MeshExportResult& value) {
      result = value;
      loop.quit();
    });
  timeout.start(5'000);
  loop.exec();
  return result;
}

struct ScopedDirectory
{
  explicit ScopedDirectory(std::filesystem::path value)
    : path(std::move(value))
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

int main(int argc, char* argv[])
{
  QCoreApplication application(argc, argv);
  using namespace cceda;

  const ScopedDirectory directory(
    std::filesystem::temp_directory_path()
      / ("cceda-export-controller-tests-"
        + std::to_string(QCoreApplication::applicationPid())));
  const auto pathFor = [&directory](std::string_view filename) {
    return std::filesystem::absolute(directory.path / filename)
      .lexically_normal();
  };
  const auto settings = supportedSettings();
  const auto publisher =
    std::make_shared<platform::WindowsStagedFilePublisher>();

  domain::ProjectState missingProject;
  auto missingExporter = std::make_shared<BlockingExporter>();
  application::MeshExportJobController missingController(
    missingProject, missingExporter, publisher);
  const auto missing = missingController.start(settings, pathFor("missing.msh"));
  expect(missing.has_value()
      && missing->status == application::MeshExportStatus::NoCurrentMesh
      && !missingController.isRunning(),
    "a missing mesh is rejected synchronously without starting a task");

  const std::filesystem::path staleFinal = pathFor("stale.msh");
  writeText(staleFinal, "preserved\n");
  domain::ProjectState staleProject = makeReadyProject();
  auto staleExporter = std::make_shared<BlockingExporter>();
  staleExporter->block = true;
  application::MeshExportJobController staleController(
    staleProject, staleExporter, publisher);
  const auto started = staleController.start(settings, staleFinal);
  expect(!started.has_value() && staleController.isRunning(),
    "a valid request starts asynchronous staging");
  expect(staleExporter->entered.tryAcquire(1, 5'000),
    "staging runs on a background thread");
  const auto duplicate = staleController.start(settings, pathFor("duplicate.msh"));
  expect(duplicate.has_value()
      && duplicate->status == application::MeshExportStatus::InvalidRequest,
    "a second request is rejected while export is active");
  staleProject.commitGeometryChange();
  staleExporter->proceed.release();
  const auto staleResult = waitForResult(staleController);
  expect(staleResult.has_value()
      && staleResult->status == application::MeshExportStatus::SnapshotChanged
      && !staleController.isRunning()
      && readText(staleFinal) == "preserved\n"
      && !std::filesystem::exists(staleExporter->receivedPath),
    "owner-thread publication rejects stale output and cleans staging");

  const std::filesystem::path successFinal = pathFor("success.msh");
  domain::ProjectState successProject = makeReadyProject();
  auto successExporter = std::make_shared<BlockingExporter>();
  application::MeshExportJobController successController(
    successProject, successExporter, publisher);
  expect(!successController.start(settings, successFinal).has_value(),
    "a current mesh begins export without an immediate failure");
  const auto success = waitForResult(successController);
  expect(success.has_value() && success->succeeded()
      && !successController.isRunning()
      && readText(successFinal) == successExporter->contents,
    "the controller stages in the background and publishes on completion");

  if (failures == 0)
  {
    std::cout << "All mesh export job controller tests passed.\n";
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
