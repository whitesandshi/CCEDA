#include "meshing/MeshBinaryCodec.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace
{
int failures = 0;

void expect(bool condition, std::string_view message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8)
  {
    value |= static_cast<std::uint32_t>(bytes.at(offset++)) << shift;
  }
  return value;
}

std::uint64_t readU64(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8)
  {
    value |= static_cast<std::uint64_t>(bytes.at(offset++)) << shift;
  }
  return value;
}

void writeU16(
  std::vector<std::uint8_t>& bytes,
  std::size_t offset,
  std::uint16_t value)
{
  bytes.at(offset) = static_cast<std::uint8_t>(value);
  bytes.at(offset + 1) = static_cast<std::uint8_t>(value >> 8);
}

void writeU32(
  std::vector<std::uint8_t>& bytes,
  std::size_t offset,
  std::uint32_t value)
{
  for (int shift = 0; shift < 32; shift += 8)
  {
    bytes.at(offset++) = static_cast<std::uint8_t>(value >> shift);
  }
}

void writeU64(
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

std::size_t directoryEntry(
  const std::vector<std::uint8_t>& bytes,
  std::uint32_t blockType)
{
  const auto directoryOffset = static_cast<std::size_t>(readU64(bytes, 88));
  const std::uint32_t entryCount = readU32(bytes, 96);
  const std::uint32_t entrySize = readU32(bytes, 100);
  for (std::uint32_t index = 0; index < entryCount; ++index)
  {
    const std::size_t offset = directoryOffset
      + static_cast<std::size_t>(index) * entrySize;
    if (readU32(bytes, offset) == blockType)
    {
      return offset;
    }
  }
  return std::numeric_limits<std::size_t>::max();
}

void refreshHeaderCrc(std::vector<std::uint8_t>& bytes)
{
  writeU32(bytes, 112, 0);
  writeU32(bytes, 112,
    crc32(bytes.data(), cceda::meshing::MeshBinaryHeaderBytes));
}

void refreshBlockCrc(
  std::vector<std::uint8_t>& bytes,
  std::size_t entryOffset)
{
  const auto blockOffset = static_cast<std::size_t>(readU64(bytes, entryOffset + 8));
  const auto blockLength = static_cast<std::size_t>(readU64(bytes, entryOffset + 24));
  writeU32(bytes, entryOffset + 32,
    crc32(bytes.data() + blockOffset, blockLength));
}

std::vector<std::uint8_t> withUnknownOptionalBlock(
  const std::vector<std::uint8_t>& source)
{
  std::vector<std::uint8_t> bytes = source;
  const auto directoryOffset = static_cast<std::size_t>(readU64(bytes, 88));
  const std::uint32_t oldEntryCount = readU32(bytes, 96);
  const std::uint32_t entrySize = readU32(bytes, 100);
  const auto oldDataOffset = static_cast<std::size_t>(
    readU64(bytes, directoryOffset + 8));
  bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(oldDataOffset),
    entrySize, std::uint8_t{0});

  for (std::uint32_t index = 0; index < oldEntryCount; ++index)
  {
    const std::size_t entryOffset = directoryOffset
      + static_cast<std::size_t>(index) * entrySize;
    writeU64(bytes, entryOffset + 8,
      readU64(bytes, entryOffset + 8) + entrySize);
  }

  while (bytes.size() % 8 != 0)
  {
    bytes.push_back(0);
  }
  const std::uint64_t optionalOffset = bytes.size();
  const std::vector<std::uint8_t> optionalData{0x12, 0x34, 0x56};
  bytes.insert(bytes.end(), optionalData.begin(), optionalData.end());

  const std::size_t newEntryOffset = directoryOffset
    + static_cast<std::size_t>(oldEntryCount) * entrySize;
  writeU32(bytes, newEntryOffset, 8);
  writeU32(bytes, newEntryOffset + 4, 0);
  writeU64(bytes, newEntryOffset + 8, optionalOffset);
  writeU64(bytes, newEntryOffset + 16, optionalData.size());
  writeU64(bytes, newEntryOffset + 24, optionalData.size());
  writeU32(bytes, newEntryOffset + 32,
    crc32(optionalData.data(), optionalData.size()));
  writeU32(bytes, 96, oldEntryCount + 1);
  writeU64(bytes, 104, bytes.size());
  writeU16(bytes, 10, 2);
  refreshHeaderCrc(bytes);
  return bytes;
}

