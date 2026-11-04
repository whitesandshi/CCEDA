#pragma once

#include "domain/Revisions.h"
#include "domain/MeshGenerationSettings.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cceda::geometry
{
inline constexpr int GeometryMapSchemaMajor = 0;
inline constexpr int GeometryMapSchemaMinor = 2;

struct GeometrySnapshotRequest
{
  domain::GeometryRevision sourceGeometryRevision = 0;
  std::filesystem::path outputDirectory;
  std::uint64_t maximumSnapshotBytes = 0;
  domain::MeshGenerationMode mode = domain::MeshGenerationMode::IndependentSolids;
};

struct GeometrySnapshotRegion
{
  std::uint64_t regionId = 0;
  std::uint64_t brepSolidIndex = 0;
  std::vector<std::string> assemblyGeometryIds;
  std::string partGeometryId;
  std::string solidGeometryId;
  std::vector<std::string> faceGeometryIds;
  std::array<double, 16> brepLocation{};
  std::array<double, 6> brepBoundsMm{};
  std::vector<std::string> sourceSolidGeometryIds;
};

struct GeometrySnapshotExclusion
{
  std::string geometryId;
  std::string reasonCode;
};

struct GeometrySnapshotSharedFaceCandidate
{
  std::string faceGeometryId;
  std::vector<std::uint64_t> regionIds;
};

struct GeometrySnapshot
{
  int schemaMajor = GeometryMapSchemaMajor;
  int schemaMinor = GeometryMapSchemaMinor;
  domain::GeometryRevision sourceGeometryRevision = 0;
  std::string canonicalUnit = "mm";
  bool transformsBaked = false;
  domain::MeshGenerationMode mode = domain::MeshGenerationMode::IndependentSolids;
  std::filesystem::path geometryPath;
  std::filesystem::path geometryMapPath;
  std::string geometrySha256;
  std::string geometryMapSha256;
  std::uint64_t totalBytes = 0;
  std::vector<GeometrySnapshotRegion> regions;
  std::vector<GeometrySnapshotSharedFaceCandidate> sharedFaceCandidates;
  std::vector<GeometrySnapshotExclusion> exclusions;
};

enum class GeometrySnapshotErrorCode
{
  None,
  InvalidRequest,
  RevisionMismatch,
  NoMeshableSolids,
  ConformalPreparationFailed,
  OutputDirectoryNotEmpty,
  RepositoryInconsistent,
  SnapshotTooLarge,
  DirectoryCreationFailed,
  GeometryWriteFailed,
  GeometryReadFailed,
  GeometryMapWriteFailed,
};

struct GeometrySnapshotResult
{
  std::optional<GeometrySnapshot> snapshot;
  GeometrySnapshotErrorCode error = GeometrySnapshotErrorCode::None;
  std::string detail;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return error == GeometrySnapshotErrorCode::None && snapshot.has_value();
  }
};

class IGeometrySnapshotService
{
public:
  virtual ~IGeometrySnapshotService() = default;
  [[nodiscard]] virtual GeometrySnapshotResult createSnapshot(
    const GeometrySnapshotRequest& request) const = 0;
};
}
