#include "MeshBinaryCodecInternal.h"

namespace cceda::meshing
{
using namespace detail;
namespace
{
MeshBinaryReadResult readMeshBinaryImpl(
  const std::vector<std::uint8_t>& bytes,
  domain::MeshValidationMode validationMode,
  const MeshBinaryLimits& limits)
{
  if (!hasValidLimits(limits))
  {
    return readFailure(MeshBinaryErrorCode::InvalidArgument,
      "Mesh binary limits are invalid");
  }
  if (bytes.size() > limits.maximumFileBytes)
  {
    return readFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
      "Mesh binary exceeds the file-byte budget");
  }
  if (bytes.size() < MeshBinaryHeaderBytes)
  {
    return readFailure(MeshBinaryErrorCode::InvalidHeader,
      "Mesh binary is smaller than the fixed header");
  }

  const std::array<std::uint8_t, 8> magic{'C', 'C', 'M', 'E', 'S', 'H', 0, 0};
  if (!std::equal(magic.begin(), magic.end(), bytes.begin()))
  {
    return readFailure(MeshBinaryErrorCode::InvalidHeader,
      "Mesh binary magic does not match CCMESH");
  }
  const std::uint16_t formatMajor = readU16At(bytes, 8);
  const std::uint16_t formatMinor = readU16At(bytes, 10);
  if (formatMajor != MeshBinaryFormatMajor || formatMinor < MeshBinaryFormatMinor)
  {
    return readFailure(MeshBinaryErrorCode::UnsupportedVersion,
      "Mesh binary version is not compatible with format 0.1");
  }

  const std::uint32_t headerSize = readU32At(bytes, 12);
  const bool currentMinor = formatMinor == MeshBinaryFormatMinor;
  if ((currentMinor && headerSize != MeshBinaryHeaderBytes)
    || (!currentMinor && (headerSize < MeshBinaryHeaderBytes || headerSize > 4096))
    || headerSize > bytes.size()
    || headerSize % 8 != 0)
  {
    return readFailure(MeshBinaryErrorCode::InvalidHeader,
      "Mesh binary header size is invalid");
  }
  if (readU32At(bytes, 16) != ByteOrderMarker)
  {
    return readFailure(MeshBinaryErrorCode::UnsupportedFeature,
      "Mesh binary byte order is unsupported");
  }
  if (readU32At(bytes, 20) != 0)
  {
    return readFailure(MeshBinaryErrorCode::UnsupportedFeature,
      "Mesh binary header flags are unsupported");
  }
  if (readU32At(bytes, 40) != CanonicalUnitMillimeters)
  {
    return readFailure(MeshBinaryErrorCode::UnsupportedFeature,
      "Mesh binary canonical unit is not millimeters");
  }
  const std::uint32_t conformity = readU32At(bytes, 44);
  if (conformity != 1 && conformity != 2)
  {
    return readFailure(MeshBinaryErrorCode::InvalidHeader,
      "Mesh binary conformity value is invalid");
  }
  for (std::size_t offset = 116; offset < headerSize; ++offset)
  {
    if (bytes.at(offset) != 0)
    {
      return readFailure(MeshBinaryErrorCode::UnsupportedFeature,
        "Mesh binary reserved header bytes are non-zero");
    }
  }
  if (readU64At(bytes, 104) != bytes.size())
  {
    return readFailure(MeshBinaryErrorCode::InvalidHeader,
      "Mesh binary file size does not match the header");
  }

  std::vector<std::uint8_t> header(bytes.begin(), bytes.begin() + headerSize);
  const std::uint32_t expectedHeaderCrc = readU32At(bytes, 112);
  std::fill(header.begin() + 112, header.begin() + 116, std::uint8_t{0});
  if (crc32(header.data(), header.size()) != expectedHeaderCrc)
  {
    return readFailure(MeshBinaryErrorCode::ChecksumMismatch,
      "Mesh binary header CRC-32 does not match");
  }

