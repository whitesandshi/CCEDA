# Gmsh Worker Meshing Protocol

Status: Draft  
Version: 0.1  
Depends on: [00-prd.md](00-prd.md), [01-domain-model.md](01-domain-model.md), [03-geometry-service.md](03-geometry-service.md)

## 1. Purpose

The main application executes Gmsh jobs in an isolated `MeshingWorker` process to contain failures and global state, support cancellation/resource limits, keep UI responsive, and retain a replaceable mesher boundary.

Process isolation is not a license workaround. Closed-source distribution still requires an appropriate Gmsh license or replacement backend.

## 2. Process Model

- The main application exclusively owns the Worker.
- At most one active mesh job per project in the first release.
- Start a clean Worker per job and exit after the job, avoiding cross-job Gmsh state.
- Restrict Worker access to its explicit job directory/resources.
- Worker never reads or writes the `.cceda` package.
- Only the main application can commit a successful result as current mesh.

## 3. Protocol Layers

```text
Control plane: versioned messages
  hello / submit / progress / cancel / result / error

Data plane: job workspace
  geometry snapshot / maps / settings / neutral mesh / quality / logs
```

Use `QProcess` for lifecycle. The first transport may be `QLocalSocket` or controlled length-prefixed standard streams; protocol semantics are transport-independent. BREP and mesh arrays never travel in control messages.

## 4. Handshake

Worker sends `hello` first:

- protocol major/minor.
- Worker version/build ID.
- backend ID (`gmsh`) and Gmsh version.
- supported element, mode, optimization, and quality capabilities.

Main sends `helloAck` with accepted protocol, job ID, absolute workspace path, and main build ID. Different protocol majors fail before loading geometry. Different minors continue only when all required capabilities are supported.

## 5. Message Envelope

Every control message contains:

- stable English `type`.
- protocol version.
- message ID.
- job ID.
- UTC timestamp.
- typed payload.

Messages have explicit length and maximum size. Parse failure, unknown required fields, or job-ID mismatch terminates the job.

## 6. Workspace

```text
<job-root>/
  request.json
  geometry/input.brep
  geometry/geometry-map.json
  output/result.meshbin
  output/geometry-map-result.json
  output/quality.json
  output/result.json
  logs/gmsh.log
  worker.lock
```

- Main writes all input, computes hashes, then seals the request.
- Worker never changes `geometry`.
- Worker writes only `output`, `logs`, and controlled state files.
- Output is written to temporary files, closed, checked, and renamed.
- Main reads output only after success and digest verification.
- Failed logs MAY be retained; temporary large data follows cleanup policy.

## 7. Request

Identity/input fields:

- job ID and source geometry revision.
- geometry snapshot version.
- geometry/map file paths and SHA-256.
- canonical unit `mm`.

Common settings:

- mode: `independent-solids` or `conformal-multi-region`.
- global/minimum/maximum size in millimeters.
- curvature adaptation.
- fixed first-release types Triangle3 and Tetrahedron4.
- optimization flag.
- automatic retry flag and maximum attempts.

The implemented protocol-0.1 executable-adapter profile uses one shared
`MeshGenerationSettings` definition with these additive typed fields:

- `minimumSizeMm` and `maximumSizeMm`: finite values in `[0, 1000000]`; zero
  selects the backend default and a positive minimum MUST NOT exceed a
  positive maximum.
- `curvatureElementsPerCircle`: integer in `[0, 1000]`; zero disables
  curvature-derived sizing and a positive value maps to Gmsh `-clcurv`.
- `optimizeTetrahedra`: boolean, default `false`; true maps to Gmsh
  `-optimize` for first-order tetrahedra.
- `maximumThreads`: integer in `[0, 256]`; zero leaves thread selection to
  Gmsh and a positive value maps to `-nt`.

The default profile is all zero values with optimization disabled and the
approved `Delaunay` volume algorithm. Protocol 0.1 accepts only the typed
`Delaunay` and `Hxt` algorithm values; arbitrary Gmsh algorithm names remain
forbidden. Unknown members or out-of-range values fail before Gmsh starts.
Hard resource-budget defaults remain a later contract.

Backend settings:

- backend ID `gmsh`.
- settings schema version.
- categorized typed values.
- unknown settings fail validation; arbitrary Gmsh command strings are forbidden.
- defaults come from a shared versioned parameter definition.

The implemented explicit resource-limit profile uses a separate
`MeshingResourceLimits` contract:

- `maximumElapsedMilliseconds`: integer in `[0, 86400000]`; zero disables the
  wall-clock limit.
- `maximumWorkingSetBytes`: integer in `[0, 274877906944]`; zero disables the
  combined Worker-plus-active-Gmsh working-set limit.

