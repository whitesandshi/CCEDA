#pragma once

#include <cstdint>
#include <string_view>

namespace cceda::visualization
{
using NativeWindowHandle = std::uintptr_t;

class IViewportController
{
public:
  virtual ~IViewportController() = default;

  virtual void attach(NativeWindowHandle window) = 0;
  virtual void resize() = 0;
  virtual void redraw() = 0;
  virtual void displayCurrentGeometry() = 0;
  virtual void fitAll() = 0;
  virtual void setIsometric() = 0;
  virtual void highlight(std::string_view geometryId) = 0;

  virtual void startRotation(int x, int y) = 0;
  virtual void rotate(int x, int y) = 0;
  virtual void pan(int deltaX, int deltaY) = 0;
  virtual void zoom(double factor) = 0;
};
}
