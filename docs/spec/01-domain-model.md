# CCEDA Domain Model

Status: Draft  
Version: 0.1  
Depends on: [00-prd.md](00-prd.md)

## 1. Purpose and Boundaries

The domain represents projects, assemblies, geometry references, issues, healing history, meshing settings, and a neutral mesh. It MUST remain independent of Qt, OCCT, Gmsh, Win32, and storage formats.

Domain objects MUST NOT own `TopoDS_Shape`, TDF/AIS handles, Gmsh tags, Qt objects, native pointers, or process-local callbacks.

```text
cceda-ui -> cceda-application -> cceda-domain
                         |-----> cceda-geometry-api
                         |-----> cceda-mesh-api
                         `-----> cceda-project-api

cceda-geometry-occ -> geometry-api + domain
cceda-mesh-gmsh    -> mesh-api + domain
cceda-project      -> project-api + domain
```

Dependencies point inward only.

## 2. Identifier Policy

| Identifier | Lifetime | Rule |
| --- | --- | --- |
| `ProjectId` | Permanent | UUID generated when the project is created |
| `AssemblyNodeId` | Permanent in a project | UUID persisted in `.cceda`; never a row index |
| `GeometryRevision` | Monotonic | Increment after every committed B-Rep change |
| `GeometryId` | One geometry revision | Revision plus persistent XCAF label and subshape locator |
| `GeometryIssueId` | One validation report | UUID tied to report and revision |
| `HealingOperationId` | Permanent | UUID used by history and undo snapshots |
| `MeshRevision` | Monotonic | Positive integer assigned when a new mesh is committed |
| `MeshNodeId` | One current mesh | Unsigned 64-bit value, unique in that mesh |
| `MeshElementId` | One current mesh | Unsigned 64-bit value, unique in that mesh |
| `MeshJobId` | Permanent summary | UUID used by protocol, logs, and job history |

Top-level XCAF definitions and assembly instances SHOULD retain permanent IDs. Face, Edge, and Vertex IDs are stable only within one `GeometryRevision`. Healing MUST produce a best-effort old-to-new map and list unmapped objects. UI code MUST NOT treat OCCT hash values, traversal order, or Gmsh tags as persistent IDs.

## 3. `Project` Aggregate Root

The aggregate contains:

- `projectId`, name, format version, created/modified times.
- `sourceArtifact`: original STEP name, digest, unit, and package reference.
- `unitSystem`: source unit, millimeter canonical unit, and scale.
- `geometryRevision` and XCAF document resource reference.
- `assemblyRoot` and assembly nodes.
- current geometry report and healing history.
- current meshing settings.
- optional current neutral mesh.
- `meshState`: `Missing`, `Ready`, `Stale`, or `Invalid`.
- lightweight mesh job summaries without old mesh arrays.

## 4. Assembly and Geometry

### 4.1 `AssemblyNode`

Required fields:

- ID, optional parent ID, and ordered child IDs.
- Kind: `Assembly`, `Component`, or `Part`.
- STEP/XCAF name or stable fallback display name.
- Optional RGBA color.
- Local transform relative to the parent.
- Referenced part definition and top-level geometry ID.
- Project visibility state.

Part definitions and component instances MUST be separate. Repeated instances share one geometry definition but retain separate IDs and transforms.

### 4.2 `GeometryEntityRef`

This is a lightweight domain reference containing:

- `GeometryId` and owning part ID.
- Kind: `Compound`, `CompSolid`, `Solid`, `Shell`, `Face`, `Wire`, `Edge`, or `Vertex`.
- Optional topological parent ID.
- Display name, effective color, validity, and `isMeshable`.

Only valid Solids have `isMeshable=true` in the first release. Detailed geometry data is queried on demand from `IGeometryQueryService`.

### 4.3 Property DTOs

| Kind | Required properties |
| --- | --- |
| Solid | unique Face/Edge/Vertex counts, volume, bounds, closure, validity |
| Shell | Face/Edge/Vertex counts, area, open-edge count, closure |
| Face | unique and usage Edge/Vertex counts, area, surface type, orientation, bounds |
| Wire | Edge/Vertex counts, total length, closure |
| Edge | endpoint count, length, curve type, degeneration |
| Vertex | 3D position and tolerance |

All values use millimeter-derived units. A failure to compute one property returns a field-level status rather than failing the entire query.

## 5. Units

`UnitSystem` contains the source length unit, canonical `Millimeter`, a finite positive scale, canonical model bounds, and scale warnings. All persisted geometry and mesh dimensions use millimeters. UI display conversion MUST NOT change storage semantics.

## 6. Geometry Issues and Healing

### 6.1 `GeometryIssue`

Required fields:

- ID, geometry revision, and target geometry IDs.
- Stable English code and category.
- Severity: `Info`, `Warning`, `Error`, or `Fatal`.
- Localizable summary key and structured numeric evidence.
- Repairability: `None`, `SafeAutomatic`, or `NeedsConfirmation`.

Categories include validity, topology, tolerance, small feature, intersection, and scale.

### 6.2 `HealingOperation`

Required fields:

- operation ID, action type, target IDs, and input parameters.
- before/after geometry revisions.
- addressed issue IDs and result.
- ID mapping summary and unmapped objects.
- automatic/confirmed flag and undo snapshot resource.

Only successfully committed operations enter the undo stack. Undo and redo restore geometry snapshots, create a new current revision, rerun validation, and mark the mesh stale.

## 7. Meshing Settings

Settings have two layers.

Common settings:

- mode: `IndependentSolids` or `ConformalMultiRegion`.
- Triangle3 surface and Tetrahedron4 volume generation.
- global, minimum, and maximum size in millimeters.
- curvature adaptation and optimization flags.
- automatic retry policy and resource budget.

Backend settings:

- backend ID (`gmsh` initially).
- backend settings schema version.
- typed, validated values; never arbitrary command-line text.

A `MeshConstraint` extension point MAY exist, but first-release projects MUST NOT create local constraints.

## 8. Neutral Mesh

`MeshModel` contains:

- positive `MeshRevision` and source `GeometryRevision`.
- creation time and trace-only backend information.
- millimeter-coordinate nodes.
- Triangle3 surface elements and Tetrahedron4 volume elements.
- regions and original-Solid mappings.
- necessary node/element to geometry associations.
- quality summaries and issue-element indexes.
- conformal/non-conformal status.

`MeshNode` contains ID, 3D coordinates, and optional geometry association. `MeshElement` contains ID, type, ordered node IDs, region ID, and geometry association. New element types require new enum values and a format version extension.

## 9. Mesh Quality

Every metric definition includes a stable English ID, applicable element kinds, numeric direction, valid range, default and user thresholds, and handling for invalid values.

Summaries contain minimum, maximum, mean, selected percentiles, and failed-element count. Issue lookup MUST use an index keyed by `MeshElementId`; selecting an issue must not scan the entire mesh.

## 10. State Models

Project lifecycle:

```text
Empty -> Importing -> GeometryReady
                    -> GeometryHasErrors
