#pragma once

#include "domain/Revisions.h"

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
  double minimumSizeMm = 0.0;
  double maximumSizeMm = 0.0;
  std::uint64_t maximumSnapshotBytes = 0;
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
