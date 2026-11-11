#include "persistence/ProjectMeshQuality.h"

#include <utility>

namespace cceda::persistence
{
namespace
{
ProjectMeshQualityReadResult evaluate(const domain::MeshModel& mesh)
{
  auto evaluation = mesh_quality::evaluateMeshQuality(
    mesh, domain::MeshValidationMode::CommittedMesh);
  if (!evaluation.succeeded())
  {
    return {{}, ProjectMeshQualityError::InvalidMesh};
  }
  if (!evaluation.report->validForCommit())
  {
    return {{}, ProjectMeshQualityError::InvalidQuality};
  }
  return {std::move(evaluation.report), ProjectMeshQualityError::None};
}
}

ProjectMeshQualityWriteResult writeProjectMeshQuality(
  const domain::MeshModel& committedMesh,
  const meshing::MeshQualityJsonLimits& limits)
{
  const auto evaluated = evaluate(committedMesh);
  if (!evaluated.succeeded())
  {
    return {{}, evaluated.error};
  }
  auto encoded = meshing::writeCommittedMeshQualityJson(*evaluated.report, limits);
  if (!encoded.succeeded())
  {
    return {{}, ProjectMeshQualityError::EncodingFailed};
  }
  return {std::move(encoded.bytes), ProjectMeshQualityError::None};
}

ProjectMeshQualityReadResult readProjectMeshQuality(
  const QByteArray& bytes,
  const domain::MeshModel& committedMesh,
  const meshing::MeshQualityJsonLimits& limits)
{
  if (static_cast<std::uint64_t>(bytes.size()) > limits.maximumInputBytes)
  {
    return {{}, ProjectMeshQualityError::InputTooLarge};
  }
  auto evaluated = evaluate(committedMesh);
  if (!evaluated.succeeded())
  {
    return evaluated;
  }
  const auto expected = meshing::writeCommittedMeshQualityJson(*evaluated.report, limits);
  if (!expected.succeeded())
  {
    return {{}, ProjectMeshQualityError::EncodingFailed};
  }
  if (bytes != expected.bytes)
  {
    return {{}, ProjectMeshQualityError::ContentMismatch};
  }
  return evaluated;
}
}