GeometryReady -> Meshing -> MeshReady
GeometryHasErrors -> healing -> GeometryReady
MeshReady -> geometry change -> MeshStale
```

Job lifecycle:

```text
Created -> Validating -> Queued -> Running
Running -> Cancelling -> Cancelled
Running -> Succeeded | Failed | WorkerLost
```

Only `Succeeded` may atomically replace the current mesh.

## 11. Invalidation Matrix

| Change | Geometry report | Current mesh | Quality report |
| --- | --- | --- | --- |
| View/visibility only | keep | keep | keep |
| Meshing settings | keep | keep for viewing; mark settings mismatch | keep with mismatch marker |
| Commit/undo/redo healing | recompute | `Stale` | `Stale` |
| Successful new mesh | keep | atomically replace as `Ready` | replace |
| Failed/cancelled job | keep | unchanged | unchanged |

## 12. Service Boundaries

- `IProjectService`: create, open, save, close.
- `IGeometryImportService`: STEP to XCAF and assembly domain.
- `IGeometryQueryService`: tree, properties, adjacency, and location.
- `IGeometryValidationService`: structured geometry issues.
- `IGeometryHealingService`: plan, confirm, apply, undo, redo.
- `IMeshingService`: validate settings, submit, cancel, and commit results.
- `IMeshQualityService`: metrics, filtering, and issue indexes.
- `IMeshExporter`: export the current neutral mesh.

Names describe responsibilities and do not require one C++ class per interface.

## 13. Domain Events

Required events include geometry imported, validation completed, healing committed/undone, geometry revision changed, mesh job progress/completion/failure, current mesh replaced/stale, and project saved. Events carry only IDs and small DTOs; large geometry and mesh data is queried through services.

## 14. Invariants

1. A `Ready` mesh source revision equals the project geometry revision.
2. A project has at most one current mesh.
3. Failed, cancelled, or lost jobs never replace that mesh.
4. Non-solid first-release objects are never meshable.
5. Persisted lengths are millimeters.
6. Healing cannot mutate the aggregate before commit.
7. Every confirmation-required action stores confirmation evidence.
8. Domain code contains no Qt, OCCT, Gmsh, or platform types.

## 15. Deferred Design

Dynamic plugin ABI, multiple STEP inputs, source synchronization, user groups, local constraints/remeshing, solver models, materials, and multiple mesh alternatives are deferred.
