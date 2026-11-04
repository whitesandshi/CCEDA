#include "MainWindow.h"

#include "geometry/OccGeometryRepository.h"
#include "geometry/OccGeometryService.h"
#include "geometry/OccGeometrySnapshotService.h"
#include "meshing/GmshMsh41MeshExporter.h"
#include "platform/WindowsStagedFilePublisher.h"
#include "visualization/OccViewportController.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTimer>

#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
cceda::application::MeshingRuntimeConfiguration meshingRuntimeConfiguration()
{
  return {
      QDir(QCoreApplication::applicationDirPath()).filePath("cceda_gmsh_worker.exe"),
      QString::fromWCharArray(CCEDA_GMSH_EXECUTABLE_PATH),
      QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("CCEDA/jobs"),
      "cceda-ui-" + QCoreApplication::applicationVersion(),
      512ULL * 1024ULL * 1024ULL,
  };
}

void configureOcctResources()
{
#ifdef _WIN32
  const std::wstring root = CCEDA_OCC_RESOURCE_DIR;
  const auto setPath = [&root](const wchar_t* name, const wchar_t* relative) {
    SetEnvironmentVariableW(name, (root + L"/" + relative).c_str());
  };
  SetEnvironmentVariableW(L"CSF_LANGUAGE", L"us");
  setPath(L"CSF_SHMessage", L"SHMessage");
  setPath(L"CSF_XSMessage", L"XSMessage");
  setPath(L"CSF_STEPDefaults", L"XSTEPResource");
  setPath(L"CSF_ShadersDirectory", L"Shaders");
  setPath(L"CSF_MDTVTexturesDirectory", L"Textures");
#endif
}
}

int main(int argc, char* argv[])
{
  configureOcctResources();
  QApplication application(argc, argv);
  application.setApplicationName("CCEDA");
  application.setApplicationVersion("0.2.0");
  application.setOrganizationName("CCEDA");
  application.setStyle("Fusion");

  const auto repository = std::make_shared<cceda::geometry::OccGeometryRepository>();
  const auto geometryService = std::make_shared<cceda::geometry::OccGeometryService>(repository);
  const auto snapshotService =
      std::make_shared<cceda::geometry::OccGeometrySnapshotService>(repository);
  const auto viewportController =
      std::make_shared<cceda::visualization::OccViewportController>(repository);
  const auto meshExporter =
      std::make_shared<cceda::meshing::GmshMsh41MeshExporter>();
  const auto filePublisher =
      std::make_shared<cceda::platform::WindowsStagedFilePublisher>();

  cceda::ui::MainWindow window(geometryService, snapshotService, viewportController,
                               meshingRuntimeConfiguration(), meshExporter,
                               filePublisher);
  window.show();

  if (argc > 1)
  {
    const QString stepPath = application.arguments().at(1);
    QTimer::singleShot(0, &window, [&window, stepPath] { window.openStepPath(stepPath); });
  }
  return application.exec();
}
