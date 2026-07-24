#include "meshing/GmshMsh41Reader.h"

#include <iostream>
#include <sstream>
#include <string>
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

std::string validMsh()
{
  return R"msh($MeshFormat
4.1 0 8
$EndMeshFormat
$Entities
0 0 0 0
$EndEntities
$Nodes
2 4 10 40
2 20 0 3
10
20
30
0 0 0
1 0 0
0 1 0
3 30 0 1
40
0 0 1
$EndNodes
$Elements
4 4 1 400
0 1 15 1
1 10
1 2 1 1
2 10 20
2 20 2 1
300 10 20 30
3 30 4 1
400 10 20 30 40
$EndElements
)msh";
}

cceda::meshing::GmshMeshMetadata metadata()
{
  return {
    8,
    "2026-07-24T00:00:00.000Z",
    "4.15.2",
    cceda::domain::MeshConformity::NonConformal,
  };
}

cceda::meshing::GmshMeshMapping mapping()
{
  using namespace cceda;

  meshing::GmshMeshMapping value;
  value.entities = {
    {2, 20, 7, "solid-A/face-20"},
    {3, 30, 7, "solid-A"},
  };
  value.regions = {
    {7, {"solid-A"}},
  };
  return value;
}

cceda::meshing::GmshMsh41ReadLimits limits()
{
  return {
    1024 * 1024,
    4096,
    100,
    100,
    100,
  };
}

cceda::meshing::GmshMsh41ReadResult read(
  const std::string& text,
  const cceda::meshing::GmshMeshMapping& meshMapping,
  const cceda::meshing::GmshMsh41ReadLimits& readLimits)
{
  std::istringstream input(text);
  return cceda::meshing::readGmshMsh41(
    input, metadata(), meshMapping, readLimits);
}

void replaceOnce(std::string& text, std::string_view before, std::string_view after)
{
  const std::size_t position = text.find(before);
  if (position != std::string::npos)
  {
    text.replace(position, before.size(), after);
  }
}
}

int main()
{
  using namespace cceda;
  using meshing::GmshMsh41ErrorCode;

  const meshing::GmshMsh41ReadResult valid = read(validMsh(), mapping(), limits());
  expect(valid.succeeded(), "a bounded canonical ASCII MSH 4.1 file is converted");
  expect(valid.mesh.has_value(), "a successful read returns a neutral mesh");
  if (valid.mesh)
  {
    expect(valid.mesh->meshRevision == 0, "a converted Worker mesh is uncommitted");
    expect(valid.mesh->sourceGeometryRevision == 8,
      "source geometry revision comes from trusted metadata");
    expect(valid.mesh->backendId == "gmsh", "backend identity is fixed by the adapter");
    expect(valid.mesh->nodes.size() == 4, "all MSH nodes are retained");
    expect(valid.mesh->elements.size() == 2,
      "point and line elements are excluded from the neutral mesh");
    expect(valid.mesh->elements.at(0).type == domain::MeshElementType::Triangle3,
      "MSH element type 2 becomes Triangle3");
    expect(valid.mesh->elements.at(0).geometryId == "solid-A/face-20",
      "surface elements receive the stable Face geometry ID");
    expect(valid.mesh->elements.at(1).type == domain::MeshElementType::Tetrahedron4,
      "MSH element type 4 becomes Tetrahedron4");
    expect(valid.mesh->elements.at(1).geometryId == "solid-A",
      "volume elements receive the stable Solid geometry ID");
    expect(domain::validateMeshModel(
        *valid.mesh, domain::MeshValidationMode::WorkerArtifact).valid(),
      "the converted model satisfies the domain Worker contract");
  }

  meshing::GmshMsh41ReadLimits inputLimit = limits();
  inputLimit.maximumInputBytes = 32;
  expect(read(validMsh(), mapping(), inputLimit).error
      == GmshMsh41ErrorCode::InputLimitExceeded,
    "input bytes are bounded before untrusted data can grow memory");

  meshing::GmshMsh41ReadLimits nodeLimit = limits();
  nodeLimit.maximumNodes = 3;
  expect(read(validMsh(), mapping(), nodeLimit).error
      == GmshMsh41ErrorCode::CountLimitExceeded,
    "declared node counts must fit the caller budget");

  std::string duplicateNode = validMsh();
  replaceOnce(duplicateNode, "10\n20\n30\n", "10\n20\n20\n");
  expect(read(duplicateNode, mapping(), limits()).error
      == GmshMsh41ErrorCode::DuplicateNodeTag,
    "duplicate node tags are rejected");

  std::string unsupportedSurface = validMsh();
  replaceOnce(unsupportedSurface, "2 20 2 1", "2 20 3 1");
  expect(read(unsupportedSurface, mapping(), limits()).error
      == GmshMsh41ErrorCode::UnsupportedElementType,
    "unsupported 2D element types are rejected");

  std::string unsupportedCurve = validMsh();
  replaceOnce(unsupportedCurve, "1 2 1 1", "1 2 99 1");
  expect(read(unsupportedCurve, mapping(), limits()).error
      == GmshMsh41ErrorCode::UnsupportedElementType,
    "unexpected auxiliary element types are rejected");

  std::string invalidCurveReference = validMsh();
  replaceOnce(invalidCurveReference, "2 10 20\n", "2 10 99\n");
  expect(read(invalidCurveReference, mapping(), limits()).error
      == GmshMsh41ErrorCode::MeshValidationFailed,
    "auxiliary element node references are validated");

  meshing::GmshMeshMapping missingMapping = mapping();
  missingMapping.entities.erase(missingMapping.entities.begin());
  expect(read(validMsh(), missingMapping, limits()).error
      == GmshMsh41ErrorCode::EntityMappingMissing,
    "every retained element block requires a stable entity mapping");

  std::string missingNode = validMsh();
  replaceOnce(missingNode, "300 10 20 30", "300 10 20 99");
  expect(read(missingNode, mapping(), limits()).error
      == GmshMsh41ErrorCode::MeshValidationFailed,
    "element references are checked after conversion");

  std::string binary = validMsh();
  replaceOnce(binary, "4.1 0 8", "4.1 1 8");
  expect(read(binary, mapping(), limits()).error
      == GmshMsh41ErrorCode::UnsupportedFormat,
    "binary or non-4.1 input is rejected");

  if (failures == 0)
  {
    std::cout << "All Gmsh MSH 4.1 reader tests passed.\n";
  }
  return failures == 0 ? 0 : 1;
}
