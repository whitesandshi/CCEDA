#include "persistence/ProjectPackage.h"

#include "geometry/IGeometryService.h"
#include "meshing/MeshBinaryCodec.h"
#include "persistence/ProjectMeshQuality.h"
#include "persistence/ProjectZipArchive.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cceda::persistence
{
namespace
{
constexpr auto ProjectPath = "project.json";
constexpr auto HistoryPath = "geometry/healing-history.json";
constexpr auto SettingsPath = "mesh/settings.json";
constexpr auto SourcePath = "source/original.step";
constexpr auto XcafPath = "geometry/model.xbf";
constexpr auto MeshPath = "mesh/current.meshbin";
constexpr auto QualityPath = "mesh/quality.json";
constexpr auto PackageFeature = "project-snapshot-0.1";
constexpr auto EmptyHealingHistory = R"({"operations":[],"schema":"cceda.healing-history/0.1"})";
constexpr qint64 MaximumSourceBytes = 2LL * 1024 * 1024 * 1024;
constexpr qint64 MaximumXcafBytes = 1024LL * 1024 * 1024;
constexpr qint64 MaximumProjectJsonBytes = 8 * 1024 * 1024;
constexpr std::uint64_t MaximumRevision = (1ULL << 53) - 1;

std::filesystem::path nativePath(const QString& path)
{
  return std::filesystem::path(path.toStdWString());
}

QString utcNow()
{
  return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

bool validSourceName(const QString& name)
{
  return !name.isEmpty() && name.size() <= 1024 && name != "." && name != ".."
    && !name.contains('/') && !name.contains('\\') && !name.contains(':')
    && std::none_of(name.begin(), name.end(), [](QChar c) {
      return c.category() == QChar::Other_Control;
    });
}

struct BytesResult
{
  qint64 size;
  QString sha256;
};

// Copies only to fixed private filenames and verifies the entire input stream.
std::optional<BytesResult> streamResource(QIODevice& input, qint64 expected,
  qint64 maximum, QIODevice* output = nullptr)
{
  if (expected < 0 || expected > maximum) { return {}; }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  std::array<char, 64 * 1024> buffer;
  qint64 remaining = expected;
  while (remaining > 0)
  {
    const qint64 count = input.read(buffer.data(),
      std::min(remaining, static_cast<qint64>(buffer.size())));
    if (count <= 0 || (output && output->write(buffer.data(), count) != count))
    { return {}; }
    hash.addData(QByteArrayView(buffer.data(), count));
    remaining -= count;
  }
  char extra;
  if (input.read(&extra, 1) != 0 || !input.atEnd()) { return {}; }
  return BytesResult{expected, QString::fromLatin1(hash.result().toHex())};
}

ProjectZipOptions zipOptions()
{
  ProjectZipOptions options;
  options.resources.supportedFeatures = {PackageFeature};
  return options;
}

const ProjectManifestEntry* entryFor(const ProjectManifest& manifest, const QString& path)
{
  const auto entry = std::find_if(manifest.entries.cbegin(), manifest.entries.cend(),
    [&path](const auto& item) { return item.path == path; });
  return entry == manifest.entries.cend() ? nullptr : &*entry;
}

QJsonArray bindings(const domain::GeometryDocument& document)
{
  // Canonical metadata is ordered by persistent ID rather than tree traversal.
  std::vector<const domain::GeometryDocument::LabelBinding*> ordered;
  ordered.reserve(document.xcafLabels.size());
  for (const auto& binding : document.xcafLabels) { ordered.push_back(&binding); }
  std::sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) {
    return a->geometryId < b->geometryId;
  });
  QJsonArray entries;
  for (const auto* binding : ordered)
  {
    QJsonArray transform;
    for (double value : binding->localTransform) { transform.append(value); }
    entries.append(QJsonObject{
      {"id", QString::fromStdString(binding->geometryId)},
      {"instancePath", QString::fromStdString(binding->instancePath)},
      {"definitionLabel", QString::fromStdString(binding->definitionLabel)},
      {"parentId", QString::fromStdString(binding->parentGeometryId)},
      {"name", QString::fromStdString(binding->name)}, {"visible", binding->visible},
      {"localTransform", transform},
      {"surfaceColor", QJsonArray{binding->color.red, binding->color.green,
        binding->color.blue, binding->color.explicitColor}}
    });
  }
  return entries;
}

QJsonObject projectJson(const ProjectManifest& manifest, domain::GeometryRevision revision,
  domain::MeshRevision lastMeshRevision, bool hasMesh,
  const domain::GeometryDocument* document)
{
  QJsonObject source;
  QJsonObject geometry;
  if (document)
  {
    QJsonArray units;
    for (const auto& unit : document->sourceLengthUnits)
    {
      units.append(QJsonObject{{"name", QString::fromStdString(unit.name)},
        {"millimetersPerUnit", unit.millimetersPerUnit}});
    }
    source = {{"resource", SourcePath}, {"name", QString::fromStdString(document->sourceName)},
      {"sha256", entryFor(manifest, SourcePath)->sha256}, {"lengthUnits", units}};
    geometry = {{"resource", XcafPath}, {"xcafLabels", bindings(*document)}};
  }
  return {{"schema", "cceda.project-snapshot/0.1"}, {"projectId", manifest.projectId},
    {"createdAt", manifest.createdAt}, {"modifiedAt", manifest.modifiedAt},
    {"geometryRevision", static_cast<qint64>(revision)},
    {"lastMeshRevision", static_cast<qint64>(lastMeshRevision)},
    {"source", source}, {"geometry", geometry}, {"settings", SettingsPath},
    {"healingHistory", HistoryPath},
    {"mesh", hasMesh ? QJsonObject{{"state", "ready"}, {"resource", MeshPath},
      {"quality", QualityPath}} : QJsonObject{{"state", "missing"}}}};
}

bool validMapping(const domain::MeshModel& mesh, const domain::GeometryDocument& document)
{
  if (mesh.sourceGeometryRevision != document.revision) { return false; }
  std::unordered_set<std::string_view> solids;
  for (const auto& entity : document.entities)
  {
    if (entity.kind == domain::GeometryKind::Solid && entity.meshable)
    { solids.insert(entity.id); }
  }
  using GeometryIds = std::unordered_set<std::string_view>;
  std::unordered_map<domain::MeshRegionId, GeometryIds> regions;
  std::unordered_set<std::string_view> mappedSolids;
  for (const auto& region : mesh.regions)
  {
    const auto [mapping, inserted] = regions.try_emplace(region.id);
    if (!inserted || region.sourceGeometryIds.empty())
    { return false; }
    for (const auto& id : region.sourceGeometryIds)
    {
      if (!solids.count(id) || !mapping->second.insert(id).second) { return false; }
      mappedSolids.insert(id);
    }
  }
  for (const auto& node : mesh.nodes)
  {
    if (!node.geometryId.empty() && !mappedSolids.count(node.geometryId)) { return false; }
  }
  return std::all_of(mesh.elements.begin(), mesh.elements.end(), [&regions](const auto& element) {
    const auto region = regions.find(element.regionId);
    return region != regions.end() && region->second.count(element.geometryId) != 0;
  });
}

bool copyEntry(const ProjectZipReader& archive, const QString& name, qint64 maxBytes,
  const QString& destination)
{
  const auto* entry = entryFor(archive.manifest(), name);
  if (!entry || entry->uncompressedBytes == 0
    || entry->uncompressedBytes > static_cast<std::uint64_t>(maxBytes)) { return false; }
  auto input = archive.openResource(name);
  QFile output(destination);
  if (!input.device || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { return false; }
  const auto copy = streamResource(*input.device,
    static_cast<qint64>(entry->uncompressedBytes), maxBytes, &output);
  return copy && copy->sha256 == entry->sha256 && output.flush();
}

bool readExact(QIODevice& input, char* bytes, qint64 size)
{
  qint64 received = 0;
  while (received < size)
  {
    const auto count = input.read(bytes + received, size - received);
    if (count <= 0) { return false; }
    received += count;
  }
  char extra;
  return input.read(&extra, 1) == 0 && input.atEnd();
}

std::optional<QByteArray> loadBytes(const ProjectZipReader& archive, const QString& name, qint64 maxBytes)
{
  const auto* entry = entryFor(archive.manifest(), name);
  if (!entry || entry->uncompressedBytes == 0
    || entry->uncompressedBytes > static_cast<std::uint64_t>(maxBytes)) { return {}; }
  auto input = archive.openResource(name);
  if (!input.device) { return {}; }
  QByteArray bytes(static_cast<qsizetype>(entry->uncompressedBytes), Qt::Uninitialized);
  return readExact(*input.device, bytes.data(), bytes.size())
    ? std::optional<QByteArray>(std::move(bytes)) : std::nullopt;
}

std::shared_ptr<const domain::MeshModel> loadMesh(const ProjectZipReader& archive)
{
  const auto* entry = entryFor(archive.manifest(), MeshPath);
  if (!entry || entry->uncompressedBytes < meshing::MeshBinaryHeaderBytes
    || entry->uncompressedBytes > meshing::MeshBinaryLimits{}.maximumFileBytes) { return {}; }
  auto input = archive.openResource(MeshPath);
  if (!input.device) { return {}; }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(entry->uncompressedBytes));
  if (!readExact(*input.device, reinterpret_cast<char*>(bytes.data()),
    static_cast<qint64>(bytes.size()))) { return {}; }
  auto decoded = meshing::readMeshBinary(bytes, domain::MeshValidationMode::CommittedMesh);
  return decoded.succeeded() ? std::make_shared<const domain::MeshModel>(std::move(*decoded.mesh))
                             : nullptr;
}

std::shared_ptr<const application::AcceptedMeshQuality> loadQuality(
  const ProjectZipReader& archive, const domain::MeshModel& mesh)
{
  const auto bytes = loadBytes(archive, QualityPath, meshing::DefaultMeshQualityJsonMaximumBytes);
  if (!bytes) { return {}; }
  const auto verified = readProjectMeshQuality(*bytes, mesh);
  if (!verified.succeeded()) { return {}; }
  auto details = mesh_quality::buildMeshQualityIssueDetails(mesh, *verified.report);
  QBuffer device;
  device.setData(*bytes);
  device.open(QIODevice::ReadOnly);
  auto decoded = meshing::readCommittedMeshQualityJson(device);
  if (!decoded.succeeded() || !details) { return {}; }
  return application::makeAcceptedMeshQuality(std::move(*decoded.artifact),
    std::move(*details), mesh.meshRevision);
}

std::optional<std::uint64_t> jsonRevision(const QJsonValue& value)
{
  const qint64 revision = value.toInteger(-1);
  if (revision < 0 || static_cast<std::uint64_t>(revision) > MaximumRevision) { return {}; }
  return static_cast<std::uint64_t>(revision);
}

bool validSourceUnit(const QString& name, double scale)
{
  return !name.isEmpty() && name.size() <= 1024 && std::isfinite(scale) && scale > 0.0;
}

bool loadSourceUnits(const QJsonValue& value, domain::GeometryDocument& document)
{
  if (!value.isArray() || value.toArray().size() > 1024) { return false; }
  for (const auto& item : value.toArray())
  {
    const auto unit = item.toObject();
    const QString name = unit.value("name").toString();
    const double scale = unit.value("millimetersPerUnit").toDouble();
    if (!validSourceUnit(name, scale)) { return false; }
    document.sourceLengthUnits.push_back({name.toStdString(), scale});
  }
  return true;
}
}

