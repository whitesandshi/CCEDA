#include "OccGeometryService.h"

#include "OccGeometryRepository.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BinXCAFDrivers_DocumentRetrievalDriver.hxx>
#include <BinXCAFDrivers_DocumentStorageDriver.hxx>
#include <GProp_GProps.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <PCDM_ReadWriter.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPConstruct_UnitContext.hxx>
#include <Standard_Version.hxx>
#include <StepBasic_ConversionBasedUnitAndLengthUnit.hxx>
#include <StepBasic_SiUnitAndLengthUnit.hxx>
#include <StepData_StepModel.hxx>
#include <TDF_Tool.hxx>
#include <NCollection_IndexedMap.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDataStd_Name.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <UnitsMethods.hxx>
#include <TDocStd_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFPrs_DocumentExplorer.hxx>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
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
using ShapeAncestors = NCollection_IndexedDataMap<TopoDS_Shape,
  NCollection_List<TopoDS_Shape>, TopTools_ShapeMapHasher>;
using ShapeMap = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;

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

std::string firstAncestorId(const TopoDS_Shape& shape,
                            const ShapeAncestors& ancestors,
                            const ShapeMap& parentShapes,
                            const std::vector<std::string>& parentIds,
                            const std::string& fallback)
{
  if (!ancestors.Contains(shape))
  {
    return fallback;
  }
  const NCollection_List<TopoDS_Shape>& candidates = ancestors.FindFromKey(shape);
  if (candidates.IsEmpty())
  {
    return fallback;
  }
  const int parentIndex = parentShapes.FindIndex(candidates.First());
  return parentIndex > 0 ? parentIds[static_cast<std::size_t>(parentIndex)] : fallback;
}

void appendTopologyEntities(domain::GeometryDocument& document,
                            const GeometryEntity& owner,
                            const TopoDS_Shape& shape,
                            std::unordered_map<std::string, TopoDS_Shape>& topologyShapes)
{
  ShapeMap solids;
  ShapeMap faces;
  ShapeMap edges;
  ShapeMap vertices;
  TopExp::MapShapes(shape, TopAbs_SOLID, solids);
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);

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

  ShapeAncestors faceAncestors;
  TopExp::MapShapesAndAncestors(shape, TopAbs_FACE, TopAbs_SOLID, faceAncestors);
  std::vector<std::string> faceIds(static_cast<std::size_t>(faces.Extent()) + 1);
  for (int index = 1; index <= faces.Extent(); ++index)
  {
    const TopoDS_Shape& face = faces.FindKey(index);
    std::string parentId = firstAncestorId(
      face, faceAncestors, solids, solidIds, owner.id);
    faceIds[static_cast<std::size_t>(index)] =
      topologyId(document.revision, owner.id, "face", index);
    document.entities.push_back(makeTopologyEntity(
      face,
      GeometryKind::Face,
      faceIds[static_cast<std::size_t>(index)],
      std::move(parentId),
      "Face " + std::to_string(index),
      owner.color));
    topologyShapes.emplace(document.entities.back().id, face);
  }

  ShapeAncestors edgeAncestors;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeAncestors);
  std::vector<std::string> edgeIds(static_cast<std::size_t>(edges.Extent()) + 1);
  for (int index = 1; index <= edges.Extent(); ++index)
  {
    const TopoDS_Shape& edge = edges.FindKey(index);
    edgeIds[static_cast<std::size_t>(index)] =
      topologyId(document.revision, owner.id, "edge", index);
    document.entities.push_back(makeTopologyEntity(
      edge,
      GeometryKind::Edge,
      edgeIds[static_cast<std::size_t>(index)],
      firstAncestorId(edge, edgeAncestors, faces, faceIds, owner.id),
      "Edge " + std::to_string(index),
      owner.color));
    topologyShapes.emplace(document.entities.back().id, edge);
  }

  ShapeAncestors vertexAncestors;
  TopExp::MapShapesAndAncestors(shape, TopAbs_VERTEX, TopAbs_EDGE, vertexAncestors);
  for (int index = 1; index <= vertices.Extent(); ++index)
  {
    const TopoDS_Shape& vertex = vertices.FindKey(index);
    const std::string id = topologyId(document.revision, owner.id, "vertex", index);
    document.entities.push_back(makeTopologyEntity(
      vertex,
      GeometryKind::Vertex,
      id,
      firstAncestorId(vertex, vertexAncestors, edges, edgeIds, owner.id),
      "Vertex " + std::to_string(index),
      owner.color));
    topologyShapes.emplace(id, vertex);
  }
}

