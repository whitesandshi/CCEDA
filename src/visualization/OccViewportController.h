#pragma once

#include "visualization/IViewportController.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_Triangulation.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <WNT_Window.hxx>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace cceda::geometry
{
class OccGeometryRepository;
}

namespace cceda::visualization
{
class OccViewportController final : public IViewportController
{
public:
  explicit OccViewportController(
    std::shared_ptr<geometry::OccGeometryRepository> repository);

  void attach(NativeWindowHandle window) override;
  void resize() override;
  void redraw() override;
  void displayCurrentGeometry() override;
  [[nodiscard]] PreparedMeshPresentationPtr prepareMeshPresentation(
    const domain::MeshModel& mesh,
    const std::vector<domain::MeshElementId>& locatableElementIds,
    const std::optional<MeshQualityColoringRequest>& coloring = std::nullopt) const override;
  [[nodiscard]] bool displayMeshPresentation(
    const PreparedMeshPresentationPtr& presentation) override;
  void clearMeshPresentation() override;
  void setMeshVisible(bool visible) override;
  [[nodiscard]] bool setIssueElementsOnly(
    domain::MeshRevision meshRevision, bool enabled) override;
  [[nodiscard]] bool highlightMeshElement(
    domain::MeshRevision meshRevision,
    domain::MeshElementId elementId) override;
  void fitAll() override;
  void setIsometric() override;
  void highlight(std::string_view geometryId) override;
  void startRotation(int x, int y) override;
  void rotate(int x, int y) override;
  void pan(int deltaX, int deltaY) override;
  void zoom(double factor) override;

private:
  [[nodiscard]] bool ready() const noexcept;
  void displayMeshSurface();
  void eraseMeshSurface();
  void setGeometryVisible(bool visible);

  std::shared_ptr<geometry::OccGeometryRepository> repository_;
  occ::handle<Aspect_DisplayConnection> displayConnection_;
  occ::handle<OpenGl_GraphicDriver> graphicDriver_;
  occ::handle<V3d_Viewer> viewer_;
  occ::handle<V3d_View> view_;
  occ::handle<WNT_Window> window_;
  occ::handle<AIS_InteractiveContext> context_;
  std::unordered_map<std::string, occ::handle<AIS_Shape>> presentations_;
  occ::handle<AIS_Triangulation> meshPresentation_;
  occ::handle<AIS_Triangulation> meshIssuesPresentation_;
  std::array<occ::handle<AIS_Triangulation>, 4> meshQualityPresentations_{};
  occ::handle<AIS_Triangulation> meshElementHighlight_;
  PreparedMeshPresentationPtr displayedMeshPresentation_;
  bool meshVisible_ = false;
  bool qualityColoring_ = false;
  bool issueElementsOnly_ = false;
};
}
