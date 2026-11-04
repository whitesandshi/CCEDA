#pragma once

#include <QIODevice>
#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace cceda::meshing
{
inline constexpr int GeometryMapSchemaMajor = 0;
inline constexpr int GeometryMapSchemaMinor = 2;

struct GeometryMapRegion
{
  std::uint64_t regionId = 0;
  std::uint64_t brepSolidIndex = 0;
  QStringList assemblyGeometryIds;
  QString partGeometryId;
  QString solidGeometryId;
  QStringList faceGeometryIds;
  std::array<double, 16> brepLocation{};
  std::array<double, 6> brepBoundsMm{};
};

struct GeometryMapSharedFaceCandidate
{
  QString faceGeometryId;
  std::vector<std::uint64_t> regionIds;
};

struct GeometryMapExclusion
{
  QString geometryId;
  QString reasonCode;
};

struct GeometryMap
{
  int schemaMajor = GeometryMapSchemaMajor;
  int schemaMinor = GeometryMapSchemaMinor;
  std::uint64_t sourceGeometryRevision = 0;
  QString mode;
  QString canonicalUnit;
  bool transformsBaked = false;
  std::vector<GeometryMapRegion> regions;
  std::vector<GeometryMapSharedFaceCandidate> sharedFaceCandidates;
  std::vector<GeometryMapExclusion> exclusions;
};

struct GeometryMapReadLimits
{
  std::uint64_t maximumInputBytes = 0;
  std::uint64_t maximumRegions = 0;
  std::uint64_t maximumFaceReferences = 0;
  std::uint64_t maximumSharedFaceCandidates = 0;
  std::uint64_t maximumExclusions = 0;
  std::uint64_t maximumIdentityCharacters = 0;
};

enum class GeometryMapReadErrorCode
{
  None,
  InvalidLimits,
  InputLimitExceeded,
  ReadFailed,
  InvalidJson,
  InvalidStructure,
  UnsupportedSchema,
  CountLimitExceeded,
  InvalidRegion,
  InvalidSharedFace,
  InvalidExclusion,
};

struct GeometryMapReadResult
{
  std::optional<GeometryMap> map;
  GeometryMapReadErrorCode error = GeometryMapReadErrorCode::None;
  QString detail;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return error == GeometryMapReadErrorCode::None && map.has_value();
  }
};

[[nodiscard]] GeometryMapReadResult readGeometryMap(
  QIODevice& input,
  const GeometryMapReadLimits& limits);
}
