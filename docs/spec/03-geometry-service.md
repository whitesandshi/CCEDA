# OCCT Geometry Service

Status: Draft  
Version: 0.1  
Depends on: [00-prd.md](00-prd.md), [01-domain-model.md](01-domain-model.md), [02-project-format.md](02-project-format.md)

## 1. Responsibilities

The geometry subsystem owns STEP/XCAF import, unit normalization, assembly metadata, B-Rep and topology indexing, property queries, validation, healing plans and commits, undo/redo, immutable meshing snapshots, and XCAF persistence.

It does not own Qt models/widgets, Gmsh settings/tags, mesh quality, mesh export, or solver concepts.

Public contracts use domain IDs, scalar values, strings, arrays, and DTOs. OCCT types remain inside `cceda-geometry-occ` and `cceda-visualization-occ`.

## 2. Logical Ports

- `IGeometryImportService`
- `IGeometryQueryService`
- `IGeometryValidationService`
- `IGeometryHealingService`
- `IGeometrySnapshotService`

The OCCT adapter contains an XCAF document repository, STEPCAF reader, topology index, validation/healing pipeline, and AIS visualization bridge.

## 3. Geometry Repository

- Own exactly one writable current XCAF document.
- Identify committed geometry by `GeometryRevision`.
- Serialize all write operations.
- Require queries to name a revision; mismatches return `RevisionMismatch`.
- Treat AIS presentations as disposable caches, not canonical data.
- Build modified geometry in a temporary document/shape and atomically commit only after indexing and validation succeed.

## 4. STEP Import

### 4.1 Input

- File path or read-only byte stream.
- Original filename and SHA-256.
- Canonical target unit, fixed to millimeters.
- Versioned import options.
- Cancellation token and progress sink.

### 4.2 Required OCCT Path

- Use `STEPCAFControl_Reader` or an equivalent XCAF path that retains assemblies, names, and colors.
- Use XCAF document, shape, color, and assembly tools for definitions and instances.
- Do not use `STEPControl_Reader::OneShape()` as the final project import representation.

### 4.3 Stages

1. Validate extension, readability, and file budget.
2. Read STEP header, protocol, and length unit.
3. Transfer into a temporary XCAF document.
4. Extract part definitions, instances, transforms, names, and colors.
5. Normalize coordinates/tolerances to millimeters.
6. Build assembly domain IDs and a subshape topology index.
7. Compute canonical bounds, counts, and scale warnings.
8. Run basic validity checks.
9. Commit only when all required data is internally consistent.

### 4.4 Success Result

Return the new revision, assembly/part/instance counts, topology counts, source unit/scale/bounds, metadata retention statistics, meshable and non-solid object lists, and basic validation summary.

Import failure MUST NOT leave a partial project or replace existing geometry.

## 5. Topology Index

The index maps domain IDs to OCCT labels/shapes and back.

It stores:

- XCAF part-definition labels and permanent domain IDs.
- component-instance labels, definitions, and transforms.
- Solid/Shell/Face/Wire/Edge/Vertex membership per top-level shape.
- parent/child and necessary adjacency relationships.
- explicit/inherited color source.
- shape type, orientation, location, and tolerance summary.

Rules:

- The same TShape/location pair maps to one `GeometryId` in one revision.
- Instance IDs and part-definition IDs are distinct.
- Subshape IDs are not assumed stable after healing.
- Rebuild checks duplicate IDs, dangling parents, and type mismatches.
- Large topology queries are paged or on-demand.

## 6. Query Semantics

The query service provides assembly roots/children, part topology, property DTOs, parent/child/adjacent objects, bounds for fit/zoom, display references, and batched ID queries.

- Solid counts use unique subshapes.
- Face queries provide unique and usage counts when relevant; UI defaults to unique.
- Length, area, and volume use millimeter-derived units.
- A failed derived property returns a field-level error, not a failed object query.

## 7. Validation

### 7.1 Stages

1. XCAF document and assembly-reference integrity.
2. Basic B-Rep validity.
3. Solid closure and orientation.
4. Open shell edges, wire closure, and degenerate topology.
5. Abnormal tolerances.
6. Small edges/faces/features relative to model and proposed mesh scale.
7. Reliably detectable duplicate topology, self-intersection, and interference.
8. Multi-Solid gaps, overlaps, and shared-interface candidates.

### 7.2 Stable Codes

- `GEO_INVALID_BREP`
- `GEO_OPEN_SHELL`
- `GEO_NON_MANIFOLD`
- `GEO_DEGENERATED_EDGE`
- `GEO_SMALL_EDGE`
- `GEO_SMALL_FACE`
- `GEO_EXCESSIVE_TOLERANCE`
- `GEO_SELF_INTERSECTION`
- `GEO_SOLID_OVERLAP`
- `GEO_INTERFACE_GAP`
- `GEO_ABNORMAL_SCALE`

Codes are stable and separate from localization. An unavailable or unreliable check MUST NOT be reported as passed.

### 7.3 Blocking Rules

