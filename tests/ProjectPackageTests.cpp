#include "geometry/OccGeometryRepository.h"
#include "geometry/OccGeometryService.h"
#include "persistence/ProjectPackage.h"
#include "persistence/ProjectZipArchive.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TDataStd_Name.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Ax1.hxx>

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool condition, const char* reason)
{
  if (!condition) { throw std::runtime_error(reason); }
}

QByteArray fileBytes(const QString& path)
{
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "test file can be read");
  return file.readAll();
}

cceda::persistence::ProjectZipOptions packageOptions()
{
  cceda::persistence::ProjectZipOptions options;
  options.resources.supportedFeatures = {"project-snapshot-0.1"};
  return options;
}

void replacePayload(const QString& original, const QString& destination, const QString& resource,
  const std::function<QByteArray(const QByteArray&)>& edit)
{
  using namespace cceda::persistence;
  auto source = ProjectZipReader::open(original, packageOptions());
  require(source.succeeded(), "read valid package for test mutation");
  auto manifest = source.archive->manifest();
  QHash<QString, QByteArray> payloads;
  for (auto& entry : manifest.entries)
  {
    auto input = source.archive->openResource(entry.path);
    auto bytes = input.device->readAll();
    if (entry.path == resource)
    {
      bytes = edit(bytes);
      entry.uncompressedBytes = static_cast<std::uint64_t>(bytes.size());
      entry.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    }
    payloads.insert(entry.path, std::move(bytes));
  }
  const auto replaced = writeNewProjectZip(destination, manifest, [&payloads](const QString& name) {
    auto buffer = std::make_unique<QBuffer>();
    buffer->setData(payloads.value(name));
    buffer->open(QIODevice::ReadOnly);
    return ProjectResourceOpenResult{std::move(buffer)};
  }, packageOptions());
  require(replaced.succeeded(), "modified archive still passes ZIP integrity checks");
}

void writeAssembly(const QString& path)
{
  const occ::handle<TDocStd_Document> document = new TDocStd_Document("BinXCAF");
  const auto shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
  const auto colors = XCAFDoc_DocumentTool::ColorTool(document->Main());
  XCAFDoc_DocumentTool::SetLengthUnit(document, 1.0, UnitsMethods_LengthUnit_Millimeter);
  const auto part = shapes->AddShape(BRepPrimAPI_MakeBox(10, 20, 30).Shape());
  TDataStd_Name::Set(part, "Colored box");
  colors->SetColor(part, Quantity_Color(0.15, 0.65, 0.8, Quantity_TOC_sRGB), XCAFDoc_ColorSurf);
  const auto assembly = shapes->NewShape();
  TDataStd_Name::Set(assembly, "Two parts");
  const auto instance1 = shapes->AddComponent(assembly, part, TopLoc_Location());
  TDataStd_Name::Set(instance1, "First instance");
  gp_Trsf transform;
  transform.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0.3);
  transform.SetTranslationPart(gp_Vec(80, 0, 0));
  const auto instance2 = shapes->AddComponent(assembly, part, TopLoc_Location(transform));
  TDataStd_Name::Set(instance2, "Second instance");
  shapes->UpdateAssemblies();
  STEPCAFControl_Writer writer;
  require(writer.Transfer(document, STEPControl_AsIs)
    && writer.Write(path.toUtf8().constData()) == IFSelect_RetDone, "write XCAF assembly fixture to STEP");
}

cceda::domain::MeshModel workerMesh(cceda::domain::GeometryRevision revision,
  const std::string& solid)
{
  using namespace cceda::domain;
  MeshModel mesh;
  mesh.sourceGeometryRevision = revision;
  mesh.creationTimeUtc = "2026-07-23T00:00:00.000Z";
  mesh.backendId = "gmsh";
  mesh.backendVersion = "4.15.2";
  mesh.conformity = MeshConformity::NonConformal;
  mesh.nodes = {{1, 0, 0, 0}, {2, 1, 0, 0}, {3, 0, 1, 0}, {4, 0, 0, 1}};
  // Shared edge with the same direction exercises restoration of accepted warnings.
  mesh.elements = {{101, MeshElementType::Triangle3, {1, 2, 3}, 10, solid},
    {102, MeshElementType::Triangle3, {1, 2, 4}, 10, solid},
    {103, MeshElementType::Tetrahedron4, {1, 2, 3, 4}, 10, solid}};
  mesh.regions = {{10, {solid}}};
  return mesh;
}
}