  const std::uint64_t nodeCount = readU64At(bytes, 48);
  const std::uint64_t triangleCount = readU64At(bytes, 56);
  const std::uint64_t tetrahedronCount = readU64At(bytes, 64);
  const std::uint64_t regionCount = readU64At(bytes, 72);
  const std::uint64_t associationCount = readU64At(bytes, 80);
  if (nodeCount > limits.maximumNodes
    || triangleCount > limits.maximumTriangles
    || tetrahedronCount > limits.maximumTetrahedra
    || regionCount > limits.maximumRegions
    || associationCount > limits.maximumAssociations
    || associationCount > std::numeric_limits<std::uint32_t>::max())
  {
    return readFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
      "Mesh binary declared counts exceed configured budgets");
  }

  const std::uint64_t directoryOffset = readU64At(bytes, 88);
  const std::uint32_t directoryEntryCount = readU32At(bytes, 96);
  const std::uint32_t directoryEntrySize = readU32At(bytes, 100);
  if (directoryEntryCount < 6
    || directoryEntryCount > limits.maximumDirectoryEntries
    || (currentMinor && directoryEntrySize != DirectoryEntryBytes)
    || (!currentMinor && directoryEntrySize < DirectoryEntryBytes)
    || directoryOffset < headerSize
    || directoryOffset % 8 != 0)
  {
    return readFailure(MeshBinaryErrorCode::InvalidDirectory,
      "Mesh binary directory shape is invalid");
  }
  std::uint64_t directoryBytes = 0;
  std::uint64_t directoryEnd = 0;
  if (!checkedMultiply(directoryEntryCount, directoryEntrySize, directoryBytes)
    || !checkedAdd(directoryOffset, directoryBytes, directoryEnd)
    || directoryEnd > bytes.size())
  {
    return readFailure(MeshBinaryErrorCode::InvalidDirectory,
      "Mesh binary directory range overflows the file");
  }
  for (std::size_t offset = headerSize;
    offset < static_cast<std::size_t>(directoryOffset); ++offset)
  {
    if (bytes.at(offset) != 0)
    {
      return readFailure(MeshBinaryErrorCode::InvalidDirectory,
        "Mesh binary header-to-directory padding is non-zero");
    }
  }

  std::map<std::uint32_t, DirectoryEntry> entries;
  std::vector<DirectoryEntry> ranges;
  std::uint32_t previousType = 0;
  for (std::uint32_t index = 0; index < directoryEntryCount; ++index)
  {
    const std::size_t entryOffset = static_cast<std::size_t>(directoryOffset)
      + static_cast<std::size_t>(index) * directoryEntrySize;
    DirectoryEntry entry;
    entry.type = readU32At(bytes, entryOffset);
    entry.flags = readU32At(bytes, entryOffset + 4);
    entry.offset = readU64At(bytes, entryOffset + 8);
    const std::uint64_t storedLength = readU64At(bytes, entryOffset + 16);
    entry.length = readU64At(bytes, entryOffset + 24);
    entry.checksum = readU32At(bytes, entryOffset + 32);
    const std::uint32_t reserved = readU32At(bytes, entryOffset + 36);
    if (entry.type == 0 || entry.type <= previousType)
    {
      return readFailure(MeshBinaryErrorCode::InvalidDirectory,
        "Mesh binary block types are not unique and increasing");
    }
    previousType = entry.type;
    if ((entry.flags & ~RequiredBlockFlag) != 0)
    {
      return readFailure(MeshBinaryErrorCode::UnsupportedFeature,
        "Mesh binary block flags or compression are unsupported");
    }
    if (reserved != 0 || storedLength != entry.length
      || entry.offset < directoryEnd || entry.offset % 8 != 0)
    {
      return readFailure(MeshBinaryErrorCode::InvalidDirectory,
        "Mesh binary block directory entry is malformed");
    }
    if (currentMinor)
    {
      for (std::size_t extension = DirectoryEntryBytes;
        extension < directoryEntrySize; ++extension)
      {
        if (bytes.at(entryOffset + extension) != 0)
        {
          return readFailure(MeshBinaryErrorCode::InvalidDirectory,
            "Mesh binary directory extension is non-zero");
        }
      }
    }
    std::uint64_t blockEnd = 0;
    if (!checkedAdd(entry.offset, entry.length, blockEnd)
      || blockEnd > bytes.size())
    {
      return readFailure(MeshBinaryErrorCode::InvalidDirectory,
        "Mesh binary block range overflows the file");
    }
    if (!isKnownBlock(entry.type)
      && (entry.flags & RequiredBlockFlag) != 0)
    {
      return readFailure(MeshBinaryErrorCode::UnsupportedFeature,
        "Mesh binary contains an unknown required block");
    }
    if (isRequiredBlock(entry.type)
      && (entry.flags & RequiredBlockFlag) == 0)
    {
      return readFailure(MeshBinaryErrorCode::InvalidDirectory,
        "Mesh binary marks a required 0.1 block optional");
    }
    if (entry.type == static_cast<std::uint32_t>(BlockType::NodeAssociations)
      && (entry.flags & RequiredBlockFlag) != 0)
    {
      return readFailure(MeshBinaryErrorCode::InvalidDirectory,
        "Mesh binary marks node associations required");
    }
    const auto* blockData = bytes.data() + static_cast<std::size_t>(entry.offset);
    if (crc32(blockData, static_cast<std::size_t>(entry.length))
      != entry.checksum)
    {
      return readFailure(MeshBinaryErrorCode::ChecksumMismatch,
        "Mesh binary block CRC-32 does not match");
    }
    entries.emplace(entry.type, entry);
    ranges.push_back(entry);
  }
  for (std::uint32_t type = static_cast<std::uint32_t>(BlockType::Metadata);
    type <= static_cast<std::uint32_t>(BlockType::Regions); ++type)
  {
    if (isRequiredBlock(type) && entries.find(type) == entries.end())
    {
      return readFailure(MeshBinaryErrorCode::InvalidDirectory,
        "Mesh binary is missing a required 0.1 block");
    }
  }

