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
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
namespace domain = cceda::domain;
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
  entity.kind = node.IsAssembly ? GeometryKind::Assembly : GeometryKind::Part;
  entity.valid = !shape.IsNull() && BRepCheck_Analyzer(shape).IsValid();
  entity.meshable = false;

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

std::string topologyId(domain::GeometryRevision revision,
                       const std::string& ownerId,
                       const char* kind,
                       int index)
{
  return "geometry/" + std::to_string(revision) + "/" + ownerId + "/"
    + kind + "/" + std::to_string(index);
}

GeometryEntity makeTopologyEntity(const TopoDS_Shape& shape,
                                  GeometryKind kind,
                                  std::string id,
                                  std::string parentId,
                                  std::string name,
                                  const domain::GeometryColor& color)
{
  GeometryEntity entity;
  entity.id = std::move(id);
  entity.parentId = std::move(parentId);
  entity.name = std::move(name);
  entity.kind = kind;
  entity.color = color;
  entity.solidCount = uniqueSubshapeCount(shape, TopAbs_SOLID);
  entity.faceCount = uniqueSubshapeCount(shape, TopAbs_FACE);
  entity.edgeCount = uniqueSubshapeCount(shape, TopAbs_EDGE);
  entity.vertexCount = uniqueSubshapeCount(shape, TopAbs_VERTEX);
  entity.valid = BRepCheck_Analyzer(shape).IsValid();
  entity.meshable = kind == GeometryKind::Solid && entity.valid;
  if (kind == GeometryKind::Solid)
  {
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    entity.volume = std::abs(properties.Mass());
  }
  return entity;
}

void appendSolidAndFaceEntities(domain::GeometryDocument& document,
                                const GeometryEntity& owner,
                                const TopoDS_Shape& shape,
                                std::unordered_map<std::string, TopoDS_Shape>& topologyShapes)
{
  NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> solids;
  NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> faces;
  TopExp::MapShapes(shape, TopAbs_SOLID, solids);
  TopExp::MapShapes(shape, TopAbs_FACE, faces);

  std::vector<std::string> solidIds(static_cast<std::size_t>(solids.Extent()) + 1);
  for (int index = 1; index <= solids.Extent(); ++index)
  {
    const TopoDS_Shape& solid = solids.FindKey(index);
    solidIds[static_cast<std::size_t>(index)] =
      topologyId(document.revision, owner.id, "solid", index);
    topologyShapes.emplace(solidIds[static_cast<std::size_t>(index)], solid);
    document.entities.push_back(makeTopologyEntity(
      solid,
      GeometryKind::Solid,
      solidIds[static_cast<std::size_t>(index)],
      owner.id,
      "Solid " + std::to_string(index),
      owner.color));
  }

  NCollection_IndexedDataMap<TopoDS_Shape,
    NCollection_List<TopoDS_Shape>, TopTools_ShapeMapHasher> faceAncestors;
  TopExp::MapShapesAndAncestors(shape, TopAbs_FACE, TopAbs_SOLID, faceAncestors);
  for (int index = 1; index <= faces.Extent(); ++index)
  {
    const TopoDS_Shape& face = faces.FindKey(index);
    std::string parentId = owner.id;
    if (faceAncestors.Contains(face))
    {
      const NCollection_List<TopoDS_Shape>& ancestors =
        faceAncestors.FindFromKey(face);
      if (!ancestors.IsEmpty())
      {
        const int solidIndex = solids.FindIndex(ancestors.First());
        if (solidIndex > 0)
        {
          parentId = solidIds[static_cast<std::size_t>(solidIndex)];
        }
      }
    }
    document.entities.push_back(makeTopologyEntity(
      face,
      GeometryKind::Face,
      topologyId(document.revision, owner.id, "face", index),
      std::move(parentId),
      "Face " + std::to_string(index),
      owner.color));
    topologyShapes.emplace(document.entities.back().id, face);
  }
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
    std::unordered_map<std::string, TopoDS_Shape> topologyShapes;
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
        appendSolidAndFaceEntities(domainDocument, entity, shape, topologyShapes);

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

    repository_->replace(
      document, domainDocument, std::move(records), std::move(topologyShapes));
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
