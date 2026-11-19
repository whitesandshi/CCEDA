#pragma once

#include "application/AcceptedMeshQuality.h"
#include "domain/ProjectState.h"
#include "geometry/IProjectGeometryStorage.h"
#include "persistence/MeshSettingsJson.h"

#include <QStringList>

class QTemporaryDir;
namespace cceda::geometry { class IGeometryService; }

namespace cceda::persistence
{
struct ProjectIdentity
{
  QString id;
  QString createdAt;
};

[[nodiscard]] ProjectIdentity newProjectIdentity();

// The workspace owns the exact source bytes used for STEP import, or copied
// from a verified package. The external source path is never needed again.
struct ProjectSource
{
  std::shared_ptr<QTemporaryDir> workspace;
  QString name;
  QString sha256;
  [[nodiscard]] QString path() const;
};

struct ProjectImportResult
{
  domain::GeometryImportResult geometry;
  ProjectSource source;
};

[[nodiscard]] ProjectImportResult importProjectStep(const QString& path,
  geometry::IGeometryService& geometry);

class ProjectSaveSnapshot
{
public:
  [[nodiscard]] static std::optional<ProjectSaveSnapshot> capture(
    const domain::ProjectState& state, const ProjectIdentity& identity,
    const ProjectSource& source, const StoredMeshSettings& settings);

private:
  friend QString saveProjectPackage(const QString&, const ProjectSaveSnapshot&,
    const geometry::IProjectGeometryStorage&, const QString&);
  ProjectIdentity identity_;
  ProjectSource source_;
  StoredMeshSettings settings_;
  domain::GeometryRevision geometryRevision_ = 0;
  domain::MeshRevision lastMeshRevision_ = 0;
  std::shared_ptr<const domain::MeshModel> mesh_;
};

struct OpenedProject
{
  ProjectIdentity identity;
  ProjectSource source;
  StoredMeshSettings settings;
  domain::MeshRevision lastMeshRevision = 0;
  std::shared_ptr<geometry::PreparedProjectGeometry> geometry;
  std::shared_ptr<const domain::MeshModel> mesh;
  std::shared_ptr<const application::AcceptedMeshQuality> quality;
  QStringList missingOptionalLogs;
};

struct ProjectOpenResult
{
  std::optional<OpenedProject> project;
  QString error;
};

// An empty error string indicates success. These package operations are
// blocking and run on one worker while the owner serializes project mutations.
[[nodiscard]] QString saveProjectPackage(const QString& path, const ProjectSaveSnapshot& snapshot,
  const geometry::IProjectGeometryStorage& geometry, const QString& createdBy);
[[nodiscard]] ProjectOpenResult openProjectPackage(const QString& path,
  const geometry::IProjectGeometryStorage& geometry);

}
