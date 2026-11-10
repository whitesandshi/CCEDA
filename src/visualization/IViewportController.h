#pragma once

#include "domain/MeshModel.h"
#include "domain/Revisions.h"
#include "mesh_quality/MeshQualityColoring.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace cceda::visualization
{
using NativeWindowHandle = std::uintptr_t;

class PreparedMeshPresentation
{
public:
  virtual ~PreparedMeshPresentation() = default;

  [[nodiscard]] virtual domain::MeshRevision meshRevision() const noexcept = 0;
  [[nodiscard]] virtual domain::GeometryRevision sourceGeometryRevision() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t triangleCount() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t locatableElementCount() const noexcept = 0;
};

using PreparedMeshPresentationPtr = std::shared_ptr<const PreparedMeshPresentation>;

struct MeshQualityColoringRequest
{
  mesh_quality::MeshQualityMetric metric =
    mesh_quality::MeshQualityMetric::TriangleMinimumAngle;
  mesh_quality::MeshQualityThresholdProfile profile;
};

class IViewportController
{
public:
  virtual ~IViewportController() = default;

  virtual void attach(NativeWindowHandle window) = 0;
  virtual void resize() = 0;
  virtual void redraw() = 0;
  virtual void displayCurrentGeometry() = 0;
  [[nodiscard]] virtual PreparedMeshPresentationPtr prepareMeshPresentation(
    const domain::MeshModel& mesh,
    const std::vector<domain::MeshElementId>& locatableElementIds,
    const std::optional<MeshQualityColoringRequest>& coloring = std::nullopt) const = 0;
  [[nodiscard]] virtual bool displayMeshPresentation(
    const PreparedMeshPresentationPtr& presentation) = 0;
  virtual void clearMeshPresentation() = 0;
  virtual void setMeshVisible(bool visible) = 0;
  [[nodiscard]] virtual bool setIssueElementsOnly(
    domain::MeshRevision meshRevision, bool enabled) = 0;
  [[nodiscard]] virtual bool highlightMeshElement(
    domain::MeshRevision meshRevision,
    domain::MeshElementId elementId) = 0;
  virtual void fitAll() = 0;
  virtual void setIsometric() = 0;
  virtual void highlight(std::string_view geometryId) = 0;

  virtual void startRotation(int x, int y) = 0;
  virtual void rotate(int x, int y) = 0;
  virtual void pan(int deltaX, int deltaY) = 0;
  virtual void zoom(double factor) = 0;
};
}
