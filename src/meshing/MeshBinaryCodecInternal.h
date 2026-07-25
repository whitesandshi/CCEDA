#pragma once

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

namespace cceda::meshing::detail
{
inline constexpr std::size_t DirectoryEntryBytes = 40;
inline constexpr std::uint32_t ByteOrderMarker = 0x01020304U;
inline constexpr std::uint32_t CanonicalUnitMillimeters = 1;
inline constexpr std::uint32_t RequiredBlockFlag = 1;

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

inline MeshBinaryWriteResult writeFailure(
  MeshBinaryErrorCode error,
  std::string detail)
{
  return {{}, error, std::move(detail)};
}

inline MeshBinaryReadResult readFailure(
  MeshBinaryErrorCode error,
  std::string detail)
{
  return {{}, error, std::move(detail)};
}

inline bool hasValidLimits(const MeshBinaryLimits& limits)
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

inline bool isValidUtf8(const std::string& value)
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

inline bool isValidString(const std::string& value, const MeshBinaryLimits& limits)
{
  return !value.empty()
    && value.size() <= limits.maximumStringBytes
    && isValidUtf8(value);
}

inline bool checkedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& result)
{
  if (right > std::numeric_limits<std::uint64_t>::max() - left)
  {
    return false;
  }
  result = left + right;
  return true;
}

inline bool checkedMultiply(
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

inline bool alignToEight(std::uint64_t value, std::uint64_t& aligned)
{
  std::uint64_t withPadding = 0;
  if (!checkedAdd(value, 7, withPadding))
  {
    return false;
  }
  aligned = withPadding & ~std::uint64_t{7};
  return true;
}

inline void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
  for (int shift = 0; shift < 32; shift += 8)
  {
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

inline void appendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
  for (int shift = 0; shift < 64; shift += 8)
  {
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

inline void appendDouble(std::vector<std::uint8_t>& bytes, double value)
{
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  appendU64(bytes, bits);
}

inline void appendString(std::vector<std::uint8_t>& bytes, const std::string& value)
{
  bytes.insert(bytes.end(), value.begin(), value.end());
}

inline void setU16(
  std::vector<std::uint8_t>& bytes,
  std::size_t offset,
  std::uint16_t value)
{
  bytes.at(offset) = static_cast<std::uint8_t>(value);
  bytes.at(offset + 1) = static_cast<std::uint8_t>(value >> 8);
}

inline void setU32(
  std::vector<std::uint8_t>& bytes,
  std::size_t offset,
  std::uint32_t value)
{
  for (int shift = 0; shift < 32; shift += 8)
  {
    bytes.at(offset++) = static_cast<std::uint8_t>(value >> shift);
  }
}

inline void setU64(
  std::vector<std::uint8_t>& bytes,
  std::size_t offset,
  std::uint64_t value)
{
  for (int shift = 0; shift < 64; shift += 8)
  {
    bytes.at(offset++) = static_cast<std::uint8_t>(value >> shift);
  }
}

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
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

inline std::uint16_t readU16At(
  const std::vector<std::uint8_t>& bytes,
  std::size_t offset)
{
  return static_cast<std::uint16_t>(bytes.at(offset))
    | static_cast<std::uint16_t>(bytes.at(offset + 1) << 8);
}

inline std::uint32_t readU32At(
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

inline std::uint64_t readU64At(
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

inline bool isKnownBlock(std::uint32_t type)
{
  return type >= static_cast<std::uint32_t>(BlockType::Metadata)
    && type <= static_cast<std::uint32_t>(BlockType::Regions);
}

inline bool isRequiredBlock(std::uint32_t type)
{
  return type == static_cast<std::uint32_t>(BlockType::Metadata)
    || type == static_cast<std::uint32_t>(BlockType::Associations)
    || type == static_cast<std::uint32_t>(BlockType::Nodes)
    || type == static_cast<std::uint32_t>(BlockType::Triangles)
    || type == static_cast<std::uint32_t>(BlockType::Tetrahedra)
    || type == static_cast<std::uint32_t>(BlockType::Regions);
}

inline const DirectoryEntry* findBlock(
  const std::map<std::uint32_t, DirectoryEntry>& entries,
  BlockType type)
{
  const auto found = entries.find(static_cast<std::uint32_t>(type));
  return found == entries.end() ? nullptr : &found->second;
}

inline Cursor blockCursor(
  const std::vector<std::uint8_t>& bytes,
  const DirectoryEntry& block)
{
  return Cursor(bytes.data() + static_cast<std::size_t>(block.offset),
    static_cast<std::size_t>(block.length));
}

inline const std::string* associationAt(
  const std::vector<std::string>& associations,
  std::uint32_t index)
{
  return index == 0 || index > associations.size()
    ? nullptr : &associations.at(index - 1);
}
}
