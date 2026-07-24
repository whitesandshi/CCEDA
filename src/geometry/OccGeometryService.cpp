#include "OccGeometryService.h"

#include "OccGeometryRepository.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <NCollection_IndexedMap.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDataStd_Name.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFPrs_DocumentExplorer.hxx>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
using cceda::domain::GeometryEntity;
using cceda::domain::GeometryKind;

std::size_t uniqueSubshapeCount(const TopoDS_Shape& shape, TopAbs_ShapeEnum kind)
{
  NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> shapes;
  TopExp::MapShapes(shape, kind, shapes);
  return static_cast<std::size_t>(shapes.Extent());
}

std::string labelName(const TDF_Label& label)
{
  occ::handle<TDataStd_Name> nameAttribute;
  if (!label.FindAttribute(TDataStd_Name::GetID(), nameAttribute) || nameAttribute.IsNull()
      || nameAttribute->Get().IsEmpty())
  {
    return {};
  }

  const TCollection_AsciiString utf8(nameAttribute->Get());
  return utf8.ToCString();
}

std::string nodeName(const XCAFPrs_DocumentNode& node, std::size_t index)
{
  std::string name = labelName(node.Label);
  if (name.empty())
  {
    name = labelName(node.RefLabel);
  }
  if (name.empty())
  {
    name = node.IsAssembly ? "Assembly " : "Part ";
    name += std::to_string(index + 1);
  }
  return name;
}

GeometryKind shapeKind(const XCAFPrs_DocumentNode& node,
                       const TopoDS_Shape& shape,
                       std::size_t solidCount)
{
  if (node.IsAssembly)
  {
    return GeometryKind::Assembly;
  }
  if (solidCount > 1 || shape.ShapeType() == TopAbs_COMPOUND
      || shape.ShapeType() == TopAbs_COMPSOLID)
  {
    return GeometryKind::Part;
  }

  switch (shape.ShapeType())
  {
    case TopAbs_SOLID: return GeometryKind::Solid;
    case TopAbs_SHELL: return GeometryKind::Shell;
    case TopAbs_FACE: return GeometryKind::Face;
    case TopAbs_WIRE: return GeometryKind::Wire;
    case TopAbs_EDGE: return GeometryKind::Edge;
    case TopAbs_VERTEX: return GeometryKind::Vertex;
    default: return GeometryKind::Other;
  }
}

GeometryEntity makeEntity(const XCAFPrs_DocumentNode& node,
                          const TopoDS_Shape& shape,
                          const std::string& parentId,
                          std::size_t index)
{
  GeometryEntity entity;
  entity.id = node.Id.ToCString();
  entity.parentId = parentId;
  entity.name = nodeName(node, index);
  entity.solidCount = uniqueSubshapeCount(shape, TopAbs_SOLID);
  entity.faceCount = uniqueSubshapeCount(shape, TopAbs_FACE);
  entity.edgeCount = uniqueSubshapeCount(shape, TopAbs_EDGE);
  entity.vertexCount = uniqueSubshapeCount(shape, TopAbs_VERTEX);
  entity.kind = shapeKind(node, shape, entity.solidCount);
  entity.valid = !shape.IsNull() && BRepCheck_Analyzer(shape).IsValid();
  entity.meshable = entity.kind == GeometryKind::Solid && entity.valid;

  if (entity.solidCount > 0)
  {
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    entity.volume = std::abs(properties.Mass());
  }

  if (node.Style.IsSetColorSurf())
  {
    const Quantity_Color& color = node.Style.GetColorSurf();
    color.Values(entity.color.red, entity.color.green, entity.color.blue, Quantity_TOC_sRGB);
    entity.color.explicitColor = true;
  }
  return entity;
}
}

