# `.cceda` Project Package

Status: Draft  
Version: 0.1  
Depends on: [00-prd.md](00-prd.md), [01-domain-model.md](01-domain-model.md)

## 1. Purpose

`.cceda` is a self-contained, checksummed, migratable single-file package. Normal project opening MUST NOT depend on an external STEP file.

The package stores XCAF geometry and assembly metadata, the original STEP, one current neutral mesh, quality results, settings, validation reports, healing history, and concise job history. It MUST NOT serialize C++ object memory, OCCT/Qt handles, or Gmsh state.

## 2. Container Rules

- Extension: `.cceda`.
- ZIP-compatible container with ZIP64 support.
- Internal paths use `/`, are relative, and are normalized.
- JSON is UTF-8 without BOM.
- Binary geometry and mesh entries use appropriate compression; already compressed sources MAY be stored without recompression.
- Packaging is an adapter detail; the domain sees logical resource references.

## 3. Layout

```text
project.cceda
  manifest.json
  project.json
  source/original.step
  geometry/model.xbf
  geometry/report.json
  geometry/healing-history.json
  geometry/snapshots/<operation-id>.xbf
  mesh/settings.json
  mesh/current.meshbin
  mesh/quality.json
  mesh/task-history.json
  logs/geometry.log
  logs/meshing-<job-id>.log
```

Always required: `manifest.json`, `project.json`, `geometry/healing-history.json`, and `mesh/settings.json`. After import, `source/original.step` and `geometry/model.xbf` are required. Current mesh and quality entries are present together. Logs and reachable undo snapshots are optional.

## 4. Manifest

`manifest.json` contains:

| Field | Rule |
| --- | --- |
| `format` | literal `cceda-project` |
| `formatVersion` | `major.minor` |
| `projectId` | UUID matching `project.json` |
| `createdBy` | application, version, build ID |
| `createdAt`, `modifiedAt` | ISO 8601 UTC |
| `occtVersion` | OCCT version that wrote XCAF |
| `canonicalLengthUnit` | literal `mm` |
| `entries` | path, role, uncompressed size, SHA-256 |
| `requiredFeatures` | reader capabilities required by this package |

SHA-256 covers uncompressed entry bytes. The manifest does not hash itself.

## 5. Project Metadata

`project.json` stores the serializable aggregate:

- project identity and timestamps.
- original STEP reference, name, digest, unit, and scale.
- current geometry revision and XCAF resource.
- assembly tree, instances, transforms, names, colors, and visibility.
- validation and healing-history resources.
- current mesh revision/state and resources.
- settings and lightweight job-history resources.

It MUST NOT inline B-Rep, node arrays, element arrays, or detailed logs.

## 6. XCAF Geometry

- `geometry/model.xbf` is the canonical geometry when opening a project.
- Read and write use OCCT binary XCAF persistence.
- The original STEP is provenance only and is not reparsed during normal open.
- Assembly definitions, instances, names, colors, and B-Rep reside in XCAF.
- `project.json` stores domain-ID to XCAF-label mapping; traversal order is not sufficient.
- The package records OCCT version. An incompatible XCAF read MUST produce a compatibility error, not silently reimport STEP and lose project state.

## 7. Undo Snapshots

- Healing undo MAY use XCAF/B-Rep snapshots; first release may prefer snapshots over inverse operations.
- Keep only snapshots reachable from the current undo/redo position.
- Name snapshots by `HealingOperationId`, never user text.
- Snapshot damage may disable affected undo steps but MUST NOT prevent loading current geometry.

## 8. Neutral Mesh Binary

`mesh/current.meshbin` represents the neutral mesh, not a Gmsh model.

Header requirements:

- magic `CCMESH`.
- mesh format major/minor.
- byte-order marker; first writer uses little-endian.
- positive committed `MeshRevision` and source `GeometryRevision`.
- canonical unit `mm`.
- node, triangle, tetrahedron, and region counts.
- block offsets, lengths, and checksums.

An uncommitted Worker artifact may use `MeshRevision=0`. The main process assigns a positive revision and writes the final header during project commit.

Data blocks:

- nodes: ID and three 64-bit coordinates.
- Triangle3: element ID, three node IDs, region, geometry association index.
- Tetrahedron4: element ID, four node IDs, region, geometry association index.
- regions: internal region ID and original Solid/fragment mapping.
- associations: compressed mesh-to-`GeometryId` mapping.

The exact header, directory, alignment, checksum, and block layouts are defined in [06-neutral-mesh-binary.md](06-neutral-mesh-binary.md). Readers use block lengths to skip unknown optional blocks in a compatible minor version.

## 9. Reports and Job History

Geometry reports store report/revision IDs, checker version, category/severity totals, and structured issues. Mesh quality stores mesh revision, metric version, thresholds, summaries, and issue indexes. Localized final strings are not persisted; message keys and arguments are.

Job history stores job ID, times, outcome, settings digest, retry count, mesh size, and concise error code. It never stores old mesh arrays.

## 10. Save Transaction

1. Capture a consistent read-only project snapshot.
2. Create a unique temporary file in the target directory.
3. Write entries and compute digests.
4. Write `manifest.json` last.
5. Close and reopen the temporary package; verify required entries and hashes.
6. Atomically replace the target on the same volume.
7. Clear the dirty flag only after replacement succeeds.

Failure MUST preserve the previous project file.

## 11. Open Transaction

