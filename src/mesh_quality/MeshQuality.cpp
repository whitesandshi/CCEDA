#include "mesh_quality/MeshQuality.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cceda::mesh_quality
{
namespace
{
constexpr double Pi = 3.141592653589793238462643383279502884;
constexpr double RadiansToDegrees = 180.0 / Pi;

struct Vector3
{
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

Vector3 operator-(const Vector3& left, const Vector3& right) noexcept
{
  return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vector3 operator-(const Vector3& value) noexcept
{
  return {-value.x, -value.y, -value.z};
}

double dot(const Vector3& left, const Vector3& right) noexcept
{
  return left.x * right.x + left.y * right.y + left.z * right.z;
}

Vector3 cross(const Vector3& left, const Vector3& right) noexcept
{
  return {
    left.y * right.z - left.z * right.y,
    left.z * right.x - left.x * right.z,
    left.x * right.y - left.y * right.x,
  };
}

double length(const Vector3& value) noexcept
{
  return std::sqrt(dot(value, value));
}

std::optional<double> angleDegrees(
  const Vector3& first,
  const Vector3& second) noexcept
{
  const double denominator = length(first) * length(second);
  if (!(denominator > 0.0) || !std::isfinite(denominator))
  {
    return std::nullopt;
  }
  const double cosine = dot(first, second) / denominator;
  if (!std::isfinite(cosine))
  {
    return std::nullopt;
  }
  return std::acos(std::clamp(cosine, -1.0, 1.0)) * RadiansToDegrees;
}

struct EdgeKey
{
  domain::MeshNodeId first = 0;
  domain::MeshNodeId second = 0;

  bool operator==(const EdgeKey& other) const noexcept
  {
    return first == other.first && second == other.second;
  }
};

struct EdgeKeyHash
{
  std::size_t operator()(const EdgeKey& edge) const noexcept
  {
    const std::size_t firstHash =
      std::hash<domain::MeshNodeId>{}(edge.first);
    const std::size_t secondHash =
      std::hash<domain::MeshNodeId>{}(edge.second);
    return firstHash ^ (secondHash + 0x9e3779b9U
      + (firstHash << 6U) + (firstHash >> 2U));
  }
};

struct EdgeUse
{
  domain::MeshElementId elementId = 0;
  std::size_t triangleIndex = 0;
  bool forward = false;
};

struct EdgeRecord
{
  std::array<EdgeUse, 2> firstUses{};
  std::size_t useCount = 0;
  std::vector<EdgeUse> additionalUses;
};

using NodeLookup =
  std::unordered_map<domain::MeshNodeId, Vector3>;
using EdgeUses =
  std::unordered_map<EdgeKey, EdgeRecord, EdgeKeyHash>;

void addIssue(
  MeshQualityReport& report,
  MeshQualityIssueCode code,
  domain::MeshElementId elementId)
{
  report.issues.push_back({code, elementId});
}

void addTriangleEdge(
  EdgeUses& edges,
  domain::MeshNodeId from,
  domain::MeshNodeId to,
  domain::MeshElementId elementId,
  std::size_t triangleIndex)
{
  const EdgeKey key{std::min(from, to), std::max(from, to)};
  EdgeRecord& record = edges[key];
  const EdgeUse use{elementId, triangleIndex, from == key.first};
  if (record.useCount < record.firstUses.size())
  {
    record.firstUses.at(record.useCount) = use;
  }
  else
  {
    record.additionalUses.push_back(use);
  }
  ++record.useCount;
}

TriangleQuality evaluateTriangle(
  const domain::MeshElement& element,
  const NodeLookup& nodes,
  MeshQualityReport& report,
  EdgeUses& edges)
{
  const Vector3& a = nodes.at(element.nodeIds.at(0));
  const Vector3& b = nodes.at(element.nodeIds.at(1));
  const Vector3& c = nodes.at(element.nodeIds.at(2));
  const Vector3 ab = b - a;
  const Vector3 ac = c - a;
  const Vector3 bc = c - b;
  const double abLength = length(ab);
  const double acLength = length(ac);
  const double bcLength = length(bc);
  const double area = 0.5 * length(cross(ab, ac));

  TriangleQuality quality;
  quality.elementId = element.id;
  quality.areaSquareMillimeters = area;
  const std::size_t triangleIndex = report.triangles.size();
  addTriangleEdge(edges, element.nodeIds.at(0), element.nodeIds.at(1),
    element.id, triangleIndex);
  addTriangleEdge(edges, element.nodeIds.at(1), element.nodeIds.at(2),
    element.id, triangleIndex);
  addTriangleEdge(edges, element.nodeIds.at(2), element.nodeIds.at(0),
    element.id, triangleIndex);

  if (!std::isfinite(area) || !std::isfinite(abLength)
    || !std::isfinite(acLength) || !std::isfinite(bcLength))
  {
    addIssue(report, MeshQualityIssueCode::NonFiniteMetric, element.id);
    return quality;
  }
  if (area == 0.0)
  {
    addIssue(report, MeshQualityIssueCode::DegenerateTriangle, element.id);
    return quality;
  }

  const double aspectRatio =
    (abLength * abLength + acLength * acLength + bcLength * bcLength)
    / (4.0 * std::sqrt(3.0) * area);
  const std::array<std::optional<double>, 3> angles{
    angleDegrees(ab, ac),
    angleDegrees(-ab, bc),
    angleDegrees(-ac, -bc),
  };
  if (!std::isfinite(aspectRatio)
    || !angles.at(0) || !angles.at(1) || !angles.at(2))
  {
    addIssue(report, MeshQualityIssueCode::NonFiniteMetric, element.id);
    return quality;
  }

  quality.aspectRatio = aspectRatio;
  quality.minimumAngleDegrees = std::min({
    *angles.at(0), *angles.at(1), *angles.at(2)});
  quality.maximumAngleDegrees = std::max({
    *angles.at(0), *angles.at(1), *angles.at(2)});
  return quality;
}

std::optional<double> minimumDihedralAngleDegrees(
  const std::array<Vector3, 4>& points) noexcept
{
  constexpr std::array<std::array<std::size_t, 4>, 4> faces{{
    {{0, 1, 2, 3}},
    {{0, 3, 1, 2}},
    {{0, 2, 3, 1}},
    {{1, 3, 2, 0}},
  }};
  std::array<Vector3, 4> normals;
  std::array<double, 4> normalLengths{};
  for (std::size_t index = 0; index < faces.size(); ++index)
  {
    const auto& face = faces.at(index);
    Vector3 normal = cross(
      points.at(face.at(1)) - points.at(face.at(0)),
      points.at(face.at(2)) - points.at(face.at(0)));
    if (dot(normal,
        points.at(face.at(3)) - points.at(face.at(0))) > 0.0)
    {
      normal = -normal;
    }
    normals.at(index) = normal;
    normalLengths.at(index) = length(normal);
    if (!(normalLengths.at(index) > 0.0)
      || !std::isfinite(normalLengths.at(index)))
    {
      return std::nullopt;
    }
  }

  double minimum = 180.0;
  for (std::size_t first = 0; first < normals.size(); ++first)
  {
    for (std::size_t second = first + 1; second < normals.size(); ++second)
    {
      const double cosine = dot(normals.at(first), normals.at(second))
        / (normalLengths.at(first) * normalLengths.at(second));
      if (!std::isfinite(cosine))
      {
        return std::nullopt;
      }
      const double angle = (Pi - std::acos(std::clamp(cosine, -1.0, 1.0)))
        * RadiansToDegrees;
      minimum = std::min(minimum, angle);
    }
  }
  return minimum;
}

std::optional<double> scaledJacobian(
  const std::array<Vector3, 4>& points) noexcept
{
  constexpr std::array<std::array<std::size_t, 3>, 4> cornerOrder{{
    {{1, 2, 3}},
    {{0, 3, 2}},
    {{0, 1, 3}},
    {{0, 2, 1}},
  }};
  double minimum = 1.0;
  for (std::size_t corner = 0; corner < cornerOrder.size(); ++corner)
  {
    const auto& order = cornerOrder.at(corner);
    const Vector3 first = points.at(order.at(0)) - points.at(corner);
    const Vector3 second = points.at(order.at(1)) - points.at(corner);
    const Vector3 third = points.at(order.at(2)) - points.at(corner);
    const double denominator =
      length(first) * length(second) * length(third);
    if (!(denominator > 0.0) || !std::isfinite(denominator))
    {
      return std::nullopt;
    }
    const double value =
      std::sqrt(2.0) * dot(first, cross(second, third)) / denominator;
    if (!std::isfinite(value))
    {
      return std::nullopt;
    }
    minimum = std::min(minimum, std::clamp(value, -1.0, 1.0));
  }
  return minimum;
}

TetrahedronQuality evaluateTetrahedron(
  const domain::MeshElement& element,
  const NodeLookup& nodes,
  MeshQualityReport& report)
{
  std::array<Vector3, 4> points{
    nodes.at(element.nodeIds.at(0)),
    nodes.at(element.nodeIds.at(1)),
    nodes.at(element.nodeIds.at(2)),
    nodes.at(element.nodeIds.at(3)),
  };
  const double determinant = dot(
    points.at(1) - points.at(0),
    cross(points.at(2) - points.at(0), points.at(3) - points.at(0)));

  TetrahedronQuality quality;
  quality.elementId = element.id;
  quality.signedVolumeCubicMillimeters = determinant / 6.0;
  if (!std::isfinite(determinant)
    || !std::isfinite(quality.signedVolumeCubicMillimeters))
  {
    addIssue(report, MeshQualityIssueCode::NonFiniteMetric, element.id);
    return quality;
  }
  if (determinant == 0.0)
  {
    addIssue(report, MeshQualityIssueCode::DegenerateTetrahedron,
      element.id);
    return quality;
  }
  if (determinant < 0.0)
  {
    addIssue(report, MeshQualityIssueCode::InvertedTetrahedron,
      element.id);
  }

  double longestEdge = 0.0;
  for (std::size_t first = 0; first < points.size(); ++first)
  {
    for (std::size_t second = first + 1; second < points.size(); ++second)
    {
      longestEdge = std::max(longestEdge,
        length(points.at(second) - points.at(first)));
    }
  }

  constexpr std::array<std::array<std::size_t, 3>, 4> faceVertices{{
    {{0, 1, 2}},
    {{0, 1, 3}},
    {{0, 2, 3}},
    {{1, 2, 3}},
  }};
  double totalFaceArea = 0.0;
  for (const auto& face : faceVertices)
  {
    totalFaceArea += 0.5 * length(cross(
      points.at(face.at(1)) - points.at(face.at(0)),
      points.at(face.at(2)) - points.at(face.at(0))));
  }
  const double inradius =
    3.0 * std::abs(quality.signedVolumeCubicMillimeters) / totalFaceArea;
  const double aspectRatio =
    longestEdge / (2.0 * std::sqrt(6.0) * inradius);
  const std::optional<double> dihedral =
    minimumDihedralAngleDegrees(points);
  const std::optional<double> jacobian = scaledJacobian(points);
  if (!std::isfinite(longestEdge) || !std::isfinite(totalFaceArea)
    || !std::isfinite(inradius) || !std::isfinite(aspectRatio)
    || !dihedral || !jacobian)
  {
    addIssue(report, MeshQualityIssueCode::NonFiniteMetric, element.id);
    return quality;
  }

  quality.aspectRatio = aspectRatio;
  quality.minimumDihedralAngleDegrees = dihedral;
  quality.scaledJacobian = jacobian;
  return quality;
}

void evaluateTriangleOrientation(
  EdgeUses& edges,
  MeshQualityReport& report)
{
  const auto markIssue = [&report](
                           const EdgeUse& use,
                           MeshQualityIssueCode code) {
    report.triangles.at(use.triangleIndex).normalConsistent = false;
    addIssue(report, code, use.elementId);
  };

  for (const auto& entry : edges)
  {
    const EdgeRecord& record = entry.second;
    if (record.useCount == 2
      && record.firstUses.at(0).forward == record.firstUses.at(1).forward)
    {
      markIssue(record.firstUses.at(0),
        MeshQualityIssueCode::InconsistentTriangleOrientation);
      markIssue(record.firstUses.at(1),
        MeshQualityIssueCode::InconsistentTriangleOrientation);
    }
    else if (record.useCount > 2)
    {
      markIssue(record.firstUses.at(0),
        MeshQualityIssueCode::NonManifoldSurfaceEdge);
      markIssue(record.firstUses.at(1),
        MeshQualityIssueCode::NonManifoldSurfaceEdge);
      for (const EdgeUse& use : record.additionalUses)
      {
        markIssue(use, MeshQualityIssueCode::NonManifoldSurfaceEdge);
      }
    }
  }
}

void sortAndDeduplicateIssues(MeshQualityReport& report)
{
  std::sort(report.issues.begin(), report.issues.end(),
    [](const MeshQualityIssue& left, const MeshQualityIssue& right) {
      if (left.elementId != right.elementId)
      {
        return left.elementId < right.elementId;
      }
      return static_cast<int>(left.code) < static_cast<int>(right.code);
    });
  report.issues.erase(
    std::unique(report.issues.begin(), report.issues.end(),
      [](const MeshQualityIssue& left, const MeshQualityIssue& right) {
        return left.elementId == right.elementId && left.code == right.code;
      }),
    report.issues.end());
}
}

std::string_view stableMetricId(MeshQualityMetric metric) noexcept
{
  switch (metric)
  {
    case MeshQualityMetric::TriangleArea:
      return "triangle.area.mm2";
    case MeshQualityMetric::TriangleAspectRatio:
      return "triangle.aspect-ratio";
    case MeshQualityMetric::TriangleMinimumAngle:
      return "triangle.minimum-angle.deg";
    case MeshQualityMetric::TriangleMaximumAngle:
      return "triangle.maximum-angle.deg";
    case MeshQualityMetric::TriangleNormalConsistent:
      return "triangle.normal-consistent";
    case MeshQualityMetric::TetrahedronSignedVolume:
      return "tetrahedron.signed-volume.mm3";
    case MeshQualityMetric::TetrahedronAspectRatio:
      return "tetrahedron.aspect-ratio";
    case MeshQualityMetric::TetrahedronMinimumDihedralAngle:
      return "tetrahedron.minimum-dihedral-angle.deg";
    case MeshQualityMetric::TetrahedronScaledJacobian:
      return "tetrahedron.scaled-jacobian";
  }
  return {};
}

MeshQualityDirection metricDirection(MeshQualityMetric metric) noexcept
{
  switch (metric)
  {
    case MeshQualityMetric::TriangleArea:
    case MeshQualityMetric::TriangleMinimumAngle:
    case MeshQualityMetric::TetrahedronMinimumDihedralAngle:
    case MeshQualityMetric::TetrahedronScaledJacobian:
      return MeshQualityDirection::HigherIsBetter;
    case MeshQualityMetric::TriangleAspectRatio:
    case MeshQualityMetric::TriangleMaximumAngle:
    case MeshQualityMetric::TetrahedronAspectRatio:
      return MeshQualityDirection::LowerIsBetter;
    case MeshQualityMetric::TetrahedronSignedVolume:
      return MeshQualityDirection::PositiveRequired;
    case MeshQualityMetric::TriangleNormalConsistent:
      return MeshQualityDirection::TrueRequired;
  }
  return MeshQualityDirection::HigherIsBetter;
}

std::string_view stableIssueId(MeshQualityIssueCode code) noexcept
{
  switch (code)
  {
    case MeshQualityIssueCode::DegenerateTriangle:
      return "degenerate-triangle";
    case MeshQualityIssueCode::InconsistentTriangleOrientation:
      return "inconsistent-triangle-orientation";
    case MeshQualityIssueCode::NonManifoldSurfaceEdge:
      return "non-manifold-surface-edge";
    case MeshQualityIssueCode::DegenerateTetrahedron:
      return "degenerate-tetrahedron";
    case MeshQualityIssueCode::InvertedTetrahedron:
      return "inverted-tetrahedron";
    case MeshQualityIssueCode::NonFiniteMetric:
      return "non-finite-metric";
  }
  return {};
}

bool isCommitBlockingIssue(MeshQualityIssueCode code) noexcept
{
  return code != MeshQualityIssueCode::InconsistentTriangleOrientation;
}

bool MeshQualityReport::validForCommit() const noexcept
{
  return std::none_of(issues.begin(), issues.end(),
    [](const MeshQualityIssue& issue) {
      return isCommitBlockingIssue(issue.code);
    });
}

MeshQualityEvaluationResult evaluateMeshQuality(
  const domain::MeshModel& mesh,
  domain::MeshValidationMode validationMode)
{
  domain::MeshValidationReport validation =
    domain::validateMeshModel(mesh, validationMode);
  if (!validation.valid())
  {
    return {
      std::nullopt,
      MeshQualityEvaluationCode::InvalidMesh,
      std::move(validation.issues),
    };
  }

  MeshQualityReport report;
  report.meshRevision = mesh.meshRevision;
  report.sourceGeometryRevision = mesh.sourceGeometryRevision;
  report.triangles.reserve(
    static_cast<std::size_t>(validation.triangleCount));
  report.tetrahedra.reserve(
    static_cast<std::size_t>(validation.tetrahedronCount));

  NodeLookup nodes;
  nodes.reserve(mesh.nodes.size());
  for (const domain::MeshNode& node : mesh.nodes)
  {
    nodes.emplace(node.id, Vector3{node.x, node.y, node.z});
  }

  EdgeUses edges;
  edges.reserve(static_cast<std::size_t>(validation.triangleCount));
  for (const domain::MeshElement& element : mesh.elements)
  {
    if (element.type == domain::MeshElementType::Triangle3)
    {
      report.triangles.push_back(
        evaluateTriangle(element, nodes, report, edges));
    }
    else
    {
      report.tetrahedra.push_back(
        evaluateTetrahedron(element, nodes, report));
    }
  }

  evaluateTriangleOrientation(edges, report);
  sortAndDeduplicateIssues(report);
  return {
    std::move(report),
    MeshQualityEvaluationCode::None,
    {},
  };
}

std::optional<MeshQualityIssueDetails> buildMeshQualityIssueDetails(
  const domain::MeshModel& mesh,
  const MeshQualityReport& report)
{
  if (report.meshRevision != mesh.meshRevision
    || report.sourceGeometryRevision != mesh.sourceGeometryRevision)
  {
    return std::nullopt;
  }

  std::unordered_set<domain::MeshElementId> requested;
  requested.reserve(report.issues.size());
  for (const MeshQualityIssue& issue : report.issues)
  {
    if (issue.elementId == 0)
    {
      return std::nullopt;
    }
    requested.insert(issue.elementId);
  }
  MeshQualityIssueDetails details;
  details.reserve(requested.size());
  if (requested.empty())
  {
    return details;
  }

  const auto makeDetail = [](const domain::MeshElement& element) {
    MeshQualityElementDetail detail;
    detail.elementId = element.id;
    detail.elementType = element.type;
    detail.nodeIds = element.nodeIds;
    detail.regionId = element.regionId;
    detail.geometryId = element.geometryId;
    return detail;
  };

  std::size_t triangleIndex = 0;
  std::size_t tetrahedronIndex = 0;
  for (const domain::MeshElement& element : mesh.elements)
  {
    if (element.type == domain::MeshElementType::Triangle3)
    {
      if (triangleIndex >= report.triangles.size()
        || report.triangles.at(triangleIndex).elementId != element.id)
      {
        return std::nullopt;
      }
      if (requested.find(element.id) != requested.end())
      {
        MeshQualityElementDetail detail = makeDetail(element);
        detail.nodeCount = 3;
        detail.triangleQuality = report.triangles.at(triangleIndex);
        details.emplace(element.id, std::move(detail));
      }
      ++triangleIndex;
    }
    else
    {
      if (tetrahedronIndex >= report.tetrahedra.size()
        || report.tetrahedra.at(tetrahedronIndex).elementId != element.id)
      {
        return std::nullopt;
      }
      if (requested.find(element.id) != requested.end())
      {
        MeshQualityElementDetail detail = makeDetail(element);
        detail.nodeCount = 4;
        detail.tetrahedronQuality = report.tetrahedra.at(tetrahedronIndex);
        details.emplace(element.id, std::move(detail));
      }
      ++tetrahedronIndex;
    }
  }

  if (triangleIndex != report.triangles.size()
    || tetrahedronIndex != report.tetrahedra.size()
    || details.size() != requested.size())
  {
    return std::nullopt;
  }
  return details;
}
}
