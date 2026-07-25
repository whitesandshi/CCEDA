#include "application/WorkerMeshArtifactValidator.h"

#include <utility>

namespace cceda::application
{
namespace
{
WorkerMeshArtifactValidationResult fail(
  WorkerMeshArtifactValidationCode code,
  std::string detail)
{
  return {std::nullopt, code, std::move(detail)};
}
}

WorkerMeshArtifactValidationResult validateWorkerMeshArtifact(
  const std::vector<std::uint8_t>& bytes,
  const WorkerMeshArtifactExpectation& expectation,
  const meshing::MeshBinaryLimits& limits)
{
  if (expectation.sourceGeometryRevision == 0
    || expectation.backendId.empty() || expectation.backendVersion.empty()
    || expectation.nodeCount == 0 || expectation.triangleCount == 0
    || expectation.tetrahedronCount == 0 || expectation.regionId == 0
    || expectation.sourceSolidGeometryId.empty())
  {
    return fail(WorkerMeshArtifactValidationCode::InvalidExpectation,
      "Worker mesh expectations must contain positive counts and stable identity");
  }

  meshing::MeshBinaryReadResult decoded = meshing::readMeshBinary(
    bytes, domain::MeshValidationMode::WorkerArtifact, limits);
  if (!decoded.succeeded())
  {
    return fail(WorkerMeshArtifactValidationCode::InvalidBinary,
      "Neutral mesh binary is invalid: " + decoded.detail);
  }

  domain::MeshModel& mesh = *decoded.mesh;
  std::uint64_t triangles = 0;
  std::uint64_t tetrahedra = 0;
  for (const domain::MeshElement& element : mesh.elements)
  {
    if (element.type == domain::MeshElementType::Triangle3)
    {
      ++triangles;
    }
    else if (element.type == domain::MeshElementType::Tetrahedron4)
    {
      ++tetrahedra;
    }
  }
  if (mesh.sourceGeometryRevision != expectation.sourceGeometryRevision
    || mesh.backendId != expectation.backendId
    || mesh.backendVersion != expectation.backendVersion
    || mesh.nodes.size() != expectation.nodeCount
    || triangles != expectation.triangleCount
    || tetrahedra != expectation.tetrahedronCount)
  {
    return fail(WorkerMeshArtifactValidationCode::MetadataMismatch,
      "Neutral mesh metadata or counts do not match the Worker result");
  }

  if (mesh.regions.size() != 1
    || mesh.regions.front().id != expectation.regionId
    || mesh.regions.front().sourceGeometryIds
      != std::vector<std::string>{expectation.sourceSolidGeometryId})
  {
    return fail(WorkerMeshArtifactValidationCode::MappingMismatch,
      "Neutral mesh region does not match the submitted Solid mapping");
  }
  for (const domain::MeshNode& node : mesh.nodes)
  {
    if (!node.geometryId.empty()
      && node.geometryId != expectation.sourceSolidGeometryId)
    {
      return fail(WorkerMeshArtifactValidationCode::MappingMismatch,
        "Neutral mesh node references geometry outside the mapped Solid");
    }
  }
  for (const domain::MeshElement& element : mesh.elements)
  {
    if (element.regionId != expectation.regionId
      || element.geometryId != expectation.sourceSolidGeometryId)
    {
      return fail(WorkerMeshArtifactValidationCode::MappingMismatch,
        "Neutral mesh element references geometry outside the mapped Solid");
    }
  }
  return {std::move(mesh), WorkerMeshArtifactValidationCode::None, {}};
}
}
