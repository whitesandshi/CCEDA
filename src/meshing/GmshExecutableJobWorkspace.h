#pragma once

#include "domain/Revisions.h"

#include <QString>

#include <cstdint>
#include <optional>

namespace cceda::meshing
{
struct GmshExecutableJobWorkspaceRequest
{
  QString workspace;
  QString sourceStep;
  QString jobId;
  domain::GeometryRevision sourceGeometryRevision = 0;
  double minimumSizeMm = 0.0;
  double maximumSizeMm = 0.0;
  std::uint64_t maximumGeometryBytes = 0;
};

struct PreparedGmshExecutableJob
{
  QString workspace;
  QString requestPath;
  QString geometryPath;
  QString geometrySha256;
  QString meshPath;
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

// Prepares the bounded protocol-0.1 workspace used by the current executable
// adapter. Final BREP, geometry-map, and meshbin formats remain deliberately
// outside this contract.
[[nodiscard]] GmshJobWorkspaceResult prepareGmshExecutableJobWorkspace(
  const GmshExecutableJobWorkspaceRequest& request);
}