struct OccPreparedGeometry final : cceda::geometry::PreparedProjectGeometry
{
  occ::handle<TDocStd_Document> xcaf;
  std::vector<cceda::geometry::OccShapeRecord> records;
  std::unordered_map<std::string, TopoDS_Shape> topologyShapes;
};

std::shared_ptr<OccPreparedGeometry> describeDocument(
  const occ::handle<TDocStd_Document>& document, domain::GeometryRevision revision,
  const std::string& sourceName)
{
  using cceda::geometry::OccShapeRecord;
  auto prepared = std::make_shared<OccPreparedGeometry>();
  prepared->xcaf = document;
  auto& domainDocument = prepared->document;
  domainDocument.revision = revision;
  domainDocument.sourceName = sourceName;

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
    TCollection_AsciiString definitionLabel;
    TDF_Tool::Entry(node.RefLabel, definitionLabel);
    domain::GeometryDocument::LabelBinding binding{entity.id, node.Id.ToCString(),
      definitionLabel.ToCString(), parentId, entity.name, entity.color, node.Style.IsVisible()};
    const gp_Trsf transform = node.LocalTrsf.Transformation();
    for (int row = 1; row <= 3; ++row)
    {
      for (int column = 1; column <= 4; ++column)
      {
        binding.localTransform[static_cast<std::size_t>((row - 1) * 4 + (column - 1))] =
          transform.Value(row, column);
      }
    }
    domainDocument.xcafLabels.push_back(std::move(binding));

    if (!node.IsAssembly && !shape.IsNull())
    {
      domainDocument.solidCount += entity.solidCount;
      domainDocument.faceCount += entity.faceCount;
      domainDocument.edgeCount += entity.edgeCount;
      domainDocument.vertexCount += entity.vertexCount;
      appendTopologyEntities(domainDocument, entity, shape, prepared->topologyShapes);

      OccShapeRecord record;
      record.id = entity.id;
      record.shape = shape;
      record.color = Quantity_Color(
        entity.color.red, entity.color.green, entity.color.blue, Quantity_TOC_sRGB);
      prepared->records.push_back(std::move(record));
    }
  }

  return prepared->records.empty() ? nullptr : prepared;
}

std::vector<domain::GeometryDocument::SourceLengthUnit> sourceUnits(STEPControl_Reader& reader)
{
  std::set<std::pair<std::string, double>> units;
  const auto model = reader.StepModel();
  StepData_Factors factors;
  factors.SetCascadeUnit(1.0);
  for (int index = 1; index <= model->NbEntities(); ++index)
  {
    const auto entity = model->Value(index);
    const auto si = occ::down_cast<StepBasic_SiUnitAndLengthUnit>(entity);
    const auto conversion = occ::down_cast<StepBasic_ConversionBasedUnitAndLengthUnit>(entity);
    if (si.IsNull() && conversion.IsNull())
    {
      continue;
    }
    STEPConstruct_UnitContext context;
    const auto named = occ::down_cast<StepBasic_NamedUnit>(entity);
    if (context.ComputeFactors(named, factors) != 0 || !context.LengthDone()
      || !std::isfinite(context.LengthFactor()) || context.LengthFactor() <= 0.0)
    {
      continue;
    }
    const char* name = UnitsMethods::DumpLengthUnit(context.LengthFactor());
    if (!conversion.IsNull() && !conversion->Name().IsNull() && !conversion->Name()->String().IsEmpty())
    {
      name = conversion->Name()->ToCString();
    }
    if (!name || !*name) { name = "SI metre (scaled)"; }
    units.emplace(name, context.LengthFactor());
  }
  std::vector<domain::GeometryDocument::SourceLengthUnit> result;
  for (const auto& [name, scale] : units)
  {
    result.push_back({name, scale});
  }
  return result;
}
}

