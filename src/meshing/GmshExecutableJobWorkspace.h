#pragma once

#include "domain/Revisions.h"
#include "domain/MeshGenerationSettings.h"
#include "domain/MeshingResourceLimits.h"

#include <QString>

#include <cstdint>
#include <optional>

namespace cceda::meshing
{
struct GmshSnapshotJobWorkspaceRequest
{
  QString workspace;
  QString sourceBrep;
  QString sourceGeometryMap;
  QString expectedGeometrySha256;
  QString expectedGeometryMapSha256;
  QString jobId;
  domain::GeometryRevision sourceGeometryRevision = 0;
  domain::MeshGenerationSettings settings;
  domain::MeshingResourceLimits resourceLimits;
  std::uint64_t maximumSnapshotBytes = 0;
  domain::MeshGenerationMode mode = domain::MeshGenerationMode::IndependentSolids;
};

struct PreparedGmshExecutableJob
{
  QString workspace;
  QString requestPath;
  QString geometryPath;
  QString geometrySha256;
  QString geometryMapPath;
  QString geometryMapSha256;
  QString meshPath;
  QString meshBinaryPath;
  QString geometryMapResultPath;
  QString qualityPath;
  QString resultPath;
};

enum class GmshJobWorkspaceErrorCode
{
  None,
  InvalidRequest,
  WorkspaceNotEmpty,
  GeometryTooLarge,
  DirectoryCreationFailed,
  GeometryReadFailed,
  GeometryWriteFailed,
  GeometryMapReadFailed,
  GeometryMapWriteFailed,
  SnapshotHashMismatch,
  GeometryMapInvalid,
  RequestWriteFailed,
};

struct GmshJobWorkspaceResult
{
  std::optional<PreparedGmshExecutableJob> job;
  GmshJobWorkspaceErrorCode error = GmshJobWorkspaceErrorCode::None;
  QString detail;

  [[nodiscard]] bool succeeded() const noexcept
  {
    return error == GmshJobWorkspaceErrorCode::None && job.has_value();
  }
};

// Copies and seals an immutable protocol-0.1 BREP snapshot. The expected
// digests bind the job to the artifacts produced for one geometry revision.
[[nodiscard]] GmshJobWorkspaceResult prepareGmshSnapshotJobWorkspace(
  const GmshSnapshotJobWorkspaceRequest& request);
}
