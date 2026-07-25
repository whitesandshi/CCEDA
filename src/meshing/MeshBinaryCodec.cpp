#include "MeshBinaryCodec.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <unordered_map>
#include <utility>

namespace cceda::meshing
{
namespace
{
constexpr std::size_t DirectoryEntryBytes = 40;
constexpr std::uint32_t ByteOrderMarker = 0x01020304U;
constexpr std::uint32_t CanonicalUnitMillimeters = 1;
constexpr std::uint32_t RequiredBlockFlag = 1;

enum class BlockType : std::uint32_t
{
  Metadata = 1,
  Associations = 2,
  Nodes = 3,
  NodeAssociations = 4,
  Triangles = 5,
  Tetrahedra = 6,
  Regions = 7,
};

struct Block
{
  BlockType type = BlockType::Metadata;
  bool required = true;
  std::vector<std::uint8_t> data;
  std::uint64_t offset = 0;
};

struct DirectoryEntry
{
  std::uint32_t type = 0;
  std::uint32_t flags = 0;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
  std::uint32_t checksum = 0;
};

MeshBinaryWriteResult writeFailure(
  MeshBinaryErrorCode error,
  std::string detail)
{
  return {{}, error, std::move(detail)};
}

MeshBinaryReadResult readFailure(
  MeshBinaryErrorCode error,
  std::string detail)
{
  return {{}, error, std::move(detail)};
}

bool hasValidLimits(const MeshBinaryLimits& limits)
{
  return limits.maximumFileBytes >= MeshBinaryHeaderBytes
    && limits.maximumNodes > 0
    && limits.maximumTriangles > 0
    && limits.maximumTetrahedra > 0
    && limits.maximumRegions > 0
    && limits.maximumAssociations > 0
    && limits.maximumDirectoryEntries >= 6
    && limits.maximumStringBytes > 0;
}

bool isValidUtf8(const std::string& value)
{
  if (value.size() >= 3
    && static_cast<std::uint8_t>(value[0]) == 0xef
    && static_cast<std::uint8_t>(value[1]) == 0xbb
    && static_cast<std::uint8_t>(value[2]) == 0xbf)
  {
    return false;
  }

  std::size_t index = 0;
  while (index < value.size())
  {
    const auto first = static_cast<std::uint8_t>(value[index]);
    if (first == 0)
    {
      return false;
    }
    if (first <= 0x7f)
    {
      ++index;
      continue;
    }

    std::size_t length = 0;
    if (first >= 0xc2 && first <= 0xdf)
    {
      length = 2;
    }
    else if (first >= 0xe0 && first <= 0xef)
    {
      length = 3;
    }
    else if (first >= 0xf0 && first <= 0xf4)
    {
      length = 4;
    }
    else
    {
      return false;
    }
    if (length > value.size() - index)
    {
      return false;
    }
    for (std::size_t continuation = 1; continuation < length; ++continuation)
    {
      const auto byte = static_cast<std::uint8_t>(value[index + continuation]);
      if ((byte & 0xc0U) != 0x80U)
      {
        return false;
      }
    }
    const auto second = static_cast<std::uint8_t>(value[index + 1]);
    if ((first == 0xe0 && second < 0xa0)
      || (first == 0xed && second >= 0xa0)
      || (first == 0xf0 && second < 0x90)
      || (first == 0xf4 && second >= 0x90))
    {
      return false;
    }
    index += length;
  }
  return true;
}

bool isValidString(const std::string& value, const MeshBinaryLimits& limits)
{
  return !value.empty()
    && value.size() <= limits.maximumStringBytes
    && isValidUtf8(value);
}

bool checkedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& result)
{
  if (right > std::numeric_limits<std::uint64_t>::max() - left)
  {
    return false;
  }
  result = left + right;
  return true;
}

bool checkedMultiply(
  std::uint64_t left,
  std::uint64_t right,
  std::uint64_t& result)
{
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
  {
    return false;
  }
  result = left * right;
  return true;
}

bool alignToEight(std::uint64_t value, std::uint64_t& aligned)
{
  std::uint64_t withPadding = 0;
  if (!checkedAdd(value, 7, withPadding))
  {
    return false;
  }
  aligned = withPadding & ~std::uint64_t{7};
  return true;
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
  for (int shift = 0; shift < 32; shift += 8)
  {
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void appendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
  for (int shift = 0; shift < 64; shift += 8)
  {
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void appendDouble(std::vector<std::uint8_t>& bytes, double value)
{
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  appendU64(bytes, bits);
}

void appendString(std::vector<std::uint8_t>& bytes, const std::string& value)
{
  bytes.insert(bytes.end(), value.begin(), value.end());
}

void setU16(
  std::vector<std::uint8_t>& bytes,
  std::size_t offset,
  std::uint16_t value)
{
  bytes.at(offset) = static_cast<std::uint8_t>(value);
  bytes.at(offset + 1) = static_cast<std::uint8_t>(value >> 8);
}

void setU32(
  std::vector<std::uint8_t>& bytes,
  std::size_t offset,
  std::uint32_t value)
{
  for (int shift = 0; shift < 32; shift += 8)
  {
    bytes.at(offset++) = static_cast<std::uint8_t>(value >> shift);
  }
}

void setU64(
  std::vector<std::uint8_t>& bytes,
  std::size_t offset,
  std::uint64_t value)
{
  for (int shift = 0; shift < 64; shift += 8)
  {
    bytes.at(offset++) = static_cast<std::uint8_t>(value >> shift);
  }
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t index = 0; index < size; ++index)
  {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit)
    {
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return crc ^ 0xffffffffU;
}

std::uint16_t readU16At(
  const std::vector<std::uint8_t>& bytes,
  std::size_t offset)
{
  return static_cast<std::uint16_t>(bytes.at(offset))
    | static_cast<std::uint16_t>(bytes.at(offset + 1) << 8);
}

std::uint32_t readU32At(
  const std::vector<std::uint8_t>& bytes,
  std::size_t offset)
{
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8)
  {
    value |= static_cast<std::uint32_t>(bytes.at(offset++)) << shift;
  }
  return value;
}

std::uint64_t readU64At(
  const std::vector<std::uint8_t>& bytes,
  std::size_t offset)
{
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8)
  {
    value |= static_cast<std::uint64_t>(bytes.at(offset++)) << shift;
  }
  return value;
}

class Cursor
{
public:
  Cursor(const std::uint8_t* data, std::size_t size)
    : data_(data), size_(size)
  {
  }

  bool readU32(std::uint32_t& value)
  {
    if (remaining() < 4)
    {
      return false;
    }
    value = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
      value |= static_cast<std::uint32_t>(data_[position_++]) << shift;
    }
    return true;
  }

  bool readU64(std::uint64_t& value)
  {
    if (remaining() < 8)
    {
      return false;
    }
    value = 0;
    for (int shift = 0; shift < 64; shift += 8)
    {
      value |= static_cast<std::uint64_t>(data_[position_++]) << shift;
    }
    return true;
  }

  bool readDouble(double& value)
  {
    std::uint64_t bits = 0;
    if (!readU64(bits))
    {
      return false;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return true;
  }

  bool readString(std::uint32_t length, std::string& value)
  {
    if (remaining() < length)
    {
      return false;
    }
    value.assign(reinterpret_cast<const char*>(data_ + position_), length);
    position_ += length;
    return true;
  }

  [[nodiscard]] std::size_t remaining() const noexcept
  {
    return size_ - position_;
  }

  [[nodiscard]] bool atEnd() const noexcept
  {
    return position_ == size_;
  }

private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t position_ = 0;
};

bool isKnownBlock(std::uint32_t type)
{
  return type >= static_cast<std::uint32_t>(BlockType::Metadata)
    && type <= static_cast<std::uint32_t>(BlockType::Regions);
}

bool isRequiredBlock(std::uint32_t type)
{
  return type == static_cast<std::uint32_t>(BlockType::Metadata)
    || type == static_cast<std::uint32_t>(BlockType::Associations)
    || type == static_cast<std::uint32_t>(BlockType::Nodes)
    || type == static_cast<std::uint32_t>(BlockType::Triangles)
    || type == static_cast<std::uint32_t>(BlockType::Tetrahedra)
    || type == static_cast<std::uint32_t>(BlockType::Regions);
}

const DirectoryEntry* findBlock(
  const std::map<std::uint32_t, DirectoryEntry>& entries,
  BlockType type)
{
  const auto found = entries.find(static_cast<std::uint32_t>(type));
  return found == entries.end() ? nullptr : &found->second;
}

Cursor blockCursor(
  const std::vector<std::uint8_t>& bytes,
  const DirectoryEntry& block)
{
  return Cursor(bytes.data() + static_cast<std::size_t>(block.offset),
    static_cast<std::size_t>(block.length));
}

const std::string* associationAt(
  const std::vector<std::string>& associations,
  std::uint32_t index)
{
  return index == 0 || index > associations.size()
    ? nullptr : &associations.at(index - 1);
}

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
