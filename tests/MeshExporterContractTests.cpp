#include "application/IMeshExporter.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
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

cceda::domain::MeshModel makeCommittedMesh()
{
  using namespace cceda::domain;

  MeshModel mesh;
  mesh.meshRevision = 3;
  mesh.sourceGeometryRevision = 7;
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

cceda::domain::MeshExportSettings makeSettings()
{
  using namespace cceda::domain;

  MeshExportSettings settings;
  settings.formatVersion = MeshExportFormatVersion{4, 1};
  settings.encoding = MeshExportEncoding::Ascii;
  settings.regionPolicyId = "physical-groups";
  return settings;
}

class RecordingExporter final : public cceda::application::IMeshExporter
{
public:
  cceda::application::MeshExportStageResult writeStagedMesh(
    const cceda::domain::MeshModel& mesh,
    const cceda::domain::MeshExportSettings& settings,
    const std::filesystem::path& stagingPath) noexcept override
  {
    receivedMeshRevision = mesh.meshRevision;
    receivedPolicy = settings.regionPolicyId;
    receivedPath = stagingPath;
    return {cceda::application::MeshExportStageStatus::Succeeded, {}, true};
  }

  cceda::domain::MeshRevision receivedMeshRevision = 0;
  std::string receivedPolicy;
  std::filesystem::path receivedPath;
};
}

int main()
{
  using namespace cceda;

  const domain::MeshModel committedMesh = makeCommittedMesh();
  const domain::MeshExportSettings settings = makeSettings();
  const std::filesystem::path stagingPath = "D:/exports/model.msh.tmp";

  expect(application::isValidMeshExportStageRequest(
      committedMesh, settings, stagingPath),
    "a committed mesh, explicit settings, and normalized absolute staging path are valid");

  domain::MeshModel workerMesh = committedMesh;
  workerMesh.meshRevision = 0;
  expect(!application::isValidMeshExportStageRequest(
      workerMesh, settings, stagingPath),
    "an uncommitted Worker mesh cannot be staged for export");

  domain::MeshExportSettings incompleteSettings;
  expect(!application::isValidMeshExportStageRequest(
      committedMesh, incompleteSettings, stagingPath),
    "incomplete settings cannot reach an exporter");

  expect(!application::isValidMeshExportStageRequest(
      committedMesh, settings, "model.msh.tmp"),
    "a relative staging path is rejected");
  expect(!application::isValidMeshExportStageRequest(
      committedMesh, settings, "D:/exports/../model.msh.tmp"),
    "a non-normalized staging path is rejected");
  expect(!application::isValidMeshExportStageRequest(
      committedMesh, settings, std::filesystem::path{}),
    "an empty staging path is rejected");
  expect(!application::isValidMeshExportStageRequest(
      committedMesh, settings, "D:/"),
    "an absolute root without a staging filename is rejected");

  using application::MeshExportStageResult;
  using application::MeshExportStageStatus;
  expect(MeshExportStageResult{
      MeshExportStageStatus::Succeeded, {}, true}.succeeded(),
    "success requires both succeeded status and staging-file ownership");
  expect(!MeshExportStageResult{
      MeshExportStageStatus::Succeeded, {}, false}.succeeded(),
    "a succeeded status without staging-file ownership fails closed");
  for (const MeshExportStageStatus status : {
         MeshExportStageStatus::InvalidRequest,
         MeshExportStageStatus::UnsupportedSettings,
         MeshExportStageStatus::WriteFailed,
         static_cast<MeshExportStageStatus>(255)})
  {
    expect(!MeshExportStageResult{status, "diagnostic"}.succeeded(),
      "every failure or unknown status fails closed");
  }

  RecordingExporter exporter;
  const MeshExportStageResult result =
    exporter.writeStagedMesh(committedMesh, settings, stagingPath);
  expect(result.succeeded()
      && exporter.receivedMeshRevision == committedMesh.meshRevision
      && exporter.receivedPolicy == settings.regionPolicyId
      && exporter.receivedPath == stagingPath,
    "the port carries neutral mesh data, explicit settings, and only a staging path");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
