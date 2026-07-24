#include "GmshMsh41Reader.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <istream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cceda::meshing
{
namespace
{
enum class LineStatus
{
  Line,
  End,
  InputLimit,
  LineLimit,
  ReadError,
};

class BoundedLineReader
{
public:
  BoundedLineReader(
    std::istream& input,
    std::uint64_t maximumInputBytes,
    std::uint64_t maximumLineBytes)
    : input_(input)
    , maximumInputBytes_(maximumInputBytes)
    , maximumLineBytes_(maximumLineBytes)
  {
  }

  LineStatus read(std::string& line)
  {
    line.clear();
    bool readAny = false;
    char character = 0;
    while (input_.get(character))
    {
      readAny = true;
      if (++bytesRead_ > maximumInputBytes_)
      {
        return LineStatus::InputLimit;
      }
      if (character == '\n')
      {
        ++lineNumber_;
        return LineStatus::Line;
      }
      if (character == '\r')
      {
        continue;
      }
      if (line.size() >= maximumLineBytes_)
      {
        return LineStatus::LineLimit;
      }
      line.push_back(character);
    }

    if (input_.bad())
    {
      return LineStatus::ReadError;
    }
    if (readAny)
    {
      ++lineNumber_;
      return LineStatus::Line;
    }
    return LineStatus::End;
  }

  [[nodiscard]] std::uint64_t lineNumber() const noexcept
  {
    return lineNumber_;
  }

private:
  std::istream& input_;
  std::uint64_t maximumInputBytes_ = 0;
  std::uint64_t maximumLineBytes_ = 0;
  std::uint64_t bytesRead_ = 0;
  std::uint64_t lineNumber_ = 0;
};

std::string_view trim(std::string_view value)
{
  while (!value.empty()
    && std::isspace(static_cast<unsigned char>(value.front())) != 0)
  {
    value.remove_prefix(1);
  }
  while (!value.empty()
    && std::isspace(static_cast<unsigned char>(value.back())) != 0)
  {
    value.remove_suffix(1);
  }
  return value;
}

class LineTokens
{
public:
  void push(std::string_view value)
  {
    if (count_ < values_.size())
    {
      values_.at(count_++) = value;
    }
    else
    {
      overflow_ = true;
    }
  }

  [[nodiscard]] std::size_t size() const noexcept
  {
    return overflow_ ? values_.size() + 1 : count_;
  }

  [[nodiscard]] std::string_view at(std::size_t index) const
  {
    return values_.at(index);
  }

private:
  std::array<std::string_view, 6> values_{};
  std::size_t count_ = 0;
  bool overflow_ = false;
};

LineTokens tokens(std::string_view line)
{
  LineTokens result;
  std::size_t position = 0;
  while (position < line.size())
  {
    while (position < line.size()
      && std::isspace(static_cast<unsigned char>(line.at(position))) != 0)
    {
      ++position;
    }
    const std::size_t start = position;
    while (position < line.size()
      && std::isspace(static_cast<unsigned char>(line.at(position))) == 0)
    {
      ++position;
    }
    if (start < position)
    {
      result.push(std::string_view(line.data() + start, position - start));
    }
  }
  return result;
}

template<typename Value>
bool parseNumber(std::string_view token, Value& value)
{
  const char* const begin = token.data();
  const char* const end = begin + token.size();
  const auto parsed = std::from_chars(begin, end, value);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

std::uint64_t entityKey(int dimension, int tag)
{
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(dimension)) << 32)
    | static_cast<std::uint32_t>(tag);
}

class Parser
{
public:
  Parser(
    std::istream& input,
    const GmshMeshMetadata& metadata,
    const GmshMeshMapping& mapping,
    const GmshMsh41ReadLimits& limits)
    : reader_(input, limits.maximumInputBytes, limits.maximumLineBytes)
    , metadata_(metadata)
    , mapping_(mapping)
    , limits_(limits)
  {
  }

  GmshMsh41ReadResult parse()
  {
    if (!validateLimits() || !validateMapping())
    {
      return std::move(result_);
    }

    mesh_.meshRevision = 0;
    mesh_.sourceGeometryRevision = metadata_.sourceGeometryRevision;
    mesh_.creationTimeUtc = metadata_.creationTimeUtc;
    mesh_.backendId = "gmsh";
    mesh_.backendVersion = metadata_.backendVersion;
    mesh_.conformity = metadata_.conformity;
    mesh_.regions = mapping_.regions;

    std::string line;
    while (true)
    {
      const LineStatus status = reader_.read(line);
      if (status == LineStatus::End)
      {
        break;
      }
      if (!handleLineStatus(status))
      {
        return std::move(result_);
      }

      const std::string_view marker = trim(line);
      if (marker.empty())
      {
        continue;
      }
      if (!sawMeshFormat_ && marker != "$MeshFormat")
      {
        fail(GmshMsh41ErrorCode::InvalidSyntax,
          "$MeshFormat must be the first section");
        return std::move(result_);
      }
      if (marker == "$MeshFormat")
      {
        if (sawMeshFormat_ || !parseMeshFormat())
        {
          return std::move(result_);
        }
        sawMeshFormat_ = true;
      }
      else if (marker == "$Nodes")
      {
        if (sawNodes_ || sawElements_ || !parseNodes())
        {
          if (result_.error == GmshMsh41ErrorCode::None)
          {
            fail(GmshMsh41ErrorCode::InvalidSyntax,
              "duplicate or out-of-order $Nodes section");
          }
          return std::move(result_);
        }
        sawNodes_ = true;
      }
      else if (marker == "$Elements")
      {
        if (!sawNodes_ || sawElements_ || !parseElements())
        {
          if (result_.error == GmshMsh41ErrorCode::None)
          {
            fail(GmshMsh41ErrorCode::InvalidSyntax,
              "duplicate or out-of-order $Elements section");
          }
          return std::move(result_);
        }
        sawElements_ = true;
      }
      else if (marker.front() == '$'
        && marker.substr(0, 4) != "$End")
      {
        if (!skipSection(marker))
        {
          return std::move(result_);
        }
      }
      else
      {
        fail(GmshMsh41ErrorCode::InvalidSyntax,
          "unexpected data outside an MSH section");
        return std::move(result_);
      }
    }

    if (!sawMeshFormat_ || !sawNodes_ || !sawElements_)
    {
      fail(GmshMsh41ErrorCode::InvalidSyntax,
        "required MSH sections are missing");
      return std::move(result_);
    }

    const domain::MeshValidationReport validation =
      domain::validateMeshModel(mesh_, domain::MeshValidationMode::WorkerArtifact);
    if (!validation.valid())
    {
      fail(GmshMsh41ErrorCode::MeshValidationFailed,
        "converted mesh violates the neutral mesh contract");
      return std::move(result_);
    }

    result_.mesh = std::move(mesh_);
    return std::move(result_);
  }

private:
  bool validateLimits()
  {
    if (limits_.maximumInputBytes == 0
      || limits_.maximumLineBytes == 0
      || limits_.maximumNodes == 0
      || limits_.maximumElements == 0
      || limits_.maximumEntityBlocks == 0
      || limits_.maximumNodes > std::numeric_limits<std::size_t>::max()
      || limits_.maximumElements > std::numeric_limits<std::size_t>::max())
    {
      return fail(GmshMsh41ErrorCode::InvalidLimits,
        "all MSH reader limits must be positive and addressable");
    }
    return true;
  }

  bool validateMapping()
  {
    std::unordered_set<domain::MeshRegionId> regionIds;
    regionIds.reserve(mapping_.regions.size());
    for (const domain::MeshRegion& region : mapping_.regions)
    {
      if (region.id == 0 || region.sourceGeometryIds.empty()
        || !regionIds.insert(region.id).second)
      {
        return fail(GmshMsh41ErrorCode::EntityMappingInvalid,
          "region mappings require unique positive IDs and source geometry");
      }
      for (const std::string& geometryId : region.sourceGeometryIds)
      {
        if (geometryId.empty())
        {
          return fail(GmshMsh41ErrorCode::EntityMappingInvalid,
            "region source geometry IDs cannot be empty");
        }
      }
    }

    entityMappings_.reserve(mapping_.entities.size());
    for (const GmshEntityMapping& entity : mapping_.entities)
    {
      if (entity.entityDimension < 0 || entity.entityDimension > 3
        || entity.entityTag <= 0 || entity.geometryId.empty()
        || ((entity.entityDimension == 2 || entity.entityDimension == 3)
          && (entity.regionId == 0
            || regionIds.find(entity.regionId) == regionIds.end())))
      {
        return fail(GmshMsh41ErrorCode::EntityMappingInvalid,
          "entity mappings contain invalid dimensions, tags, regions, or geometry IDs");
      }
      if (!entityMappings_.emplace(
          entityKey(entity.entityDimension, entity.entityTag), &entity).second)
      {
        return fail(GmshMsh41ErrorCode::EntityMappingInvalid,
          "entity mappings must be unique by dimension and tag");
      }
    }
    return true;
  }

  bool parseMeshFormat()
  {
    std::string line;
    if (!readRequiredLine(line))
    {
      return false;
    }
    const auto fields = tokens(line);
    if (fields.size() != 3
      || fields.at(0) != "4.1"
      || fields.at(1) != "0"
      || fields.at(2) != "8")
    {
      return fail(GmshMsh41ErrorCode::UnsupportedFormat,
        "expected ASCII MSH 4.1 with 8-byte size values");
    }
    return expectEndMarker("$EndMeshFormat");
  }

  bool parseNodes()
  {
    std::string line;
    if (!readRequiredLine(line))
    {
      return false;
    }
    const auto header = tokens(line);
    std::uint64_t blockCount = 0;
    std::uint64_t declaredCount = 0;
    std::uint64_t declaredMinimum = 0;
    std::uint64_t declaredMaximum = 0;
    if (header.size() != 4
      || !parseNumber(header.at(0), blockCount)
      || !parseNumber(header.at(1), declaredCount)
      || !parseNumber(header.at(2), declaredMinimum)
      || !parseNumber(header.at(3), declaredMaximum)
      || blockCount == 0 || declaredCount == 0
      || declaredMinimum == 0 || declaredMaximum < declaredMinimum)
    {
      return fail(GmshMsh41ErrorCode::InvalidSyntax,
        "invalid $Nodes header");
    }
    if (blockCount > limits_.maximumEntityBlocks
      || declaredCount > limits_.maximumNodes)
    {
      return fail(GmshMsh41ErrorCode::CountLimitExceeded,
        "$Nodes counts exceed the caller budget");
    }

    mesh_.nodes.reserve(static_cast<std::size_t>(declaredCount));
    nodeIds_.reserve(static_cast<std::size_t>(declaredCount));
    std::uint64_t actualMinimum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t actualMaximum = 0;
    std::uint64_t actualCount = 0;

    for (std::uint64_t block = 0; block < blockCount; ++block)
    {
      if (!readRequiredLine(line))
      {
        return false;
      }
      const auto blockHeader = tokens(line);
      int dimension = -1;
      int tag = 0;
      int parametric = -1;
      std::uint64_t nodesInBlock = 0;
      if (blockHeader.size() != 4
        || !parseNumber(blockHeader.at(0), dimension)
        || !parseNumber(blockHeader.at(1), tag)
        || !parseNumber(blockHeader.at(2), parametric)
        || !parseNumber(blockHeader.at(3), nodesInBlock)
        || dimension < 0 || dimension > 3 || tag <= 0
        || (parametric != 0 && parametric != 1)
        || nodesInBlock > declaredCount - actualCount)
      {
        return fail(GmshMsh41ErrorCode::InvalidSyntax,
          "invalid MSH node block header");
      }

      std::vector<domain::MeshNodeId> blockNodeIds;
      blockNodeIds.reserve(static_cast<std::size_t>(nodesInBlock));
      for (std::uint64_t index = 0; index < nodesInBlock; ++index)
      {
        if (!readRequiredLine(line))
        {
          return false;
        }
        const auto nodeTagFields = tokens(line);
        domain::MeshNodeId nodeId = 0;
        if (nodeTagFields.size() != 1
          || !parseNumber(nodeTagFields.at(0), nodeId) || nodeId == 0)
        {
          return fail(GmshMsh41ErrorCode::InvalidSyntax,
            "invalid MSH node tag");
        }
        if (!nodeIds_.insert(nodeId).second)
        {
          return fail(GmshMsh41ErrorCode::DuplicateNodeTag,
            "MSH node tags must be globally unique");
        }
        blockNodeIds.push_back(nodeId);
        actualMinimum = (std::min)(actualMinimum, nodeId);
        actualMaximum = (std::max)(actualMaximum, nodeId);
      }

      const auto mappingIterator = entityMappings_.find(entityKey(dimension, tag));
      const std::string geometryId = mappingIterator == entityMappings_.end()
        ? std::string{}
        : mappingIterator->second->geometryId;
      const std::size_t expectedCoordinateFields =
        3 + (parametric == 1 ? static_cast<std::size_t>(dimension) : 0);
      for (const domain::MeshNodeId nodeId : blockNodeIds)
      {
        if (!readRequiredLine(line))
        {
          return false;
        }
        const auto coordinates = tokens(line);
        domain::MeshNode node;
        node.id = nodeId;
        node.geometryId = geometryId;
        if (coordinates.size() != expectedCoordinateFields
          || !parseNumber(coordinates.at(0), node.x)
          || !parseNumber(coordinates.at(1), node.y)
          || !parseNumber(coordinates.at(2), node.z))
        {
          return fail(GmshMsh41ErrorCode::InvalidSyntax,
            "invalid MSH node coordinates");
        }
        for (std::size_t index = 3; index < coordinates.size(); ++index)
        {
          double parameter = 0.0;
          if (!parseNumber(coordinates.at(index), parameter))
          {
            return fail(GmshMsh41ErrorCode::InvalidSyntax,
              "invalid parametric node coordinate");
          }
        }
        mesh_.nodes.push_back(std::move(node));
      }
      actualCount += nodesInBlock;
    }

    if (!expectEndMarker("$EndNodes"))
    {
      return false;
    }
    if (actualCount != declaredCount
      || actualMinimum != declaredMinimum
      || actualMaximum != declaredMaximum)
    {
      return fail(GmshMsh41ErrorCode::CountMismatch,
        "$Nodes counts or tag bounds do not match the payload");
    }
    return true;
  }

  bool parseElements()
  {
    std::string line;
    if (!readRequiredLine(line))
    {
      return false;
    }
    const auto header = tokens(line);
    std::uint64_t blockCount = 0;
    std::uint64_t declaredCount = 0;
    std::uint64_t declaredMinimum = 0;
    std::uint64_t declaredMaximum = 0;
    if (header.size() != 4
      || !parseNumber(header.at(0), blockCount)
      || !parseNumber(header.at(1), declaredCount)
      || !parseNumber(header.at(2), declaredMinimum)
      || !parseNumber(header.at(3), declaredMaximum)
      || blockCount == 0 || declaredCount == 0
      || declaredMinimum == 0 || declaredMaximum < declaredMinimum)
    {
      return fail(GmshMsh41ErrorCode::InvalidSyntax,
        "invalid $Elements header");
    }
    if (blockCount > limits_.maximumEntityBlocks
      || declaredCount > limits_.maximumElements)
    {
      return fail(GmshMsh41ErrorCode::CountLimitExceeded,
        "$Elements counts exceed the caller budget");
    }

    mesh_.elements.reserve(static_cast<std::size_t>(declaredCount));
    std::unordered_set<domain::MeshElementId> elementIds;
    elementIds.reserve(static_cast<std::size_t>(declaredCount));
    std::uint64_t actualMinimum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t actualMaximum = 0;
    std::uint64_t actualCount = 0;

    for (std::uint64_t block = 0; block < blockCount; ++block)
    {
      if (!readRequiredLine(line))
      {
        return false;
      }
      const auto blockHeader = tokens(line);
      int dimension = -1;
      int tag = 0;
      int elementType = 0;
      std::uint64_t elementsInBlock = 0;
      if (blockHeader.size() != 4
        || !parseNumber(blockHeader.at(0), dimension)
        || !parseNumber(blockHeader.at(1), tag)
        || !parseNumber(blockHeader.at(2), elementType)
        || !parseNumber(blockHeader.at(3), elementsInBlock)
        || dimension < 0 || dimension > 3 || tag <= 0 || elementType <= 0
        || elementsInBlock > declaredCount - actualCount)
      {
        return fail(GmshMsh41ErrorCode::InvalidSyntax,
          "invalid MSH element block header");
      }

      const bool isPoint = dimension == 0 && elementType == 15;
      const bool isLine = dimension == 1 && elementType == 1;
      const bool isTriangle = dimension == 2 && elementType == 2;
      const bool isTetrahedron = dimension == 3 && elementType == 4;
      if (!isPoint && !isLine && !isTriangle && !isTetrahedron)
      {
        return fail(GmshMsh41ErrorCode::UnsupportedElementType,
          "element block type is outside the first-release low-order set");
      }

      const bool retainBlock = isTriangle || isTetrahedron;
      const GmshEntityMapping* entityMapping = nullptr;
      if (retainBlock)
      {
        const auto mappingIterator =
          entityMappings_.find(entityKey(dimension, tag));
        if (mappingIterator == entityMappings_.end())
        {
          return fail(GmshMsh41ErrorCode::EntityMappingMissing,
            "retained element block has no stable entity mapping");
        }
        entityMapping = mappingIterator->second;
      }

      for (std::uint64_t index = 0; index < elementsInBlock; ++index)
      {
        if (!readRequiredLine(line))
        {
          return false;
        }
        const auto elementFields = tokens(line);
        const std::size_t expectedFields = isPoint ? 2
          : (isLine ? 3 : (isTriangle ? 4 : 5));
        if (elementFields.size() != expectedFields)
        {
          return fail(GmshMsh41ErrorCode::InvalidSyntax,
            "invalid MSH element record");
        }

        domain::MeshElementId elementId = 0;
        if (!parseNumber(elementFields.at(0), elementId) || elementId == 0)
        {
          return fail(GmshMsh41ErrorCode::InvalidSyntax,
            "invalid MSH element tag");
        }
        if (!elementIds.insert(elementId).second)
        {
          return fail(GmshMsh41ErrorCode::DuplicateElementTag,
            "MSH element tags must be globally unique");
        }
        actualMinimum = (std::min)(actualMinimum, elementId);
        actualMaximum = (std::max)(actualMaximum, elementId);

        std::array<domain::MeshNodeId, 4> elementNodeIds{};
        for (std::size_t nodeIndex = 1;
          nodeIndex < elementFields.size(); ++nodeIndex)
        {
          domain::MeshNodeId nodeId = 0;
          if (!parseNumber(elementFields.at(nodeIndex), nodeId)
            || nodeId == 0 || nodeIds_.find(nodeId) == nodeIds_.end())
          {
            return fail(GmshMsh41ErrorCode::MeshValidationFailed,
              "MSH element references an unknown node tag");
          }
          elementNodeIds.at(nodeIndex - 1) = nodeId;
        }

        if (retainBlock)
        {
          domain::MeshElement element;
          element.id = elementId;
          element.type = isTriangle
            ? domain::MeshElementType::Triangle3
            : domain::MeshElementType::Tetrahedron4;
          element.regionId = entityMapping->regionId;
          element.geometryId = entityMapping->geometryId;
          element.nodeIds = elementNodeIds;
          mesh_.elements.push_back(std::move(element));
        }
      }
      actualCount += elementsInBlock;
    }

    if (!expectEndMarker("$EndElements"))
    {
      return false;
    }
    if (actualCount != declaredCount
      || actualMinimum != declaredMinimum
      || actualMaximum != declaredMaximum)
    {
      return fail(GmshMsh41ErrorCode::CountMismatch,
        "$Elements counts or tag bounds do not match the payload");
    }
    nodeIds_.clear();
    nodeIds_.rehash(0);
    return true;
  }

  bool skipSection(std::string_view startMarker)
  {
    if (startMarker.size() < 2
      || startMarker.find_first_of(" \t") != std::string_view::npos)
    {
      return fail(GmshMsh41ErrorCode::InvalidSyntax,
        "invalid MSH section marker");
    }
    const std::string endMarker = "$End" + std::string(startMarker.substr(1));
    std::string line;
    while (readRequiredLine(line))
    {
      if (trim(line) == endMarker)
      {
        return true;
      }
    }
    return false;
  }

  bool expectEndMarker(std::string_view marker)
  {
    std::string line;
    if (!readRequiredLine(line))
    {
      return false;
    }
    if (trim(line) != marker)
    {
      return fail(GmshMsh41ErrorCode::InvalidSyntax,
        "MSH section end marker is missing");
    }
    return true;
  }

  bool readRequiredLine(std::string& line)
  {
    const LineStatus status = reader_.read(line);
    if (status == LineStatus::Line)
    {
      return true;
    }
    if (status == LineStatus::End)
    {
      return fail(GmshMsh41ErrorCode::InvalidSyntax,
        "unexpected end of MSH input");
    }
    return handleLineStatus(status);
  }

  bool handleLineStatus(LineStatus status)
  {
    switch (status)
    {
      case LineStatus::Line:
      case LineStatus::End:
        return true;
      case LineStatus::InputLimit:
        return fail(GmshMsh41ErrorCode::InputLimitExceeded,
          "MSH input exceeds the byte budget");
      case LineStatus::LineLimit:
        return fail(GmshMsh41ErrorCode::LineLimitExceeded,
          "MSH line exceeds the byte budget");
      case LineStatus::ReadError:
        return fail(GmshMsh41ErrorCode::InvalidSyntax,
          "MSH input stream failed");
    }
    return fail(GmshMsh41ErrorCode::InvalidSyntax,
      "unknown MSH reader state");
  }

  bool fail(GmshMsh41ErrorCode code, std::string detail)
  {
    if (result_.error == GmshMsh41ErrorCode::None)
    {
      result_.error = code;
      result_.line = reader_.lineNumber();
      result_.detail = std::move(detail);
    }
    return false;
  }

  BoundedLineReader reader_;
  const GmshMeshMetadata& metadata_;
  const GmshMeshMapping& mapping_;
  const GmshMsh41ReadLimits& limits_;
  std::unordered_map<std::uint64_t, const GmshEntityMapping*> entityMappings_;
  std::unordered_set<domain::MeshNodeId> nodeIds_;
  domain::MeshModel mesh_;
  GmshMsh41ReadResult result_;
  bool sawMeshFormat_ = false;
  bool sawNodes_ = false;
  bool sawElements_ = false;
};
}

GmshMsh41ReadResult readGmshMsh41(
  std::istream& input,
  const GmshMeshMetadata& metadata,
  const GmshMeshMapping& mapping,
  const GmshMsh41ReadLimits& limits)
{
  try
  {
    return Parser(input, metadata, mapping, limits).parse();
  }
  catch (const std::bad_alloc&)
  {
    GmshMsh41ReadResult result;
    result.error = GmshMsh41ErrorCode::AllocationFailed;
    result.detail = "memory allocation failed within the caller budget";
    return result;
  }
  catch (const std::length_error&)
  {
    GmshMsh41ReadResult result;
    result.error = GmshMsh41ErrorCode::AllocationFailed;
    result.detail = "container allocation exceeds the platform limit";
    return result;
  }
}
}