Both limits default to zero. A configured wall-clock limit is enforced by the
main process and checked cooperatively by the Worker. The working-set limit is
checked at stage transitions and during Gmsh execution at no more than 4 Hz;
it can overshoot by one sampling interval. Exceeding either limit fails with
`MESH_RESOURCE_LIMIT`, removes incomplete formal output, and never commits a
mesh. The Windows main-process adapter places the Worker and its descendants in
a kill-on-close Job Object so hard termination cannot orphan Gmsh. Job Object
committed-memory limits are intentionally not used as a substitute for the
specified working-set metric.

Warning thresholds, dynamic budgets from currently available memory, retry
budgets, and bounded diagnostic-log retention remain later contracts.

## 8. Geometry Mapping Input

`geometry-map.json` schema `cceda.geometry-map/0.2` contains stable snapshot
region IDs, source Assembly/Part/Solid `GeometryId` values, BREP location
information, baked-transform status, known shared Faces/interface candidates,
and excluded objects/reasons.

Every region MUST include `brepBoundsMm` as exactly six JSON numbers in
`[xmin, ymin, zmin, xmax, ymax, zmax]` order. Values are expressed in the
canonical millimeter coordinate system, apply to the located Solid as written
to `input.brep`, MUST be finite, and MUST satisfy `min <= max` on every axis.
The geometry adapter computes the bounds from BREP geometry without requiring
or adding triangulation.

For a multi-region snapshot, the bounds are reconciled against MSH 4.1
`$Entities` volume bounds with an absolute per-coordinate tolerance of
`1e-6 mm`. Every snapshot region MUST match exactly one tetrahedron-bearing
volume, every such volume MUST match exactly one region, and every retained
Triangle3 Surface MUST belong to exactly one matched volume boundary. Missing,
extra, unmatched, or ambiguous entities fail topology preparation; they never
produce a guessed mapping.

For a single-region snapshot, the submitted region count itself proves that
all retained volume and surface entities belong to that region, so no
approximate bounds comparison is required. This preserves valid cases where
Gmsh represents one submitted Solid with more than one volume entity.

A Worker MUST NOT infer stable region identity from BREP import order or Gmsh
tag order. It may use Gmsh tags internally but MUST convert output back to
stable region and geometry IDs.

## 9. Stages and Progress

Stable stages:

1. `initializing`
2. `loading-geometry`
3. `preparing-topology`
4. `generating-surface`
5. `validating-surface`
6. `generating-volume`
7. `optimizing`
8. `evaluating-quality`
9. `writing-result`
10. `completed`

Progress includes stage, optional determinate percentage, processed/total Solids, current node/surface/volume counts, elapsed time, process memory/budget ratio, and a concise message key. Rate limit to at most about 10 updates per second.

The implemented executable-adapter telemetry uses integer
`elapsedMilliseconds` from a monotonic clock and integer `workingSetBytes`.
On Windows, working set is the sum of the Worker and active Gmsh child process
working sets. The Worker samples stage transitions and Gmsh execution at no
more than 4 Hz. Values MUST be non-negative and elapsed time MUST NOT regress.

## 10. Independent-Solid Mode

- Mesh every valid Solid independently.
- Continue other Solids after one fails, but report partial success explicitly.
- Default first-release policy does not silently commit partial results. The task fails unless the user explicitly chooses to retain successful regions.
- Adjacent Solid interfaces may contain two non-matching node sets; mark output non-conformal.
- Associate every element with its source Solid.

## 11. Conformal Multi-Region Mode

- Prefer already shared topology.
- Perform required Fragment/Imprint on the task snapshot copy, never canonical project geometry. The current adapter performs this step with OCCT `BRepAlgoAPI_Splitter` before the BREP is handed to the Worker.
- Return original Solid to one-or-more output-region mapping.
- Shared interfaces have one consistent node and face-element set.
- Unresolved gaps, overlap, or non-manifold results fail the task.
- Distance-only node merging may not hide different interface triangulations.

The implemented snapshot map records `sourceSolidGeometryIds` for every
conformal fragment. A fragment may reference multiple source Solids; its
primary `sourceSolidGeometryId` remains the first stable source ID for element
association, while the complete list remains available on the region mapping.

## 12. Automatic Retry

- Attempt one uses user settings.
- Automatic retry is disabled by default. When enabled, the approved default
  and hard maximum are two attempts.
- The only approved fallback algorithms are `Delaunay` and `Hxt`. Attempt two
  uses the other approved algorithm; it does not silently alter size,
  curvature, optimization, or thread settings.
- Only `MESH_SURFACE_GENERATION_FAILED` and
  `MESH_VOLUME_GENERATION_FAILED` are retryable in this profile. Input,
  topology, resource, cancellation, Worker-loss, output-validation, quality,
  stale-revision, and commit failures are never retried automatically.
- Each attempt uses a fresh Gmsh model.
- Each retry uses a fresh Worker process and a separate workspace while
  retaining the same logical job ID, geometry snapshot, and exact source
  geometry revision.
- Stop at attempt, total elapsed-time, or per-process working-set budget. Time
  already spent by earlier attempts is subtracted before a later Worker starts.
- Record the effective successful profile.
- User summary shows attempt count and main result/reason only.
- Full settings and Gmsh detail stay in diagnostic logs.