cceda::domain::MeshModel makeWorkerMesh()
{
  using namespace cceda::domain;

  MeshModel mesh;
  mesh.meshRevision = 0;
  mesh.sourceGeometryRevision = 19;
  mesh.creationTimeUtc = "2026-07-25T12:34:56.000Z";
  mesh.backendId = "gmsh";
  mesh.backendVersion = "4.15.2";
  mesh.conformity = MeshConformity::NonConformal;
  mesh.nodes = {
    {11, 0.0, 0.0, 0.0, "solid-1/vertex-1"},
    {12, 1.0, 0.0, 0.0, ""},
    {13, 0.0, 1.0, 0.0, "solid-1/vertex-3"},
    {14, 0.0, 0.0, 1.0, ""},
  };
  mesh.elements = {
    {101, MeshElementType::Triangle3, {11, 12, 13, 0},
      7, "solid-1/face-1"},
    {102, MeshElementType::Tetrahedron4, {11, 12, 13, 14},
      7, "solid-1"},
  };
  mesh.regions = {
    {7, {"solid-1", "fragment-1"}},
  };
  return mesh;
}

bool sameMesh(
  const cceda::domain::MeshModel& left,
  const cceda::domain::MeshModel& right)
{
  if (left.meshRevision != right.meshRevision
    || left.sourceGeometryRevision != right.sourceGeometryRevision
    || left.creationTimeUtc != right.creationTimeUtc
    || left.backendId != right.backendId
    || left.backendVersion != right.backendVersion
    || left.conformity != right.conformity
    || left.nodes.size() != right.nodes.size()
    || left.elements.size() != right.elements.size()
    || left.regions.size() != right.regions.size())
  {
    return false;
  }
  for (std::size_t index = 0; index < left.nodes.size(); ++index)
  {
    const auto& a = left.nodes.at(index);
    const auto& b = right.nodes.at(index);
    if (a.id != b.id || a.x != b.x || a.y != b.y || a.z != b.z
      || a.geometryId != b.geometryId)
    {
      return false;
    }
  }
  for (std::size_t index = 0; index < left.elements.size(); ++index)
  {
    const auto& a = left.elements.at(index);
    const auto& b = right.elements.at(index);
    if (a.id != b.id || a.type != b.type || a.nodeIds != b.nodeIds
      || a.regionId != b.regionId || a.geometryId != b.geometryId)
    {
      return false;
    }
  }
  for (std::size_t index = 0; index < left.regions.size(); ++index)
  {
    const auto& a = left.regions.at(index);
    const auto& b = right.regions.at(index);
    if (a.id != b.id || a.sourceGeometryIds != b.sourceGeometryIds)
    {
      return false;
    }
  }
  return true;
}
} // namespace

