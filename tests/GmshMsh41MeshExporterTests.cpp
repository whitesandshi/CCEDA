#include "meshing/GmshMsh41MeshExporter.h"
#include "meshing/GmshMsh41Reader.h"

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
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

cceda::domain::MeshModel makeCommittedMesh()
{
  using namespace cceda::domain;

  MeshModel mesh;
  mesh.meshRevision = 9;
  mesh.sourceGeometryRevision = 5;
  mesh.creationTimeUtc = "2026-07-28T00:00:00.000Z";
  mesh.backendId = "gmsh";
  mesh.backendVersion = "4.15.2";
  mesh.conformity = MeshConformity::NonConformal;
  mesh.nodes = {
    {1, 0.0, 0.0, 0.0, "solid-a/vertex-1"},
    {2, 1.0, 0.0, 0.0, "solid-a/vertex-2"},
    {3, 0.0, 1.0, 0.0, "solid-a/vertex-3"},
    {4, 0.0, 0.0, 1.0, "solid-a/vertex-4"},
    {11, 2.0, 0.0, 0.0, "solid-b/vertex-1"},
    {12, 3.0, 0.0, 0.0, "solid-b/vertex-2"},
    {13, 2.0, 1.0, 0.0, "solid-b/vertex-3"},
    {14, 2.0, 0.0, 1.0, "solid-b/vertex-4"},
  };
  mesh.elements = {
    {101, MeshElementType::Triangle3, {1, 3, 2, 0}, 10, "solid-a/face-1"},
    {102, MeshElementType::Triangle3, {1, 2, 4, 0}, 10, "solid-a/face-2"},
    {103, MeshElementType::Triangle3, {2, 3, 4, 0}, 10, "solid-a/face-3"},
    {104, MeshElementType::Triangle3, {3, 1, 4, 0}, 10, "solid-a/face-4"},
    {105, MeshElementType::Tetrahedron4, {1, 2, 3, 4}, 10, "solid-a"},
    {201, MeshElementType::Triangle3, {11, 13, 12, 0}, 20, "solid-b/face-1"},
    {202, MeshElementType::Triangle3, {11, 12, 14, 0}, 20, "solid-b/face-2"},
    {203, MeshElementType::Triangle3, {12, 13, 14, 0}, 20, "solid-b/face-3"},
    {204, MeshElementType::Triangle3, {13, 11, 14, 0}, 20, "solid-b/face-4"},
    {205, MeshElementType::Tetrahedron4, {11, 12, 13, 14}, 20, "solid-b"},
  };
  mesh.regions = {{10, {"solid-a"}}, {20, {"solid-b"}}};
  return mesh;
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

std::filesystem::path pathFromQString(const QString& path)
{
  return std::filesystem::path(path.toStdWString());
}

std::string readFile(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
  {
    return {};
  }
  return file.readAll().toStdString();
}

cceda::meshing::GmshMeshMapping expectedMapping()
{
  using namespace cceda;

  meshing::GmshMeshMapping mapping;
  mapping.entities = {
    {2, 1, 10, "solid-a/face-1"},
    {2, 2, 10, "solid-a/face-2"},
    {2, 3, 10, "solid-a/face-3"},
    {2, 4, 10, "solid-a/face-4"},
    {2, 5, 20, "solid-b/face-1"},
    {2, 6, 20, "solid-b/face-2"},
    {2, 7, 20, "solid-b/face-3"},
    {2, 8, 20, "solid-b/face-4"},
    {3, 1, 10, "solid-a"},
    {3, 2, 20, "solid-b"},
  };
  mapping.regions = {{10, {"solid-a"}}, {20, {"solid-b"}}};
  return mapping;
}
}