1. Validate extension, container, and internal path safety.
2. Read manifest and validate the format major version.
3. Validate required features, sizes, presence, and hashes.
4. Read `project.json` and validate identity/references.
5. Load XCAF and rebuild assembly mapping.
6. If a mesh exists, validate format, references, and source revision.
7. Load settings and reports.
8. Missing optional logs do not block open; corrupt required data does.

Opening MUST NOT reimport STEP or silently modify the package.

## 12. Versioning and Migration

- Newer incompatible major: refuse and request a newer application.
- Newer minor: read only when all required features are supported; ignore unknown optional fields.
- Older minor: migrate in memory and write current format on the next explicit save.
- Migration records source/target versions and warnings.
- Migration failure never modifies the source file.
- Irreversible XCAF or mesh changes increment the relevant internal format version.

## 13. Security and Resource Limits

- Reject absolute paths, `..`, duplicate normalized paths, and links outside the package.
- Check declared size, compression ratio, entry count, and total budget before extraction.
- Limit JSON depth, strings, and array counts.
- Check integer multiplication/overflow before binary allocations.
- Treat names, logs, STEP bytes, and mesh data as untrusted.
- Never execute scripts, commands, or dynamic-library paths from a project.

## 14. Package Invariants

1. Manifest and project IDs match.
2. A `Ready` mesh source revision equals current geometry revision.
3. Required entries exist and hashes match.
4. There is at most one `mesh/current.meshbin`.
5. Original STEP bytes and digest remain unchanged.
6. Canonical length is millimeters.
7. Unknown required features are never ignored.

## 15. Deferred Features

Incremental/autosave logs, multiple mesh versions, external resources, cloud collaboration, encryption, signing, and access control are deferred.

## 16. Mesh Settings Entry Version 0.1

The implemented `cceda_project_settings` adapter encodes and decodes the logical
`mesh/settings.json` entry as UTF-8 JSON. Its byte API returns a complete typed
value or a typed error. File I/O and project-state application belong to the
future package transaction.

Every field below is required. All objects have exactly the listed members.

| Object | Members |
| --- | --- |
| Root | `schema`, `canonicalUnit`, `mode`, `generation`, `resourceLimits`, `retry` |
| `schema` | `name` = `cceda.mesh-settings`, `major` = 0, `minor` = 1 |
| `generation` | `minimumSizeMm`, `maximumSizeMm`, `curvatureElementsPerCircle`, `optimizeTetrahedra`, `maximumThreads`, `algorithm` |
| `resourceLimits` | `maximumElapsedMilliseconds`, `maximumWorkingSetBytes` |
| `retry` | `automaticRetry`, `maximumAttempts` |

`canonicalUnit` is `mm`. Mode is `independent-solids` or
`conformal-multi-region`. Algorithm is `delaunay` or `hxt`. Boolean settings
require JSON booleans. Counts and limits require nonnegative integral JSON
numbers within the existing domain limits, and sizes require finite numbers
satisfying `isValidMeshGenerationSettings`. Zero retains its existing automatic
size/thread or disabled resource-limit meaning. Both retry fields are recorded
even when automatic retry is disabled.

Readers enforce a 16 KiB byte budget and maximum nesting depth of four before
parsing. Malformed UTF-8, BOM, missing or unknown members, duplicate keys,
unsupported schema versions, unknown enum strings, and invalid field types or
ranges are rejected. Versions other than 0.1 require a future explicit reader
or migration. Missing fields produce errors. Writers validate the complete
value and emit deterministic compact JSON with all fields present.

Acceptance covers all-field roundtrip, exact large memory limits, explicit
zero/disabled settings, deterministic re-encoding, and rejection of malformed
or incompatible data without returning partial settings. Full ZIP packaging,
geometry persistence, and workbench project Save/Open remain pending.

## 17. Committed Mesh Quality Entry Version 0.1

The `cceda_project_quality` adapter produces `mesh/quality.json` using schema
`cceda.mesh-quality-project/0.1`. The root contains `schema`, `meshRevision`,
`sourceGeometryRevision`, `metricVersion`, `validForCommit`, `counts`,
`summaries`, and `issues`. Summaries, issue ordering, metric identifiers, and
formulas follow the quality specification. Both revisions are positive and
within the quality JSON safe-integer range. Each supported stored mesh has
at least one Triangle3 and one Tetrahedron4, matching the current volume pipeline.

Writing accepts a structurally valid committed `MeshModel`, recomputes its
quality, and requires commit-valid results. Orientation warnings may be stored.
The entry carries committed revision identity. Job provenance belongs in the
task history. The shared serializer supplies the existing canonical numeric
summaries and issue representation.

Reading requires the corresponding decoded committed mesh. It checks the input
byte budget first, independently evaluates the mesh, generates canonical project
quality JSON, and requires byte-for-byte equality with the stored entry. The
format therefore requires canonical compact JSON, including key order and number
formatting. Edited whitespace, duplicate or extra fields, unknown versions,
changed summaries, and mismatched revisions all fail validation. Callers receive
a typed error with no report on failure, or the recomputed transient report on
success. Bounded issue details can then be rebuilt before discarding per-element
arrays. Default output/input and issue budgets reuse 16 MiB and 100,000 issues.

This module validates quality consistency. The package transaction must also
verify mesh and quality entry hashes, geometry mappings, and the current geometry
revision before applying either entry to live project state. Threshold policies
and package Save/Open integration remain pending.

Acceptance includes paired meshbin/quality roundtrip, exact committed revisions,
preserved accepted warning IDs and reconstructed details, rejection of modified
metrics or revisions, resource-budget failures, and separation of Worker and
project schemas. Cross-build floating-point reproducibility remains an acceptance
requirement for future package compatibility support.