  std::sort(ranges.begin(), ranges.end(),
    [](const DirectoryEntry& left, const DirectoryEntry& right) {
      return left.offset < right.offset;
    });
  std::uint64_t previousEnd = directoryEnd;
  for (const DirectoryEntry& range : ranges)
  {
    if (range.offset < previousEnd)
    {
      return readFailure(MeshBinaryErrorCode::InvalidDirectory,
        "Mesh binary block ranges overlap");
    }
    for (std::size_t offset = static_cast<std::size_t>(previousEnd);
      offset < static_cast<std::size_t>(range.offset); ++offset)
    {
      if (bytes.at(offset) != 0)
      {
        return readFailure(MeshBinaryErrorCode::InvalidDirectory,
          "Mesh binary inter-block padding is non-zero");
      }
    }
    previousEnd = range.offset + range.length;
  }
  if (previousEnd != bytes.size())
  {
    return readFailure(MeshBinaryErrorCode::InvalidDirectory,
      "Mesh binary contains unowned trailing bytes");
  }

  domain::MeshModel mesh;
  mesh.meshRevision = readU64At(bytes, 24);
  mesh.sourceGeometryRevision = readU64At(bytes, 32);
  mesh.conformity = conformity == 1
    ? domain::MeshConformity::NonConformal
    : domain::MeshConformity::Conformal;

  Cursor metadata = blockCursor(bytes,
    *findBlock(entries, BlockType::Metadata));
  std::uint32_t metadataSchema = 0;
  std::uint32_t creationLength = 0;
  std::uint32_t backendIdLength = 0;
  std::uint32_t backendVersionLength = 0;
  if (!metadata.readU32(metadataSchema) || metadataSchema != 1
    || !metadata.readU32(creationLength)
    || !metadata.readU32(backendIdLength)
    || !metadata.readU32(backendVersionLength)
    || creationLength > limits.maximumStringBytes
    || backendIdLength > limits.maximumStringBytes
    || backendVersionLength > limits.maximumStringBytes
    || !metadata.readString(creationLength, mesh.creationTimeUtc)
    || !metadata.readString(backendIdLength, mesh.backendId)
    || !metadata.readString(backendVersionLength, mesh.backendVersion)
    || !metadata.atEnd()
    || !isValidString(mesh.creationTimeUtc, limits)
    || !isValidString(mesh.backendId, limits)
    || !isValidString(mesh.backendVersion, limits))
  {
    return readFailure(MeshBinaryErrorCode::InvalidBlock,
      "Mesh binary metadata block is invalid");
  }