ProjectIdentity newProjectIdentity()
{
  return {QUuid::createUuid().toString(QUuid::WithoutBraces), utcNow()};
}

QString ProjectSource::path() const
{
  return workspace ? workspace->filePath("original.step") : QString{};
}

ProjectImportResult importProjectStep(const QString& path, geometry::IGeometryService& geometry)
{
  ProjectSource source{std::make_shared<QTemporaryDir>(), QFileInfo(path).fileName(), {}};
  const auto failed = [] {
    return ProjectImportResult{{false, {}, "GEOMETRY_SOURCE_COPY_FAILED",
      "The original STEP could not be retained for project saving."}, {}};
  };
  if (!source.workspace->isValid() || !validSourceName(source.name)) { return failed(); }
  QFile input(path);
  QFile output(source.path());
  if (!input.open(QIODevice::ReadOnly) || input.size() == 0
    || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
  { return failed(); }
  const auto retained = streamResource(input, input.size(), MaximumSourceBytes, &output);
  if (!retained || !output.flush()) { return failed(); }
  source.sha256 = retained->sha256;
  output.close();
  auto result = geometry.importStep(nativePath(source.path()), source.name.toStdString());
  return {std::move(result), std::move(source)};
}

std::optional<ProjectSaveSnapshot> ProjectSaveSnapshot::capture(
  const domain::ProjectState& state, const ProjectIdentity& identity,
  const ProjectSource& source, const StoredMeshSettings& settings)
{
  if (!state.isOwnerThread() || ((state.geometryRevision() > 0) != bool(source.workspace)))
  { return {}; }
  ProjectSaveSnapshot snapshot;
  snapshot.identity_ = identity;
  snapshot.source_ = source;
  snapshot.settings_ = settings;
  snapshot.geometryRevision_ = state.geometryRevision();
  snapshot.lastMeshRevision_ = state.meshRevision();
  if (const auto current = state.captureCurrentMeshForExport()) { snapshot.mesh_ = current->mesh; }
  return snapshot;
}

QString saveProjectPackage(const QString& path, const ProjectSaveSnapshot& snapshot,
  const geometry::IProjectGeometryStorage& storage, const QString& createdBy)
{
  if (snapshot.geometryRevision_ > MaximumRevision || snapshot.lastMeshRevision_ > MaximumRevision)
  { return QObject::tr("The project revisions exceed the package format limits."); }

  ProjectManifest manifest;
  manifest.projectId = snapshot.identity_.id;
  manifest.createdBy = createdBy;
  manifest.createdAt = snapshot.identity_.createdAt;
  manifest.modifiedAt = utcNow();
  manifest.occtVersion = QString::fromStdString(storage.writerVersion());
  manifest.requiredFeatures = {PackageFeature};
  QHash<QString, QByteArray> data;
  QHash<QString, QString> files;
  const auto addBytes = [&](const QString& name, const QString& role, QByteArray bytes) {
    manifest.entries.push_back({name, role, static_cast<std::uint64_t>(bytes.size()),
      QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())});
    data.insert(name, std::move(bytes));
  };
  const auto addFile = [&](const QString& name, const QString& role, const QString& from, qint64 max,
    const QString& expectedHash = QString{}) {
    QFile file(from);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0) { return false; }
    const auto content = streamResource(file, file.size(), max);
    if (!content || (!expectedHash.isEmpty() && expectedHash != content->sha256)) { return false; }
    manifest.entries.push_back({name, role, static_cast<std::uint64_t>(content->size), content->sha256});
    files.insert(name, from);
    return true;
  };

  const auto settings = writeMeshSettingsJson(snapshot.settings_);
  if (!settings.succeeded()) { return QObject::tr("The mesh settings are invalid."); }
  addBytes(SettingsPath, "mesh-settings", settings.bytes);
  addBytes(HistoryPath, "healing-history", EmptyHealingHistory);

  QTemporaryDir work;
  std::optional<domain::GeometryDocument> document;
  if (snapshot.geometryRevision_)
  {
    if (!work.isValid()) { return QObject::tr("Could not create a project workspace."); }
    const auto xbf = work.filePath("model.xbf");
    document = storage.writeXcaf(snapshot.geometryRevision_, nativePath(xbf));
    if (!document || document->revision != snapshot.geometryRevision_
      || QString::fromStdString(document->sourceName) != snapshot.source_.name
      || !addFile(XcafPath, "canonical-xcaf", xbf, MaximumXcafBytes))
    { return QObject::tr("Could not save the current XCAF geometry."); }
    if (document->sourceLengthUnits.size() > 1024
      || !std::all_of(document->sourceLengthUnits.begin(), document->sourceLengthUnits.end(),
        [](const auto& unit) {
          return validSourceUnit(QString::fromStdString(unit.name), unit.millimetersPerUnit);
        }))
    { return QObject::tr("The original STEP length units exceed the project metadata limits."); }
    if (snapshot.source_.sha256.isEmpty()
      || !addFile(SourcePath, "original-step", snapshot.source_.path(), MaximumSourceBytes,
        snapshot.source_.sha256))
    { return QObject::tr("Could not verify the original STEP for saving."); }
  }

  meshing::MeshBinaryWriteResult encodedMesh;
  if (snapshot.mesh_)
  {
    if (!document || !validMapping(*snapshot.mesh_, *document))
    { return QObject::tr("The current mesh does not match the project geometry."); }
    encodedMesh = meshing::writeMeshBinary(*snapshot.mesh_, domain::MeshValidationMode::CommittedMesh);
    const auto quality = writeProjectMeshQuality(*snapshot.mesh_);
    if (!encodedMesh.succeeded() || !quality.succeeded())
    { return QObject::tr("Could not encode the current mesh and quality report."); }
    addBytes(MeshPath, "neutral-mesh", QByteArray::fromRawData(
      reinterpret_cast<const char*>(encodedMesh.bytes.data()),
      static_cast<qsizetype>(encodedMesh.bytes.size())));
    addBytes(QualityPath, "mesh-quality", quality.bytes);
  }

  const auto metadata = QJsonDocument(projectJson(manifest, snapshot.geometryRevision_,
    snapshot.lastMeshRevision_, bool(snapshot.mesh_), document ? &*document : nullptr))
    .toJson(QJsonDocument::Compact);
  if (metadata.size() > MaximumProjectJsonBytes)
  { return QObject::tr("The project metadata exceeds the package size limit."); }
  addBytes(ProjectPath, "project", metadata);
  const auto archived = replaceProjectZip(path, manifest, [&](const QString& name) {
    std::unique_ptr<QIODevice> device;
    if (const auto file = files.constFind(name); file != files.cend())
    {
      device = std::make_unique<QFile>(*file);
    }
    else if (const auto bytes = data.constFind(name); bytes != data.cend())
    {
      auto buffer = std::make_unique<QBuffer>();
      buffer->setData(*bytes);
      device = std::move(buffer);
    }
    if (!device || !device->open(QIODevice::ReadOnly))
    { return ProjectResourceOpenResult{{}, ProjectResourceOpenError::ReadFailed}; }
    return ProjectResourceOpenResult{std::move(device), ProjectResourceOpenError::None};
  }, zipOptions());
  if (archived.succeeded()) { return {}; }
  if (archived.error == ProjectZipError::PublishFailed)
  { return QObject::tr("The verified project could not replace the destination file."); }
  return QObject::tr("Could not write or verify the project package (%1).")
    .arg(archived.subject.isEmpty() ? path : archived.subject);
}

