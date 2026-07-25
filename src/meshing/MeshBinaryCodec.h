#pragma once

#include "domain/MeshModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cceda::meshing
{
inline constexpr std::size_t MeshBinaryHeaderBytes = 128;
inline constexpr std::uint16_t MeshBinaryFormatMajor = 0;
inline constexpr std::uint16_t MeshBinaryFormatMinor = 1;

struct MeshBinaryLimits
{
  std::uint64_t maximumFileBytes = 512ULL * 1024ULL * 1024ULL;
  std::uint64_t maximumNodes = 10'000'000;
  std::uint64_t maximumTriangles = 20'000'000;
  std::uint64_t maximumTetrahedra = 20'000'000;
  std::uint64_t maximumRegions = 100'000;
  std::uint64_t maximumAssociations = 2'000'000;
  std::uint32_t maximumDirectoryEntries = 64;
  std::uint32_t maximumStringBytes = 4096;
};

enum class MeshBinaryErrorCode
{
  None,
  InvalidArgument,
  InvalidMesh,
  ResourceLimitExceeded,
  InvalidHeader,
  UnsupportedVersion,
  UnsupportedFeature,
  InvalidDirectory,
  ChecksumMismatch,
  InvalidBlock,
};

struct MeshBinaryWriteResult
{
  std::vector<std::uint8_t> bytes;
  MeshBinaryErrorCode error = MeshBinaryErrorCode::None;
  std::string detail;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return error == MeshBinaryErrorCode::None;
  }
};

struct MeshBinaryReadResult
{
  std::optional<domain::MeshModel> mesh;
  MeshBinaryErrorCode error = MeshBinaryErrorCode::None;
  std::string detail;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return error == MeshBinaryErrorCode::None && mesh.has_value();
  }
};

// Serializes one validated Worker or committed mesh into canonical format 0.1.
[[nodiscard]] MeshBinaryWriteResult writeMeshBinary(
  const domain::MeshModel& mesh,
  domain::MeshValidationMode validationMode,
  const MeshBinaryLimits& limits = {});

// Treats input as untrusted and validates all budgets before reconstruction.
[[nodiscard]] MeshBinaryReadResult readMeshBinary(
  const std::vector<std::uint8_t>& bytes,
  domain::MeshValidationMode validationMode,
  const MeshBinaryLimits& limits = {});
}
