#include "meshing/GeometryMap.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
namespace meshing = cceda::meshing;
constexpr qint64 ReadChunkBytes = 64 * 1024;
constexpr qsizetype MaximumAssemblyDepth = 128;

meshing::GeometryMapReadResult fail(
  meshing::GeometryMapReadErrorCode error,
  QString detail)
{
  return {std::nullopt, error, std::move(detail)};
}

bool hasOnlyKeys(const QJsonObject& object, const QSet<QString>& allowed)
{
  for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator)
  {
    if (!allowed.contains(iterator.key()))
    {
      return false;
    }
  }
  return true;
}

template<typename Value>
bool insertUnique(QSet<Value>& values, const Value& value)
{
  if (values.contains(value))
  {
    return false;
  }
  values.insert(value);
  return true;
}

bool validIdentity(const QJsonValue& value, std::uint64_t maximumCharacters)
{
  if (!value.isString())
  {
    return false;
  }
  const QString identity = value.toString();
  if (identity.isEmpty()
      || static_cast<std::uint64_t>(identity.size()) > maximumCharacters)
  {
    return false;
  }
  for (const QChar character : identity)
  {
    if (character.unicode() < 0x20 || character.unicode() == 0x7f)
    {
      return false;
    }
  }
  return true;
}

std::optional<std::uint64_t> positiveInteger(const QJsonValue& value)
{
  if (!value.isDouble())
  {
    return std::nullopt;
  }
  const qint64 integer = value.toInteger(-1);
  return integer > 0
    ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(integer))
    : std::nullopt;
}

std::optional<QStringList> identityList(
  const QJsonValue& value,
  std::uint64_t maximumCharacters,
  std::uint64_t maximumCount)
{
  if (!value.isArray())
  {
    return std::nullopt;
  }
  const QJsonArray array = value.toArray();
  if (static_cast<std::uint64_t>(array.size()) > maximumCount)
  {
    return std::nullopt;
  }
  QStringList result;
  QSet<QString> unique;
  result.reserve(array.size());
  for (const QJsonValue& item : array)
  {
    if (!validIdentity(item, maximumCharacters)
        || !insertUnique(unique, item.toString()))
    {
      return std::nullopt;
    }
    result.push_back(item.toString());
  }
  return result;
}

std::optional<std::array<double, 16>> matrix(const QJsonValue& value)
{
  if (!value.isArray() || value.toArray().size() != 16)
  {
    return std::nullopt;
  }
  std::array<double, 16> result{};
  const QJsonArray values = value.toArray();
  for (qsizetype index = 0; index < values.size(); ++index)
  {
    if (!values.at(index).isDouble())
    {
      return std::nullopt;
    }
    const double component = values.at(index).toDouble();
    if (!std::isfinite(component))
    {
      return std::nullopt;
    }
    result[static_cast<std::size_t>(index)] = component;
  }
  return result;
}

std::optional<std::array<double, 6>> bounds(const QJsonValue& value)
{
  if (!value.isArray() || value.toArray().size() != 6)
  {
    return std::nullopt;
  }
  std::array<double, 6> result{};
  const QJsonArray values = value.toArray();
  for (qsizetype index = 0; index < values.size(); ++index)
  {
    if (!values.at(index).isDouble())
    {
      return std::nullopt;
    }
    const double component = values.at(index).toDouble();
    if (!std::isfinite(component))
    {
      return std::nullopt;
    }
    result[static_cast<std::size_t>(index)] = component;
  }
  if (result[0] > result[3]
      || result[1] > result[4]
      || result[2] > result[5])
  {
    return std::nullopt;
  }
  return result;
}

std::optional<QByteArray> readBounded(
  QIODevice& input,
  std::uint64_t maximumBytes,
  meshing::GeometryMapReadResult& error)
{
  QByteArray bytes;
  while (!input.atEnd())
  {
    const std::uint64_t remaining = maximumBytes - bytes.size();
    const qint64 requested = static_cast<qint64>(
      std::min<std::uint64_t>(ReadChunkBytes, remaining + 1));
    const QByteArray chunk = input.read(requested);
    if (chunk.isEmpty() && !input.atEnd())
    {
      error = fail(meshing::GeometryMapReadErrorCode::ReadFailed,
        "Cannot read geometry-map input");
      return std::nullopt;
    }
    bytes.append(chunk);
    if (static_cast<std::uint64_t>(bytes.size()) > maximumBytes)
    {
      error = fail(meshing::GeometryMapReadErrorCode::InputLimitExceeded,
        "geometry-map input exceeds the byte budget");
      return std::nullopt;
    }
  }
  return bytes;
}
}

