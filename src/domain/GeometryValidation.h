#pragma once

#include "domain/Revisions.h"

#include <optional>
#include <string>
#include <vector>

namespace cceda::domain
{
enum class GeometryIssueCategory
{
  Validity,
};

enum class GeometryIssueSeverity
{
  Info,
  Warning,
  Error,
  Fatal,
};

enum class GeometryIssueRepairability
{
  None,
  SafeAutomatic,
  NeedsConfirmation,
};

struct GeometryIssue
{
  std::string id;
  std::string code;
  GeometryIssueCategory category = GeometryIssueCategory::Validity;
  GeometryIssueSeverity severity = GeometryIssueSeverity::Error;
  std::string summaryKey;
  GeometryIssueRepairability repairability = GeometryIssueRepairability::None;
};

enum class SolidBrepCheckStatus
{
  CheckUnavailable,
  InvalidRequest,
  RevisionMismatch,
  TargetNotFound,
  UnsupportedTarget,
  Valid,
  Invalid,
};

struct SolidBrepCheckResult
{
  GeometryRevision revision = 0;
  std::string targetId;
  SolidBrepCheckStatus status = SolidBrepCheckStatus::CheckUnavailable;
  std::optional<GeometryIssue> issue;
};

enum class GeometryValidationReportStatus
{
  CheckUnavailable,
  InvalidRequest,
  RevisionMismatch,
  Completed,
};

struct GeometryValidationReport
{
  GeometryRevision revision = 0;
  GeometryValidationReportStatus status = GeometryValidationReportStatus::CheckUnavailable;
  std::vector<SolidBrepCheckResult> solidChecks;
};
}
