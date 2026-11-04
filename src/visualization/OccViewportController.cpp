#include "OccViewportController.h"

#include "OccMeshTriangulation.h"
#include "geometry/OccGeometryRepository.h"

#include <AIS_DisplayMode.hxx>
#include <Aspect_TypeOfTriedronPosition.hxx>
#include <Bnd_Box.hxx>
#include <Quantity_Color.hxx>
#include <V3d_TypeOfOrientation.hxx>
#include <V3d_TypeOfVisualization.hxx>

#include <algorithm>
#include <utility>

namespace cceda::visualization
{
namespace
{
class OccPreparedMeshPresentation final : public PreparedMeshPresentation
{
public:
  OccPreparedMeshPresentation(occ::handle<Poly_Triangulation> triangulation,
                              OccElementTriangulations elementTriangulations,
                              domain::MeshRevision meshRevision,
                              domain::GeometryRevision sourceGeometryRevision)
      : triangulation_(std::move(triangulation)),
        elementTriangulations_(std::move(elementTriangulations)),
        issueTriangulation_(buildOccIssueTriangulation(elementTriangulations_)),
        meshRevision_(meshRevision),
        sourceGeometryRevision_(sourceGeometryRevision)
  {
  }

  domain::MeshRevision meshRevision() const noexcept override
  {
    return meshRevision_;
  }

  domain::GeometryRevision sourceGeometryRevision() const noexcept override
  {
    return sourceGeometryRevision_;
  }

  std::uint64_t triangleCount() const noexcept override
  {
    return triangulation_.IsNull()
      ? 0
      : static_cast<std::uint64_t>(triangulation_->NbTriangles());
  }

  std::uint64_t locatableElementCount() const noexcept override
  {
    return static_cast<std::uint64_t>(elementTriangulations_.size());
  }

  const occ::handle<Poly_Triangulation>& triangulation() const noexcept
  {
    return triangulation_;
  }

  occ::handle<Poly_Triangulation> elementTriangulation(
    domain::MeshElementId elementId) const
  {
    const auto found = elementTriangulations_.find(elementId);
    return found == elementTriangulations_.end()
      ? occ::handle<Poly_Triangulation>{}
      : found->second;
  }