  Cursor associationCursor = blockCursor(bytes,
    *findBlock(entries, BlockType::Associations));
  std::uint64_t storedAssociationCount = 0;
  if (!associationCursor.readU64(storedAssociationCount)
    || storedAssociationCount != associationCount)
  {
    return readFailure(MeshBinaryErrorCode::InvalidBlock,
      "Mesh binary association count does not match the header");
  }
  std::vector<std::string> associations;
  associations.reserve(static_cast<std::size_t>(associationCount));
  for (std::uint64_t index = 0; index < associationCount; ++index)
  {
    std::uint32_t length = 0;
    std::string value;
    if (!associationCursor.readU32(length)
      || length == 0 || length > limits.maximumStringBytes
      || !associationCursor.readString(length, value)
      || !isValidString(value, limits)
      || (!associations.empty() && associations.back() >= value))
    {
      return readFailure(MeshBinaryErrorCode::InvalidBlock,
        "Mesh binary association table is invalid or not canonical");
    }
    associations.push_back(std::move(value));
  }
  if (!associationCursor.atEnd())
  {
    return readFailure(MeshBinaryErrorCode::InvalidBlock,
      "Mesh binary association block has trailing bytes");
  }

  const DirectoryEntry& nodeBlock = *findBlock(entries, BlockType::Nodes);
  std::uint64_t expectedNodeBytes = 0;
  if (!checkedMultiply(nodeCount, 32, expectedNodeBytes)
    || nodeBlock.length != expectedNodeBytes)
  {
    return readFailure(MeshBinaryErrorCode::InvalidBlock,
      "Mesh binary node block length does not match its count");
  }
  Cursor nodeCursor = blockCursor(bytes, nodeBlock);
  mesh.nodes.reserve(static_cast<std::size_t>(nodeCount));
  for (std::uint64_t index = 0; index < nodeCount; ++index)
  {
    domain::MeshNode node;
    if (!nodeCursor.readU64(node.id)
      || !nodeCursor.readDouble(node.x)
      || !nodeCursor.readDouble(node.y)
      || !nodeCursor.readDouble(node.z))
    {
      return readFailure(MeshBinaryErrorCode::InvalidBlock,
        "Mesh binary node record is truncated");
    }
    mesh.nodes.push_back(std::move(node));
  }

  if (const DirectoryEntry* nodeAssociations =
      findBlock(entries, BlockType::NodeAssociations))
  {
    Cursor nodeAssociationCursor = blockCursor(bytes, *nodeAssociations);
    std::uint64_t recordCount = 0;
    std::uint64_t recordBytes = 0;
    std::uint64_t expectedBytes = 0;
    if (!nodeAssociationCursor.readU64(recordCount)
      || recordCount > nodeCount
      || !checkedMultiply(recordCount, 16, recordBytes)
      || !checkedAdd(8, recordBytes, expectedBytes)
      || expectedBytes != nodeAssociations->length)
    {
      return readFailure(MeshBinaryErrorCode::InvalidBlock,
        "Mesh binary node-association block length is invalid");
    }
    std::unordered_map<domain::MeshNodeId, std::size_t> nodePositions;
    nodePositions.reserve(mesh.nodes.size());
    for (std::size_t index = 0; index < mesh.nodes.size(); ++index)
    {
      if (!nodePositions.emplace(mesh.nodes.at(index).id, index).second)
      {
        return readFailure(MeshBinaryErrorCode::InvalidBlock,
          "Node associations cannot target duplicate node IDs");
      }
    }
    bool hasPreviousPosition = false;
    std::size_t previousPosition = 0;
    for (std::uint64_t index = 0; index < recordCount; ++index)
    {
      std::uint64_t nodeId = 0;
      std::uint32_t associationIndex = 0;
      std::uint32_t reserved = 0;
      if (!nodeAssociationCursor.readU64(nodeId)
        || !nodeAssociationCursor.readU32(associationIndex)
        || !nodeAssociationCursor.readU32(reserved)
        || reserved != 0)
      {
        return readFailure(MeshBinaryErrorCode::InvalidBlock,
          "Mesh binary node-association record is invalid");
      }
      const auto position = nodePositions.find(nodeId);
      const std::string* geometryId = associationAt(associations, associationIndex);
      if (position == nodePositions.end() || geometryId == nullptr
        || (hasPreviousPosition && position->second <= previousPosition))
      {
        return readFailure(MeshBinaryErrorCode::InvalidBlock,
          "Mesh binary node associations are missing, duplicate, or out of order");
      }
      mesh.nodes.at(position->second).geometryId = *geometryId;
      previousPosition = position->second;
      hasPreviousPosition = true;
    }
  }

