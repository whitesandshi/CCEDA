#pragma once

#include "visualization/IViewportController.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <WNT_Window.hxx>

#include <memory>
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
  void fitAll() override;
  void setIsometric() override;
  void highlight(std::string_view geometryId) override;
  void startRotation(int x, int y) override;
  void rotate(int x, int y) override;
  void pan(int deltaX, int deltaY) override;
  void zoom(double factor) override;

private:
  [[nodiscard]] bool ready() const noexcept;

  std::shared_ptr<geometry::OccGeometryRepository> repository_;
  occ::handle<Aspect_DisplayConnection> displayConnection_;
  occ::handle<OpenGl_GraphicDriver> graphicDriver_;
  occ::handle<V3d_Viewer> viewer_;
  occ::handle<V3d_View> view_;
  occ::handle<WNT_Window> window_;
  occ::handle<AIS_InteractiveContext> context_;
  std::unordered_map<std::string, occ::handle<AIS_Shape>> presentations_;
};
}
