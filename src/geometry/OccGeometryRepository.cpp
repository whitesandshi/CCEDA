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
                                    std::vector<OccShapeRecord> shapes)
{
  std::lock_guard<std::mutex> lock(mutex_);
  document_ = document;
  domainDocument_ = std::move(domainDocument);
  shapes_ = std::move(shapes);
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
}
