#include "OccViewportWidget.h"

#include <QMouseEvent>
#include <QPaintEngine>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>
#include <exception>
#include <utility>

namespace cceda::ui
{
OccViewportWidget::OccViewportWidget(
  std::shared_ptr<visualization::IViewportController> controller,
  QWidget* parent)
    : QWidget(parent), controller_(std::move(controller))
{
  setAttribute(Qt::WA_NativeWindow);
  setAttribute(Qt::WA_PaintOnScreen);
  setAttribute(Qt::WA_NoSystemBackground);
  setAutoFillBackground(false);
  setFocusPolicy(Qt::StrongFocus);
  setAccessibleName("3D model view");
  setMouseTracking(true);
}

QPaintEngine* OccViewportWidget::paintEngine() const
{
  return nullptr;
}

void OccViewportWidget::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);
  if (!attached_)
  {
    try
    {
      controller_->attach(static_cast<visualization::NativeWindowHandle>(winId()));
      attached_ = true;
      QTimer::singleShot(0, this, [this] {
        controller_->resize();
      });
    }
    catch (const std::exception&)
    {
      setToolTip("The Open CASCADE view could not be initialized.");
    }
    catch (...)
    {
      setToolTip("The Open CASCADE view could not be initialized.");
    }
  }
}

void OccViewportWidget::paintEvent(QPaintEvent*)
{
  controller_->redraw();
}

void OccViewportWidget::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  controller_->resize();
}

void OccViewportWidget::mousePressEvent(QMouseEvent* event)
{
  setFocus();
  lastMousePosition_ = pixelPosition(event->position());
  if (event->button() == Qt::MiddleButton)
  {
    mouseMode_ = MouseMode::Pan;
    grabMouse();
  }
  else if (event->button() == Qt::RightButton)
  {
    mouseMode_ = MouseMode::Rotate;
    controller_->startRotation(lastMousePosition_.x(), lastMousePosition_.y());
    grabMouse();
  }
  QWidget::mousePressEvent(event);
}

void OccViewportWidget::mouseMoveEvent(QMouseEvent* event)
{
  const QPoint position = pixelPosition(event->position());
  if (mouseMode_ == MouseMode::Pan)
  {
    controller_->pan(position.x() - lastMousePosition_.x(),
                     lastMousePosition_.y() - position.y());
    lastMousePosition_ = position;
  }
  else if (mouseMode_ == MouseMode::Rotate)
  {
    controller_->rotate(position.x(), position.y());
  }
  QWidget::mouseMoveEvent(event);
}

void OccViewportWidget::mouseReleaseEvent(QMouseEvent* event)
{
  if ((event->button() == Qt::MiddleButton || event->button() == Qt::RightButton)
      && mouseMode_ != MouseMode::None)
  {
    mouseMode_ = MouseMode::None;
    releaseMouse();
  }
  QWidget::mouseReleaseEvent(event);
}

void OccViewportWidget::wheelEvent(QWheelEvent* event)
{
  const double steps = static_cast<double>(event->angleDelta().y()) / 120.0;
  controller_->zoom(std::pow(1.15, steps));
  event->accept();
}

QPoint OccViewportWidget::pixelPosition(const QPointF& position) const
{
  const qreal scale = devicePixelRatioF();
  return QPoint(qRound(position.x() * scale), qRound(position.y() * scale));
}
}