- `Fatal`: document or shape cannot be used; meshing is blocked.
- `Error`: the target Solid is invalid; that Solid is blocked.
- `Warning`: user may continue after seeing a summary.
- `Info`: understanding only.

In independent mode, one blocked Solid does not automatically block other valid Solids, but skipping it requires explicit confirmation. In conformal mode, any blocking error in a participating region blocks the job.

## 8. Healing

Validation creates issues; it never mutates geometry. Healing is plan-then-commit.

### 8.1 `HealingPlan`

Contains target revision, issue IDs, ordered actions/parameters, classification (`SafeAutomatic` or `NeedsConfirmation`), estimated affected objects/topology changes, and undo/resource strategy.

### 8.2 Classification

- Only operations proven by fixed acceptance models not to remove meaningful features or merge independent regions are safe automatic repairs.
- Sewing, shell solidification, small-feature removal, merging/splitting, Boolean Fragment, and material tolerance changes require confirmation by default.
- Severe self-intersection and semantically ambiguous overlap are not automatic repairs.
- Letting Gmsh attempt a bad Solid is not a successful geometry repair.

### 8.3 Commit Transaction

1. Verify the plan revision is current.
2. Create an undo snapshot.
3. Apply actions to a temporary document/shape.
4. Rebuild the index and rerun relevant checks.
5. Produce old-to-new ID mapping and unmapped objects.
6. Roll back if validity worsens, required metadata is lost, or consistency fails.
7. Atomically commit, increment geometry revision, and mark the mesh stale.

## 9. Undo and Redo

- Only committed geometry healing enters the stack.
- Each commit has one `HealingOperationId`.
- Undo/redo restores a snapshot, rebuilds the index, reruns validation, and creates the new current revision.
- A new repair after undo truncates the redo branch and unreachable snapshots.
- Units, views, mesh jobs, and export do not enter the healing undo stack.
- Project open restores available history and the current undo position.

## 10. Meshing Snapshot

`IGeometrySnapshotService` creates immutable Worker input:

- Include geometry revision and SHA-256.
- Include only participating valid Solids; first release defaults to all valid Solids.
- Preserve a separate source-region mapping in independent mode.
- Record finite ordered axis-aligned BREP bounds for each located Solid in
  canonical millimeters. Bounds are computed from geometry without adding
  triangulation and are sealed by the mapping-file SHA-256.
- Prefer existing shared topology in conformal mode.
- Run task-only imprint/fragmentation on a copy, never silently on canonical project geometry. The explicit conformal meshing mode now applies OCCT Fragment/Imprint to the revision-bound snapshot and records each fragment's source Solid mapping.
- Output original-Solid to task-region mapping for any task-only topology changes.
- Persisting such a change requires a separate confirmed healing transaction.

Recommended bulk representation is OCCT BREP plus a JSON mapping file.

## 11. AIS Visualization Bridge

- The bridge may own AIS Context, V3d View, and AIS presentations.
- UI/domain request display, highlight, isolate, transparency, and fit by `GeometryId`.
- Maintain a short-lived `GeometryId -> AIS presentation` cache.
- Clear all old-subshape presentations after a geometry revision change.
- Convert selection callbacks to domain IDs before notifying the application layer.
- Do not pre-create one AIS object per Face for very large models; build selection/highlight representations on demand.

## 12. Concurrency and Cancellation

- Serialize XCAF writes.
- Import, full validation, and healing may run in background tasks but never manipulate UI AIS Context directly.
- Queries read an immutable revision snapshot.
- Cancellation takes effect at safe OCCT checkpoints; UI reports when an uninterruptible call is finishing.
- Allow only one geometry write task at a time.
- While meshing, viewing is allowed. A geometry-changing commit must first cancel or wait for the mesh job.

## 13. Error Contract

Errors contain stable code, stage, localizable summary key/arguments, optional geometry IDs, retryability, and required user action. OCCT detail belongs in diagnostic logs.

Minimum codes:

- `GEOMETRY_FILE_NOT_READABLE`
- `GEOMETRY_STEP_READ_FAILED`
- `GEOMETRY_XCAF_TRANSFER_FAILED`
- `GEOMETRY_UNIT_UNKNOWN`
- `GEOMETRY_REVISION_MISMATCH`
- `GEOMETRY_QUERY_NOT_FOUND`
- `GEOMETRY_HEALING_CONFIRMATION_REQUIRED`
- `GEOMETRY_HEALING_FAILED`
- `GEOMETRY_SNAPSHOT_FAILED`
- `GEOMETRY_CANCELLED`

## 14. Acceptance

1. Repeated part instances retain shared definition, distinct transforms, names, and colors.
2. Metadata survives XCAF save and project reopen.
3. Solid/Face/Edge statistics match OCCT topology traversal.
4. Non-solids display but never enter a first-release meshing snapshot.
5. Issues locate through `GeometryId` in tree and view.
6. Safe healing, confirmation, failure rollback, undo, and redo follow revision rules.
7. Healing marks old mesh stale.
8. Creating a meshing snapshot does not mutate canonical XCAF geometry.
