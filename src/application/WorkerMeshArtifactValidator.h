#pragma once

#include "domain/MeshModel.h"
#include "meshing/MeshBinaryCodec.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cceda::application
{
struct WorkerMeshArtifactExpectation
{
  domain::GeometryRevision sourceGeometryRevision = 0;
  std::string backendId;
  std::string backendVersion;
  std::uint64_t nodeCount = 0;
  std::uint64_t triangleCount = 0;
  std::uint64_t tetrahedronCount = 0;
  domain::MeshRegionId regionId = 0;
  std::string sourceSolidGeometryId;
};

enum class WorkerMeshArtifactValidationCode
{
  None,
  InvalidExpectation,
  InvalidBinary,
  MetadataMismatch,
  MappingMismatch,
};

struct WorkerMeshArtifactValidationResult
{
  std::optional<domain::MeshModel> mesh;
  WorkerMeshArtifactValidationCode code =
    WorkerMeshArtifactValidationCode::None;
  std::string detail;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return code == WorkerMeshArtifactValidationCode::None && mesh.has_value();
  }
};

[[nodiscard]] WorkerMeshArtifactValidationResult validateWorkerMeshArtifact(
  const std::vector<std::uint8_t>& bytes,
  const WorkerMeshArtifactExpectation& expectation,
  const meshing::MeshBinaryLimits& limits);
}
