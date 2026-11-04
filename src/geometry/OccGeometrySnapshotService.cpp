#include "geometry/OccGeometrySnapshotService.h"

#include "geometry/OccGeometryRepository.h"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <NCollection_DataMap.hxx>
#include <NCollection_IndexedMap.hxx>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopTools_FormatVersion.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS_Compound.hxx>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
namespace domain = cceda::domain;
namespace geometry = cceda::geometry;
using ShapeMap = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;
using ShapeIdMap =
  NCollection_DataMap<TopoDS_Shape, std::string, TopTools_ShapeMapHasher>;

constexpr qint64 HashBufferBytes = 1024 * 1024;

const char* modeId(domain::MeshGenerationMode mode)
{
  return mode == domain::MeshGenerationMode::ConformalMultiRegion
    ? "conformal-multi-region" : "independent-solids";
}

class OwnedFileCleanup
{
public:
  explicit OwnedFileCleanup(std::filesystem::path path)
      : path_(std::move(path))
  {
  }

  ~OwnedFileCleanup()
  {
    if (!path_.empty())
    {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }

  void track(std::filesystem::path path)
  {
    path_ = std::move(path);
  }

  void release()
  {
    path_.clear();
  }

private:
  std::filesystem::path path_;
};

geometry::GeometrySnapshotResult fail(
  geometry::GeometrySnapshotErrorCode error,
  std::string detail)
{
  return {std::nullopt, error, std::move(detail)};
}

QString qtPath(const std::filesystem::path& path)
{
  const std::string utf8 = path.u8string();
  return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

bool directoryIsEmpty(const std::filesystem::path& path)
{
  return std::filesystem::directory_iterator(path)
    == std::filesystem::directory_iterator();
}

bool boundsOverlap(
  const std::array<double, 6>& left,
  const std::array<double, 6>& right)
{
  constexpr double tolerance = 1.0e-9;
  return left[0] <= right[3] + tolerance && right[0] <= left[3] + tolerance
    && left[1] <= right[4] + tolerance && right[1] <= left[4] + tolerance
    && left[2] <= right[5] + tolerance && right[2] <= left[5] + tolerance;
}

std::optional<std::string> sha256File(const std::filesystem::path& path)
{
  QFile file(qtPath(path));
  if (!file.open(QIODevice::ReadOnly))
  {
    return std::nullopt;
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!file.atEnd())
  {
    const QByteArray chunk = file.read(HashBufferBytes);
    if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
    {
      return std::nullopt;
    }
    hash.addData(QByteArrayView(chunk));
  }
  return hash.result().toHex().toStdString();
}

std::array<double, 16> locationMatrix(const TopoDS_Shape& shape)
{
  const gp_Trsf transformation = shape.Location().Transformation();
  return {
    transformation.Value(1, 1), transformation.Value(1, 2),
    transformation.Value(1, 3), transformation.Value(1, 4),
    transformation.Value(2, 1), transformation.Value(2, 2),
    transformation.Value(2, 3), transformation.Value(2, 4),
    transformation.Value(3, 1), transformation.Value(3, 2),
    transformation.Value(3, 3), transformation.Value(3, 4),
    0.0, 0.0, 0.0, 1.0,
  };
}

QJsonArray stringsJson(const std::vector<std::string>& values)
{
  QJsonArray result;
  for (const std::string& value : values)
  {
    result.append(QString::fromUtf8(value));
  }
  return result;
}

QJsonArray integersJson(const std::vector<std::uint64_t>& values)
{
  QJsonArray result;
  for (const std::uint64_t value : values)
  {
    result.append(static_cast<qint64>(value));
  }
  return result;
}

QJsonArray matrixJson(const std::array<double, 16>& values)
{
  QJsonArray result;
  for (const double value : values)
  {
    result.append(value);
  }
  return result;
}

QJsonArray boundsJson(const std::array<double, 6>& values)
{
  QJsonArray result;
  for (const double value : values)
  {
    result.append(value);
  }
  return result;
}

std::vector<std::string> assemblyAncestors(
  const std::string& partId,
  const std::unordered_map<std::string, const domain::GeometryEntity*>& entities)
{
  std::vector<std::string> ancestors;
  auto current = entities.find(partId);
  for (std::size_t depth = 0;
       current != entities.end() && depth < entities.size();
       ++depth)
  {
    const std::string& parentId = current->second->parentId;
    current = entities.find(parentId);
    if (current != entities.end()
        && current->second->kind == domain::GeometryKind::Assembly)
    {
      ancestors.push_back(current->second->id);
    }
  }
  std::reverse(ancestors.begin(), ancestors.end());
  return ancestors;
}

QByteArray geometryMapJson(const geometry::GeometrySnapshot& snapshot)
{
  QJsonArray regions;
  for (const geometry::GeometrySnapshotRegion& region : snapshot.regions)
  {
    QJsonObject json{
      {"regionId", static_cast<qint64>(region.regionId)},
      {"brepSolidIndex", static_cast<qint64>(region.brepSolidIndex)},
      {"sourceAssemblyGeometryIds", stringsJson(region.assemblyGeometryIds)},
      {"sourcePartGeometryId", QString::fromUtf8(region.partGeometryId)},
      {"sourceSolidGeometryId", QString::fromUtf8(region.solidGeometryId)},
      {"sourceFaceGeometryIds", stringsJson(region.faceGeometryIds)},
      {"brepBoundsMm", boundsJson(region.brepBoundsMm)},
      {"brepLocation", QJsonObject{
        {"transformsBaked", snapshot.transformsBaked},
        {"matrixRowMajor", matrixJson(region.brepLocation)},
      }},
    };
    if (snapshot.mode == domain::MeshGenerationMode::ConformalMultiRegion)
    {
      QJsonArray sourceSolids;
      for (const std::string& source : region.sourceSolidGeometryIds)
      {
        sourceSolids.append(QString::fromUtf8(source));
      }
      json.insert("sourceSolidGeometryIds", sourceSolids);
    }
    regions.append(json);
  }

  QJsonArray sharedFaces;
  for (const geometry::GeometrySnapshotSharedFaceCandidate& face
       : snapshot.sharedFaceCandidates)
  {
    sharedFaces.append(QJsonObject{
      {"sourceFaceGeometryId", QString::fromUtf8(face.faceGeometryId)},
      {"regionIds", integersJson(face.regionIds)},
    });
  }

  QJsonArray exclusions;
  for (const geometry::GeometrySnapshotExclusion& exclusion : snapshot.exclusions)
  {
    exclusions.append(QJsonObject{
      {"geometryId", QString::fromUtf8(exclusion.geometryId)},
      {"reasonCode", QString::fromUtf8(exclusion.reasonCode)},
    });
  }

  return QJsonDocument(QJsonObject{
    {"schema", QJsonObject{
      {"name", "cceda.geometry-map"},
      {"major", geometry::GeometryMapSchemaMajor},
      {"minor", geometry::GeometryMapSchemaMinor},
    }},
    {"sourceGeometryRevision",
      static_cast<qint64>(snapshot.sourceGeometryRevision)},
    {"mode", modeId(snapshot.mode)},
    {"canonicalUnit", QString::fromUtf8(snapshot.canonicalUnit)},
    {"transformsBaked", snapshot.transformsBaked},
    {"regions", regions},
    {"sharedFaceCandidates", sharedFaces},
    {"excluded", exclusions},
  }).toJson(QJsonDocument::Indented);
}

}

namespace cceda::geometry
{
OccGeometrySnapshotService::OccGeometrySnapshotService(
  std::shared_ptr<OccGeometryRepository> repository)
    : repository_(std::move(repository))
{
}

GeometrySnapshotResult OccGeometrySnapshotService::createSnapshot(
  const GeometrySnapshotRequest& request) const
{
  if (!repository_ || request.sourceGeometryRevision == 0
      || request.maximumSnapshotBytes == 0
      || request.outputDirectory.empty()
      || !request.outputDirectory.is_absolute()
      || request.sourceGeometryRevision
        > static_cast<domain::GeometryRevision>(std::numeric_limits<qint64>::max()))
  {
    return fail(GeometrySnapshotErrorCode::InvalidRequest,
      "Snapshot revision, absolute output directory, or byte budget is invalid");
  }

  const OccTopologySnapshot topology =
    repository_->topologySnapshot(request.sourceGeometryRevision);
  if (topology.status == OccShapeLookupStatus::RevisionMismatch)
  {
    return fail(GeometrySnapshotErrorCode::RevisionMismatch,
      "The requested geometry revision is not current");
  }

  try
  {
    if (std::filesystem::exists(request.outputDirectory))
    {
      if (!std::filesystem::is_directory(request.outputDirectory)
          || std::filesystem::is_symlink(request.outputDirectory))
      {
        return fail(GeometrySnapshotErrorCode::InvalidRequest,
          "Snapshot output must be a real directory");
      }
      if (!directoryIsEmpty(request.outputDirectory))
      {
        return fail(GeometrySnapshotErrorCode::OutputDirectoryNotEmpty,
          "Snapshot output directory must be empty");
      }
    }

    std::unordered_map<std::string, const domain::GeometryEntity*> entities;
    for (const domain::GeometryEntity& entity : topology.document.entities)
    {
      entities.emplace(entity.id, &entity);
    }

    GeometrySnapshot snapshot;
    snapshot.schemaMajor = GeometryMapSchemaMajor;
    snapshot.schemaMinor = GeometryMapSchemaMinor;
    snapshot.sourceGeometryRevision = request.sourceGeometryRevision;
    if (!domain::isValidMeshGenerationMode(request.mode))
    {
      return fail(GeometrySnapshotErrorCode::InvalidRequest,
        "Unsupported geometry snapshot mode");
    }
    snapshot.mode = request.mode;

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    std::unordered_map<std::string, std::vector<std::uint64_t>> faceRegions;
    struct SourceSolid
    {
      TopoDS_Shape shape;
      GeometrySnapshotRegion region;
    };
    std::vector<SourceSolid> sourceSolids;
    ShapeIdMap faceIds;

    for (const domain::GeometryEntity& entity : topology.document.entities)
    {
      if (entity.kind != domain::GeometryKind::Face)
      {
        continue;
      }
      const auto face = topology.topologyShapes.find(entity.id);
      if (face == topology.topologyShapes.end() || face->second.IsNull()
          || face->second.ShapeType() != TopAbs_FACE)
      {
        return fail(GeometrySnapshotErrorCode::RepositoryInconsistent,
          "An indexed Face does not resolve to its OCCT shape");
      }
      if (!faceIds.IsBound(face->second))
      {
        faceIds.Bind(face->second, entity.id);
      }
    }

    for (const domain::GeometryEntity& entity : topology.document.entities)
    {
      if (entity.kind == domain::GeometryKind::Part && entity.solidCount == 0)
      {
        snapshot.exclusions.push_back({entity.id, "NON_SOLID_PART"});
      }
      if (entity.kind != domain::GeometryKind::Solid)
      {
        continue;
      }
      if (!entity.valid || !entity.meshable)
      {
        snapshot.exclusions.push_back({entity.id, "INVALID_SOLID"});
        continue;
      }

      const auto shape = topology.topologyShapes.find(entity.id);
      const auto part = entities.find(entity.parentId);
      if (shape == topology.topologyShapes.end() || shape->second.IsNull()
          || shape->second.ShapeType() != TopAbs_SOLID
          || part == entities.end()
          || part->second->kind != domain::GeometryKind::Part)
      {
        return fail(GeometrySnapshotErrorCode::RepositoryInconsistent,
          "A meshable Solid does not resolve to its indexed OCCT shape and Part");
      }

      GeometrySnapshotRegion region;
      region.regionId = sourceSolids.size() + 1;
      region.brepSolidIndex = region.regionId;
      region.assemblyGeometryIds = assemblyAncestors(entity.parentId, entities);
      region.partGeometryId = entity.parentId;
      region.solidGeometryId = entity.id;
      region.brepLocation = locationMatrix(shape->second);
      Bnd_Box solidBounds;
      BRepBndLib::AddOptimal(shape->second, solidBounds, false, false);
      if (solidBounds.IsVoid() || solidBounds.IsOpen())
      {
        return fail(GeometrySnapshotErrorCode::RepositoryInconsistent,
          "A meshable Solid does not have finite BREP bounds");
      }
      solidBounds.Get(
        region.brepBoundsMm[0], region.brepBoundsMm[1],
        region.brepBoundsMm[2], region.brepBoundsMm[3],
        region.brepBoundsMm[4], region.brepBoundsMm[5]);
      if (!std::all_of(region.brepBoundsMm.begin(), region.brepBoundsMm.end(),
            [](double value) { return std::isfinite(value); })
          || region.brepBoundsMm[0] > region.brepBoundsMm[3]
          || region.brepBoundsMm[1] > region.brepBoundsMm[4]
          || region.brepBoundsMm[2] > region.brepBoundsMm[5])
      {
        return fail(GeometrySnapshotErrorCode::RepositoryInconsistent,
          "A meshable Solid has invalid BREP bounds");
      }

      ShapeMap solidFaces;
      TopExp::MapShapes(shape->second, TopAbs_FACE, solidFaces);
      for (int faceIndex = 1; faceIndex <= solidFaces.Extent(); ++faceIndex)
      {
        const TopoDS_Shape& face = solidFaces.FindKey(faceIndex);
        if (!faceIds.IsBound(face))
        {
          return fail(GeometrySnapshotErrorCode::RepositoryInconsistent,
            "A meshable Solid contains a Face missing from the topology index");
        }
        const std::string& faceId = faceIds.Find(face);
        region.faceGeometryIds.push_back(faceId);
        faceRegions[faceId].push_back(region.regionId);
      }

      region.sourceSolidGeometryIds.push_back(entity.id);
      sourceSolids.push_back({shape->second, std::move(region)});
    }

    if (sourceSolids.empty())
    {
      return fail(GeometrySnapshotErrorCode::NoMeshableSolids,
        "The current revision contains no valid meshable Solids");
    }
    if (request.mode == domain::MeshGenerationMode::IndependentSolids)
    {
      for (SourceSolid& source : sourceSolids)
      {
        builder.Add(compound, source.shape);
        snapshot.regions.push_back(std::move(source.region));
      }
    }
    else
    {
      NCollection_List<TopoDS_Shape> arguments;
      for (const SourceSolid& source : sourceSolids)
      {
        arguments.Append(source.shape);
      }
      std::unique_ptr<BRepAlgoAPI_Splitter> splitter;
      if (sourceSolids.size() > 1)
      {
        splitter = std::make_unique<BRepAlgoAPI_Splitter>();
        splitter->SetArguments(arguments);
        splitter->SetNonDestructive(true);
        splitter->SetToFillHistory(true);
        splitter->Build();
        if (!splitter->IsDone() || splitter->Shape().IsNull())
        {
          return fail(GeometrySnapshotErrorCode::ConformalPreparationFailed,
            "OCCT Fragment/Imprint failed for the task snapshot");
        }
      }

      struct Fragment
      {
        TopoDS_Shape shape;
        std::vector<std::size_t> sources;
      };
      std::vector<Fragment> fragments;
      if (sourceSolids.size() == 1)
      {
        fragments.push_back({sourceSolids.front().shape, {0}});
      }
      else
      {
        ShapeMap resultSolids;
        TopExp::MapShapes(splitter->Shape(), TopAbs_SOLID, resultSolids);
        for (int solidIndex = 1; solidIndex <= resultSolids.Extent(); ++solidIndex)
        {
          const TopoDS_Shape& piece = resultSolids.FindKey(solidIndex);
          Bnd_Box pieceBounds;
          BRepBndLib::AddOptimal(piece, pieceBounds, false, false);
          if (piece.IsNull() || piece.ShapeType() != TopAbs_SOLID
              || pieceBounds.IsVoid() || pieceBounds.IsOpen())
          {
            continue;
          }
          std::array<double, 6> bounds{};
          pieceBounds.Get(bounds[0], bounds[1], bounds[2],
            bounds[3], bounds[4], bounds[5]);
          std::vector<std::size_t> sources;
          for (std::size_t sourceIndex = 0;
               sourceIndex < sourceSolids.size(); ++sourceIndex)
          {
            if (boundsOverlap(bounds, sourceSolids[sourceIndex].region.brepBoundsMm))
            {
              sources.push_back(sourceIndex);
            }
          }
          if (!sources.empty())
          {
            fragments.push_back({piece, std::move(sources)});
          }
        }
      }
      if (fragments.empty())
      {
        return fail(GeometrySnapshotErrorCode::ConformalPreparationFailed,
          "OCCT Fragment/Imprint produced no solid fragments");
      }
      for (Fragment& fragment : fragments)
      {
        GeometrySnapshotRegion region;
        region.regionId = snapshot.regions.size() + 1;
        region.brepSolidIndex = region.regionId;
        region.brepLocation = locationMatrix(fragment.shape);
        std::unordered_set<std::string> uniqueFaces;
        for (const std::size_t sourceIndex : fragment.sources)
        {
          const GeometrySnapshotRegion& source = sourceSolids[sourceIndex].region;
          if (region.partGeometryId.empty())
          {
            region.partGeometryId = source.partGeometryId;
            region.assemblyGeometryIds = source.assemblyGeometryIds;
          }
          region.sourceSolidGeometryIds.insert(
            region.sourceSolidGeometryIds.end(),
            source.sourceSolidGeometryIds.begin(),
            source.sourceSolidGeometryIds.end());
          for (const std::string& face : source.faceGeometryIds)
          {
            if (uniqueFaces.insert(face).second)
            {
              region.faceGeometryIds.push_back(face);
            }
          }
        }
        if (region.sourceSolidGeometryIds.empty() || region.faceGeometryIds.empty())
        {
          return fail(GeometrySnapshotErrorCode::ConformalPreparationFailed,
            "A conformal fragment has no source mapping");
        }
        region.solidGeometryId = region.sourceSolidGeometryIds.front();
        Bnd_Box bounds;
        BRepBndLib::AddOptimal(fragment.shape, bounds, false, false);
        if (bounds.IsVoid() || bounds.IsOpen())
        {
          return fail(GeometrySnapshotErrorCode::ConformalPreparationFailed,
            "A conformal fragment has invalid bounds");
        }
        bounds.Get(region.brepBoundsMm[0], region.brepBoundsMm[1],
          region.brepBoundsMm[2], region.brepBoundsMm[3],
          region.brepBoundsMm[4], region.brepBoundsMm[5]);
        builder.Add(compound, fragment.shape);
        snapshot.regions.push_back(std::move(region));
      }
    }

    if (request.mode == domain::MeshGenerationMode::IndependentSolids)
    {
      for (auto& [faceId, regionIds] : faceRegions)
      {
        if (regionIds.size() > 1)
        {
          snapshot.sharedFaceCandidates.push_back({faceId, std::move(regionIds)});
        }
      }
    }
    std::sort(snapshot.sharedFaceCandidates.begin(),
      snapshot.sharedFaceCandidates.end(),
      [](const auto& left, const auto& right) {
        return left.faceGeometryId < right.faceGeometryId;
      });

    if (!std::filesystem::exists(request.outputDirectory)
        && !std::filesystem::create_directories(request.outputDirectory))
    {
      return fail(GeometrySnapshotErrorCode::DirectoryCreationFailed,
        "Cannot create the snapshot output directory");
    }

    snapshot.geometryPath = request.outputDirectory / "input.brep";
    snapshot.geometryMapPath = request.outputDirectory / "geometry-map.json";
    const std::filesystem::path brepTemporary =
      request.outputDirectory / ".input.brep.tmp";
    OwnedFileCleanup geometryCleanup(brepTemporary);

    {
      std::ofstream output(brepTemporary, std::ios::binary | std::ios::trunc);
      if (!output)
      {
        return fail(GeometrySnapshotErrorCode::GeometryWriteFailed,
          "Cannot create the temporary BREP snapshot");
      }
      BRepTools::Write(compound, output, false, false,
        TopTools_FormatVersion_CURRENT);
      output.flush();
      if (!output)
      {
        return fail(GeometrySnapshotErrorCode::GeometryWriteFailed,
          "OCCT could not write the BREP snapshot");
      }
    }

    const std::uint64_t brepBytes = std::filesystem::file_size(brepTemporary);
    if (brepBytes == 0 || brepBytes > request.maximumSnapshotBytes)
    {
      return fail(GeometrySnapshotErrorCode::SnapshotTooLarge,
        "BREP snapshot exceeds the caller byte budget");
    }
    const std::optional<std::string> geometryHash = sha256File(brepTemporary);
    if (!geometryHash)
    {
      return fail(GeometrySnapshotErrorCode::GeometryReadFailed,
        "Cannot read the BREP snapshot for SHA-256");
    }
    snapshot.geometrySha256 = *geometryHash;

    const QByteArray mapBytes = geometryMapJson(snapshot);
    const std::uint64_t mapSize = static_cast<std::uint64_t>(mapBytes.size());
    if (mapSize > request.maximumSnapshotBytes - brepBytes)
    {
      return fail(GeometrySnapshotErrorCode::SnapshotTooLarge,
        "BREP and geometry map exceed the caller byte budget");
    }
    snapshot.geometryMapSha256 = QCryptographicHash::hash(
      mapBytes, QCryptographicHash::Sha256).toHex().toStdString();
    snapshot.totalBytes = brepBytes + mapSize;

    QSaveFile mapOutput(qtPath(snapshot.geometryMapPath));
    if (!mapOutput.open(QIODevice::WriteOnly)
        || mapOutput.write(mapBytes) != mapBytes.size())
    {
      return fail(GeometrySnapshotErrorCode::GeometryMapWriteFailed,
        "Cannot write the versioned geometry map");
    }

    std::filesystem::rename(brepTemporary, snapshot.geometryPath);
    geometryCleanup.track(snapshot.geometryPath);
    if (!mapOutput.commit())
    {
      return fail(GeometrySnapshotErrorCode::GeometryMapWriteFailed,
        "Cannot publish the versioned geometry map");
    }
    geometryCleanup.release();
    return {std::move(snapshot), GeometrySnapshotErrorCode::None, {}};
  }
  catch (const Standard_Failure& failure)
  {
    return fail(GeometrySnapshotErrorCode::GeometryWriteFailed,
      failure.what() != nullptr ? failure.what() : "OCCT snapshot failure");
  }
  catch (const std::filesystem::filesystem_error& failure)
  {
    return fail(GeometrySnapshotErrorCode::GeometryWriteFailed, failure.what());
  }
}
}
