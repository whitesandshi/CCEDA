#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cceda::domain
{
inline constexpr std::size_t MaximumMeshExportPolicyIdLength = 64;

struct MeshExportFormatVersion
{
  std::uint16_t major = 0;
  std::uint16_t minor = 0;
};

enum class MeshExportEncoding : std::uint8_t
{
  Ascii,
  Binary,
};

struct MeshExportSettings
{
  std::optional<MeshExportFormatVersion> formatVersion;
  std::optional<MeshExportEncoding> encoding;
  std::string regionPolicyId;
};

[[nodiscard]] inline bool isValidMeshExportPolicyId(
  std::string_view policyId) noexcept
{
  if (policyId.empty() || policyId.size() > MaximumMeshExportPolicyIdLength)
  {
    return false;
  }

  bool separatorPending = false;
  for (std::size_t index = 0; index < policyId.size(); ++index)
  {
    const char character = policyId[index];
    const bool lowerAscii = character >= 'a' && character <= 'z';
    const bool digit = character >= '0' && character <= '9';
    if ((index == 0 && !lowerAscii)
      || (!lowerAscii && !digit && character != '-' && character != '.'))
    {
      return false;
    }
    if (lowerAscii || digit)
    {
      separatorPending = false;
      continue;
    }
    if (separatorPending)
    {
      return false;
    }
    separatorPending = true;
  }
  return !separatorPending;
}

[[nodiscard]] inline bool isValidMeshExportSettings(
  const MeshExportSettings& settings) noexcept
{
  return settings.formatVersion.has_value()
    && settings.formatVersion->major > 0
    && settings.encoding.has_value()
    && (*settings.encoding == MeshExportEncoding::Ascii
      || *settings.encoding == MeshExportEncoding::Binary)
    && isValidMeshExportPolicyId(settings.regionPolicyId);
}
}
