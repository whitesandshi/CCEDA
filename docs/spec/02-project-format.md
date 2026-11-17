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

## 18. Manifest Entry Version 0.1

`manifest.json` uses the root fields `format`, `formatVersion`, `projectId`,
`createdBy`, `createdAt`, `modifiedAt`, `occtVersion`, `canonicalLengthUnit`,
`entries`, and `requiredFeatures`. The format is `cceda-project`, version
`0.1`, and the canonical length unit is `mm`. `createdBy` is a nonempty string
identifying application/version/build (for example `CCEDA/0.2.0+build-id`).
Project IDs use the UUID 8-4-4-4-12 hexadecimal form. Timestamps are valid UTC
ISO 8601 date-times ending in `Z`, with optional one-to-three fractional digits.

Each entry has `path`, `role`, `uncompressedBytes`, and a lowercase hexadecimal
SHA-256 digest. Paths are relative slash-separated package paths without empty
segments, `.`, `..`, drive prefixes, absolute roots, backslashes, or repeated
slashes or control characters. `manifest.json` cannot appear in its own entry
list. Path uniqueness is case-folded to prevent Windows aliases. Required feature
IDs are non-empty and unique. The manifest has at least one entry,
at most 10,000 entries, a 2 MiB encoded size limit, and a 512 GiB per-entry
uncompressed-size limit. Strings are bounded to 1,024 UTF-16 code units,
required features to 128, and JSON nesting to three levels.

Readers require exact root and entry members, supported format version, valid
identities, safe logical paths, valid hashes, and bounded integral sizes.
Missing, extra, duplicate, malformed, BOM, invalid UTF-8, or trailing non-whitespace
data is rejected. Valid JSON whitespace and input ordering are accepted; readers
normalize entry and feature ordering. Writers validate, sort, and emit deterministic
compact JSON. The manifest does not hash itself. Package transactions write
`manifest.json` last after all resource hashes have been computed.

The manifest codec covers bytes and validation. Resource integrity and ZIP
container I/O are provided by the following adapters. Resource decoding and
atomic package Save/Open integration remain pending.

## 19. Streaming Project Resource Verification

`cceda_project_resources` validates a manifest and the uncompressed resource
streams supplied by a container adapter. Before any resource is opened it checks
the manifest bounds, explicitly supported required features, total declared size,
and declarations for `project.json`, `geometry/healing-history.json`, and
`mesh/settings.json`. Original STEP and canonical XCAF must be declared together;
current mesh and quality must be declared together. A stored mesh also requires
the geometry pair. The total resource budget defaults to 8 GiB and is configurable
by the caller; summation uses overflow-safe subtraction. The manifest itself is
bounded by its separate 2 MiB limit.

Each declared resource is opened once and hashed with SHA-256 through a fixed
64 KiB buffer. Short reads and sequential devices are supported. Streams must
be fresh, open, binary, and readable. A premature end, extra bytes, read failure,
zero read before EOF, or digest mismatch fails verification with an entry path.
At most one extra byte beyond a declaration is requested to detect excess data.
Only a genuinely missing `logs/` entry with role `log` is tolerated, and its path
is returned as a warning on complete success. Present corrupted logs and log I/O
errors fail verification. Failed results do not expose a partial success report.

The provider must supply streams from one immutable package snapshot and
distinguish absence from I/O failure. It owns container validation, duplicate
archive names, compression-ratio limits, link handling, and safe resource lookup.
Logical names must not be resolved directly against an untrusted filesystem
directory. This verifier neither extracts resources nor changes project state.
Payload decoding, project-ID/reference checks, exact geometry/mesh revision
checks, optional undo-snapshot recovery, and atomic Save/Open integration
remain separate pending work. Until undo recovery is implemented, declared
snapshot corruption is conservatively rejected.

Acceptance covers required declarations, resource pairs, unsupported features,
pre-I/O budgets, missing resources, same-size tampering, truncated/extra data,
missing optional logs, mid-stream failures, invalid devices, sequential short
reads, empty resources, and multi-buffer real-file verification.

## 20. ZIP Container Adapter

`cceda_project_zip` implements ZIP32/ZIP64 reading and exclusive new ZIP64 staging
file writing on Windows. miniz 3.1.2 is vendored at a fixed commit; its MIT license
and provenance are in `third_party/miniz`. The domain and public adapter headers
contain no miniz types. Build-time network access and external archive tools are
not required. No extraction API is enabled.

`ProjectZipReader::open` requires a `.cceda` path and retains a Windows file handle
that denies other writers and deletion. Before returning a reader it checks the
archive directory and local headers, decodes the manifest, reconciles exact entry
names and sizes, and verifies all resources through the streaming verifier.
Resource devices retain the shared handle and may outlive the reader. The reader
and its devices are single-thread objects; independent packages can be opened
on independent threads. Consumers must read to EOF to observe final stream CRC
status. The initial open always performs that complete verification.

The initial container profile accepts Stored and Deflate entries. It rejects
encryption, directory entries, links/special file attributes, invalid UTF-8 names,
case-folded duplicates, unsafe logical paths, file/directory name collisions,
overlapping entry ranges, local/central header disagreements, undeclared files,
and invalid CRC or SHA-256. No entry name is resolved as a filesystem path.
ZIP32 and ZIP64 data descriptors, with or without signatures, are validated
against the central-directory sizes and CRC. Metadata checks include empty files.

Default compressed archive size is limited to 9 GiB, declared resource bytes to
8 GiB, and each entry's expansion ratio to 2,000; callers can configure these
limits. The manifest has its independent 2 MiB limit. Entry count is at most
10,001 including the manifest, filenames at most 511 UTF-8 bytes, and miniz
allocations (including allocation headers) have a 32 MiB per-archive budget before
central-directory parsing. Large resource payloads are streamed. Full large-entry
acceptance beyond the default resource budget remains a separate performance gate.

`writeNewProjectZip` takes an immutable manifest/resource set and creates a new
staging path using `CREATE_NEW`. It prevalidates inputs, streams and hashes them
again while writing, compresses resources with Deflate, and writes the bounded
manifest last as a Stored entry. It finalizes and closes the ZIP writer, flushes,
and opens a fresh reader for full verification while retaining an ownership
handle that excludes competing writes/renames/deletion. The ownership handle is
released only after verification. Failure marks that owned handle for deletion;
cleanup never re-resolves a path that another process could replace. Existing
files are never overwritten. A save requires every declared resource, including
logs; an open retains the verifier's missing-optional-log warnings.

This is the container layer. Capturing a consistent live project snapshot,
encoding `project.json`/XCAF, checking domain identity and revisions on restore,
atomically replacing an existing project, and clearing its dirty flag are still
pending. No project Save/Open command is exposed by this increment.

Acceptance includes compressed/stored roundtrip, Unicode names, empty resources,
retained stream lifetime and file locks, failed-write cleanup, unchanged existing
targets, changed source bytes, count/memory/ratio/size budgets, malformed and
tampered containers, and real ZIP64 offsets above 4 GiB using sparse fixtures.
When Python is available, a standard-library interoperability test checks the
writer's CRCs/hashes and reads Python-produced ZIP32/ZIP64 packages with both
seekable output and sequential data descriptors.
