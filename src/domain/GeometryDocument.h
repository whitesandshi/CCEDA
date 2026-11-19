#pragma once

#include "domain/Revisions.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace cceda::domain
{
enum class GeometryKind
{
  Assembly,
  Part,
  Solid,
  Shell,
  Face,
  Wire,
  Edge,
  Vertex,
  Other,
};

struct GeometryColor
{
  double red = 0.72;
  double green = 0.76;
  double blue = 0.82;
  bool explicitColor = false;
};

struct GeometryEntity
{
  std::string id;
  std::string parentId;
  std::string name;
  GeometryKind kind = GeometryKind::Other;
  GeometryColor color;
  std::size_t solidCount = 0;
  std::size_t faceCount = 0;
  std::size_t edgeCount = 0;
  std::size_t vertexCount = 0;
  double volume = 0.0;
  bool valid = false;
  bool meshable = false;
};

struct GeometryDocument
{
  struct LabelBinding
  {
    std::string geometryId;
    std::string instancePath;
    std::string definitionLabel;
    std::string parentGeometryId;
    std::string name;
    GeometryColor color;
    bool visible = true;
    std::array<double, 12> localTransform{};
  };

  struct SourceLengthUnit
  {
    std::string name;
    double millimetersPerUnit = 0.0;
  };

  GeometryRevision revision = 0;
  std::string sourceName;
  std::vector<GeometryEntity> entities;
  std::size_t solidCount = 0;
  std::size_t faceCount = 0;
  std::size_t edgeCount = 0;
  std::size_t vertexCount = 0;
  std::vector<LabelBinding> xcafLabels;
  std::vector<SourceLengthUnit> sourceLengthUnits;
};

struct GeometryImportResult
{
  bool success = false;
  GeometryDocument document;
  std::string errorCode;
  std::string message;
};
}
