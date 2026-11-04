#include "GmshMsh41MeshExporter.h"

#include <QFile>
#include <QLocale>
#include <QStringConverter>
#include <QTextStream>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace cceda::meshing
{
namespace
{
using ElementKey = std::pair<domain::MeshRegionId, std::string>;

struct ElementGroup
{
  domain::MeshRegionId regionId = 0;
  int entityTag = 0;
  std::vector<const domain::MeshElement*> elements;
};

struct Bounds
{
  double minimumX = std::numeric_limits<double>::infinity();
  double minimumY = std::numeric_limits<double>::infinity();
  double minimumZ = std::numeric_limits<double>::infinity();
  double maximumX = -std::numeric_limits<double>::infinity();
  double maximumY = -std::numeric_limits<double>::infinity();
  double maximumZ = -std::numeric_limits<double>::infinity();
};

struct ExportPlan
{
  std::vector<const domain::MeshNode*> nodes;
  std::vector<ElementGroup> surfaces;
  std::vector<ElementGroup> volumes;
  std::map<domain::MeshNodeId, const domain::MeshNode*> nodesById;
  std::set<domain::MeshRegionId> surfaceRegions;
  std::set<domain::MeshRegionId> volumeRegions;
};

struct NodeClassification
{
  int dimension = 0;
  int entityTag = 0;

  [[nodiscard]] bool operator<(const NodeClassification& other) const noexcept
  {
    return std::tie(dimension, entityTag)
      < std::tie(other.dimension, other.entityTag);
  }
};

[[nodiscard]] bool supports(const domain::MeshExportSettings& settings)
{
  return settings.formatVersion.has_value()
    && settings.formatVersion->major == 4
    && settings.formatVersion->minor == 1
    && settings.encoding == domain::MeshExportEncoding::Ascii
    && settings.regionPolicyId == "physical-groups";
}

[[nodiscard]] std::vector<ElementGroup> makeGroups(
  std::map<ElementKey, std::vector<const domain::MeshElement*>> grouped)
{
  if (grouped.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
  {
    throw std::length_error("too many MSH entity groups");
  }

  std::vector<ElementGroup> result;
  result.reserve(grouped.size());
  int entityTag = 1;
  for (auto& [key, elements] : grouped)
  {
    ElementGroup group;
    group.regionId = key.first;
    group.entityTag = entityTag++;
    group.elements = std::move(elements);
    std::sort(group.elements.begin(), group.elements.end(),
      [](const domain::MeshElement* left, const domain::MeshElement* right)
      {
        return left->id < right->id;
      });
    result.push_back(std::move(group));
  }
  return result;
}

[[nodiscard]] ExportPlan makePlan(const domain::MeshModel& mesh)
{
  ExportPlan plan;
  for (const domain::MeshRegion& region : mesh.regions)
  {
    if (region.id > static_cast<domain::MeshRegionId>(
        std::numeric_limits<int>::max()))
    {
      throw std::range_error("a region ID exceeds the MSH physical-tag range");
    }
  }

  plan.nodes.reserve(mesh.nodes.size());
  for (const domain::MeshNode& node : mesh.nodes)
  {
    plan.nodes.push_back(&node);
    plan.nodesById.emplace(node.id, &node);
  }
  std::sort(plan.nodes.begin(), plan.nodes.end(),
    [](const domain::MeshNode* left, const domain::MeshNode* right)
    {
      return left->id < right->id;
    });

  std::map<ElementKey, std::vector<const domain::MeshElement*>> surfaces;
  std::map<ElementKey, std::vector<const domain::MeshElement*>> volumes;
  for (const domain::MeshElement& element : mesh.elements)
  {
    const ElementKey key{element.regionId, element.geometryId};
    if (element.type == domain::MeshElementType::Triangle3)
    {
      surfaces[key].push_back(&element);
      plan.surfaceRegions.insert(element.regionId);
    }
    else
    {
      volumes[key].push_back(&element);
      plan.volumeRegions.insert(element.regionId);
    }
  }
  plan.surfaces = makeGroups(std::move(surfaces));
  plan.volumes = makeGroups(std::move(volumes));
  return plan;
}

[[nodiscard]] Bounds boundsFor(
  const ElementGroup& group,
  const std::map<domain::MeshNodeId, const domain::MeshNode*>& nodesById)
{
  Bounds bounds;
  for (const domain::MeshElement* element : group.elements)
  {
    const std::size_t nodeCount =
      element->type == domain::MeshElementType::Triangle3 ? 3 : 4;
    for (std::size_t index = 0; index < nodeCount; ++index)
    {
      const domain::MeshNode& node = *nodesById.at(element->nodeIds.at(index));
      bounds.minimumX = std::min(bounds.minimumX, node.x);
      bounds.minimumY = std::min(bounds.minimumY, node.y);
      bounds.minimumZ = std::min(bounds.minimumZ, node.z);
      bounds.maximumX = std::max(bounds.maximumX, node.x);
      bounds.maximumY = std::max(bounds.maximumY, node.y);
      bounds.maximumZ = std::max(bounds.maximumZ, node.z);
    }
  }
  return bounds;
}

void writeBounds(QTextStream& output, const Bounds& bounds)
{
  output << bounds.minimumX << ' ' << bounds.minimumY << ' ' << bounds.minimumZ
         << ' ' << bounds.maximumX << ' ' << bounds.maximumY << ' '
         << bounds.maximumZ;
}

void writePhysicalNames(QTextStream& output, const ExportPlan& plan)
{
  output << "$PhysicalNames\n"
         << static_cast<qulonglong>(
              plan.surfaceRegions.size() + plan.volumeRegions.size()) << '\n';
  std::set<domain::MeshRegionId> regions = plan.surfaceRegions;
  regions.insert(plan.volumeRegions.begin(), plan.volumeRegions.end());
  for (const domain::MeshRegionId regionId : regions)
  {
    if (plan.surfaceRegions.count(regionId) != 0)
    {
      output << "2 " << static_cast<qulonglong>(regionId) << " \"region-"
             << static_cast<qulonglong>(regionId) << "-surface\"\n";
    }
    if (plan.volumeRegions.count(regionId) != 0)
    {
      output << "3 " << static_cast<qulonglong>(regionId) << " \"region-"
             << static_cast<qulonglong>(regionId) << "-volume\"\n";
    }
  }
  output << "$EndPhysicalNames\n";
}

void writeEntities(QTextStream& output, const ExportPlan& plan)
{
  std::map<domain::MeshRegionId, std::vector<int>> boundarySurfacesByRegion;
  for (const ElementGroup& surface : plan.surfaces)
  {
    boundarySurfacesByRegion[surface.regionId].push_back(surface.entityTag);
  }

  output << "$Entities\n0 0 "
         << static_cast<qulonglong>(plan.surfaces.size()) << ' '
         << static_cast<qulonglong>(plan.volumes.size()) << '\n';
  for (const ElementGroup& group : plan.surfaces)
  {
    output << group.entityTag << ' ';
    writeBounds(output, boundsFor(group, plan.nodesById));
    output << " 1 " << static_cast<qulonglong>(group.regionId) << " 0\n";
  }
  for (const ElementGroup& group : plan.volumes)
  {
    output << group.entityTag << ' ';
    writeBounds(output, boundsFor(group, plan.nodesById));
    output << " 1 " << static_cast<qulonglong>(group.regionId);
    const auto boundary = boundarySurfacesByRegion.find(group.regionId);
    const std::size_t boundaryCount = boundary == boundarySurfacesByRegion.end()
      ? 0 : boundary->second.size();
    output << ' ' << static_cast<qulonglong>(boundaryCount);
    if (boundary != boundarySurfacesByRegion.end())
    {
      for (const int entityTag : boundary->second)
      {
        output << ' ' << entityTag;
      }
    }
    output << '\n';
  }
  output << "$EndEntities\n";
}

void writeNodes(QTextStream& output, const ExportPlan& plan)
{
  std::map<domain::MeshNodeId, NodeClassification> classifications;
  const auto classify = [&](const std::vector<ElementGroup>& groups, int dimension)
  {
    for (const ElementGroup& group : groups)
    {
      for (const domain::MeshElement* element : group.elements)
      {
        const std::size_t nodeCount = dimension == 2 ? 3 : 4;
        for (std::size_t index = 0; index < nodeCount; ++index)
        {
          classifications.try_emplace(
            element->nodeIds.at(index),
            NodeClassification{dimension, group.entityTag});
        }
      }
    }
  };
  classify(plan.volumes, 3);
  classify(plan.surfaces, 2);

  const NodeClassification fallback{3, plan.volumes.front().entityTag};
  std::map<NodeClassification, std::vector<const domain::MeshNode*>> nodeBlocks;
  for (const domain::MeshNode* node : plan.nodes)
  {
    const auto found = classifications.find(node->id);
    nodeBlocks[found == classifications.end() ? fallback : found->second]
      .push_back(node);
  }

  output << "$Nodes\n" << static_cast<qulonglong>(nodeBlocks.size()) << ' '
         << static_cast<qulonglong>(plan.nodes.size()) << ' '
         << static_cast<qulonglong>(plan.nodes.front()->id) << ' '
         << static_cast<qulonglong>(plan.nodes.back()->id) << '\n';
  for (const auto& [classification, nodes] : nodeBlocks)
  {
    output << classification.dimension << ' ' << classification.entityTag
           << " 0 " << static_cast<qulonglong>(nodes.size()) << '\n';
    for (const domain::MeshNode* node : nodes)
    {
      output << static_cast<qulonglong>(node->id) << '\n';
    }
    for (const domain::MeshNode* node : nodes)
    {
      output << node->x << ' ' << node->y << ' ' << node->z << '\n';
    }
  }
  output << "$EndNodes\n";
}

void writeElementGroup(
  QTextStream& output,
  const ElementGroup& group,
  int dimension,
  int elementType)
{
  output << dimension << ' ' << group.entityTag << ' ' << elementType << ' '
         << static_cast<qulonglong>(group.elements.size()) << '\n';
  const std::size_t nodeCount = elementType == 2 ? 3 : 4;
  for (const domain::MeshElement* element : group.elements)
  {
    output << static_cast<qulonglong>(element->id);
    for (std::size_t index = 0; index < nodeCount; ++index)
    {
      output << ' ' << static_cast<qulonglong>(element->nodeIds.at(index));
    }
    output << '\n';
  }
}

void writeElements(QTextStream& output, const ExportPlan& plan)
{
  domain::MeshElementId minimumTag = std::numeric_limits<domain::MeshElementId>::max();
  domain::MeshElementId maximumTag = 0;
  std::uint64_t elementCount = 0;
  const auto accumulate = [&](const std::vector<ElementGroup>& groups)
  {
    for (const ElementGroup& group : groups)
    {
      elementCount += group.elements.size();
      minimumTag = std::min(minimumTag, group.elements.front()->id);
      maximumTag = std::max(maximumTag, group.elements.back()->id);
    }
  };
  accumulate(plan.surfaces);
  accumulate(plan.volumes);

  output << "$Elements\n"
         << static_cast<qulonglong>(plan.surfaces.size() + plan.volumes.size())
         << ' ' << static_cast<qulonglong>(elementCount) << ' '
         << static_cast<qulonglong>(minimumTag) << ' '
         << static_cast<qulonglong>(maximumTag) << '\n';
  for (const ElementGroup& group : plan.surfaces)
  {
    writeElementGroup(output, group, 2, 2);
  }
  for (const ElementGroup& group : plan.volumes)
  {
    writeElementGroup(output, group, 3, 4);
  }
  output << "$EndElements\n";
}

void writeMsh41(QTextStream& output, const ExportPlan& plan)
{
  // Grammar: https://gmsh.info/doc/texinfo/gmsh.html#MSH-file-format
  output.setEncoding(QStringConverter::Utf8);
  output.setGenerateByteOrderMark(false);
  output.setLocale(QLocale::c());
  output.setRealNumberNotation(QTextStream::SmartNotation);
  output.setRealNumberPrecision(std::numeric_limits<double>::max_digits10);
  output << "$MeshFormat\n4.1 0 " << sizeof(std::size_t)
         << "\n$EndMeshFormat\n";
  writePhysicalNames(output, plan);
  writeEntities(output, plan);
  writeNodes(output, plan);
  writeElements(output, plan);
}
}

application::MeshExportStageResult GmshMsh41MeshExporter::writeStagedMesh(
  const domain::MeshModel& mesh,
  const domain::MeshExportSettings& settings,
  const std::filesystem::path& stagingPath) noexcept
{
  using application::MeshExportStageResult;
  using application::MeshExportStageStatus;

  QFile file(stagingPath);
  bool stagingFileOwned = false;
  const auto removeCreatedFile = [&file, &stagingFileOwned]() noexcept {
    if (!stagingFileOwned)
    {
      return true;
    }
    file.close();
    if (!file.remove())
    {
      return false;
    }
    stagingFileOwned = false;
    return true;
  };

  try
  {
    if (!application::isValidMeshExportStageRequest(mesh, settings, stagingPath))
    {
      return {MeshExportStageStatus::InvalidRequest, "invalid export request"};
    }
    if (!supports(settings))
    {
      return {MeshExportStageStatus::UnsupportedSettings,
        "unsupported MSH export settings"};
    }

    const ExportPlan plan = makePlan(mesh);
    // NewOnly is an OS-guaranteed exclusive create operation.
    // Source: https://doc.qt.io/qt-6/qiodevicebase.html#OpenModeFlag-enum
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
      return {MeshExportStageStatus::WriteFailed,
        "staging file cannot be created exclusively"};
    }
    stagingFileOwned = true;
    QTextStream output(&file);
    writeMsh41(output, plan);
    output.flush();
    if (output.status() != QTextStream::Ok || !file.flush())
    {
      const bool removed = removeCreatedFile();
      return {MeshExportStageStatus::WriteFailed,
        "staging file write failed", !removed};
    }
    file.close();
    return {MeshExportStageStatus::Succeeded, {}, true};
  }
  catch (const std::bad_alloc&)
  {
    const bool removed = removeCreatedFile();
    return {MeshExportStageStatus::WriteFailed,
      "insufficient memory while preparing MSH output", !removed};
  }
  catch (const std::exception& exception)
  {
    const bool removed = removeCreatedFile();
    return {MeshExportStageStatus::InvalidRequest, exception.what(), !removed};
  }
  catch (...)
  {
    const bool removed = removeCreatedFile();
    return {MeshExportStageStatus::WriteFailed,
      "unexpected MSH export failure", !removed};
  }
}
}