ProjectOpenResult openProjectPackage(const QString& path, const geometry::IProjectGeometryStorage& storage)
{
  const auto opened = ProjectZipReader::open(path, zipOptions());
  if (!opened.succeeded())
  {
    return {{}, QObject::tr("Could not open or verify the project package (%1).")
      .arg(opened.status.subject.isEmpty() ? path : opened.status.subject)};
  }
  const auto& archive = *opened.archive;
  const auto& manifest = archive.manifest();
  const auto bytes = loadBytes(archive, ProjectPath, MaximumProjectJsonBytes);
  const auto settingsBytes = loadBytes(archive, SettingsPath, MaximumMeshSettingsJsonBytes);
  const auto healing = loadBytes(archive, HistoryPath, MaximumProjectJsonBytes);
  if (!bytes || !settingsBytes || !healing || !manifest.requiredFeatures.contains(PackageFeature))
  { return {{}, QObject::tr("This project snapshot format is unsupported or incomplete.")}; }
  if (*healing != EmptyHealingHistory)
  { return {{}, QObject::tr("This project uses an unsupported healing history.")}; }

  const auto json = QJsonDocument::fromJson(*bytes);
  const auto root = json.object();
  const auto revision = jsonRevision(root.value("geometryRevision"));
  const auto meshRevision = jsonRevision(root.value("lastMeshRevision"));
  const bool hasGeometry = entryFor(manifest, XcafPath) != nullptr;
  const bool hasMesh = entryFor(manifest, MeshPath) != nullptr;
  const auto settings = readMeshSettingsJson(*settingsBytes);
  if (!revision || !meshRevision || hasGeometry != (*revision != 0) || (!hasGeometry && *meshRevision)
    || (hasMesh && *meshRevision == 0) || !settings.succeeded()
    || root.value("projectId").toString() != manifest.projectId)
  { return {{}, QObject::tr("The project metadata, revisions, or settings are invalid.")}; }

  constexpr std::array supportedPaths{ProjectPath, HistoryPath, SettingsPath,
    SourcePath, XcafPath, MeshPath, QualityPath};
  for (const auto& entry : manifest.entries)
  {
    if (std::none_of(supportedPaths.begin(), supportedPaths.end(),
        [&entry](const char* supported) { return entry.path == supported; })
      && !(entry.role == "log" && entry.path.startsWith("logs/")))
    { return {{}, QObject::tr("Unsupported project resource (%1).").arg(entry.path)}; }
  }

  OpenedProject project;
  project.identity = {manifest.projectId, manifest.createdAt};
  project.settings = *settings.settings;
  project.lastMeshRevision = *meshRevision;
  project.missingOptionalLogs = archive.missingOptionalLogs();
  if (hasGeometry)
  {
    const auto source = root.value("source").toObject();
    project.source = {std::make_shared<QTemporaryDir>(), source.value("name").toString(),
      entryFor(manifest, SourcePath)->sha256};
    QTemporaryDir work;
    if (!validSourceName(project.source.name) || !project.source.workspace->isValid() || !work.isValid())
    { return {{}, QObject::tr("Could not prepare the original STEP workspace.")}; }
    const auto xbf = work.filePath("model.xbf");
    if (!copyEntry(archive, XcafPath, MaximumXcafBytes, xbf)
      || !copyEntry(archive, SourcePath, MaximumSourceBytes, project.source.path()))
    { return {{}, QObject::tr("The geometry resources exceed limits or could not be read.")}; }
    project.geometry = storage.prepareXcaf(nativePath(xbf), *revision, project.source.name.toStdString());
    if (!project.geometry)
    { return {{}, QObject::tr("Cannot read the project's binary XCAF geometry (OCCT %1).").arg(manifest.occtVersion)}; }
    if (!loadSourceUnits(source.value("lengthUnits"), project.geometry->document))
    { return {{}, QObject::tr("The STEP length-unit provenance is invalid.")}; }
  }
  // Rebuilding the sealed metadata also rejects duplicate fields, unknown
  // references and a persisted label map that does not match the loaded XCAF.
  const auto expected = projectJson(manifest, *revision, *meshRevision, hasMesh,
    project.geometry ? &project.geometry->document : nullptr);
  if (QJsonDocument(expected).toJson(QJsonDocument::Compact) != *bytes)
  { return {{}, QObject::tr("The project metadata or XCAF label mapping does not match the package.")}; }

  if (hasMesh)
  {
    project.mesh = loadMesh(archive);
    if (!project.mesh || project.mesh->meshRevision != *meshRevision
      || !validMapping(*project.mesh, project.geometry->document))
    { return {{}, QObject::tr("The stored mesh does not match the current geometry and revision.")}; }
    project.quality = loadQuality(archive, *project.mesh);
    if (!project.quality)
    { return {{}, QObject::tr("The stored mesh quality does not match the decoded mesh.")}; }
  }
  return {std::move(project), {}};
}
}
