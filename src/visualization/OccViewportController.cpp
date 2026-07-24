#include "OccViewportController.h"

#include "geometry/OccGeometryRepository.h"

#include <AIS_DisplayMode.hxx>
#include <Aspect_TypeOfTriedronPosition.hxx>
#include <Quantity_Color.hxx>
#include <V3d_TypeOfOrientation.hxx>
#include <V3d_TypeOfVisualization.hxx>

#include <algorithm>
#include <utility>

namespace cceda::visualization
{
OccViewportController::OccViewportController(
  std::shared_ptr<geometry::OccGeometryRepository> repository)
    : repository_(std::move(repository))
{
}

void OccViewportController::attach(NativeWindowHandle window)
{
  if (ready())
  {
    return;
  }

  displayConnection_ = new Aspect_DisplayConnection();
  graphicDriver_ = new OpenGl_GraphicDriver(displayConnection_);
  viewer_ = new V3d_Viewer(graphicDriver_);
  viewer_->SetDefaultLights();
  viewer_->SetLightOn();

  view_ = viewer_->CreateView();
  window_ = new WNT_Window(reinterpret_cast<Aspect_Handle>(window));
  view_->SetWindow(window_);
  view_->SetBackgroundColor(Quantity_Color(0.91, 0.93, 0.95, Quantity_TOC_sRGB));
  view_->SetProj(V3d_TypeOfOrientation_Zup_AxoRight);
  view_->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_GRAY40, 0.08, V3d_ZBUFFER);
  if (!window_->IsMapped())
  {
    window_->Map();
  }

  context_ = new AIS_InteractiveContext(viewer_);
  context_->SetDisplayMode(AIS_Shaded, false);
  view_->MustBeResized();
  view_->Redraw();
}

void OccViewportController::resize()
{
  if (ready())
  {
    view_->MustBeResized();
    view_->Redraw();
  }
}

void OccViewportController::redraw()
{
  if (ready())
  {
    view_->Redraw();
  }
}

void OccViewportController::displayCurrentGeometry()
{
  if (!ready())
  {
    return;
  }

  context_->RemoveAll(false);
  presentations_.clear();
  for (const geometry::OccShapeRecord& record : repository_->displayShapes())
  {
    if (record.shape.IsNull())
    {
      continue;
    }

    occ::handle<AIS_Shape> presentation = new AIS_Shape(record.shape);
    presentation->SetColor(record.color);
    context_->Display(presentation, AIS_Shaded, 0, false);
    presentations_.emplace(record.id, presentation);
  }
  fitAll();
}

void OccViewportController::fitAll()
{
  if (ready())
  {
    view_->FitAll(0.01, false);
    view_->ZFitAll();
    view_->Redraw();
  }
}

void OccViewportController::setIsometric()
{
  if (ready())
  {
    view_->SetProj(V3d_TypeOfOrientation_Zup_AxoRight);
    fitAll();
  }
}

void OccViewportController::highlight(std::string_view geometryId)
{
  if (!ready())
  {
    return;
  }

  context_->ClearSelected(false);
  const auto found = presentations_.find(std::string(geometryId));
  if (found != presentations_.end())
  {
    context_->SetSelected(found->second, false);
  }
  view_->Redraw();
}

void OccViewportController::startRotation(int x, int y)
{
  if (ready())
  {
    view_->StartRotation(x, y);
  }
}

void OccViewportController::rotate(int x, int y)
{
  if (ready())
  {
    view_->Rotation(x, y);
  }
}

void OccViewportController::pan(int deltaX, int deltaY)
{
  if (ready())
  {
    view_->Pan(deltaX, deltaY);
  }
}

void OccViewportController::zoom(double factor)
{
  if (ready())
  {
    view_->SetZoom(std::clamp(factor, 0.1, 10.0), true);
    view_->Redraw();
  }
}

bool OccViewportController::ready() const noexcept
{
  return !view_.IsNull() && !context_.IsNull();
}
}
