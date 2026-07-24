#pragma once

#include "visualization/IViewportController.h"

#include <QWidget>

#include <memory>

class QPaintEngine;

namespace cceda::ui
{
class OccViewportWidget final : public QWidget
{
public:
  explicit OccViewportWidget(
    std::shared_ptr<visualization::IViewportController> controller,
    QWidget* parent = nullptr);

protected:
  QPaintEngine* paintEngine() const override;
  void showEvent(QShowEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

private:
  [[nodiscard]] QPoint pixelPosition(const QPointF& position) const;

  std::shared_ptr<visualization::IViewportController> controller_;
  bool attached_ = false;
  enum class MouseMode
  {
    None,
    Pan,
    Rotate,
  } mouseMode_ = MouseMode::None;
  QPoint lastMousePosition_;
};
}
