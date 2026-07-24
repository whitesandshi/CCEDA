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

Backend settings:

- backend ID `gmsh`.
- settings schema version.
- categorized typed values.
- unknown settings fail validation; arbitrary Gmsh command strings are forbidden.
- defaults come from a shared versioned parameter definition.

Resource budget:

- memory budget and warning threshold.
- optional time budget.
- retry budget.
- log size limit.

## 8. Geometry Mapping Input

`geometry-map.json` contains stable snapshot region IDs, source Assembly/Part/Solid `GeometryId` values, BREP location information, baked-transform status, known shared Faces/interface candidates, and excluded objects/reasons.

Worker may use Gmsh tags internally but MUST convert output back to stable region and geometry IDs.

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

## 10. Independent-Solid Mode

- Mesh every valid Solid independently.
- Continue other Solids after one fails, but report partial success explicitly.
- Default first-release policy does not silently commit partial results. The task fails unless the user explicitly chooses to retain successful regions.
- Adjacent Solid interfaces may contain two non-matching node sets; mark output non-conformal.
- Associate every element with its source Solid.

## 11. Conformal Multi-Region Mode

- Prefer already shared topology.
- Perform required Gmsh Fragment/Imprint on the task snapshot copy, never canonical project geometry.
- Return original Solid to one-or-more output-region mapping.
- Shared interfaces have one consistent node and face-element set.
- Unresolved gaps, overlap, or non-manifold results fail the task.
- Distance-only node merging may not hide different interface triangulations.

## 12. Automatic Retry

- Attempt one uses user settings.
- Later attempts vary only approved algorithms, size guards, and optimization options.
- Each attempt uses a fresh Gmsh model.
- Stop at attempt, time, or memory budget.
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
- `geometry-map-result.json`: source Solid/Face to regions/interfaces.
- `quality.json`: metric version, summaries, and issue indexes.
- `result.json`: result, effective settings, counts, time, peak memory, and file hashes.
- bounded `gmsh.log`.

The success control message contains summaries and hashes, never arrays.

Before commit, main validates:

- job ID, source geometry revision, and hashes.
- neutral mesh version and count budgets.
- all element node references.
- unique node/element IDs.
- valid region and geometry mappings.
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
7. Results are committed only after all hash/reference/format checks pass.
8. Quality issues locate by `MeshElementId`.
9. Output requires no Gmsh API to interpret.
