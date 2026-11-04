#include "OccMeshTriangulation.h"

#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Poly_Triangle.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <Quantity_Color.hxx>
#include <gp_Pnt.hxx>

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cceda::visualization
{
namespace
{
using NodeLookup =
  std::unordered_map<domain::MeshNodeId, const domain::MeshNode*>;

occ::handle<Poly_Triangulation> buildElementTriangulation(
  const domain::MeshElement& element,
  const NodeLookup& nodesById)
{
  const int nodeCount = element.type == domain::MeshElementType::Triangle3
    ? 3 : 4;
  const int triangleCount = element.type == domain::MeshElementType::Triangle3
    ? 1 : 4;
  occ::handle<Poly_Triangulation> triangulation =
    new Poly_Triangulation(nodeCount, triangleCount, false, false);
  for (int index = 0; index < nodeCount; ++index)
  {
    const auto found = nodesById.find(
      element.nodeIds[static_cast<std::size_t>(index)]);
    if (found == nodesById.end())
    {
      return {};
    }
    const domain::MeshNode& node = *found->second;
    triangulation->SetNode(index + 1, gp_Pnt(node.x, node.y, node.z));
  }

  if (element.type == domain::MeshElementType::Triangle3)
  {
    triangulation->SetTriangle(1, Poly_Triangle(1, 2, 3));
  }
  else
  {
    triangulation->SetTriangle(1, Poly_Triangle(1, 3, 2));
    triangulation->SetTriangle(2, Poly_Triangle(1, 2, 4));
    triangulation->SetTriangle(3, Poly_Triangle(2, 3, 4));
    triangulation->SetTriangle(4, Poly_Triangle(3, 1, 4));
  }
  triangulation->ComputeNormals();
  triangulation->UpdateCachedMinMax();
  return triangulation;
}
}

occ::handle<Poly_Triangulation> buildOccSurfaceTriangulation(
  const domain::MeshModel& mesh)
{
  const domain::MeshValidationReport report =
    domain::validateMeshModel(mesh, domain::MeshValidationMode::CommittedMesh);
  if (!report.valid()
    || report.triangleCount > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
  {
    return {};
  }

  std::unordered_map<domain::MeshNodeId, const domain::MeshNode*> nodesById;
  nodesById.reserve(mesh.nodes.size());
  for (const domain::MeshNode& node : mesh.nodes)
  {
    nodesById.emplace(node.id, &node);
  }

  std::unordered_map<domain::MeshNodeId, int> surfaceNodeIndices;
  std::vector<const domain::MeshNode*> surfaceNodes;
  for (const domain::MeshElement& element : mesh.elements)
  {
    if (element.type != domain::MeshElementType::Triangle3)
    {
      continue;
    }

    for (std::size_t index = 0; index < 3; ++index)
    {
      const domain::MeshNodeId nodeId = element.nodeIds[index];
      if (surfaceNodeIndices.find(nodeId) != surfaceNodeIndices.end())
      {
        continue;
      }
      if (surfaceNodes.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
      {
        return {};
      }

      const auto node = nodesById.find(nodeId);
      if (node == nodesById.end())
      {
        return {};
      }
      const int occIndex = static_cast<int>(surfaceNodes.size()) + 1;
      surfaceNodeIndices.emplace(nodeId, occIndex);
      surfaceNodes.push_back(node->second);
    }
  }

  if (surfaceNodes.empty())
  {
    return {};
  }

  occ::handle<Poly_Triangulation> triangulation =
    new Poly_Triangulation(static_cast<int>(surfaceNodes.size()),
                           static_cast<int>(report.triangleCount),
                           false,
                           false);
  for (std::size_t index = 0; index < surfaceNodes.size(); ++index)
  {
    const domain::MeshNode& node = *surfaceNodes[index];
    triangulation->SetNode(static_cast<int>(index) + 1, gp_Pnt(node.x, node.y, node.z));
  }

  int triangleIndex = 1;
  for (const domain::MeshElement& element : mesh.elements)
  {
    if (element.type != domain::MeshElementType::Triangle3)
    {
      continue;
    }

    triangulation->SetTriangle(
      triangleIndex++,
      Poly_Triangle(surfaceNodeIndices.at(element.nodeIds[0]),
                    surfaceNodeIndices.at(element.nodeIds[1]),
                    surfaceNodeIndices.at(element.nodeIds[2])));
  }

  triangulation->ComputeNormals();
  triangulation->UpdateCachedMinMax();
  return triangulation;
}

OccElementTriangulations buildOccElementTriangulations(
  const domain::MeshModel& mesh,
  const std::vector<domain::MeshElementId>& elementIds)
{
  OccElementTriangulations triangulations;
  if (elementIds.empty()
    || !domain::validateMeshModel(
      mesh, domain::MeshValidationMode::CommittedMesh).valid())
  {
    return triangulations;
  }

  std::unordered_set<domain::MeshElementId> requested(
    elementIds.cbegin(), elementIds.cend());
  NodeLookup nodesById;
  nodesById.reserve(mesh.nodes.size());
  for (const domain::MeshNode& node : mesh.nodes)
  {
    nodesById.emplace(node.id, &node);
  }
  triangulations.reserve(requested.size());
  for (const domain::MeshElement& element : mesh.elements)
  {
    if (requested.find(element.id) == requested.end())
    {
      continue;
    }
    occ::handle<Poly_Triangulation> triangulation =
      buildElementTriangulation(element, nodesById);
    if (!triangulation.IsNull())
    {
      triangulations.emplace(element.id, std::move(triangulation));
    }
  }
  return triangulations;
}

occ::handle<Poly_Triangulation> buildOccIssueTriangulation(
  const OccElementTriangulations& elementTriangulations)
{
  const std::uint64_t maximumCount =
    static_cast<std::uint64_t>(std::numeric_limits<int>::max());
  std::uint64_t nodeCount = 0;
  std::uint64_t triangleCount = 0;
  for (const auto& entry : elementTriangulations)
  {
    const occ::handle<Poly_Triangulation>& triangulation = entry.second;
    if (triangulation.IsNull())
    {
      continue;
    }
    const std::uint64_t elementNodeCount =
      static_cast<std::uint64_t>(triangulation->NbNodes());
    const std::uint64_t elementTriangleCount =
      static_cast<std::uint64_t>(triangulation->NbTriangles());
    if (elementNodeCount > maximumCount - nodeCount
      || elementTriangleCount > maximumCount - triangleCount)
    {
      return {};
    }
    nodeCount += elementNodeCount;
    triangleCount += elementTriangleCount;
  }
  if (nodeCount == 0 || triangleCount == 0)
  {
    return {};
  }

  occ::handle<Poly_Triangulation> combined = new Poly_Triangulation(
    static_cast<int>(nodeCount), static_cast<int>(triangleCount), false, false);
  int nodeOffset = 0;
  int nextTriangle = 1;
  for (const auto& entry : elementTriangulations)
  {
    const occ::handle<Poly_Triangulation>& triangulation = entry.second;
    if (triangulation.IsNull())
    {
      continue;
    }
    for (int nodeIndex = 1; nodeIndex <= triangulation->NbNodes(); ++nodeIndex)
    {
      combined->SetNode(nodeOffset + nodeIndex, triangulation->Node(nodeIndex));
    }
    for (int triangleIndex = 1;
         triangleIndex <= triangulation->NbTriangles(); ++triangleIndex)
    {
      int node1 = 0;
      int node2 = 0;
      int node3 = 0;
      triangulation->Triangle(triangleIndex).Get(node1, node2, node3);
      combined->SetTriangle(nextTriangle++, Poly_Triangle(
        nodeOffset + node1, nodeOffset + node2, nodeOffset + node3));
    }
    nodeOffset += triangulation->NbNodes();
  }
  combined->ComputeNormals();
  combined->UpdateCachedMinMax();
  return combined;
}

occ::handle<Poly_Triangulation> buildOccElementTriangulation(
  const domain::MeshModel& mesh,
  domain::MeshElementId elementId)
{
  OccElementTriangulations triangulations =
    buildOccElementTriangulations(mesh, {elementId});
  const auto found = triangulations.find(elementId);
  return found == triangulations.end()
    ? occ::handle<Poly_Triangulation>{}
    : found->second;
}

void configureOccSurfacePresentation(
  const occ::handle<AIS_Triangulation>& presentation)
{
  if (presentation.IsNull())
  {
    return;
  }

  const Quantity_Color surfaceColor(
    0.10, 0.55, 0.70, Quantity_TOC_sRGB);
  presentation->Attributes()->SetupOwnShadingAspect();
  presentation->SetColor(surfaceColor);
  presentation->Attributes()->ShadingAspect()->SetColor(surfaceColor);
  const occ::handle<Graphic3d_AspectFillArea3d>& surfaceAspect =
    presentation->Attributes()->ShadingAspect()->Aspect();
  surfaceAspect->SetEdgeOn();
  surfaceAspect->SetEdgeColor(
    Quantity_Color(0.03, 0.12, 0.16, Quantity_TOC_sRGB));
  surfaceAspect->SetEdgeWidth(1.0);
}

void configureOccElementHighlight(
  const occ::handle<AIS_Triangulation>& presentation)
{
  if (presentation.IsNull())
  {
    return;
  }
  const Quantity_Color highlightColor(
    0.92, 0.24, 0.12, Quantity_TOC_sRGB);
  presentation->Attributes()->SetupOwnShadingAspect();
  presentation->SetZLayer(Graphic3d_ZLayerId_Topmost);
  presentation->SetColor(highlightColor);
  presentation->Attributes()->ShadingAspect()->SetColor(highlightColor);
  const occ::handle<Graphic3d_AspectFillArea3d>& aspect =
    presentation->Attributes()->ShadingAspect()->Aspect();
  aspect->SetEdgeOn();
  aspect->SetEdgeColor(Quantity_Color(0.35, 0.03, 0.01, Quantity_TOC_sRGB));
  aspect->SetEdgeWidth(2.0);
}
}