namespace cceda::geometry
{
OccGeometryService::OccGeometryService(std::shared_ptr<OccGeometryRepository> repository)
    : repository_(std::move(repository))
{
}

domain::GeometryImportResult OccGeometryService::importStep(const std::filesystem::path& path,
  const std::string& sourceName)
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
    const occ::handle<TDocStd_Document> document = new TDocStd_Document("BinXCAF");
    document->Open(new TDocStd_Application());
    XCAFDoc_DocumentTool::Set(document->Main());
    XCAFDoc_DocumentTool::SetLengthUnit(document, 1.0, UnitsMethods_LengthUnit_Millimeter);

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

    auto prepared = describeDocument(document, repository_->nextRevision(),
      sourceName.empty() ? path.filename().u8string() : sourceName);
    if (!prepared)
    {
      result.errorCode = "GEOMETRY_NO_DISPLAYABLE_SHAPES";
      result.message = "The STEP file contains no displayable geometry.";
      return result;
    }
    prepared->document.sourceLengthUnits = sourceUnits(reader.ChangeReader());
    result.document = prepared->document;
    activate(std::move(prepared));
    result.success = true;
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

std::string OccGeometryService::writerVersion() const
{
  return OCC_VERSION_STRING_EXT;
}

std::optional<domain::GeometryDocument> OccGeometryService::writeXcaf(domain::GeometryRevision revision,
  const std::filesystem::path& path) const
{
  try
  {
    auto snapshot = repository_->xcafDocument(revision);
    std::ofstream stream(path, std::ios::binary);
    if (snapshot.xcaf.IsNull() || !stream)
    {
      return {};
    }
    // Write the binary document without changing OCCT's saved flag or session.
    BinXCAFDrivers_DocumentStorageDriver writer;
    writer.SetFormat("BinXCAF");
    writer.SetIsError(false);
    writer.SetStoreStatus(PCDM_SS_OK);
    writer.Write(snapshot.xcaf, stream, Message_ProgressRange());
    stream.close();
    if (writer.GetStoreStatus() == PCDM_SS_OK && stream.good())
    {
      return std::move(snapshot.document);
    }
  }
  catch (const Standard_Failure&)
  {
  }
  return {};
}

std::shared_ptr<PreparedProjectGeometry> OccGeometryService::prepareXcaf(
  const std::filesystem::path& path, domain::GeometryRevision revision,
  const std::string& sourceName) const
{
  try
  {
    std::ifstream stream(path, std::ios::binary);
    occ::handle<Storage_Data> storage;
    if (!stream || PCDM_ReadWriter::FileFormat(stream, storage) != "BinXCAF")
    {
      return {};
    }
    const occ::handle<TDocStd_Document> document = new TDocStd_Document("BinXCAF");
    const occ::handle<TDocStd_Application> application = new TDocStd_Application();
    document->Open(application);
    BinXCAFDrivers_DocumentRetrievalDriver reader;
    reader.Read(stream, storage, document, application);
    double lengthUnit = 0.0;
    if (reader.GetStatus() != PCDM_RS_OK || stream.bad()
      || !XCAFDoc_DocumentTool::GetLengthUnit(document, lengthUnit, UnitsMethods_LengthUnit_Millimeter)
      || lengthUnit != 1.0)
    {
      return {};
    }
    return describeDocument(document, revision, sourceName);
  }
  catch (const Standard_Failure&)
  {
    return {};
  }
}

void OccGeometryService::activate(std::shared_ptr<PreparedProjectGeometry> prepared)
{
  if (!prepared)
  {
    repository_->replace({}, {}, {}, {});
    return;
  }
  auto geometry = std::dynamic_pointer_cast<OccPreparedGeometry>(prepared);
  if (geometry)
  {
    repository_->replace(geometry->xcaf, std::move(geometry->document),
      std::move(geometry->records), std::move(geometry->topologyShapes));
  }
}

std::shared_ptr<IGeometryService> createOccGeometryService()
{
  return std::make_shared<OccGeometryService>(std::make_shared<OccGeometryRepository>());
}
}
