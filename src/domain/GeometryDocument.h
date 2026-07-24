#pragma once

#include "domain/ProjectState.h"

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
  GeometryRevision revision = 0;
  std::string sourceName;
  std::vector<GeometryEntity> entities;
  std::size_t solidCount = 0;
  std::size_t faceCount = 0;
  std::size_t edgeCount = 0;
  std::size_t vertexCount = 0;
};

struct GeometryImportResult
{
  bool success = false;
  GeometryDocument document;
  std::string errorCode;
  std::string message;
};
}
