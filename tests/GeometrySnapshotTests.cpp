#include "geometry/IGeometrySnapshotService.h"
#include "geometry/OccGeometryRepository.h"
#include "geometry/OccGeometryService.h"
#include "geometry/OccGeometrySnapshotService.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <NCollection_IndexedMap.hxx>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <STEPControl_Writer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <gp.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>

namespace
{
int failures = 0;

void expect(bool condition, const char* message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

QByteArray readAll(const std::filesystem::path& path)
{
  QFile file(QString::fromUtf8(path.u8string().c_str()));
  if (!file.open(QIODevice::ReadOnly))
  {
    return {};
  }
  return file.readAll();
}

std::size_t meshableSolidCount(const cceda::domain::GeometryDocument& document)
{
  std::size_t count = 0;
  for (const cceda::domain::GeometryEntity& entity : document.entities)
  {
    if (entity.kind == cceda::domain::GeometryKind::Solid
        && entity.valid && entity.meshable)
    {
      ++count;
    }
  }
  return count;
}
}

int main(int argc, char** argv)
{
  using namespace cceda::geometry;
  if (argc != 2)
  {
    std::cerr << "Expected one STEP path.\n";
    return 2;
  }

  const auto repository = std::make_shared<OccGeometryRepository>();
  OccGeometryService geometryService(repository);
  OccGeometrySnapshotService snapshotService(repository);
  const auto imported = geometryService.importStep(std::filesystem::path(argv[1]));
  expect(imported.success, "the STEP fixture imports before snapshot creation");

  QTemporaryDir root(QDir::current().filePath(
    "cceda-geometry-snapshot-XXXXXX"));
  expect(root.isValid(), "a temporary snapshot root is available");
  const std::filesystem::path outputDirectory =
    std::filesystem::path(root.path().toStdWString()) / "geometry";
  const GeometrySnapshotResult created = snapshotService.createSnapshot({
    imported.document.revision,
    outputDirectory,
    32ULL * 1024ULL * 1024ULL,
  });
  if (!created.succeeded())
  {
    std::cerr << "Snapshot creation error " << static_cast<int>(created.error)
              << ": " << created.detail << '\n';
  }

  expect(created.succeeded(), "a current valid geometry revision produces a snapshot");
  expect(created.snapshot.has_value(), "successful creation returns snapshot metadata");
  if (created.snapshot)
  {
    const GeometrySnapshot& snapshot = *created.snapshot;
    expect(snapshot.sourceGeometryRevision == imported.document.revision,
      "snapshot owns the exact imported revision");
    expect(snapshot.schemaMajor == 0 && snapshot.schemaMinor == 2,
      "geometry map schema is explicitly versioned");
    expect(snapshot.canonicalUnit == "mm", "snapshot uses the canonical millimeter unit");
    expect(!snapshot.transformsBaked,
      "BREP locations are declared rather than misreported as baked transforms");
    expect(snapshot.regions.size() == meshableSolidCount(imported.document),
      "every valid meshable Solid has one independent region");
    expect(std::filesystem::is_regular_file(snapshot.geometryPath),
      "the BREP artifact is published");
    expect(std::filesystem::is_regular_file(snapshot.geometryMapPath),
      "the geometry map is published");
    expect(snapshot.geometrySha256.size() == 64
        && snapshot.geometryMapSha256.size() == 64,
      "both artifacts have SHA-256 digests");

    const QByteArray brepBytes = readAll(snapshot.geometryPath);
    const QByteArray mapBytes = readAll(snapshot.geometryMapPath);
    expect(!brepBytes.isEmpty() && !mapBytes.isEmpty(),
      "published snapshot artifacts are readable and non-empty");
    expect(snapshot.geometrySha256
        == QCryptographicHash::hash(brepBytes, QCryptographicHash::Sha256)
          .toHex().toStdString(),
      "BREP digest seals the published bytes");
    expect(snapshot.geometryMapSha256
        == QCryptographicHash::hash(mapBytes, QCryptographicHash::Sha256)
          .toHex().toStdString(),
      "geometry-map digest seals the published bytes");

    BRep_Builder builder;
    TopoDS_Shape brepShape;
    expect(BRepTools::Read(
        brepShape, snapshot.geometryPath.u8string().c_str(), builder),
      "the published BREP can be read by OCCT");
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> solids;
    TopExp::MapShapes(brepShape, TopAbs_SOLID, solids);
    expect(static_cast<std::size_t>(solids.Extent()) == snapshot.regions.size(),
      "the BREP contains exactly one shape for every mapped Solid region");

    QJsonParseError parseError;
    const QJsonDocument mapDocument = QJsonDocument::fromJson(mapBytes, &parseError);
    expect(parseError.error == QJsonParseError::NoError && mapDocument.isObject(),
      "geometry-map.json is valid JSON");
    const QJsonObject map = mapDocument.object();
    expect(map.value("schema").toObject().value("name").toString()
        == "cceda.geometry-map",
      "geometry map identifies its schema");
    expect(map.value("sourceGeometryRevision").toInteger()
        == static_cast<qint64>(imported.document.revision),
      "geometry map records the source revision");
    expect(map.value("mode").toString() == "independent-solids",
      "geometry map records independent-solid mode");
    expect(map.value("regions").toArray().size()
        == static_cast<qsizetype>(snapshot.regions.size()),
      "serialized and returned region counts agree");

    std::unordered_set<std::string> solidIds;
    std::uint64_t expectedRegionId = 1;
    for (const GeometrySnapshotRegion& region : snapshot.regions)
    {
      expect(region.regionId == expectedRegionId++,
        "snapshot region IDs are deterministic positive integers");
      expect(!region.partGeometryId.empty() && !region.solidGeometryId.empty(),
        "each region retains its source Part and Solid IDs");
      expect(solidIds.insert(region.solidGeometryId).second,
        "each source Solid occurs in exactly one independent region");
      expect(region.brepLocation.size() == 16,
        "each region carries one explicit 4x4 BREP location matrix");
      const auto& bounds = region.brepBoundsMm;
      expect(std::all_of(bounds.begin(), bounds.end(),
          [](double value) { return std::isfinite(value); })
          && bounds[0] <= bounds[3]
          && bounds[1] <= bounds[4]
          && bounds[2] <= bounds[5],
        "each region carries finite ordered BREP bounds in millimeters");
    }
    expect(geometryService.currentDocument().revision == imported.document.revision,
      "creating a snapshot does not mutate canonical geometry");
  }

  const std::filesystem::path conformalOutput =
    std::filesystem::path(root.path().toStdWString()) / "conformal";
  const GeometrySnapshotResult conformal = snapshotService.createSnapshot({
    imported.document.revision,
    conformalOutput,
    32ULL * 1024ULL * 1024ULL,
    cceda::domain::MeshGenerationMode::ConformalMultiRegion,
  });
  expect(conformal.succeeded(),
    "conformal mode creates a task-only fragmented snapshot");
  if (!conformal.succeeded())
  {
    std::cerr << "Conformal snapshot error " << static_cast<int>(conformal.error)
      << ": " << conformal.detail << '\n';
  }
  if (conformal.snapshot)
  {
    expect(conformal.snapshot->mode
        == cceda::domain::MeshGenerationMode::ConformalMultiRegion,
      "conformal snapshot records its explicit mode");
    const QByteArray mapBytes = readAll(conformal.snapshot->geometryMapPath);
    const QJsonDocument mapDocument = QJsonDocument::fromJson(mapBytes);
    const QJsonObject map = mapDocument.object();
    expect(map.value("mode").toString() == "conformal-multi-region",
      "conformal geometry map records its mode");
    const QJsonArray regions = map.value("regions").toArray();
    expect(!regions.isEmpty()
        && regions.first().toObject().value("sourceSolidGeometryIds").isArray(),
      "conformal regions preserve source Solid mappings");
    expect(geometryService.currentDocument().revision == imported.document.revision,
      "conformal snapshot preparation does not mutate canonical geometry");
  }

  const std::filesystem::path staleOutput =
    std::filesystem::path(root.path().toStdWString()) / "stale";
  const GeometrySnapshotResult stale = snapshotService.createSnapshot({
    imported.document.revision + 1,
    staleOutput,
    1024,
  });
  expect(stale.error == GeometrySnapshotErrorCode::RevisionMismatch,
    "a stale or future geometry revision is rejected");
  expect(!std::filesystem::exists(staleOutput),
    "revision rejection creates no artifacts");

  const std::filesystem::path limitedOutput =
    std::filesystem::path(root.path().toStdWString()) / "limited";
  const GeometrySnapshotResult limited = snapshotService.createSnapshot({
    imported.document.revision,
    limitedOutput,
    1,
  });
  if (limited.error != GeometrySnapshotErrorCode::SnapshotTooLarge)
  {
    std::cerr << "Limited snapshot error " << static_cast<int>(limited.error)
              << ": " << limited.detail << '\n';
  }
  expect(limited.error == GeometrySnapshotErrorCode::SnapshotTooLarge,
    "the caller-provided snapshot byte budget is enforced");
  expect(!std::filesystem::exists(limitedOutput / "input.brep")
      && !std::filesystem::exists(limitedOutput / "geometry-map.json"),
    "an oversized snapshot publishes no final artifacts");

  const std::filesystem::path surfacePath =
    std::filesystem::path(root.path().toStdWString()) / "surface.step";
  const TopoDS_Face face =
    BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), -10.0, 10.0, -10.0, 10.0).Face();
  STEPControl_Writer surfaceWriter;
  const bool surfaceWritten =
    surfaceWriter.Transfer(face, STEPControl_AsIs) == IFSelect_RetDone
    && surfaceWriter.Write(surfacePath.u8string().c_str()) == IFSelect_RetDone;
  expect(surfaceWritten, "a surface-only STEP snapshot fixture can be written");
  if (surfaceWritten)
  {
    const auto surfaceImported = geometryService.importStep(surfacePath);
    expect(surfaceImported.success && surfaceImported.document.solidCount == 0,
      "the surface-only fixture remains inspectable without becoming a Solid");
    const std::filesystem::path surfaceOutput =
      std::filesystem::path(root.path().toStdWString()) / "surface-geometry";
    const GeometrySnapshotResult surfaceSnapshot = snapshotService.createSnapshot({
      surfaceImported.document.revision,
      surfaceOutput,
      1024 * 1024,
    });
    expect(surfaceSnapshot.error == GeometrySnapshotErrorCode::NoMeshableSolids,
      "a non-solid model never enters a first-release meshing snapshot");
    expect(!std::filesystem::exists(surfaceOutput),
      "a non-solid model publishes no snapshot artifacts");
  }

  BRep_Builder assemblyBuilder;
  TopoDS_Compound overlappingCompound;
  assemblyBuilder.MakeCompound(overlappingCompound);
  assemblyBuilder.Add(overlappingCompound,
    BRepPrimAPI_MakeBox(gp_Pnt(0.0, 0.0, 0.0), 10.0, 10.0, 10.0).Solid());
  assemblyBuilder.Add(overlappingCompound,
    BRepPrimAPI_MakeBox(gp_Pnt(5.0, 0.0, 0.0), 10.0, 10.0, 10.0).Solid());
  const std::filesystem::path overlappingPath =
    std::filesystem::path(root.path().toStdWString()) / "overlapping.step";
  STEPControl_Writer overlappingWriter;
  const bool overlappingWritten =
    overlappingWriter.Transfer(overlappingCompound, STEPControl_AsIs) == IFSelect_RetDone
    && overlappingWriter.Write(overlappingPath.u8string().c_str()) == IFSelect_RetDone;
  expect(overlappingWritten, "an overlapping multi-Solid conformal fixture can be written");
  if (overlappingWritten)
  {
    const auto overlappingImported = geometryService.importStep(overlappingPath);
    expect(overlappingImported.success && meshableSolidCount(overlappingImported.document) == 2,
      "the overlapping fixture imports as two meshable Solids");
    const auto overlappingSnapshot = snapshotService.createSnapshot({
      overlappingImported.document.revision,
      std::filesystem::path(root.path().toStdWString()) / "overlapping-conformal",
      32ULL * 1024ULL * 1024ULL,
      cceda::domain::MeshGenerationMode::ConformalMultiRegion,
    });
    expect(overlappingSnapshot.succeeded(),
      "conformal Fragment/Imprint handles overlapping task Solids");
    if (overlappingSnapshot.snapshot)
    {
      expect(overlappingSnapshot.snapshot->regions.size() >= 2,
        "overlapping conformal input produces mapped fragments");
      expect(geometryService.currentDocument().revision == overlappingImported.document.revision,
        "multi-Solid conformal preparation preserves canonical revision");
    }
  }

  if (failures == 0)
  {
    std::cout << "Geometry meshing snapshot tests passed.\n";
  }
  return failures == 0 ? 0 : 1;
}
