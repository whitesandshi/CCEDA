#pragma once

#include "domain/MeshModel.h"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace cceda::meshing
{
struct GmshMeshMetadata
{
  domain::GeometryRevision sourceGeometryRevision = 0;
  std::string creationTimeUtc;
  std::string backendVersion;
  domain::MeshConformity conformity = domain::MeshConformity::Unknown;
};

struct GmshEntityMapping
{
  int entityDimension = -1;
  int entityTag = 0;
  domain::MeshRegionId regionId = 0;
  std::string geometryId;
};

struct GmshMeshMapping
{
  std::vector<GmshEntityMapping> entities;
  std::vector<domain::MeshRegion> regions;
};

struct GmshMsh41ReadLimits
{
  std::uint64_t maximumInputBytes = 0;
  std::uint64_t maximumLineBytes = 0;
  std::uint64_t maximumNodes = 0;
  std::uint64_t maximumElements = 0;
  std::uint64_t maximumEntityBlocks = 0;
};

enum class GmshMsh41ErrorCode
{
  None,
  InvalidLimits,
  InputLimitExceeded,
  LineLimitExceeded,
  InvalidSyntax,
  UnsupportedFormat,
  CountLimitExceeded,
  CountMismatch,
  DuplicateNodeTag,
  DuplicateElementTag,
  UnsupportedElementType,
  EntityMappingInvalid,
  EntityMappingMissing,
  MeshValidationFailed,
  AllocationFailed,
};

struct GmshMsh41ReadResult
{
  std::optional<domain::MeshModel> mesh;
  GmshMsh41ErrorCode error = GmshMsh41ErrorCode::None;
  std::uint64_t line = 0;
  std::string detail;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return error == GmshMsh41ErrorCode::None && mesh.has_value();
  }
};

// Reads canonical ASCII output produced by the Gmsh 4.1 MSH writer.
// Format grammar: https://gmsh.info/doc/texinfo/#MSH-file-format
[[nodiscard]] GmshMsh41ReadResult readGmshMsh41(
  std::istream& input,
  const GmshMeshMetadata& metadata,
  const GmshMeshMapping& mapping,
  const GmshMsh41ReadLimits& limits);
}
