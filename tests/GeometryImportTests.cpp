#include "geometry/IGeometryService.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Writer.hxx>
#include <gp.hxx>
#include <gp_Pln.hxx>

#include <filesystem>
#include <iostream>

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    std::cerr << "Expected one STEP path.\n";
    return 2;
  }

  const std::filesystem::path stepPath(argv[1]);
  if (!std::filesystem::exists(stepPath))
  {
    std::cerr << "STEP fixture does not exist: " << stepPath << '\n';
    return 2;
  }

  const auto service = cceda::geometry::createOccGeometryService();
  const auto result = service->importStep(stepPath);
  if (!result.success)
  {
    std::cerr << result.errorCode << ": " << result.message << '\n';
    return 1;
  }

  const bool hasInspectableEntity = !result.document.entities.empty()
    && result.document.entities.front().faceCount > 0
    && result.document.entities.front().edgeCount > 0;

  if (result.document.revision != 1 || result.document.solidCount == 0
      || !hasInspectableEntity)
  {
    std::cerr << "Imported document does not satisfy the geometry contract.\n";
    return 1;
  }

  const std::filesystem::path surfacePath =
    std::filesystem::temp_directory_path() / "cceda_surface_contract.step";
  const TopoDS_Face face =
    BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), -10.0, 10.0, -10.0, 10.0).Face();
  STEPControl_Writer writer;
  if (writer.Transfer(face, STEPControl_AsIs) != IFSelect_RetDone
      || writer.Write(surfacePath.u8string().c_str()) != IFSelect_RetDone)
  {
    std::cerr << "Could not create the surface-only STEP contract fixture.\n";
    return 2;
  }

  const auto surfaceResult = service->importStep(surfacePath);
  std::filesystem::remove(surfacePath);
  if (!surfaceResult.success || surfaceResult.document.solidCount != 0
      || surfaceResult.document.faceCount == 0)
  {
    std::cerr << "A surface-only STEP model must remain inspectable.\n";
    return 1;
  }

  std::cout << "Imported " << result.document.solidCount << " solid(s), "
            << result.document.faceCount << " face(s).\n";
  return 0;
}
