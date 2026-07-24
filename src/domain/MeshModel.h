#pragma once

#include "domain/Revisions.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cceda::domain
{
using MeshNodeId = std::uint64_t;
using MeshElementId = std::uint64_t;
using MeshRegionId = std::uint64_t;

enum class MeshElementType
{
  Triangle3,
  Tetrahedron4,
};

enum class MeshConformity
{
  Unknown,
  NonConformal,
  Conformal,
};

struct MeshNode
{
  MeshNodeId id = 0;
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  std::string geometryId;
};

struct MeshElement
{
  MeshElementId id = 0;
  MeshElementType type = MeshElementType::Triangle3;
  std::array<MeshNodeId, 4> nodeIds{};
  MeshRegionId regionId = 0;
  std::string geometryId;
};

struct MeshRegion
{
  MeshRegionId id = 0;
  std::vector<std::string> sourceGeometryIds;
};

struct MeshModel
{
  MeshRevision meshRevision = 0;
  GeometryRevision sourceGeometryRevision = 0;
  std::string creationTimeUtc;
  std::string backendId;
  std::string backendVersion;
  MeshConformity conformity = MeshConformity::Unknown;
  std::vector<MeshNode> nodes;
  std::vector<MeshElement> elements;
  std::vector<MeshRegion> regions;
};

enum class MeshValidationMode
{
  WorkerArtifact,
  CommittedMesh,
};

enum class MeshValidationCode
{
  WorkerMeshRevisionNotZero,
  CommittedMeshRevisionMissing,
  SourceGeometryRevisionMissing,
  CreationTimeMissing,
  BackendTraceMissing,
  ConformityMissing,
  NodeSetEmpty,
  NodeIdInvalid,
  DuplicateNodeId,
  NonFiniteNodeCoordinate,
  ElementSetEmpty,
  TriangleSetEmpty,
  TetrahedronSetEmpty,
  ElementIdInvalid,
  DuplicateElementId,
  UnsupportedElementType,
  ElementNodeMissing,
  ElementNodeRepeated,
  TriangleFourthNodeNotZero,
  ElementRegionMissing,
  ElementGeometryAssociationMissing,
  RegionSetEmpty,
  RegionIdInvalid,
  DuplicateRegionId,
  RegionGeometryMappingMissing,
};

struct MeshValidationIssue
{
  MeshValidationCode code = MeshValidationCode::SourceGeometryRevisionMissing;
  MeshNodeId nodeId = 0;
  MeshElementId elementId = 0;
  MeshRegionId regionId = 0;
};

struct MeshValidationReport
{
  std::vector<MeshValidationIssue> issues;
  std::uint64_t nodeCount = 0;
  std::uint64_t triangleCount = 0;
  std::uint64_t tetrahedronCount = 0;
  std::uint64_t regionCount = 0;

  [[nodiscard]] bool valid() const noexcept
  {
    return issues.empty();
  }
};

[[nodiscard]] MeshValidationReport validateMeshModel(
  const MeshModel& mesh,
  MeshValidationMode mode);
}