int main()
{
  using namespace cceda;

  const domain::MeshModel workerMesh = makeWorkerMesh();
  const meshing::MeshBinaryWriteResult encoded = meshing::writeMeshBinary(
    workerMesh, domain::MeshValidationMode::WorkerArtifact);
  expect(encoded.succeeded(), "a valid Worker mesh encodes");
  if (encoded.succeeded())
  {
    expect(encoded.bytes.size() >= meshing::MeshBinaryHeaderBytes,
      "encoded mesh contains the fixed header");

    const meshing::MeshBinaryReadResult decoded = meshing::readMeshBinary(
      encoded.bytes, domain::MeshValidationMode::WorkerArtifact);
    expect(decoded.succeeded(), "an encoded Worker mesh decodes");
    expect(decoded.mesh && sameMesh(workerMesh, *decoded.mesh),
      "Worker mesh round-trip preserves every domain field");

    const auto encodedAgain = meshing::writeMeshBinary(
      workerMesh, domain::MeshValidationMode::WorkerArtifact);
    expect(encodedAgain.bytes == encoded.bytes,
      "encoding the same mesh produces deterministic bytes");

    auto unsupportedVersion = encoded.bytes;
    unsupportedVersion.at(8) = 1;
    expect(meshing::readMeshBinary(unsupportedVersion,
        domain::MeshValidationMode::WorkerArtifact).error
        == meshing::MeshBinaryErrorCode::UnsupportedVersion,
      "an unknown format major is rejected before block parsing");

    auto corruptBlock = encoded.bytes;
    corruptBlock.back() ^= 0x01;
    expect(meshing::readMeshBinary(corruptBlock,
        domain::MeshValidationMode::WorkerArtifact).error
        == meshing::MeshBinaryErrorCode::ChecksumMismatch,
      "block corruption is rejected by CRC-32");

    meshing::MeshBinaryLimits smallFileLimit;
    smallFileLimit.maximumFileBytes = encoded.bytes.size() - 1;
    expect(meshing::readMeshBinary(encoded.bytes,
        domain::MeshValidationMode::WorkerArtifact, smallFileLimit).error
        == meshing::MeshBinaryErrorCode::ResourceLimitExceeded,
      "the read file-byte budget is enforced before parsing blocks");

    meshing::MeshBinaryLimits smallNodeLimit;
    smallNodeLimit.maximumNodes = workerMesh.nodes.size() - 1;
    expect(meshing::readMeshBinary(encoded.bytes,
        domain::MeshValidationMode::WorkerArtifact, smallNodeLimit).error
        == meshing::MeshBinaryErrorCode::ResourceLimitExceeded,
      "declared node counts are checked against limits before allocation");

    auto invalidAssociation = encoded.bytes;
    const std::size_t triangleEntry = directoryEntry(invalidAssociation, 5);
    const auto triangleOffset = static_cast<std::size_t>(
      readU64(invalidAssociation, triangleEntry + 8));
    writeU32(invalidAssociation, triangleOffset + 40, 0);
    refreshBlockCrc(invalidAssociation, triangleEntry);
    expect(meshing::readMeshBinary(invalidAssociation,
        domain::MeshValidationMode::WorkerArtifact).error
        == meshing::MeshBinaryErrorCode::InvalidBlock,
      "an invalid association index is rejected even with a valid block CRC");

    auto compressedBlock = encoded.bytes;
    const std::size_t metadataEntry = directoryEntry(compressedBlock, 1);
    writeU32(compressedBlock, metadataEntry + 4, 0x101);
    expect(meshing::readMeshBinary(compressedBlock,
        domain::MeshValidationMode::WorkerArtifact).error
        == meshing::MeshBinaryErrorCode::UnsupportedFeature,
      "an unimplemented block compression algorithm is rejected");

    const auto newerMinor = withUnknownOptionalBlock(encoded.bytes);
    const auto newerMinorResult = meshing::readMeshBinary(newerMinor,
      domain::MeshValidationMode::WorkerArtifact);
    expect(newerMinorResult.mesh && sameMesh(workerMesh, *newerMinorResult.mesh),
      "a newer minor unknown optional block is checksum-validated and skipped");

    auto unknownRequired = newerMinor;
    const std::size_t unknownEntry = directoryEntry(unknownRequired, 8);
    writeU32(unknownRequired, unknownEntry + 4, 1);
    expect(meshing::readMeshBinary(unknownRequired,
        domain::MeshValidationMode::WorkerArtifact).error
        == meshing::MeshBinaryErrorCode::UnsupportedFeature,
      "an unknown required block is rejected");
  }

  domain::MeshModel committedMesh = workerMesh;
  committedMesh.meshRevision = 8;
  const auto committedBytes = meshing::writeMeshBinary(
    committedMesh, domain::MeshValidationMode::CommittedMesh);
  expect(committedBytes.succeeded(), "a committed mesh encodes");
  expect(meshing::readMeshBinary(committedBytes.bytes,
      domain::MeshValidationMode::CommittedMesh).succeeded(),
    "a committed mesh decodes in committed mode");
  expect(meshing::readMeshBinary(committedBytes.bytes,
      domain::MeshValidationMode::WorkerArtifact).error
      == meshing::MeshBinaryErrorCode::InvalidMesh,
    "a committed mesh is rejected as a Worker artifact");

  domain::MeshModel mixedElementOrder = workerMesh;
  std::swap(mixedElementOrder.elements.at(0), mixedElementOrder.elements.at(1));
  const auto canonicalBytes = meshing::writeMeshBinary(
    mixedElementOrder, domain::MeshValidationMode::WorkerArtifact);
  const auto canonicalMesh = meshing::readMeshBinary(
    canonicalBytes.bytes, domain::MeshValidationMode::WorkerArtifact);
  expect(canonicalMesh.mesh && canonicalMesh.mesh->elements.at(0).id == 101
      && canonicalMesh.mesh->elements.at(1).id == 102,
    "decoded elements use canonical Triangle3-then-Tetrahedron4 order");

  domain::MeshModel invalidUtf8 = workerMesh;
  invalidUtf8.backendId = std::string("\xc0\x80", 2);
  expect(meshing::writeMeshBinary(invalidUtf8,
      domain::MeshValidationMode::WorkerArtifact).error
      == meshing::MeshBinaryErrorCode::InvalidMesh,
    "the writer rejects non-canonical UTF-8 metadata");

  if (failures == 0)
  {
    std::cout << "Mesh binary codec tests passed.\n";
  }
  return failures == 0 ? 0 : 1;
}