  const occ::handle<Poly_Triangulation>& issueTriangulation() const noexcept
  {
    return issueTriangulation_;
  }

private:
  occ::handle<Poly_Triangulation> triangulation_;
  OccElementTriangulations elementTriangulations_;
  occ::handle<Poly_Triangulation> issueTriangulation_;
  domain::MeshRevision meshRevision_ = 0;
  domain::GeometryRevision sourceGeometryRevision_ = 0;
};
}

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
  meshPresentation_.Nullify();
  meshIssuesPresentation_.Nullify();
  meshElementHighlight_.Nullify();
  displayedMeshPresentation_.reset();
  meshVisible_ = false;
  issueElementsOnly_ = false;
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

PreparedMeshPresentationPtr OccViewportController::prepareMeshPresentation(
  const domain::MeshModel& mesh,
  const std::vector<domain::MeshElementId>& locatableElementIds) const
{
  occ::handle<Poly_Triangulation> triangulation = buildOccSurfaceTriangulation(mesh);
  if (triangulation.IsNull())
  {
    return {};
  }

  return std::make_shared<OccPreparedMeshPresentation>(
    std::move(triangulation),
    buildOccElementTriangulations(mesh, locatableElementIds),
    mesh.meshRevision,
    mesh.sourceGeometryRevision);
}

bool OccViewportController::displayMeshPresentation(
  const PreparedMeshPresentationPtr& presentation)
{
  if (!ready() || presentation == nullptr)
  {
    return false;
  }

  const auto prepared =
    std::dynamic_pointer_cast<const OccPreparedMeshPresentation>(presentation);
  if (prepared == nullptr || prepared->triangulation().IsNull())
  {
    return false;
  }

  clearMeshPresentation();
  meshPresentation_ = new AIS_Triangulation(prepared->triangulation());
  configureOccSurfacePresentation(meshPresentation_);
  if (!prepared->issueTriangulation().IsNull())
  {
    meshIssuesPresentation_ =
      new AIS_Triangulation(prepared->issueTriangulation());
    configureOccSurfacePresentation(meshIssuesPresentation_);
  }
  context_->ClearSelected(false);
  setGeometryVisible(false);
  context_->Display(meshPresentation_, 0, -1, false);
  displayedMeshPresentation_ = presentation;
  meshVisible_ = true;
  issueElementsOnly_ = false;
  view_->Redraw();
  return true;
}

void OccViewportController::clearMeshPresentation()
{
  if (ready() && !meshElementHighlight_.IsNull())
  {
    context_->Remove(meshElementHighlight_, false);
  }
  if (ready() && !meshPresentation_.IsNull())
  {
    context_->Remove(meshPresentation_, false);
  }
  if (ready() && !meshIssuesPresentation_.IsNull())
  {
    context_->Remove(meshIssuesPresentation_, false);
  }
  meshPresentation_.Nullify();
  meshIssuesPresentation_.Nullify();
  meshElementHighlight_.Nullify();
  displayedMeshPresentation_.reset();
  meshVisible_ = false;
  issueElementsOnly_ = false;
  setGeometryVisible(true);
  if (ready())
  {
    view_->Redraw();
  }
}

void OccViewportController::setMeshVisible(bool visible)
{
  if (!ready() || meshPresentation_.IsNull() || meshVisible_ == visible)
  {
    return;
  }

  if (visible)
  {
    context_->ClearSelected(false);
    setGeometryVisible(false);
    context_->Display(
      issueElementsOnly_ ? meshIssuesPresentation_ : meshPresentation_,
      0, -1, false);
  }
  else
  {
    if (!meshElementHighlight_.IsNull())
    {
      context_->Remove(meshElementHighlight_, false);
      meshElementHighlight_.Nullify();
    }
    context_->Erase(meshPresentation_, false);
    if (!meshIssuesPresentation_.IsNull())
    {
      context_->Erase(meshIssuesPresentation_, false);
    }
    setGeometryVisible(true);
  }
  meshVisible_ = visible;
  view_->Redraw();
}

bool OccViewportController::setIssueElementsOnly(
  domain::MeshRevision meshRevision, bool enabled)
{
  if (!ready() || !meshVisible_ || meshRevision == 0
    || displayedMeshPresentation_ == nullptr
    || displayedMeshPresentation_->meshRevision() != meshRevision
    || (enabled && meshIssuesPresentation_.IsNull()))
  {
    return false;
  }
  if (issueElementsOnly_ == enabled)
  {
    return true;
  }

  context_->Erase(
    issueElementsOnly_ ? meshIssuesPresentation_ : meshPresentation_, false);
  context_->Display(
    enabled ? meshIssuesPresentation_ : meshPresentation_, 0, -1, false);
  issueElementsOnly_ = enabled;
  view_->Redraw();
  return true;
}

bool OccViewportController::highlightMeshElement(
  domain::MeshRevision meshRevision,
  domain::MeshElementId elementId)
{
  if (!ready() || !meshVisible_ || meshRevision == 0 || elementId == 0
    || displayedMeshPresentation_ == nullptr
    || displayedMeshPresentation_->meshRevision() != meshRevision)
  {
    return false;
  }
  const auto prepared =
    std::dynamic_pointer_cast<const OccPreparedMeshPresentation>(
      displayedMeshPresentation_);
  if (prepared == nullptr)
  {
    return false;
  }
  const occ::handle<Poly_Triangulation> locator =
    prepared->elementTriangulation(elementId);
  if (locator.IsNull())
  {
    return false;
  }

  if (!meshElementHighlight_.IsNull())
  {
    context_->Remove(meshElementHighlight_, false);
  }
  meshElementHighlight_ = new AIS_Triangulation(locator);
  configureOccElementHighlight(meshElementHighlight_);
  context_->ClearSelected(false);
  context_->Display(meshElementHighlight_, 0, -1, false);
  Bnd_Box bounds;
  meshElementHighlight_->BoundingBox(bounds);
  if (!bounds.IsVoid())
  {
    view_->FitAll(bounds, 0.15, false);
    view_->ZFitAll();
  }
  view_->Redraw();
  return true;
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
  if (meshVisible_)
  {
    view_->Redraw();
    return;
  }

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

void OccViewportController::setGeometryVisible(bool visible)
{
  if (!ready())
  {
    return;
  }

  for (const auto& [id, presentation] : presentations_)
  {
    (void)id;
    if (visible)
    {
      context_->Display(presentation, AIS_Shaded, 0, false);
    }
    else
    {
      context_->Erase(presentation, false);
    }
  }
}
}
