#include "MeshModel.h"

#include <cmath>
#include <unordered_set>

namespace cceda::domain
{
namespace
{
void addIssue(
  MeshValidationReport& report,
  MeshValidationCode code,
  MeshNodeId nodeId = 0,
  MeshElementId elementId = 0,
  MeshRegionId regionId = 0)
{
  report.issues.push_back({code, nodeId, elementId, regionId});
}
}

MeshValidationReport validateMeshModel(
  const MeshModel& mesh,
  MeshValidationMode mode)
{
  MeshValidationReport report;
  report.nodeCount = static_cast<std::uint64_t>(mesh.nodes.size());
  report.regionCount = static_cast<std::uint64_t>(mesh.regions.size());

  if (mode == MeshValidationMode::WorkerArtifact && mesh.meshRevision != 0)
  {
    addIssue(report, MeshValidationCode::WorkerMeshRevisionNotZero);
  }
  if (mode == MeshValidationMode::CommittedMesh && mesh.meshRevision == 0)
  {
    addIssue(report, MeshValidationCode::CommittedMeshRevisionMissing);
  }
  if (mesh.sourceGeometryRevision == 0)
  {
    addIssue(report, MeshValidationCode::SourceGeometryRevisionMissing);
  }
  if (mesh.creationTimeUtc.empty())
  {
    addIssue(report, MeshValidationCode::CreationTimeMissing);
  }
  if (mesh.backendId.empty() || mesh.backendVersion.empty())
  {
    addIssue(report, MeshValidationCode::BackendTraceMissing);
  }
  if (mesh.conformity != MeshConformity::NonConformal
    && mesh.conformity != MeshConformity::Conformal)
  {
    addIssue(report, MeshValidationCode::ConformityMissing);
  }

  std::unordered_set<MeshNodeId> nodeIds;
  nodeIds.reserve(mesh.nodes.size());
  if (mesh.nodes.empty())
  {
    addIssue(report, MeshValidationCode::NodeSetEmpty);
  }
  for (const MeshNode& node : mesh.nodes)
  {
    if (node.id == 0)
    {
      addIssue(report, MeshValidationCode::NodeIdInvalid, node.id);
    }
    else if (!nodeIds.insert(node.id).second)
    {
      addIssue(report, MeshValidationCode::DuplicateNodeId, node.id);
    }
    if (!std::isfinite(node.x) || !std::isfinite(node.y) || !std::isfinite(node.z))
    {
      addIssue(report, MeshValidationCode::NonFiniteNodeCoordinate, node.id);
    }
  }

  std::unordered_set<MeshRegionId> regionIds;
  regionIds.reserve(mesh.regions.size());
  if (mesh.regions.empty())
  {
    addIssue(report, MeshValidationCode::RegionSetEmpty);
  }
  for (const MeshRegion& region : mesh.regions)
  {
    if (region.id == 0)
    {
      addIssue(report, MeshValidationCode::RegionIdInvalid, 0, 0, region.id);
    }
    else if (!regionIds.insert(region.id).second)
    {
      addIssue(report, MeshValidationCode::DuplicateRegionId, 0, 0, region.id);
    }

    bool mappingMissing = region.sourceGeometryIds.empty();
    for (const std::string& geometryId : region.sourceGeometryIds)
    {
      if (geometryId.empty())
      {
        mappingMissing = true;
        break;
      }
    }
    if (mappingMissing)
    {
      addIssue(report, MeshValidationCode::RegionGeometryMappingMissing, 0, 0, region.id);
    }
  }

  std::unordered_set<MeshElementId> elementIds;
  elementIds.reserve(mesh.elements.size());
  if (mesh.elements.empty())
  {
    addIssue(report, MeshValidationCode::ElementSetEmpty);
  }
  for (const MeshElement& element : mesh.elements)
  {
    if (element.id == 0)
    {
      addIssue(report, MeshValidationCode::ElementIdInvalid, 0, element.id);
    }
    else if (!elementIds.insert(element.id).second)
    {
      addIssue(report, MeshValidationCode::DuplicateElementId, 0, element.id);
    }

    std::size_t requiredNodeCount = 0;
    switch (element.type)
    {
      case MeshElementType::Triangle3:
        ++report.triangleCount;
        requiredNodeCount = 3;
        if (element.nodeIds.at(3) != 0)
        {
          addIssue(report, MeshValidationCode::TriangleFourthNodeNotZero,
            element.nodeIds.at(3), element.id);
        }
        break;
      case MeshElementType::Tetrahedron4:
        ++report.tetrahedronCount;
        requiredNodeCount = 4;
        break;
      default:
        addIssue(report, MeshValidationCode::UnsupportedElementType,
          0, element.id, element.regionId);
        break;
    }

    std::unordered_set<MeshNodeId> elementNodeIds;
    elementNodeIds.reserve(requiredNodeCount);
    for (std::size_t index = 0; index < requiredNodeCount; ++index)
    {
      const MeshNodeId nodeId = element.nodeIds.at(index);
      if (nodeId == 0 || nodeIds.find(nodeId) == nodeIds.end())
      {
        addIssue(report, MeshValidationCode::ElementNodeMissing, nodeId, element.id);
      }
      if (nodeId != 0 && !elementNodeIds.insert(nodeId).second)
      {
        addIssue(report, MeshValidationCode::ElementNodeRepeated, nodeId, element.id);
      }
    }

    if (element.regionId == 0 || regionIds.find(element.regionId) == regionIds.end())
    {
      addIssue(report, MeshValidationCode::ElementRegionMissing,
        0, element.id, element.regionId);
    }
    if (element.geometryId.empty())
    {
      addIssue(report, MeshValidationCode::ElementGeometryAssociationMissing,
        0, element.id, element.regionId);
    }
  }

  if (report.triangleCount == 0)
  {
    addIssue(report, MeshValidationCode::TriangleSetEmpty);
  }
  if (report.tetrahedronCount == 0)
  {
    addIssue(report, MeshValidationCode::TetrahedronSetEmpty);
  }

  return report;
}
}
