#include "ui/OccViewportWidget.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace
{
int failures = 0;

void expect(bool condition, std::string_view message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class RecordingViewportController final : public cceda::visualization::IViewportController
{
public:
  void attach(cceda::visualization::NativeWindowHandle window) override
  {
    attached = window != 0;
    ++attachCalls;
  }

  void resize() override
  {
    if (attached)
    {
      ++resizeCallsAfterAttach;
    }
  }

  void redraw() override {}
  void displayCurrentGeometry() override {}
  cceda::visualization::PreparedMeshPresentationPtr prepareMeshPresentation(
    const cceda::domain::MeshModel&,
    const std::vector<cceda::domain::MeshElementId>&) const override
  {
    return {};
  }
  bool displayMeshPresentation(
    const cceda::visualization::PreparedMeshPresentationPtr&) override
  {
    return false;
  }
  void clearMeshPresentation() override {}
  void setMeshVisible(bool) override {}
  bool setIssueElementsOnly(
    cceda::domain::MeshRevision, bool) override { return false; }
  bool highlightMeshElement(
    cceda::domain::MeshRevision,
    cceda::domain::MeshElementId) override { return false; }
  void fitAll() override {}
  void setIsometric() override {}
  void highlight(std::string_view) override {}
  void startRotation(int, int) override {}
  void rotate(int, int) override {}
  void pan(int, int) override {}
  void zoom(double) override {}

  bool attached = false;
  int attachCalls = 0;
  int resizeCallsAfterAttach = 0;
};
}

int main(int argc, char* argv[])
{
  QApplication application(argc, argv);
  const auto controller = std::make_shared<RecordingViewportController>();
  cceda::ui::OccViewportWidget viewport(controller);
  viewport.resize(640, 480);

  viewport.show();
  QCoreApplication::processEvents(QEventLoop::AllEvents);

  expect(controller->attachCalls == 1,
         "the viewport attaches exactly once when first shown");
  expect(controller->resizeCallsAfterAttach >= 1,
         "the viewport resizes OCCT after the native window has been attached");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
