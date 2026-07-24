#include "geometry/IGeometryService.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Writer.hxx>
#include <gp.hxx>
#include <gp_Pln.hxx>

#include <filesystem>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

namespace
{
struct TopologySummary
{
  std::size_t solids = 0;
  std::size_t faces = 0;
  bool uniqueIds = true;
  bool faceParentsAreSolids = true;
  std::unordered_set<std::string> ids;
};

TopologySummary summarizeTopology(const cceda::domain::GeometryDocument& document)
{
  TopologySummary summary;
  std::unordered_set<std::string> allIds;
  std::unordered_map<std::string, cceda::domain::GeometryKind> kindsById;
  for (const cceda::domain::GeometryEntity& entity : document.entities)
  {
    summary.uniqueIds = allIds.insert(entity.id).second && summary.uniqueIds;
    kindsById.emplace(entity.id, entity.kind);
    if (entity.kind == cceda::domain::GeometryKind::Solid)
    {
      summary.ids.insert(entity.id);
      ++summary.solids;
    }
    else if (entity.kind == cceda::domain::GeometryKind::Face)
    {
      summary.ids.insert(entity.id);
      ++summary.faces;
    }
  }
  for (const cceda::domain::GeometryEntity& entity : document.entities)
  {
    if (entity.kind != cceda::domain::GeometryKind::Face)
    {
      continue;
    }
    const auto parent = kindsById.find(entity.parentId);
    summary.faceParentsAreSolids = parent != kindsById.end()
      && parent->second == cceda::domain::GeometryKind::Solid
      && summary.faceParentsAreSolids;
  }
  return summary;
}
}

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    std::cerr << "Expected one STEP path.\n";
    return 2;
  }

  const std::filesystem::path stepPath(argv[1]);
  if (!std::filesystem::exists(stepPath))
  {
    std::cerr << "STEP fixture does not exist: " << stepPath << '\n';
    return 2;
  }

  const auto service = cceda::geometry::createOccGeometryService();
  const auto result = service->importStep(stepPath);
  if (!result.success)
  {
    std::cerr << result.errorCode << ": " << result.message << '\n';
    return 1;
  }

  const bool hasInspectableEntity = !result.document.entities.empty()
    && result.document.entities.front().faceCount > 0
    && result.document.entities.front().edgeCount > 0;

  if (result.document.revision != 1 || result.document.solidCount == 0
      || !hasInspectableEntity)
  {
    std::cerr << "Imported document does not satisfy the geometry contract.\n";
    return 1;
  }

  const TopologySummary topology = summarizeTopology(result.document);
  if (!topology.uniqueIds || topology.solids != result.document.solidCount
      || topology.faces != result.document.faceCount || !topology.faceParentsAreSolids)
  {
    std::cerr << "Imported Solid/Face topology is not indexed by unique domain IDs.\n";
    return 1;
  }

  const auto repeated = service->importStep(stepPath);
  const TopologySummary repeatedTopology = summarizeTopology(repeated.document);
  bool revisionsShareTopologyIds = false;
  for (const std::string& id : topology.ids)
  {
    if (repeatedTopology.ids.count(id) > 0)
    {
      revisionsShareTopologyIds = true;
      break;
    }
  }
  if (!repeated.success || repeated.document.revision != 2
      || !repeatedTopology.uniqueIds
      || repeatedTopology.solids != repeated.document.solidCount
      || repeatedTopology.faces != repeated.document.faceCount
      || revisionsShareTopologyIds)
  {
    std::cerr << "Topology IDs must be scoped to exactly one geometry revision.\n";
    return 1;
  }

  const std::filesystem::path surfacePath =
    std::filesystem::temp_directory_path() / "cceda_surface_contract.step";
  const TopoDS_Face face =
    BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), -10.0, 10.0, -10.0, 10.0).Face();
  STEPControl_Writer writer;
  if (writer.Transfer(face, STEPControl_AsIs) != IFSelect_RetDone
      || writer.Write(surfacePath.u8string().c_str()) != IFSelect_RetDone)
  {
    std::cerr << "Could not create the surface-only STEP contract fixture.\n";
    return 2;
  }

  const auto surfaceResult = service->importStep(surfacePath);
  std::filesystem::remove(surfacePath);
  const TopologySummary surfaceTopology = summarizeTopology(surfaceResult.document);
  if (!surfaceResult.success || surfaceResult.document.solidCount != 0
      || surfaceResult.document.faceCount == 0 || surfaceTopology.solids != 0
      || surfaceTopology.faces != surfaceResult.document.faceCount)
  {
    std::cerr << "A surface-only STEP model must remain inspectable.\n";
    return 1;
  }

  std::cout << "Imported " << result.document.solidCount << " solid(s), "
            << result.document.faceCount << " face(s).\n";
  return 0;
}
