#pragma once

#include "domain/GeometryDocument.h"

#include <Quantity_Color.hxx>
#include <TDocStd_Document.hxx>
#include <TopoDS_Shape.hxx>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cceda::geometry
{
struct OccShapeRecord
{
  std::string id;
  TopoDS_Shape shape;
  Quantity_Color color = Quantity_Color(0.72, 0.76, 0.82, Quantity_TOC_sRGB);
};

enum class OccShapeLookupStatus
{
  Found,
  RevisionMismatch,
  NotFound,
};

struct OccShapeLookupResult
{
  OccShapeLookupStatus status = OccShapeLookupStatus::NotFound;
  TopoDS_Shape shape;
};

struct OccTopologySnapshot
{
  OccShapeLookupStatus status = OccShapeLookupStatus::NotFound;
  domain::GeometryDocument document;
  std::unordered_map<std::string, TopoDS_Shape> topologyShapes;
};

class OccGeometryRepository
{
public:
  [[nodiscard]] domain::GeometryRevision nextRevision() const;
  void replace(const occ::handle<TDocStd_Document>& document,
               domain::GeometryDocument domainDocument,
               std::vector<OccShapeRecord> shapes,
               std::unordered_map<std::string, TopoDS_Shape> topologyShapes);

  [[nodiscard]] domain::GeometryDocument domainDocument() const;
  [[nodiscard]] std::vector<OccShapeRecord> displayShapes() const;
  [[nodiscard]] OccShapeLookupResult lookupTopologyShape(
    domain::GeometryRevision revision,
    const std::string& geometryId) const;
  [[nodiscard]] OccTopologySnapshot topologySnapshot(
    domain::GeometryRevision revision) const;

private:
  mutable std::mutex mutex_;
  occ::handle<TDocStd_Document> document_;
  domain::GeometryDocument domainDocument_;
  std::vector<OccShapeRecord> shapes_;
  std::unordered_map<std::string, TopoDS_Shape> topologyShapes_;
};
}
