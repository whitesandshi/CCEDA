#include "MeshBinaryCodecInternal.h"

namespace cceda::meshing
{
using namespace detail;
namespace
{
MeshBinaryWriteResult writeMeshBinaryImpl(
  const domain::MeshModel& mesh,
  domain::MeshValidationMode validationMode,
  const MeshBinaryLimits& limits)
{
  if (!hasValidLimits(limits))
  {
    return writeFailure(MeshBinaryErrorCode::InvalidArgument,
      "Mesh binary limits are invalid");
  }
  if (!domain::validateMeshModel(mesh, validationMode).valid())
  {
    return writeFailure(MeshBinaryErrorCode::InvalidMesh,
      "MeshModel failed domain validation");
  }

  std::uint64_t triangleCount = 0;
  std::uint64_t tetrahedronCount = 0;
  for (const domain::MeshElement& element : mesh.elements)
  {
    if (element.type == domain::MeshElementType::Triangle3)
    {
      ++triangleCount;
    }
    else if (element.type == domain::MeshElementType::Tetrahedron4)
    {
      ++tetrahedronCount;
    }
  }
  if (mesh.nodes.size() > limits.maximumNodes
    || triangleCount > limits.maximumTriangles
    || tetrahedronCount > limits.maximumTetrahedra
    || mesh.regions.size() > limits.maximumRegions)
  {
    return writeFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
      "Mesh counts exceed the configured write budget");
  }
  if (!isValidString(mesh.creationTimeUtc, limits)
    || !isValidString(mesh.backendId, limits)
    || !isValidString(mesh.backendVersion, limits))
  {
    return writeFailure(MeshBinaryErrorCode::InvalidMesh,
      "Mesh metadata is empty, oversized, or not canonical UTF-8");
  }

