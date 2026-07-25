#include "meshing/GeometryMap.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

namespace
{
int failures = 0;

void expect(bool condition, const char* message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

QJsonArray identityMatrix()
{
  return QJsonArray{
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0,
  };
}

QJsonObject validMap()
{
  return {
    {"schema", QJsonObject{
      {"name", "cceda.geometry-map"},
      {"major", 0},
      {"minor", 1},
    }},
    {"sourceGeometryRevision", 42},
    {"mode", "independent-solids"},
    {"canonicalUnit", "mm"},
    {"transformsBaked", false},
    {"regions", QJsonArray{
      QJsonObject{
        {"regionId", 1},
        {"brepSolidIndex", 1},
        {"sourceAssemblyGeometryIds", QJsonArray{"assembly/1"}},
        {"sourcePartGeometryId", "part/1"},
        {"sourceSolidGeometryId", "solid/1"},
        {"sourceFaceGeometryIds", QJsonArray{"face/shared", "face/1"}},
        {"brepLocation", QJsonObject{
          {"transformsBaked", false},
          {"matrixRowMajor", identityMatrix()},
        }},
      },
      QJsonObject{
        {"regionId", 2},
        {"brepSolidIndex", 2},
        {"sourceAssemblyGeometryIds", QJsonArray{"assembly/1"}},
        {"sourcePartGeometryId", "part/2"},
        {"sourceSolidGeometryId", "solid/2"},
        {"sourceFaceGeometryIds", QJsonArray{"face/shared", "face/2"}},
        {"brepLocation", QJsonObject{
          {"transformsBaked", false},
          {"matrixRowMajor", identityMatrix()},
        }},
      },
    }},
    {"sharedFaceCandidates", QJsonArray{
      QJsonObject{
        {"sourceFaceGeometryId", "face/shared"},
        {"regionIds", QJsonArray{1, 2}},
      },
    }},
    {"excluded", QJsonArray{
      QJsonObject{
        {"geometryId", "part/surface"},
        {"reasonCode", "NON_SOLID_PART"},
      },
    }},
  };
}

cceda::meshing::GeometryMapReadLimits limits()
{
  return {
    1024 * 1024,
    100,
    1000,
    100,
    100,
    4096,
  };
}

cceda::meshing::GeometryMapReadResult read(
  const QJsonObject& object,
  cceda::meshing::GeometryMapReadLimits readLimits = limits())
{
  QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  QBuffer input(&bytes);
  input.open(QIODevice::ReadOnly);
  return cceda::meshing::readGeometryMap(input, readLimits);
}
}

int main()
{
  using namespace cceda::meshing;

  const GeometryMapReadResult valid = read(validMap());
  expect(valid.succeeded(), "a protocol-0.1 independent geometry map is accepted");
  expect(valid.map.has_value(), "a valid map returns typed data");
  if (valid.map)
  {
    expect(valid.map->sourceGeometryRevision == 42,
      "the source geometry revision is retained");
    expect(valid.map->regions.size() == 2,
      "all independent regions are retained");
    expect(valid.map->regions.at(0).regionId == 1
        && valid.map->regions.at(0).solidGeometryId == "solid/1",
      "stable region and Solid identities are retained");
    expect(valid.map->sharedFaceCandidates.size() == 1,
      "validated shared-Face candidates are retained");
  }

  QJsonObject unknownField = validMap();
  unknownField.insert("futureGuess", true);
  expect(read(unknownField).error == GeometryMapReadErrorCode::InvalidStructure,
    "unknown root fields are rejected");

  QJsonObject duplicateRegion = validMap();
  QJsonArray duplicateRegions = duplicateRegion.value("regions").toArray();
  QJsonObject secondRegion = duplicateRegions.at(1).toObject();
  secondRegion.insert("regionId", 1);
  duplicateRegions.replace(1, secondRegion);
  duplicateRegion.insert("regions", duplicateRegions);
  expect(read(duplicateRegion).error == GeometryMapReadErrorCode::InvalidRegion,
    "duplicate stable region IDs are rejected");

  QJsonObject danglingSharedFace = validMap();
  QJsonArray sharedFaces = danglingSharedFace.value("sharedFaceCandidates").toArray();
  QJsonObject sharedFace = sharedFaces.at(0).toObject();
  sharedFace.insert("regionIds", QJsonArray{1, 99});
  sharedFaces.replace(0, sharedFace);
  danglingSharedFace.insert("sharedFaceCandidates", sharedFaces);
  expect(read(danglingSharedFace).error
      == GeometryMapReadErrorCode::InvalidSharedFace,
    "shared Faces cannot reference an unknown region");

  QJsonObject inconsistentTransform = validMap();
  QJsonArray transformedRegions = inconsistentTransform.value("regions").toArray();
  QJsonObject transformedRegion = transformedRegions.at(0).toObject();
  QJsonObject location = transformedRegion.value("brepLocation").toObject();
  location.insert("transformsBaked", true);
  transformedRegion.insert("brepLocation", location);
  transformedRegions.replace(0, transformedRegion);
  inconsistentTransform.insert("regions", transformedRegions);
  expect(read(inconsistentTransform).error == GeometryMapReadErrorCode::InvalidRegion,
    "region and root transform semantics must agree");

  QJsonObject unsupportedSchema = validMap();
  QJsonObject schema = unsupportedSchema.value("schema").toObject();
  schema.insert("major", 1);
  unsupportedSchema.insert("schema", schema);
  expect(read(unsupportedSchema).error
      == GeometryMapReadErrorCode::UnsupportedSchema,
    "an unsupported geometry-map major is rejected");

  GeometryMapReadLimits tinyInput = limits();
  tinyInput.maximumInputBytes = 8;
  expect(read(validMap(), tinyInput).error
      == GeometryMapReadErrorCode::InputLimitExceeded,
    "the input byte budget is enforced before parsing");

  GeometryMapReadLimits oneRegion = limits();
  oneRegion.maximumRegions = 1;
  expect(read(validMap(), oneRegion).error
      == GeometryMapReadErrorCode::CountLimitExceeded,
    "the region count budget is enforced");

  if (failures == 0)
  {
    std::cout << "Geometry map tests passed.\n";
  }
  return failures == 0 ? 0 : 1;
}