int main(int argc, char* argv[])
{
  using namespace cceda;
  using application::MeshExportStageStatus;

  if (argc != 2)
  {
    std::cerr << "Expected the configured Gmsh executable path.\n";
    return EXIT_FAILURE;
  }

  QTemporaryDir temporaryDirectory;
  expect(temporaryDirectory.isValid(), "a temporary export directory is available");

  const domain::MeshModel mesh = makeCommittedMesh();
  const domain::MeshExportSettings settings = supportedSettings();
  meshing::GmshMsh41MeshExporter exporter;
  const QString stagingFile = temporaryDirectory.filePath("mesh.stage.msh");
  const auto result = exporter.writeStagedMesh(
    mesh, settings, pathFromQString(stagingFile));
  expect(result.succeeded(), "supported settings write a staged MSH file");

  const std::string contents = readFile(stagingFile);
  expect(contents.find("$MeshFormat\n4.1 0 8\n") != std::string::npos,
    "the staged file declares ASCII MSH 4.1");
  expect(contents.find("2 10 \"region-10-surface\"") != std::string::npos
      && contents.find("3 20 \"region-20-volume\"") != std::string::npos,
    "stable region IDs become dimensioned Physical Groups");
  expect(contents.find("101 1 3 2") != std::string::npos
      && contents.find("205 11 12 13 14") != std::string::npos,
    "neutral element and node IDs are preserved as MSH tags");
  expect(contents.find("$Nodes\n2 8 1 14\n3 1 0 4\n1\n2\n3\n4\n")
      != std::string::npos
      && contents.find("3 2 0 4\n11\n12\n13\n14\n") != std::string::npos,
    "nodes are classified by their actual volume entity");

  const QString repeatFile = temporaryDirectory.filePath("repeat.stage.msh");
  expect(exporter.writeStagedMesh(
      mesh, settings, pathFromQString(repeatFile)).succeeded()
      && readFile(repeatFile) == contents,
    "the same neutral mesh produces deterministic staged bytes");

  std::istringstream input(contents);
  const meshing::GmshMsh41ReadResult decoded = meshing::readGmshMsh41(
    input,
    {mesh.sourceGeometryRevision, mesh.creationTimeUtc, mesh.backendVersion,
      mesh.conformity},
    expectedMapping(),
    {1024 * 1024, 4096, 100, 100, 100});
  expect(decoded.succeeded()
      && decoded.mesh->nodes.size() == mesh.nodes.size()
      && decoded.mesh->elements.size() == mesh.elements.size()
      && decoded.mesh->regions.size() == mesh.regions.size(),
    "the existing bounded reader recovers all exported neutral mesh counts");

  QProcess gmsh;
  gmsh.start(QString::fromLocal8Bit(argv[1]), {stagingFile, "-check"});
  expect(gmsh.waitForStarted(5000) && gmsh.waitForFinished(15000)
      && gmsh.exitStatus() == QProcess::NormalExit && gmsh.exitCode() == 0,
    "the configured Gmsh executable accepts the staged file");

  const QString existingFile = temporaryDirectory.filePath("existing.stage.msh");
  QFile existing(existingFile);
  expect(existing.open(QIODevice::WriteOnly) && existing.write("sentinel") == 8,
    "the overwrite sentinel is created");
  existing.close();
  expect(exporter.writeStagedMesh(
      mesh, settings, pathFromQString(existingFile)).status
      == MeshExportStageStatus::WriteFailed
      && readFile(existingFile) == "sentinel",
    "an existing staging file is rejected without truncation");

  domain::MeshExportSettings unsupported = settings;
  unsupported.encoding = domain::MeshExportEncoding::Binary;
  const QString unsupportedFile = temporaryDirectory.filePath("binary.msh.stage");
  expect(exporter.writeStagedMesh(
      mesh, unsupported, pathFromQString(unsupportedFile)).status
      == MeshExportStageStatus::UnsupportedSettings
      && !QFile::exists(unsupportedFile),
    "binary output fails closed without creating a file");

  unsupported = settings;
  unsupported.formatVersion = domain::MeshExportFormatVersion{4, 0};
  expect(exporter.writeStagedMesh(
      mesh, unsupported,
      pathFromQString(temporaryDirectory.filePath("msh40.stage"))).status
      == MeshExportStageStatus::UnsupportedSettings,
    "a non-4.1 version is not silently upgraded");

  unsupported = settings;
  unsupported.regionPolicyId = "element-data";
  expect(exporter.writeStagedMesh(
      mesh, unsupported,
      pathFromQString(temporaryDirectory.filePath("policy.stage"))).status
      == MeshExportStageStatus::UnsupportedSettings,
    "an unknown region policy is not interpreted by the adapter");

  domain::MeshModel workerMesh = mesh;
  workerMesh.meshRevision = 0;
  const QString invalidFile = temporaryDirectory.filePath("worker.stage");
  expect(exporter.writeStagedMesh(
      workerMesh, settings, pathFromQString(invalidFile)).status
      == MeshExportStageStatus::InvalidRequest
      && !QFile::exists(invalidFile),
    "an uncommitted Worker mesh cannot create an export file");

  domain::MeshModel oversizedRegion = mesh;
  const domain::MeshRegionId unrepresentableRegion =
    static_cast<domain::MeshRegionId>(std::numeric_limits<int>::max()) + 1;
  oversizedRegion.regions.front().id = unrepresentableRegion;
  for (domain::MeshElement& element : oversizedRegion.elements)
  {
    if (element.regionId == 10)
    {
      element.regionId = unrepresentableRegion;
    }
  }
  const QString oversizedRegionFile =
    temporaryDirectory.filePath("oversized-region.stage");
  expect(exporter.writeStagedMesh(
      oversizedRegion, settings, pathFromQString(oversizedRegionFile)).status
      == MeshExportStageStatus::InvalidRequest
      && !QFile::exists(oversizedRegionFile),
    "an unrepresentable Physical Group tag is rejected before file creation");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