  std::uint64_t totalElementCount = 0;
  if (!checkedAdd(triangleCount, tetrahedronCount, totalElementCount)
    || totalElementCount > std::numeric_limits<std::size_t>::max())
  {
    return readFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
      "Mesh binary total element count overflows memory limits");
  }
  mesh.elements.reserve(static_cast<std::size_t>(totalElementCount));

  const DirectoryEntry& triangleBlock = *findBlock(entries, BlockType::Triangles);
  std::uint64_t expectedTriangleBytes = 0;
  if (!checkedMultiply(triangleCount, 48, expectedTriangleBytes)
    || triangleBlock.length != expectedTriangleBytes)
  {
    return readFailure(MeshBinaryErrorCode::InvalidBlock,
      "Mesh binary Triangle3 block length does not match its count");
  }
  Cursor triangleCursor = blockCursor(bytes, triangleBlock);
  for (std::uint64_t index = 0; index < triangleCount; ++index)
  {
    domain::MeshElement element;
    element.type = domain::MeshElementType::Triangle3;
    std::uint32_t associationIndex = 0;
    std::uint32_t reserved = 0;
    if (!triangleCursor.readU64(element.id)
      || !triangleCursor.readU64(element.nodeIds.at(0))
      || !triangleCursor.readU64(element.nodeIds.at(1))
      || !triangleCursor.readU64(element.nodeIds.at(2))
      || !triangleCursor.readU64(element.regionId)
      || !triangleCursor.readU32(associationIndex)
      || !triangleCursor.readU32(reserved) || reserved != 0)
    {
      return readFailure(MeshBinaryErrorCode::InvalidBlock,
        "Mesh binary Triangle3 record is invalid");
    }
    const std::string* geometryId = associationAt(associations, associationIndex);
    if (geometryId == nullptr)
    {
      return readFailure(MeshBinaryErrorCode::InvalidBlock,
        "Mesh binary Triangle3 association index is invalid");
    }
    element.geometryId = *geometryId;
    mesh.elements.push_back(std::move(element));
  }

  const DirectoryEntry& tetrahedronBlock =
    *findBlock(entries, BlockType::Tetrahedra);
  std::uint64_t expectedTetrahedronBytes = 0;
  if (!checkedMultiply(tetrahedronCount, 56, expectedTetrahedronBytes)
    || tetrahedronBlock.length != expectedTetrahedronBytes)
  {
    return readFailure(MeshBinaryErrorCode::InvalidBlock,
      "Mesh binary Tetrahedron4 block length does not match its count");
  }
  Cursor tetrahedronCursor = blockCursor(bytes, tetrahedronBlock);
  for (std::uint64_t index = 0; index < tetrahedronCount; ++index)
  {
    domain::MeshElement element;
    element.type = domain::MeshElementType::Tetrahedron4;
    std::uint32_t associationIndex = 0;
    std::uint32_t reserved = 0;
    if (!tetrahedronCursor.readU64(element.id)
      || !tetrahedronCursor.readU64(element.nodeIds.at(0))
      || !tetrahedronCursor.readU64(element.nodeIds.at(1))
      || !tetrahedronCursor.readU64(element.nodeIds.at(2))
      || !tetrahedronCursor.readU64(element.nodeIds.at(3))
      || !tetrahedronCursor.readU64(element.regionId)
      || !tetrahedronCursor.readU32(associationIndex)
      || !tetrahedronCursor.readU32(reserved) || reserved != 0)
    {
      return readFailure(MeshBinaryErrorCode::InvalidBlock,
        "Mesh binary Tetrahedron4 record is invalid");
    }
    const std::string* geometryId = associationAt(associations, associationIndex);
    if (geometryId == nullptr)
    {
      return readFailure(MeshBinaryErrorCode::InvalidBlock,
        "Mesh binary Tetrahedron4 association index is invalid");
    }
    element.geometryId = *geometryId;
    mesh.elements.push_back(std::move(element));
  }

  Cursor regionCursor = blockCursor(bytes,
    *findBlock(entries, BlockType::Regions));
  mesh.regions.reserve(static_cast<std::size_t>(regionCount));
  for (std::uint64_t index = 0; index < regionCount; ++index)
  {
    domain::MeshRegion region;
    std::uint32_t mappingCount = 0;
    std::uint32_t reserved = 0;
    if (!regionCursor.readU64(region.id)
      || !regionCursor.readU32(mappingCount)
      || !regionCursor.readU32(reserved)
      || mappingCount == 0 || mappingCount > limits.maximumAssociations
      || reserved != 0)
    {
      return readFailure(MeshBinaryErrorCode::InvalidBlock,
        "Mesh binary region record header is invalid");
    }
    const std::uint64_t mappingBytes =
      static_cast<std::uint64_t>(mappingCount) * 4
      + (mappingCount % 2 != 0 ? 4 : 0);
    if (mappingBytes > regionCursor.remaining())
    {
      return readFailure(MeshBinaryErrorCode::InvalidBlock,
        "Mesh binary region mappings exceed the remaining block bytes");
    }
    region.sourceGeometryIds.reserve(mappingCount);
    for (std::uint32_t mapping = 0; mapping < mappingCount; ++mapping)
    {
      std::uint32_t associationIndex = 0;
      if (!regionCursor.readU32(associationIndex))
      {
        return readFailure(MeshBinaryErrorCode::InvalidBlock,
          "Mesh binary region mapping is truncated");
      }
      const std::string* geometryId = associationAt(associations, associationIndex);
      if (geometryId == nullptr)
      {
        return readFailure(MeshBinaryErrorCode::InvalidBlock,
          "Mesh binary region association index is invalid");
      }
      region.sourceGeometryIds.push_back(*geometryId);
    }
    if (mappingCount % 2 != 0)
    {
      std::uint32_t padding = 0;
      if (!regionCursor.readU32(padding) || padding != 0)
      {
        return readFailure(MeshBinaryErrorCode::InvalidBlock,
          "Mesh binary region alignment padding is invalid");
      }
    }
    mesh.regions.push_back(std::move(region));
  }
  if (!regionCursor.atEnd())
  {
    return readFailure(MeshBinaryErrorCode::InvalidBlock,
      "Mesh binary region block has trailing bytes");
  }

  if (!domain::validateMeshModel(mesh, validationMode).valid())
  {
    return readFailure(MeshBinaryErrorCode::InvalidMesh,
      "Decoded MeshModel failed domain validation");
  }
  return {std::move(mesh), MeshBinaryErrorCode::None, {}};
}
} // namespace

MeshBinaryReadResult readMeshBinary(
  const std::vector<std::uint8_t>& bytes,
  domain::MeshValidationMode validationMode,
  const MeshBinaryLimits& limits)
{
  try
  {
    return readMeshBinaryImpl(bytes, validationMode, limits);
  }
  catch (const std::bad_alloc&)
  {
    return readFailure(MeshBinaryErrorCode::ResourceLimitExceeded,
      "Mesh binary decoding allocation failed");
  }
}
}
