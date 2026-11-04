#include "application/WorkerMeshArtifactValidator.h"

#include <utility>
#include <unordered_map>
#include <unordered_set>

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
    || expectation.tetrahedronCount == 0 || expectation.regions.empty()
    || expectation.regions.size() > limits.maximumRegions)
  {
    return fail(WorkerMeshArtifactValidationCode::InvalidExpectation,
      "Worker mesh expectations must contain positive counts and stable identity");
  }

  std::unordered_map<domain::MeshRegionId, std::string> expectedRegions;
  std::unordered_set<std::string> expectedSolidGeometryIds;
  for (const WorkerMeshArtifactRegionExpectation& region : expectation.regions)
  {
    if (region.regionId == 0 || region.sourceSolidGeometryId.empty()
      || !expectedRegions.emplace(
        region.regionId, region.sourceSolidGeometryId).second)
    {
      return fail(WorkerMeshArtifactValidationCode::InvalidExpectation,
        "Worker mesh region expectations must have unique positive IDs and stable Solid identities");
    }
    expectedSolidGeometryIds.insert(region.sourceSolidGeometryId);
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

  if (mesh.regions.size() != expectedRegions.size())
  {
    return fail(WorkerMeshArtifactValidationCode::MappingMismatch,
      "Neutral mesh regions do not match the submitted Solid mappings");
  }
  for (const domain::MeshRegion& region : mesh.regions)
  {
    const auto expected = expectedRegions.find(region.id);
    if (expected == expectedRegions.end()
      || region.sourceGeometryIds != std::vector<std::string>{expected->second})
    {
      return fail(WorkerMeshArtifactValidationCode::MappingMismatch,
        "Neutral mesh region does not match the submitted Solid mapping");
    }
  }
  for (const domain::MeshNode& node : mesh.nodes)
  {
    if (!node.geometryId.empty()
      && expectedSolidGeometryIds.find(node.geometryId)
        == expectedSolidGeometryIds.end())
    {
      return fail(WorkerMeshArtifactValidationCode::MappingMismatch,
        "Neutral mesh node references geometry outside the mapped Solids");
    }
  }
  for (const domain::MeshElement& element : mesh.elements)
  {
    const auto expected = expectedRegions.find(element.regionId);
    if (expected == expectedRegions.end()
      || element.geometryId != expected->second)
    {
      return fail(WorkerMeshArtifactValidationCode::MappingMismatch,
        "Neutral mesh element does not match its mapped Solid region");
    }
  }
  return {std::move(mesh), WorkerMeshArtifactValidationCode::None, {}};
}
}