namespace cceda::geometry
{
OccGeometryService::OccGeometryService(std::shared_ptr<OccGeometryRepository> repository)
    : repository_(std::move(repository))
{
}

domain::GeometryImportResult OccGeometryService::importStep(const std::filesystem::path& path)
{
  domain::GeometryImportResult result;
  if (!std::filesystem::is_regular_file(path))
  {
    result.errorCode = "GEOMETRY_FILE_NOT_READABLE";
    result.message = "The selected STEP file is not readable.";
    return result;
  }

  try
  {
    occ::handle<TDocStd_Document> document;
    XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", document);

    STEPCAFControl_Reader reader;
    reader.SetColorMode(true);
    reader.SetNameMode(true);
    reader.SetLayerMode(true);
    reader.SetPropsMode(true);

    const std::string utf8Path = path.u8string();
    if (reader.ReadFile(utf8Path.c_str()) != IFSelect_RetDone)
    {
      result.errorCode = "GEOMETRY_STEP_READ_FAILED";
      result.message = "Open CASCADE could not read the STEP file.";
      return result;
    }
    if (!reader.Transfer(document))
    {
      result.errorCode = "GEOMETRY_XCAF_TRANSFER_FAILED";
      result.message = "The STEP model could not be transferred into XCAF.";
      return result;
    }

    domain::GeometryDocument domainDocument;
    domainDocument.revision = repository_->nextRevision();
    domainDocument.sourceName = path.filename().u8string();

    std::vector<OccShapeRecord> records;
    std::vector<std::string> idsByDepth;
    std::size_t nodeIndex = 0;
    for (XCAFPrs_DocumentExplorer explorer(document, XCAFPrs_DocumentExplorerFlags_None);
         explorer.More(); explorer.Next(), ++nodeIndex)
    {
      const XCAFPrs_DocumentNode& node = explorer.Current();
      const int depth = std::max(0, explorer.CurrentDepth());
      if (idsByDepth.size() <= static_cast<std::size_t>(depth))
      {
        idsByDepth.resize(static_cast<std::size_t>(depth) + 1);
      }
      const std::string parentId = depth > 0 ? idsByDepth[static_cast<std::size_t>(depth - 1)]
                                             : std::string();
      idsByDepth[static_cast<std::size_t>(depth)] = node.Id.ToCString();

      TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(node.RefLabel);
      if (!shape.IsNull())
      {
        shape.Location(node.Location);
      }

      GeometryEntity entity = makeEntity(node, shape, parentId, nodeIndex);
      domainDocument.entities.push_back(entity);

      if (!node.IsAssembly && !shape.IsNull())
      {
        domainDocument.solidCount += entity.solidCount;
        domainDocument.faceCount += entity.faceCount;
        domainDocument.edgeCount += entity.edgeCount;
        domainDocument.vertexCount += entity.vertexCount;

        OccShapeRecord record;
        record.id = entity.id;
        record.shape = shape;
        record.color = Quantity_Color(
          entity.color.red, entity.color.green, entity.color.blue, Quantity_TOC_sRGB);
        records.push_back(std::move(record));
      }
    }

    if (domainDocument.entities.empty() || records.empty())
    {
      result.errorCode = "GEOMETRY_NO_DISPLAYABLE_SHAPES";
      result.message = "The STEP file contains no displayable geometry.";
      return result;
    }

    repository_->replace(document, domainDocument, std::move(records));
    result.success = true;
    result.document = std::move(domainDocument);
    return result;
  }
  catch (const Standard_Failure& failure)
  {
    result.errorCode = "GEOMETRY_OCC_FAILURE";
    result.message = failure.what() != nullptr ? failure.what()
                                               : "Open CASCADE reported an error.";
    return result;
  }
}

std::shared_ptr<OccGeometryRepository> OccGeometryService::repository() const
{
  return repository_;
}

domain::GeometryDocument OccGeometryService::currentDocument() const
{
  return repository_->domainDocument();
}

std::shared_ptr<IGeometryService> createOccGeometryService()
{
  return std::make_shared<OccGeometryService>(std::make_shared<OccGeometryRepository>());
}
}
