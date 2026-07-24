#include "OccGeometryRepository.h"

#include <utility>

namespace cceda::geometry
{
domain::GeometryRevision OccGeometryRepository::nextRevision() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return domainDocument_.revision + 1;
}

void OccGeometryRepository::replace(const occ::handle<TDocStd_Document>& document,
                                    domain::GeometryDocument domainDocument,
                                    std::vector<OccShapeRecord> shapes,
                                    std::unordered_map<std::string, TopoDS_Shape> topologyShapes)
{
  std::lock_guard<std::mutex> lock(mutex_);
  document_ = document;
  domainDocument_ = std::move(domainDocument);
  shapes_ = std::move(shapes);
  topologyShapes_ = std::move(topologyShapes);
}

domain::GeometryDocument OccGeometryRepository::domainDocument() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return domainDocument_;
}

std::vector<OccShapeRecord> OccGeometryRepository::displayShapes() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return shapes_;
}

OccShapeLookupResult OccGeometryRepository::lookupTopologyShape(
  domain::GeometryRevision revision,
  const std::string& geometryId) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (revision != domainDocument_.revision)
  {
    return {OccShapeLookupStatus::RevisionMismatch, {}};
  }
  const auto shape = topologyShapes_.find(geometryId);
  if (shape == topologyShapes_.end())
  {
    return {};
  }
  return {OccShapeLookupStatus::Found, shape->second};
}
}