  std::set<std::string> associationSet;
  const auto addAssociation = [&](const std::string& geometryId) -> bool {
    if (geometryId.empty())
    {
      return true;
    }
    if (!isValidString(geometryId, limits))
    {
      return false;
    }
    associationSet.insert(geometryId);
    return associationSet.size() <= limits.maximumAssociations
      && associationSet.size() <= std::numeric_limits<std::uint32_t>::max();
  };
  for (const domain::MeshNode& node : mesh.nodes)
  {
    if (!addAssociation(node.geometryId))
    {
      return writeFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
        "Node geometry associations exceed a string or count budget");
    }
  }
  for (const domain::MeshElement& element : mesh.elements)
  {
    if (!addAssociation(element.geometryId))
    {
      return writeFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
        "Element geometry associations exceed a string or count budget");
    }
  }
  for (const domain::MeshRegion& region : mesh.regions)
  {
    for (const std::string& geometryId : region.sourceGeometryIds)
    {
      if (!addAssociation(geometryId))
      {
        return writeFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
          "Region geometry associations exceed a string or count budget");
      }
    }
  }

  std::unordered_map<std::string, std::uint32_t> associationIndexes;
  associationIndexes.reserve(associationSet.size());
  std::uint32_t nextAssociationIndex = 1;
  for (const std::string& geometryId : associationSet)
  {
    associationIndexes.emplace(geometryId, nextAssociationIndex++);
  }

  std::vector<Block> blocks;
  Block metadata{BlockType::Metadata, true, {}};
  appendU32(metadata.data, 1);
  appendU32(metadata.data, static_cast<std::uint32_t>(mesh.creationTimeUtc.size()));
  appendU32(metadata.data, static_cast<std::uint32_t>(mesh.backendId.size()));
  appendU32(metadata.data, static_cast<std::uint32_t>(mesh.backendVersion.size()));
  appendString(metadata.data, mesh.creationTimeUtc);
  appendString(metadata.data, mesh.backendId);
  appendString(metadata.data, mesh.backendVersion);
  blocks.push_back(std::move(metadata));

  Block associations{BlockType::Associations, true, {}};
  appendU64(associations.data, associationSet.size());
  for (const std::string& geometryId : associationSet)
  {
    appendU32(associations.data, static_cast<std::uint32_t>(geometryId.size()));
    appendString(associations.data, geometryId);
  }
  blocks.push_back(std::move(associations));

  Block nodes{BlockType::Nodes, true, {}};
  std::uint64_t nodeBytes = 0;
  if (!checkedMultiply(mesh.nodes.size(), 32, nodeBytes)
    || nodeBytes > limits.maximumFileBytes)
  {
    return writeFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
      "Node block exceeds the file budget");
  }
  nodes.data.reserve(static_cast<std::size_t>(nodeBytes));
  for (const domain::MeshNode& node : mesh.nodes)
  {
    appendU64(nodes.data, node.id);
    appendDouble(nodes.data, node.x);
    appendDouble(nodes.data, node.y);
    appendDouble(nodes.data, node.z);
  }
  blocks.push_back(std::move(nodes));

  const auto mappedNodeCount = static_cast<std::uint64_t>(std::count_if(
    mesh.nodes.begin(), mesh.nodes.end(), [](const domain::MeshNode& node) {
      return !node.geometryId.empty();
    }));
  if (mappedNodeCount > 0)
  {
    Block nodeAssociations{BlockType::NodeAssociations, false, {}};
    appendU64(nodeAssociations.data, mappedNodeCount);
    for (const domain::MeshNode& node : mesh.nodes)
    {
      if (!node.geometryId.empty())
      {
        appendU64(nodeAssociations.data, node.id);
        appendU32(nodeAssociations.data, associationIndexes.at(node.geometryId));
        appendU32(nodeAssociations.data, 0);
      }
    }
    blocks.push_back(std::move(nodeAssociations));
  }

  Block triangles{BlockType::Triangles, true, {}};
  Block tetrahedra{BlockType::Tetrahedra, true, {}};
  for (const domain::MeshElement& element : mesh.elements)
  {
    std::vector<std::uint8_t>& data = element.type == domain::MeshElementType::Triangle3
      ? triangles.data : tetrahedra.data;
    appendU64(data, element.id);
    const std::size_t nodeCount = element.type == domain::MeshElementType::Triangle3
      ? 3 : 4;
    for (std::size_t index = 0; index < nodeCount; ++index)
    {
      appendU64(data, element.nodeIds.at(index));
    }
    appendU64(data, element.regionId);
    appendU32(data, associationIndexes.at(element.geometryId));
    appendU32(data, 0);
  }
  blocks.push_back(std::move(triangles));
  blocks.push_back(std::move(tetrahedra));

  Block regions{BlockType::Regions, true, {}};
  for (const domain::MeshRegion& region : mesh.regions)
  {
    appendU64(regions.data, region.id);
    appendU32(regions.data,
      static_cast<std::uint32_t>(region.sourceGeometryIds.size()));
    appendU32(regions.data, 0);
    for (const std::string& geometryId : region.sourceGeometryIds)
    {
      appendU32(regions.data, associationIndexes.at(geometryId));
    }
    if (region.sourceGeometryIds.size() % 2 != 0)
    {
      appendU32(regions.data, 0);
    }
  }
  blocks.push_back(std::move(regions));

  if (blocks.size() > limits.maximumDirectoryEntries)
  {
    return writeFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
      "Block directory exceeds its entry budget");
  }
  std::uint64_t directoryBytes = 0;
  std::uint64_t dataOffset = 0;
  if (!checkedMultiply(blocks.size(), DirectoryEntryBytes, directoryBytes)
    || !checkedAdd(MeshBinaryHeaderBytes, directoryBytes, dataOffset)
    || !alignToEight(dataOffset, dataOffset))
  {
    return writeFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
      "Block directory size overflows");
  }
  std::uint64_t fileBytes = dataOffset;
  for (Block& block : blocks)
  {
    if (!alignToEight(fileBytes, block.offset)
      || !checkedAdd(block.offset, block.data.size(), fileBytes))
    {
      return writeFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
        "Mesh block offsets overflow");
    }
  }
  if (fileBytes > limits.maximumFileBytes
    || fileBytes > std::numeric_limits<std::size_t>::max())
  {
    return writeFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
      "Encoded mesh exceeds the file-byte budget");
  }

  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileBytes), 0);
  const std::array<std::uint8_t, 8> magic{'C', 'C', 'M', 'E', 'S', 'H', 0, 0};
  std::copy(magic.begin(), magic.end(), bytes.begin());
  setU16(bytes, 8, MeshBinaryFormatMajor);
  setU16(bytes, 10, MeshBinaryFormatMinor);
  setU32(bytes, 12, static_cast<std::uint32_t>(MeshBinaryHeaderBytes));
  setU32(bytes, 16, ByteOrderMarker);
  setU64(bytes, 24, mesh.meshRevision);
  setU64(bytes, 32, mesh.sourceGeometryRevision);
  setU32(bytes, 40, CanonicalUnitMillimeters);
  setU32(bytes, 44, mesh.conformity == domain::MeshConformity::NonConformal ? 1U : 2U);
  setU64(bytes, 48, mesh.nodes.size());
  setU64(bytes, 56, triangleCount);
  setU64(bytes, 64, tetrahedronCount);
  setU64(bytes, 72, mesh.regions.size());
  setU64(bytes, 80, associationSet.size());
  setU64(bytes, 88, MeshBinaryHeaderBytes);
  setU32(bytes, 96, static_cast<std::uint32_t>(blocks.size()));
  setU32(bytes, 100, static_cast<std::uint32_t>(DirectoryEntryBytes));
  setU64(bytes, 104, fileBytes);

  for (std::size_t index = 0; index < blocks.size(); ++index)
  {
    const Block& block = blocks.at(index);
    const std::size_t entryOffset = MeshBinaryHeaderBytes
      + index * DirectoryEntryBytes;
    setU32(bytes, entryOffset, static_cast<std::uint32_t>(block.type));
    setU32(bytes, entryOffset + 4, block.required ? RequiredBlockFlag : 0);
    setU64(bytes, entryOffset + 8, block.offset);
    setU64(bytes, entryOffset + 16, block.data.size());
    setU64(bytes, entryOffset + 24, block.data.size());
    setU32(bytes, entryOffset + 32,
      crc32(block.data.data(), block.data.size()));
    std::copy(block.data.begin(), block.data.end(),
      bytes.begin() + static_cast<std::ptrdiff_t>(block.offset));
  }
  setU32(bytes, 112, crc32(bytes.data(), MeshBinaryHeaderBytes));
  return {std::move(bytes), MeshBinaryErrorCode::None, {}};
}
} // namespace

MeshBinaryWriteResult writeMeshBinary(
  const domain::MeshModel& mesh,
  domain::MeshValidationMode validationMode,
  const MeshBinaryLimits& limits)
{
  try
  {
    return writeMeshBinaryImpl(mesh, validationMode, limits);
  }
  catch (const std::bad_alloc&)
  {
    return writeFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
      "Mesh binary encoding allocation failed");
  }
}
}