namespace cceda::meshing
{
GeometryMapReadResult readGeometryMap(
  QIODevice& input,
  const GeometryMapReadLimits& limits)
{
  if (!input.isOpen() || !input.isReadable()
      || limits.maximumInputBytes == 0
      || limits.maximumRegions == 0
      || limits.maximumFaceReferences == 0
      || limits.maximumSharedFaceCandidates == 0
      || limits.maximumExclusions == 0
      || limits.maximumIdentityCharacters == 0
      || limits.maximumInputBytes
        > static_cast<std::uint64_t>(std::numeric_limits<qsizetype>::max()))
  {
    return fail(GeometryMapReadErrorCode::InvalidLimits,
      "Geometry-map reader limits or input device are invalid");
  }

  GeometryMapReadResult readError;
  const std::optional<QByteArray> bytes =
    readBounded(input, limits.maximumInputBytes, readError);
  if (!bytes)
  {
    return readError;
  }

  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(*bytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
  {
    return fail(GeometryMapReadErrorCode::InvalidJson,
      QString("Invalid geometry-map JSON: %1").arg(parseError.errorString()));
  }

  const QJsonObject root = document.object();
  if (!hasOnlyKeys(root, {
      "schema", "sourceGeometryRevision", "mode", "canonicalUnit",
      "transformsBaked", "regions", "sharedFaceCandidates", "excluded"}))
  {
    return fail(GeometryMapReadErrorCode::InvalidStructure,
      "geometry-map contains unknown root fields");
  }
  const QJsonObject schema = root.value("schema").toObject();
  if (!hasOnlyKeys(schema, {"name", "major", "minor"})
      || schema.value("name").toString() != "cceda.geometry-map"
      || schema.value("major").toInt(-1) != GeometryMapSchemaMajor
      || schema.value("minor").toInt(-1) != GeometryMapSchemaMinor)
  {
    return fail(GeometryMapReadErrorCode::UnsupportedSchema,
      "geometry-map schema is not supported");
  }

  const std::optional<std::uint64_t> revision =
    positiveInteger(root.value("sourceGeometryRevision"));
  if (!revision || root.value("mode").toString() != "independent-solids"
      || root.value("canonicalUnit").toString() != "mm"
      || !root.value("transformsBaked").isBool()
      || !root.value("regions").isArray()
      || !root.value("sharedFaceCandidates").isArray()
      || !root.value("excluded").isArray())
  {
    return fail(GeometryMapReadErrorCode::InvalidStructure,
      "geometry-map identity, mode, unit, or arrays are invalid");
  }

  GeometryMap result;
  result.sourceGeometryRevision = *revision;
  result.mode = root.value("mode").toString();
  result.canonicalUnit = root.value("canonicalUnit").toString();
  result.transformsBaked = root.value("transformsBaked").toBool();

  const QJsonArray regions = root.value("regions").toArray();
  if (regions.isEmpty()
      || static_cast<std::uint64_t>(regions.size()) > limits.maximumRegions)
  {
    return fail(GeometryMapReadErrorCode::CountLimitExceeded,
      "geometry-map region count is empty or exceeds its budget");
  }
  QSet<std::uint64_t> regionIds;
  QSet<std::uint64_t> brepSolidIndices;
  QSet<QString> solidGeometryIds;
  QHash<std::uint64_t, QSet<QString>> facesByRegion;
  std::uint64_t faceReferenceCount = 0;
  result.regions.reserve(static_cast<std::size_t>(regions.size()));
  for (const QJsonValue& value : regions)
  {
    const QJsonObject region = value.toObject();
    if (!value.isObject() || !hasOnlyKeys(region, {
        "regionId", "brepSolidIndex", "sourceAssemblyGeometryIds",
        "sourcePartGeometryId", "sourceSolidGeometryId",
        "sourceFaceGeometryIds", "brepLocation", "brepBoundsMm"}))
    {
      return fail(GeometryMapReadErrorCode::InvalidRegion,
        "geometry-map region fields are invalid");
    }
    const auto regionId = positiveInteger(region.value("regionId"));
    const auto solidIndex = positiveInteger(region.value("brepSolidIndex"));
    const auto assemblies = identityList(
      region.value("sourceAssemblyGeometryIds"),
      limits.maximumIdentityCharacters,
      MaximumAssemblyDepth);
    const auto faces = identityList(
      region.value("sourceFaceGeometryIds"),
      limits.maximumIdentityCharacters,
      limits.maximumFaceReferences);
    const QJsonObject location = region.value("brepLocation").toObject();
    const auto locationMatrix = matrix(location.value("matrixRowMajor"));
    const auto brepBounds = bounds(region.value("brepBoundsMm"));
    if (!regionId || !solidIndex || !assemblies || !faces
        || !validIdentity(region.value("sourcePartGeometryId"),
          limits.maximumIdentityCharacters)
        || !validIdentity(region.value("sourceSolidGeometryId"),
          limits.maximumIdentityCharacters)
        || !region.value("brepLocation").isObject()
        || !hasOnlyKeys(location, {"transformsBaked", "matrixRowMajor"})
        || !location.value("transformsBaked").isBool()
        || location.value("transformsBaked").toBool() != result.transformsBaked
        || !locationMatrix
        || !brepBounds
        || !insertUnique(regionIds, *regionId)
        || !insertUnique(brepSolidIndices, *solidIndex)
        || !insertUnique(solidGeometryIds,
          region.value("sourceSolidGeometryId").toString()))
    {
      return fail(GeometryMapReadErrorCode::InvalidRegion,
        "geometry-map region identity, transform, bounds, or uniqueness is invalid");
    }
    if (faces->isEmpty())
    {
      return fail(GeometryMapReadErrorCode::InvalidRegion,
        "geometry-map region must reference at least one Face");
    }
    if (static_cast<std::uint64_t>(faces->size())
        > limits.maximumFaceReferences - faceReferenceCount)
    {
      return fail(GeometryMapReadErrorCode::CountLimitExceeded,
        "geometry-map Face references exceed their budget");
    }
    faceReferenceCount += static_cast<std::uint64_t>(faces->size());
    facesByRegion.insert(*regionId, QSet<QString>(faces->begin(), faces->end()));
    result.regions.push_back({
      *regionId,
      *solidIndex,
      *assemblies,
      region.value("sourcePartGeometryId").toString(),
      region.value("sourceSolidGeometryId").toString(),
      *faces,
      *locationMatrix,
      *brepBounds,
    });
  }

  const QJsonArray sharedFaces = root.value("sharedFaceCandidates").toArray();
  if (static_cast<std::uint64_t>(sharedFaces.size())
      > limits.maximumSharedFaceCandidates)
  {
    return fail(GeometryMapReadErrorCode::CountLimitExceeded,
      "shared-Face candidates exceed their budget");
  }
  QSet<QString> sharedFaceIds;
  for (const QJsonValue& value : sharedFaces)
  {
    const QJsonObject sharedFace = value.toObject();
    const QJsonArray references = sharedFace.value("regionIds").toArray();
    const QString faceId = sharedFace.value("sourceFaceGeometryId").toString();
    if (!value.isObject()
        || !hasOnlyKeys(sharedFace, {"sourceFaceGeometryId", "regionIds"})
        || !validIdentity(sharedFace.value("sourceFaceGeometryId"),
          limits.maximumIdentityCharacters)
        || !sharedFace.value("regionIds").isArray()
        || references.size() < 2
        || !insertUnique(sharedFaceIds, faceId))
    {
      return fail(GeometryMapReadErrorCode::InvalidSharedFace,
        "shared-Face candidate fields are invalid");
    }
    GeometryMapSharedFaceCandidate candidate;
    candidate.faceGeometryId = faceId;
    QSet<std::uint64_t> uniqueReferences;
    for (const QJsonValue& reference : references)
    {
      const auto regionId = positiveInteger(reference);
      if (!regionId || !regionIds.contains(*regionId)
          || !insertUnique(uniqueReferences, *regionId)
          || !facesByRegion.value(*regionId).contains(faceId))
      {
        return fail(GeometryMapReadErrorCode::InvalidSharedFace,
          "shared Face references an unknown region or Face association");
      }
      candidate.regionIds.push_back(*regionId);
    }
    result.sharedFaceCandidates.push_back(std::move(candidate));
  }

  const QJsonArray exclusions = root.value("excluded").toArray();
  if (static_cast<std::uint64_t>(exclusions.size()) > limits.maximumExclusions)
  {
    return fail(GeometryMapReadErrorCode::CountLimitExceeded,
      "geometry-map exclusions exceed their budget");
  }
  QSet<QString> excludedIds;
  for (const QJsonValue& value : exclusions)
  {
    const QJsonObject exclusion = value.toObject();
    const QString reason = exclusion.value("reasonCode").toString();
    if (!value.isObject()
        || !hasOnlyKeys(exclusion, {"geometryId", "reasonCode"})
        || !validIdentity(exclusion.value("geometryId"),
          limits.maximumIdentityCharacters)
        || (reason != "NON_SOLID_PART" && reason != "INVALID_SOLID")
        || solidGeometryIds.contains(exclusion.value("geometryId").toString())
        || !insertUnique(excludedIds, exclusion.value("geometryId").toString()))
    {
      return fail(GeometryMapReadErrorCode::InvalidExclusion,
        "geometry-map exclusion is invalid");
    }
    result.exclusions.push_back({
      exclusion.value("geometryId").toString(),
      reason,
    });
  }

  return {std::move(result), GeometryMapReadErrorCode::None, {}};
}
}