int main(int argc, char** argv)
{
  using namespace cceda;
  using namespace cceda::persistence;
  QCoreApplication application(argc, argv);
  try
  {
    require(argc == 2, "expected a STEP fixture path");
    QTemporaryDir temp;
    require(temp.isValid(), "temporary directory is available");
    const auto step = temp.filePath("Original.step");
    require(QFile::copy(QString::fromLocal8Bit(argv[1]), step), "STEP fixture is copied");
    const auto original = fileBytes(step);
    const auto repository = std::make_shared<geometry::OccGeometryRepository>();
    geometry::OccGeometryService geometry(repository);
    auto imported = importProjectStep(step, geometry);
    if (!imported.geometry.success) { std::cerr << imported.geometry.message << '\n'; }
    require(imported.geometry.success && QFile::remove(step),
      "import retained the source before the external file was deleted");
    require(!imported.geometry.document.sourceLengthUnits.empty()
      && imported.geometry.document.sourceLengthUnits.front().millimetersPerUnit == 1.0,
      "millimeter STEP source units and conversion are recorded");
    const auto& entities = imported.geometry.document.entities;
    const auto solid = std::find_if(entities.begin(), entities.end(),
      [](const auto& entity) { return entity.kind == domain::GeometryKind::Solid && entity.meshable; });
    require(solid != entities.end(), "fixture has a meshable Solid");
    domain::ProjectState state;
    state.commitImportedGeometry();
    require(state.commitMesh(workerMesh(state.geometryRevision(), solid->id))
      && state.commitMesh(workerMesh(state.geometryRevision(), solid->id)),
      "test mesh commits with a noninitial revision");

    StoredMeshSettings settings;
    settings.mode = domain::MeshGenerationMode::ConformalMultiRegion;
    settings.resourceLimits.maximumElapsedMilliseconds = 125432;
    settings.generation.maximumSizeMm = 0.015234567;
    const auto identity = newProjectIdentity();
    const auto snapshot = ProjectSaveSnapshot::capture(state, identity, imported.source, settings);
    require(bool(snapshot), "capture on owner thread");
    const auto path = temp.filePath("Roundtrip.cceda");
    auto result = saveProjectPackage(path, *snapshot, geometry, "CCEDA/test");
    if (!result.isEmpty()) { std::cerr << result.toStdString() << '\n'; }
    require(result.isEmpty(), "save publishes the complete package");
    auto opened = openProjectPackage(path, geometry);
    if (!opened.error.isEmpty()) { std::cerr << opened.error.toStdString() << '\n'; }
    require(bool(opened.project), "verified XCAF and mesh reopen without an external STEP");
    auto& project = *opened.project;
    require(project.identity.id == identity.id && project.identity.createdAt == identity.createdAt,
      "open retains the project identity");
    require(fileBytes(project.source.path()) == original, "stored STEP bytes are exact");
    require(project.geometry->document.sourceName == "Original.step"
      && project.geometry->document.solidCount == imported.geometry.document.solidCount,
      "XCAF preserves the source name and geometry counts");
    require(project.mesh->meshRevision == 2 && project.mesh->sourceGeometryRevision == 1
      && project.quality->meshRevision == 2 && project.quality->issueElementDetails.count(101) == 1,
      "accepted quality and issue detail use the restored committed revision");
    require(project.settings.resourceLimits.maximumElapsedMilliseconds == 125432
      && project.settings.generation.maximumSizeMm == settings.generation.maximumSizeMm
      && project.settings.mode == settings.mode, "settings round-trip exactly");

    auto updatedSettings = settings;
    updatedSettings.resourceLimits.maximumElapsedMilliseconds = 250864;
    const auto replacement = ProjectSaveSnapshot::capture(state, identity, imported.source, updatedSettings);
    require(bool(replacement), "capture changed settings for replacement");
    auto locked = ProjectZipReader::open(path, packageOptions());
    require(locked.succeeded(), "target can be held read-only");
    const auto oldBytes = fileBytes(path);
    require(!saveProjectPackage(path, *replacement, geometry, "CCEDA/test").isEmpty()
      && fileBytes(path) == oldBytes, "failed replacement preserves the previous file");
    require(QDir(temp.path()).entryList({".cceda-save-*"}, QDir::Files | QDir::Hidden).isEmpty(),
      "staging file is removed when publication fails");
    locked.archive.reset();
    const auto preserved = openProjectPackage(path, geometry);
    require(preserved.project && preserved.project->settings.resourceLimits.maximumElapsedMilliseconds
      == settings.resourceLimits.maximumElapsedMilliseconds, "failed replacement leaves the old settings readable");
    require(saveProjectPackage(path, *replacement, geometry, "CCEDA/test").isEmpty(),
      "retry can atomically replace the previous project");
    const auto replaced = openProjectPackage(path, geometry);
    require(replaced.project && replaced.project->settings.resourceLimits.maximumElapsedMilliseconds
      == updatedSettings.resourceLimits.maximumElapsedMilliseconds, "reopening after retry reads the updated settings");

    const auto mismatched = temp.filePath("Mismatched.cceda");
    replacePayload(path, mismatched, "project.json", [](const auto& bytes) {
      auto projectJson = QJsonDocument::fromJson(bytes).object();
      projectJson.insert("lastMeshRevision", 57);
      return QJsonDocument(projectJson).toJson(QJsonDocument::Compact);
    });
    require(!openProjectPackage(mismatched, geometry).project,
      "valid resource digests do not make a mismatched mesh revision acceptable");
    const auto incompatible = temp.filePath("Incompatible.cceda");
    replacePayload(path, incompatible, "geometry/model.xbf", [](const auto&) {
      return QByteArray("not an XCAF document");
    });
    require(!openProjectPackage(incompatible, geometry).project,
      "incompatible XCAF cannot open via a fallback STEP import");
    const auto badQuality = temp.filePath("BadQuality.cceda");
    replacePayload(path, badQuality, "mesh/quality.json", [](const auto& bytes) {
      return bytes + ' ';
    });
    require(!openProjectPackage(badQuality, geometry).project,
      "quality is recomputed even when archive verification succeeds");

    domain::ProjectState empty;
    const auto emptySnapshot = ProjectSaveSnapshot::capture(empty, newProjectIdentity(), {}, {});
    const auto emptyPath = temp.filePath("Empty.cceda");
    require(emptySnapshot && saveProjectPackage(emptyPath, *emptySnapshot, geometry, "CCEDA/test").isEmpty(),
      "an empty project can be saved");
    const auto reopenedEmpty = openProjectPackage(emptyPath, geometry);
    require(reopenedEmpty.project && !reopenedEmpty.project->geometry && !reopenedEmpty.project->mesh,
      "an empty project can be prepared without replacing live geometry");
    require(geometry.currentDocument().revision == imported.geometry.document.revision,
      "preparing project open leaves live geometry untouched");

    const auto assemblyStep = temp.filePath("Assembly.step");
    writeAssembly(assemblyStep);
    const auto assembly = importProjectStep(assemblyStep, geometry);
    require(assembly.geometry.success && assembly.geometry.document.solidCount == 2,
      "two located instances import with separate topology");
    auto located = [](const auto& binding) {
      return binding.localTransform[3] > 79.9 && binding.color.explicitColor;
    };
    const auto& labels = assembly.geometry.document.xcafLabels;
    require(labels.size() == 3 && std::any_of(labels.begin(), labels.end(), located),
      "the assembly exposes its named hierarchy, placement, and color");
    state.commitImportedGeometry();
    const auto geometryOnly = ProjectSaveSnapshot::capture(state, newProjectIdentity(), assembly.source, {});
    const auto assemblyPackage = temp.filePath("Assembly.cceda");
    require(geometryOnly && saveProjectPackage(assemblyPackage, *geometryOnly, geometry, "CCEDA/test").isEmpty(),
      "geometry-only save omits the stale mesh");
    const auto restoredAssembly = openProjectPackage(assemblyPackage, geometry);
    if (!restoredAssembly.error.isEmpty()) { std::cerr << restoredAssembly.error.toStdString() << '\n'; }
    require(restoredAssembly.project && !restoredAssembly.project->mesh
      && restoredAssembly.project->lastMeshRevision == 2, "geometry-only open preserves the mesh revision counter");
    const auto& restoredLabels = restoredAssembly.project->geometry->document.xcafLabels;
    require(std::any_of(restoredLabels.begin(), restoredLabels.end(), located),
      "reopened XCAF reapplies the exact instance placement and color");
    return 0;
  }
  catch (const std::exception& failure)
  {
    std::cerr << "FAIL: " << failure.what() << '\n';
    return 1;
  }
}
