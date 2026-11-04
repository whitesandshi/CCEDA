#include "meshing/MeshQualityJson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace
{
namespace meshing = cceda::meshing;
namespace quality = cceda::mesh_quality;

constexpr qint64 ReadChunkBytes = 64 * 1024;
constexpr std::uint64_t JsonSafeInteger = 9007199254740991ULL;
constexpr qsizetype MaximumJobIdCharacters = 128;

constexpr std::array<quality::MeshQualityMetric, 9> MetricOrder{
  quality::MeshQualityMetric::TriangleArea,
  quality::MeshQualityMetric::TriangleAspectRatio,
  quality::MeshQualityMetric::TriangleMinimumAngle,
  quality::MeshQualityMetric::TriangleMaximumAngle,
  quality::MeshQualityMetric::TriangleNormalConsistent,
  quality::MeshQualityMetric::TetrahedronSignedVolume,
  quality::MeshQualityMetric::TetrahedronAspectRatio,
  quality::MeshQualityMetric::TetrahedronMinimumDihedralAngle,
  quality::MeshQualityMetric::TetrahedronScaledJacobian,
};

meshing::MeshQualityJsonReadResult readFailure(
  meshing::MeshQualityJsonErrorCode error,
  QString detail)
{
  return {std::nullopt, error, std::move(detail)};
}

meshing::MeshQualityJsonWriteResult writeFailure(
  meshing::MeshQualityJsonErrorCode error,
  QString detail)
{
  return {{}, error, std::move(detail)};
}

bool validLimits(const meshing::MeshQualityJsonLimits& limits)
{
  return limits.maximumInputBytes > 0
    && limits.maximumIssues > 0
    && limits.maximumNestingDepth >= 4
    && limits.maximumInputBytes
      <= static_cast<std::uint64_t>(std::numeric_limits<qsizetype>::max());
}

bool hasExactlyKeys(const QJsonObject& object, const QSet<QString>& expected)
{
  if (object.size() != expected.size())
  {
    return false;
  }
  for (auto iterator = object.constBegin(); iterator != object.constEnd();
       ++iterator)
  {
    if (!expected.contains(iterator.key()))
    {
      return false;
    }
  }
  return true;
}

bool validJobId(const QString& jobId)
{
  if (jobId.isEmpty() || jobId.size() > MaximumJobIdCharacters)
  {
    return false;
  }
  const ushort first = jobId.front().unicode();
  if (!((first >= 'a' && first <= 'z')
      || (first >= 'A' && first <= 'Z')
      || (first >= '0' && first <= '9')))
  {
    return false;
  }
  return std::all_of(jobId.cbegin(), jobId.cend(), [](QChar character) {
    const ushort value = character.unicode();
    return (value >= 'a' && value <= 'z')
      || (value >= 'A' && value <= 'Z')
      || (value >= '0' && value <= '9')
      || value == '-' || value == '_' || value == '.';
  });
}

QString jsonMetricId(quality::MeshQualityMetric metric)
{
  const std::string_view id = quality::stableMetricId(metric);
  return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
}

QString jsonIssueId(quality::MeshQualityIssueCode code)
{
  const std::string_view id = quality::stableIssueId(code);
  return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
}

std::optional<quality::MeshQualityIssueCode> issueCode(const QString& id)
{
  using Code = quality::MeshQualityIssueCode;
  constexpr std::array<Code, 6> codes{
    Code::DegenerateTriangle,
    Code::InconsistentTriangleOrientation,
    Code::NonManifoldSurfaceEdge,
    Code::DegenerateTetrahedron,
    Code::InvertedTetrahedron,
    Code::NonFiniteMetric,
  };
  for (const Code code : codes)
  {
    if (jsonIssueId(code) == id)
    {
      return code;
    }
  }
  return std::nullopt;
}

std::optional<std::uint64_t> unsignedInteger(const QJsonValue& value)
{
  if (!value.isDouble())
  {
    return std::nullopt;
  }
  const double number = value.toDouble(
    std::numeric_limits<double>::quiet_NaN());
  if (!std::isfinite(number) || number < 0.0
      || number > static_cast<double>(JsonSafeInteger)
      || std::floor(number) != number)
  {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(number);
}

QJsonValue jsonInteger(std::uint64_t value)
{
  return QJsonValue(static_cast<qint64>(value));
}

bool issueLess(
  const quality::MeshQualityIssue& left,
  const quality::MeshQualityIssue& right)
{
  if (left.elementId != right.elementId)
  {
    return left.elementId < right.elementId;
  }
  return static_cast<int>(left.code) < static_cast<int>(right.code);
}

struct NumericAccumulator
{
  std::uint64_t count = 0;
  double minimum = 0.0;
  double maximum = 0.0;
  double average = 0.0;

  bool add(double value)
  {
    if (!std::isfinite(value))
    {
      return false;
    }
    if (count == 0)
    {
      minimum = value;
      maximum = value;
    }
    else
    {
      minimum = std::min(minimum, value);
      maximum = std::max(maximum, value);
    }
    ++count;
    const double nextCount = static_cast<double>(count);
    const double previousWeight =
      static_cast<double>(count - 1) / nextCount;
    average = average * previousWeight + value / nextCount;
    average = std::clamp(average, minimum, maximum);
    return true;
  }

  double mean() const
  {
    return average;
  }
};

QJsonObject numericSummary(
  quality::MeshQualityMetric metric,
  const NumericAccumulator& values)
{
  QJsonObject summary{
    {"metricId", jsonMetricId(metric)},
    {"kind", "numeric"},
    {"sampleCount", jsonInteger(values.count)},
  };
  if (values.count > 0)
  {
    summary.insert("minimum", values.minimum);
    summary.insert("maximum", values.maximum);
    summary.insert("mean", values.mean());
  }
  return summary;
}

std::optional<QByteArray> readBounded(
  QIODevice& input,
  std::uint64_t maximumBytes,
  meshing::MeshQualityJsonReadResult& error)
{
  QByteArray bytes;
  while (!input.atEnd())
  {
    const std::uint64_t remaining =
      maximumBytes - static_cast<std::uint64_t>(bytes.size());
    const qint64 requested = static_cast<qint64>(
      std::min<std::uint64_t>(ReadChunkBytes, remaining + 1));
    const QByteArray chunk = input.read(requested);
    if (chunk.isEmpty() && !input.atEnd())
    {
      error = readFailure(meshing::MeshQualityJsonErrorCode::ReadFailed,
        "Cannot read mesh quality JSON input");
      return std::nullopt;
    }
    bytes.append(chunk);
    if (static_cast<std::uint64_t>(bytes.size()) > maximumBytes)
    {
      error = readFailure(
        meshing::MeshQualityJsonErrorCode::InputLimitExceeded,
        "Mesh quality JSON exceeds the byte budget");
      return std::nullopt;
    }
  }
  return bytes;
}
bool withinNestingDepth(
  const QByteArray& bytes,
  std::uint64_t maximumDepth)
{
  std::uint64_t depth = 0;
  bool insideString = false;
  bool escaped = false;
  for (const char byte : bytes)
  {
    if (insideString)
    {
      if (escaped)
      {
        escaped = false;
      }
      else if (byte == '\\')
      {
        escaped = true;
      }
      else if (byte == '"')
      {
        insideString = false;
      }
      continue;
    }
    if (byte == '"')
    {
      insideString = true;
    }
    else if (byte == '{' || byte == '[')
    {
      ++depth;
      if (depth > maximumDepth)
      {
        return false;
      }
    }
    else if ((byte == '}' || byte == ']') && depth > 0)
    {
      --depth;
    }
  }
  return true;
}



bool knownIssueCode(quality::MeshQualityIssueCode code)
{
  return !quality::stableIssueId(code).empty();
}

bool isTriangleMetric(quality::MeshQualityMetric metric)
{
  return metric == quality::MeshQualityMetric::TriangleArea
    || metric == quality::MeshQualityMetric::TriangleAspectRatio
    || metric == quality::MeshQualityMetric::TriangleMinimumAngle
    || metric == quality::MeshQualityMetric::TriangleMaximumAngle
    || metric == quality::MeshQualityMetric::TriangleNormalConsistent;
}

bool requiredNumericMetric(quality::MeshQualityMetric metric)
{
  return metric == quality::MeshQualityMetric::TriangleArea
    || metric == quality::MeshQualityMetric::TetrahedronSignedVolume;
}
}

namespace cceda::meshing
{
MeshQualityJsonWriteResult writeMeshQualityJson(
  const mesh_quality::MeshQualityReport& report,
  const QString& jobId,
  const MeshQualityJsonLimits& limits)
{
  if (!validLimits(limits))
  {
    return writeFailure(MeshQualityJsonErrorCode::InvalidLimits,
      "Mesh quality JSON writer limits are invalid");
  }
  if (!validJobId(jobId)
      || report.meshRevision != 0
      || report.sourceGeometryRevision == 0
      || report.sourceGeometryRevision > JsonSafeInteger)
  {
    return writeFailure(MeshQualityJsonErrorCode::InvalidIdentity,
      "Worker quality identity or revision is invalid");
  }
  if (report.triangles.empty() || report.tetrahedra.empty()
      || report.triangles.size() > JsonSafeInteger
      || report.tetrahedra.size() > JsonSafeInteger
      || report.issues.size() > limits.maximumIssues
      || report.issues.size() > JsonSafeInteger)
  {
    return writeFailure(MeshQualityJsonErrorCode::CountLimitExceeded,
      "Mesh quality report counts are empty or exceed their budget");
  }

  NumericAccumulator triangleArea;
  NumericAccumulator triangleAspect;
  NumericAccumulator triangleMinimumAngle;
  NumericAccumulator triangleMaximumAngle;
  NumericAccumulator tetrahedronVolume;
  NumericAccumulator tetrahedronAspect;
  NumericAccumulator tetrahedronMinimumDihedral;
  NumericAccumulator tetrahedronScaledJacobian;
  std::uint64_t inconsistentNormals = 0;
  std::unordered_set<domain::MeshElementId> elementIds;
  elementIds.reserve(report.triangles.size() + report.tetrahedra.size());

  for (const mesh_quality::TriangleQuality& triangle : report.triangles)
  {
    if (triangle.elementId == 0 || triangle.elementId > JsonSafeInteger
        || !elementIds.insert(triangle.elementId).second
        || !triangleArea.add(triangle.areaSquareMillimeters)
        || (triangle.aspectRatio
          && !triangleAspect.add(*triangle.aspectRatio))
        || (triangle.minimumAngleDegrees
          && !triangleMinimumAngle.add(*triangle.minimumAngleDegrees))
        || (triangle.maximumAngleDegrees
          && !triangleMaximumAngle.add(*triangle.maximumAngleDegrees)))
    {
      return writeFailure(MeshQualityJsonErrorCode::InconsistentReport,
        "Triangle quality values or identities are inconsistent");
    }
    if (!triangle.normalConsistent)
    {
      ++inconsistentNormals;
    }
  }

  for (const mesh_quality::TetrahedronQuality& tetrahedron :
       report.tetrahedra)
  {
    if (tetrahedron.elementId == 0
        || tetrahedron.elementId > JsonSafeInteger
        || !elementIds.insert(tetrahedron.elementId).second
        || !tetrahedronVolume.add(
          tetrahedron.signedVolumeCubicMillimeters)
        || (tetrahedron.aspectRatio
          && !tetrahedronAspect.add(*tetrahedron.aspectRatio))
        || (tetrahedron.minimumDihedralAngleDegrees
          && !tetrahedronMinimumDihedral.add(
            *tetrahedron.minimumDihedralAngleDegrees))
        || (tetrahedron.scaledJacobian
          && !tetrahedronScaledJacobian.add(*tetrahedron.scaledJacobian)))
    {
      return writeFailure(MeshQualityJsonErrorCode::InconsistentReport,
        "Tetrahedron quality values or identities are inconsistent");
    }
  }

  for (std::size_t index = 0; index < report.issues.size(); ++index)
  {
    const mesh_quality::MeshQualityIssue& issue = report.issues[index];
    if (issue.elementId == 0 || issue.elementId > JsonSafeInteger
        || !knownIssueCode(issue.code)
        || elementIds.find(issue.elementId) == elementIds.end()
        || (index > 0
          && !issueLess(report.issues[index - 1], issue)))
    {
      return writeFailure(MeshQualityJsonErrorCode::InconsistentReport,
        "Mesh quality issues are invalid, duplicated, or out of order");
    }
  }

  QJsonArray summaries;
  summaries.append(numericSummary(
    mesh_quality::MeshQualityMetric::TriangleArea, triangleArea));
  summaries.append(numericSummary(
    mesh_quality::MeshQualityMetric::TriangleAspectRatio, triangleAspect));
  summaries.append(numericSummary(
    mesh_quality::MeshQualityMetric::TriangleMinimumAngle,
    triangleMinimumAngle));
  summaries.append(numericSummary(
    mesh_quality::MeshQualityMetric::TriangleMaximumAngle,
    triangleMaximumAngle));
  summaries.append(QJsonObject{
    {"metricId", jsonMetricId(
      mesh_quality::MeshQualityMetric::TriangleNormalConsistent)},
    {"kind", "boolean"},
    {"sampleCount", jsonInteger(report.triangles.size())},
    {"failedElementCount", jsonInteger(inconsistentNormals)},
  });
  summaries.append(numericSummary(
    mesh_quality::MeshQualityMetric::TetrahedronSignedVolume,
    tetrahedronVolume));
  summaries.append(numericSummary(
    mesh_quality::MeshQualityMetric::TetrahedronAspectRatio,
    tetrahedronAspect));
  summaries.append(numericSummary(
    mesh_quality::MeshQualityMetric::TetrahedronMinimumDihedralAngle,
    tetrahedronMinimumDihedral));
  summaries.append(numericSummary(
    mesh_quality::MeshQualityMetric::TetrahedronScaledJacobian,
    tetrahedronScaledJacobian));

  QJsonArray issues;
  for (const mesh_quality::MeshQualityIssue& issue : report.issues)
  {
    issues.append(QJsonObject{
      {"elementId", jsonInteger(issue.elementId)},
      {"code", jsonIssueId(issue.code)},
    });
  }

  const QJsonObject root{
    {"schema", QJsonObject{
      {"name", "cceda.mesh-quality-worker"},
      {"major", MeshQualityJsonSchemaMajor},
      {"minor", MeshQualityJsonSchemaMinor},
    }},
    {"jobId", jobId},
    {"meshRevision", jsonInteger(report.meshRevision)},
    {"sourceGeometryRevision",
      jsonInteger(report.sourceGeometryRevision)},
    {"metricVersion", QString::fromUtf8(
      mesh_quality::MeshQualityMetricVersion.data(),
      static_cast<qsizetype>(
        mesh_quality::MeshQualityMetricVersion.size()))},
    {"validForCommit", report.validForCommit()},
    {"counts", QJsonObject{
      {"triangles", jsonInteger(report.triangles.size())},
      {"tetrahedra", jsonInteger(report.tetrahedra.size())},
      {"issues", jsonInteger(report.issues.size())},
    }},
    {"summaries", summaries},
    {"issues", issues},
  };
  const QByteArray bytes =
    QJsonDocument(root).toJson(QJsonDocument::Compact);
  if (static_cast<std::uint64_t>(bytes.size()) > limits.maximumInputBytes)
  {
    return writeFailure(MeshQualityJsonErrorCode::InputLimitExceeded,
      "Mesh quality JSON exceeds the byte budget");
  }
  return {bytes, MeshQualityJsonErrorCode::None, {}};
}

MeshQualityJsonReadResult readMeshQualityJson(
  QIODevice& input,
  const MeshQualityJsonLimits& limits)
{
  if (!input.isOpen() || !input.isReadable() || !validLimits(limits))
  {
    return readFailure(MeshQualityJsonErrorCode::InvalidLimits,
      "Mesh quality JSON reader limits or input device are invalid");
  }

  MeshQualityJsonReadResult boundedError;
  const std::optional<QByteArray> bytes =
    readBounded(input, limits.maximumInputBytes, boundedError);
  if (!bytes)
  {
    return boundedError;
  }

  if (!withinNestingDepth(*bytes, limits.maximumNestingDepth))
  {
    return readFailure(MeshQualityJsonErrorCode::CountLimitExceeded,
      "Mesh quality JSON exceeds the nesting-depth budget");
  }

  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(*bytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
  {
    return readFailure(MeshQualityJsonErrorCode::InvalidJson,
      QString("Invalid mesh quality JSON: %1")
        .arg(parseError.errorString()));
  }

  const QJsonObject root = document.object();
  if (!hasExactlyKeys(root, {
      "schema", "jobId", "meshRevision", "sourceGeometryRevision",
      "metricVersion", "validForCommit", "counts", "summaries", "issues"}))
  {
    return readFailure(MeshQualityJsonErrorCode::InvalidStructure,
      "Mesh quality JSON root fields are invalid");
  }

  if (!root.value("schema").isObject())
  {
    return readFailure(MeshQualityJsonErrorCode::UnsupportedSchema,
      "Mesh quality JSON schema is missing");
  }
  const QJsonObject schema = root.value("schema").toObject();
  const auto schemaMajor = unsignedInteger(schema.value("major"));
  const auto schemaMinor = unsignedInteger(schema.value("minor"));
  if (!hasExactlyKeys(schema, {"name", "major", "minor"})
      || schema.value("name").toString()
        != "cceda.mesh-quality-worker"
      || !schemaMajor
      || *schemaMajor != MeshQualityJsonSchemaMajor
      || !schemaMinor
      || *schemaMinor != MeshQualityJsonSchemaMinor)
  {
    return readFailure(MeshQualityJsonErrorCode::UnsupportedSchema,
      "Mesh quality JSON schema is not supported");
  }

  const auto meshRevision = unsignedInteger(root.value("meshRevision"));
  const auto geometryRevision =
    unsignedInteger(root.value("sourceGeometryRevision"));
  const QString metricVersion = root.value("metricVersion").toString();
  const QString expectedMetricVersion = QString::fromUtf8(
    mesh_quality::MeshQualityMetricVersion.data(),
    static_cast<qsizetype>(
      mesh_quality::MeshQualityMetricVersion.size()));
  if (!root.value("jobId").isString()
      || !validJobId(root.value("jobId").toString())
      || !meshRevision || *meshRevision != 0
      || !geometryRevision || *geometryRevision == 0
      || !root.value("metricVersion").isString()
      || metricVersion != expectedMetricVersion
      || !root.value("validForCommit").isBool())
  {
    return readFailure(MeshQualityJsonErrorCode::InvalidIdentity,
      "Mesh quality identity, revision, or metric version is invalid");
  }

  if (!root.value("counts").isObject()
      || !root.value("summaries").isArray()
      || !root.value("issues").isArray())
  {
    return readFailure(MeshQualityJsonErrorCode::InvalidStructure,
      "Mesh quality counts, summaries, or issues are invalid");
  }
  const QJsonObject counts = root.value("counts").toObject();
  if (!hasExactlyKeys(counts, {"triangles", "tetrahedra", "issues"}))
  {
    return readFailure(MeshQualityJsonErrorCode::InvalidStructure,
      "Mesh quality count fields are invalid");
  }
  const auto triangleCount = unsignedInteger(counts.value("triangles"));
  const auto tetrahedronCount =
    unsignedInteger(counts.value("tetrahedra"));
  const auto issueCount = unsignedInteger(counts.value("issues"));
  const QJsonArray issueValues = root.value("issues").toArray();
  if (!triangleCount || *triangleCount == 0
      || !tetrahedronCount || *tetrahedronCount == 0
      || !issueCount || *issueCount > limits.maximumIssues
      || *issueCount != static_cast<std::uint64_t>(issueValues.size()))
  {
    return readFailure(MeshQualityJsonErrorCode::CountLimitExceeded,
      "Mesh quality counts are empty, mismatched, or exceed their budget");
  }

  MeshQualityJsonArtifact artifact;
  artifact.jobId = root.value("jobId").toString();
  artifact.meshRevision = *meshRevision;
  artifact.sourceGeometryRevision = *geometryRevision;
  artifact.metricVersion = metricVersion;
  artifact.validForCommit = root.value("validForCommit").toBool();
  artifact.triangleCount = *triangleCount;
  artifact.tetrahedronCount = *tetrahedronCount;

  const QJsonArray summaries = root.value("summaries").toArray();
  if (summaries.size() != static_cast<qsizetype>(MetricOrder.size()))
  {
    return readFailure(MeshQualityJsonErrorCode::InvalidSummary,
      "Mesh quality JSON must contain all metric summaries");
  }
  artifact.numericSummaries.reserve(MetricOrder.size() - 1);
  for (qsizetype index = 0; index < summaries.size(); ++index)
  {
    const QJsonValue value = summaries.at(index);
    const QJsonObject summary = value.toObject();
    const mesh_quality::MeshQualityMetric metric =
      MetricOrder.at(static_cast<std::size_t>(index));
    if (!value.isObject()
        || summary.value("metricId").toString() != jsonMetricId(metric))
    {
      return readFailure(MeshQualityJsonErrorCode::InvalidSummary,
        "Mesh quality metric summaries are missing, duplicated, or out of order");
    }

    const std::uint64_t elementCount = isTriangleMetric(metric)
      ? *triangleCount : *tetrahedronCount;
    const auto sampleCount =
      unsignedInteger(summary.value("sampleCount"));
    if (!sampleCount || *sampleCount > elementCount)
    {
      return readFailure(MeshQualityJsonErrorCode::InvalidSummary,
        "Mesh quality summary sample count is invalid");
    }

    if (metric
        == mesh_quality::MeshQualityMetric::TriangleNormalConsistent)
    {
      const auto failed =
        unsignedInteger(summary.value("failedElementCount"));
      if (!hasExactlyKeys(summary, {
          "metricId", "kind", "sampleCount", "failedElementCount"})
          || summary.value("kind").toString() != "boolean"
          || *sampleCount != *triangleCount
          || !failed || *failed > *sampleCount)
      {
        return readFailure(MeshQualityJsonErrorCode::InvalidSummary,
          "Mesh quality boolean summary is invalid");
      }
      artifact.booleanSummary = {metric, *sampleCount, *failed};
      continue;
    }

    const QSet<QString> expectedKeys = *sampleCount == 0
      ? QSet<QString>{"metricId", "kind", "sampleCount"}
      : QSet<QString>{
          "metricId", "kind", "sampleCount",
          "minimum", "maximum", "mean"};
    if (!hasExactlyKeys(summary, expectedKeys)
        || summary.value("kind").toString() != "numeric"
        || (requiredNumericMetric(metric) && *sampleCount != elementCount))
    {
      return readFailure(MeshQualityJsonErrorCode::InvalidSummary,
        "Mesh quality numeric summary fields are invalid");
    }

    MeshQualityNumericSummary numeric;
    numeric.metric = metric;
    numeric.sampleCount = *sampleCount;
    if (*sampleCount > 0)
    {
      if (!summary.value("minimum").isDouble()
          || !summary.value("maximum").isDouble()
          || !summary.value("mean").isDouble())
      {
        return readFailure(MeshQualityJsonErrorCode::InvalidSummary,
          "Mesh quality numeric summary values are missing");
      }
      const double minimum = summary.value("minimum").toDouble();
      const double maximum = summary.value("maximum").toDouble();
      const double mean = summary.value("mean").toDouble();
      if (!std::isfinite(minimum) || !std::isfinite(maximum)
          || !std::isfinite(mean)
          || minimum > mean || mean > maximum)
      {
        return readFailure(MeshQualityJsonErrorCode::InvalidSummary,
          "Mesh quality numeric summary values are invalid");
      }
      numeric.minimum = minimum;
      numeric.maximum = maximum;
      numeric.mean = mean;
    }
    artifact.numericSummaries.push_back(std::move(numeric));
  }

  artifact.issues.reserve(static_cast<std::size_t>(*issueCount));
  for (const QJsonValue& value : issueValues)
  {
    const QJsonObject issue = value.toObject();
    const auto elementId = unsignedInteger(issue.value("elementId"));
    const auto code = issueCode(issue.value("code").toString());
    if (!value.isObject()
        || !hasExactlyKeys(issue, {"elementId", "code"})
        || !elementId || *elementId == 0
        || !issue.value("code").isString() || !code)
    {
      return readFailure(MeshQualityJsonErrorCode::InvalidIssue,
        "Mesh quality issue fields are invalid");
    }
    const mesh_quality::MeshQualityIssue parsed{*code, *elementId};
    if (!artifact.issues.empty()
        && !issueLess(artifact.issues.back(), parsed))
    {
      return readFailure(MeshQualityJsonErrorCode::InvalidIssue,
        "Mesh quality issues are duplicated or out of order");
    }
    artifact.issues.push_back(parsed);
  }

  const bool derivedValidity = std::none_of(
    artifact.issues.cbegin(), artifact.issues.cend(),
    [](const mesh_quality::MeshQualityIssue& issue) {
      return mesh_quality::isCommitBlockingIssue(issue.code);
    });
  if (artifact.validForCommit != derivedValidity)
  {
    return readFailure(MeshQualityJsonErrorCode::InvalidIssue,
      "Mesh quality commit validity contradicts the issue list");
  }

  return {std::move(artifact), MeshQualityJsonErrorCode::None, {}};
}
}
