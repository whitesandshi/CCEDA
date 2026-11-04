#include "visualization/OccMeshTriangulation.h"
#include "geometry/OccGeometryRepository.h"
#include "visualization/OccViewportController.h"

#include <Graphic3d_ZLayerId.hxx>
#include <Poly_Triangle.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <Quantity_Color.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
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

bool close(double left, double right)
{
  return std::abs(left - right) < 1.0e-12;
}

cceda::domain::MeshModel makeCommittedMesh()
{
  using namespace cceda::domain;

  MeshModel mesh;
  mesh.meshRevision = 7;
  mesh.sourceGeometryRevision = 3;
  mesh.creationTimeUtc = "2026-07-26T00:00:00.000Z";
  mesh.backendId = "gmsh";
  mesh.backendVersion = "4.15.2";
  mesh.conformity = MeshConformity::NonConformal;
  mesh.nodes = {
    {10, 1.0, 0.0, 0.0, "solid-1/vertex-1"},
    {42, 0.0, 0.0, 0.0, "solid-1/vertex-2"},
    {900, 0.0, 1.0, 0.0, "solid-1/vertex-3"},
    {901, 0.0, 0.0, 1.0, {}},
  };
  mesh.elements = {
    {1001, MeshElementType::Triangle3, {42, 900, 10, 0}, 11, "solid-1/face-1"},
    {1002, MeshElementType::Tetrahedron4, {10, 42, 900, 901}, 11, "solid-1"},
  };
  mesh.regions = {
    {11, {"solid-1"}},
  };
  return mesh;
}
}

int main()
{
  using cceda::visualization::buildOccSurfaceTriangulation;

  const occ::handle<Poly_Triangulation> triangulation =
    buildOccSurfaceTriangulation(makeCommittedMesh());
  expect(!triangulation.IsNull(), "a valid committed mesh produces a surface triangulation");
  if (!triangulation.IsNull())
  {
    expect(triangulation->NbNodes() == 3,
      "tetrahedron-only interior nodes are omitted from the surface presentation");
    expect(triangulation->NbTriangles() == 1,
      "only Triangle3 elements become surface triangles");

    const gp_Pnt firstNode = triangulation->Node(1);
    const gp_Pnt secondNode = triangulation->Node(2);
    const gp_Pnt thirdNode = triangulation->Node(3);
    expect(close(firstNode.X(), 0.0) && close(firstNode.Y(), 0.0),
      "the first sparse node ID retains its coordinates");
    expect(close(secondNode.X(), 0.0) && close(secondNode.Y(), 1.0),
      "the second sparse node ID retains its coordinates");
    expect(close(thirdNode.X(), 1.0) && close(thirdNode.Y(), 0.0),
      "the third sparse node ID retains its coordinates");

    int node1 = 0;
    int node2 = 0;
    int node3 = 0;
    triangulation->Triangle(1).Get(node1, node2, node3);
    expect(node1 == 1 && node2 == 2 && node3 == 3,
      "triangle connectivity is remapped to OCCT one-based node indices");
    expect(triangulation->HasNormals(), "surface normals are computed for shaded display");

    occ::handle<AIS_Triangulation> presentation = new AIS_Triangulation(triangulation);
    cceda::visualization::configureOccSurfacePresentation(presentation);
    expect(presentation->Attributes()->HasOwnShadingAspect(),
      "the mesh surface presentation owns its shading style");
    const Quantity_Color expectedSurfaceColor(
      0.10, 0.55, 0.70, Quantity_TOC_sRGB);
    expect(presentation->Attributes()->ShadingAspect()->Color().Distance(
        expectedSurfaceColor) < 1.0e-12,
      "the mesh surface presentation uses the dedicated mesh color");
    expect(presentation->Attributes()->ShadingAspect()->Aspect()->ToDrawEdges(),
      "the mesh surface presentation draws triangle edges");
  }

  const occ::handle<Poly_Triangulation> triangleLocator =
    cceda::visualization::buildOccElementTriangulation(
      makeCommittedMesh(), 1001);
  expect(!triangleLocator.IsNull()
      && triangleLocator->NbNodes() == 3
      && triangleLocator->NbTriangles() == 1,
    "a Triangle3 issue produces one exact locator face");
  const occ::handle<Poly_Triangulation> tetrahedronLocator =
    cceda::visualization::buildOccElementTriangulation(
      makeCommittedMesh(), 1002);
  expect(!tetrahedronLocator.IsNull()
      && tetrahedronLocator->NbNodes() == 4
      && tetrahedronLocator->NbTriangles() == 4,
    "a Tetrahedron4 issue produces its four exact locator faces");
  if (!tetrahedronLocator.IsNull())
  {
    occ::handle<AIS_Triangulation> highlight =
      new AIS_Triangulation(tetrahedronLocator);
    cceda::visualization::configureOccElementHighlight(highlight);
    expect(highlight->ZLayer() == Graphic3d_ZLayerId_Topmost,
      "issue highlight uses independent depth so interior elements remain visible");
  }
  const cceda::visualization::OccElementTriangulations issueLocators =
    cceda::visualization::buildOccElementTriangulations(
      makeCommittedMesh(), {1001, 1002, 9999, 1001});
  const occ::handle<Poly_Triangulation> issueTriangulation =
    cceda::visualization::buildOccIssueTriangulation(issueLocators);
  expect(!issueTriangulation.IsNull()
      && issueTriangulation->NbNodes() == 7
      && issueTriangulation->NbTriangles() == 5,
    "accepted issue locators combine into one bounded issue-only presentation");
  expect(cceda::visualization::buildOccIssueTriangulation({}).IsNull(),
    "an empty accepted issue set has no issue-only presentation");
  expect(cceda::visualization::buildOccElementTriangulation(
      makeCommittedMesh(), 9999).IsNull(),
    "an unknown element ID cannot produce locator geometry");

  const auto repository = std::make_shared<cceda::geometry::OccGeometryRepository>();
  cceda::visualization::OccViewportController controller(repository);
  const cceda::visualization::PreparedMeshPresentationPtr prepared =
    controller.prepareMeshPresentation(makeCommittedMesh(), {1001, 1002, 9999});
  expect(prepared != nullptr && prepared->meshRevision() == 7
      && prepared->sourceGeometryRevision() == 3 && prepared->triangleCount() == 1
      && prepared->locatableElementCount() == 2,
    "the opaque viewport contract retains exact revision and surface metadata");
  expect(!controller.displayMeshPresentation(prepared),
    "display fails closed before the viewport has a native window");

  cceda::domain::MeshModel invalidMesh = makeCommittedMesh();
  invalidMesh.elements.front().nodeIds[0] = 999999;
  expect(buildOccSurfaceTriangulation(invalidMesh).IsNull(),
    "an invalid committed mesh is rejected before conversion");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
