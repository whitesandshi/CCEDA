#pragma once

#include "domain/MeshGenerationSettings.h"
#include "domain/MeshingResourceLimits.h"
#include "domain/MeshingRetryPolicy.h"

#include <QByteArray>
#include <optional>

namespace cceda::persistence
{
// Logical contents of mesh/settings.json in a future project package.
struct StoredMeshSettings
{
  domain::MeshGenerationMode mode = domain::MeshGenerationMode::IndependentSolids;
  domain::MeshGenerationSettings generation;
  domain::MeshingResourceLimits resourceLimits;
  domain::MeshingRetryPolicy retry;
};

inline constexpr qsizetype MaximumMeshSettingsJsonBytes = 16 * 1024;

enum class MeshSettingsJsonError
{
  None,
  InputTooLarge,
  InvalidJson,
  InvalidStructure,
  UnsupportedSchema,
  InvalidSettings,
};

struct MeshSettingsReadResult
{
  std::optional<StoredMeshSettings> settings;
  MeshSettingsJsonError error = MeshSettingsJsonError::None;
  [[nodiscard]] bool succeeded() const noexcept
  {
    return settings.has_value() && error == MeshSettingsJsonError::None;
  }
};

struct MeshSettingsWriteResult
{
  QByteArray bytes;
  MeshSettingsJsonError error = MeshSettingsJsonError::None;
  [[nodiscard]] bool succeeded() const noexcept
  {
    return !bytes.isEmpty() && error == MeshSettingsJsonError::None;
  }
};

[[nodiscard]] MeshSettingsWriteResult writeMeshSettingsJson(const StoredMeshSettings& settings);
[[nodiscard]] MeshSettingsReadResult readMeshSettingsJson(const QByteArray& bytes);
}