## 13. Cancellation

1. Main sends `cancel` with reason and deadline.
2. Worker acknowledges and requests cancellation at safe Gmsh checkpoints.
3. Worker removes incomplete formal output, sends `cancelled`, and exits.
4. Main may terminate the process after the deadline.
5. Cancelled output is never committed.
6. Existing project geometry and mesh remain unchanged.

UI distinguishes `Cancelling` from `Cancelled`.

## 14. Success Output

Worker writes:

- `result.meshbin`: neutral mesh with uncommitted `MeshRevision=0`.
- `geometry-map-result.json`: every submitted region ID and source Solid ID,
  with one or more unique positive Gmsh volume entity tags.
- `quality.json`: metric version, summaries, and issue indexes.
- `result.json`: result, effective settings, counts, time, peak memory, and file hashes.
- bounded `gmsh.log`.

The success control message contains summaries and hashes, never arrays.
Both `result.json` and the success control payload contain a `statistics`
object with non-negative `elapsedMilliseconds` and `peakWorkingSetBytes`.
Peak working set is the maximum combined working-set sample observed during
the job; it is diagnostic telemetry, not a resource-limit assertion.

Before commit, main validates:

- job ID, source geometry revision, and hashes.
- neutral mesh version and count budgets.
- the result map contains exactly the submitted region/Solid set and globally
  unique positive volume tags.
- all element node references.
- unique node/element IDs.
- every retained element uses the Solid association assigned to its own region.
- Worker mesh revision is exactly zero.

Main then assigns a positive `MeshRevision` and writes the committed mesh header.

## 15. Mesh Quality

Quality is a neutral-mesh capability, not a Gmsh setting. A shared `cceda-mesh-quality` module MAY run inside Worker to avoid another large copy.

- Version formulas, direction, ranges, and thresholds.
- Triangle: area, aspect ratio, minimum/maximum angle, normal consistency.
- Tetrahedron: signed volume, aspect ratio, minimum dihedral angle, scaled Jacobian.
- Invalid metrics or negative volume prevent a fully successful result.
- Issue indexes map directly to `MeshElementId`.

## 16. Automatic Optimization

- Changes mesh only, never canonical geometry.
- Recompute all metrics before and after.
- Preserve regions and geometry associations.
- Roll back on negative volume, lost association, invalidity, or material quality regression.
- No manual node editing or local remeshing command in first release.

## 17. Error Contract

Each error includes stable code, failed stage, localizable summary key/arguments, optional source geometry IDs, retryability, and detailed-log path.

Minimum codes:

- `MESH_PROTOCOL_INCOMPATIBLE`
- `MESH_INPUT_INVALID`
- `MESH_GEOMETRY_LOAD_FAILED`
- `MESH_TOPOLOGY_PREPARATION_FAILED`
- `MESH_SURFACE_GENERATION_FAILED`
- `MESH_SURFACE_NOT_CLOSED`
- `MESH_VOLUME_GENERATION_FAILED`
- `MESH_OPTIMIZATION_FAILED`
- `MESH_QUALITY_INVALID`
- `MESH_RESOURCE_LIMIT`
- `MESH_CANCELLED`
- `MESH_WORKER_LOST`
- `MESH_OUTPUT_CORRUPT`
- `MESH_COMMIT_REJECTED`

Do not show only raw Gmsh text or integer codes to users.

## 18. Worker Loss

- Abnormal process exit, IPC disconnect, or heartbeat timeout becomes `WorkerLost`.
- Record exit code, last stage, and last valid progress.
- Quarantine incomplete output.
- Keep the previous current mesh.
- Main stays responsive and offers the diagnostic-log location.
- Retrying always starts a new job and a clean process.

## 19. Resource Protection

- Main computes budget from configured and currently available memory.
- Worker reports working set periodically.
- Memory warning stops retries; hard budget requests cancellation and may terminate Worker.
- Check all count/file-size multiplication for overflow.
- Bound or truncate logs while preserving start, end, and error context.
- Never place a live job workspace inside `.cceda` or overwrite the source STEP.

## 20. Security

- Worker accepts no arbitrary Gmsh script, shell command, or dynamic-library path.
- Normalize paths and require them to stay under job root.
- Bound control message/JSON size, depth, and counts.
- Validate Gmsh settings against a typed whitelist.
- Treat Worker output as untrusted until main validates it.

## 21. Acceptance

1. Incompatible protocol fails before a job starts.
2. Independent mode returns stable per-Solid region mapping.
3. A conformal case has no duplicate interface nodes or inconsistent interface facets.
4. Retry is bounded and produces a concise summary.
5. Cooperative and forced cancellation never create a current mesh.
6. Worker-crash simulation leaves main responsive and old mesh unchanged.
7. Results are committed only after all hash/reference/format checks and independent quality recomputation pass.
8. Quality issues locate by `MeshElementId`.
9. Output requires no Gmsh API to interpret.
