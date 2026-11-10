# CCEDA Preprocessor

CCEDA is a solver-neutral geometry and mesh preprocessing desktop application. The current vertical slice uses Qt 6 Widgets for the workbench UI and Open CASCADE Technology (OCCT) 8.0.0 for STEP/XCAF import and 3D visualization.

## Features

- Open `.step` and `.stp` files asynchronously.
- Preserve the XCAF assembly hierarchy, names, colors, and placements.
- Inspect solid, face, edge, vertex, volume, validity, and meshability counts.
- Create a revision-bound independent-Solid OCCT BREP snapshot with per-region bounds signatures, a versioned geometry map, and SHA-256 digests.
- Select independent-Solid or conformal multi-region meshing; conformal jobs fragment only their revision-bound OCCT snapshot and retain source-Solid mappings.
- Select tree entities and highlight the corresponding shape.
- Pan, rotate, zoom, fit, and restore an isometric view.
- Open a STEP file directly from the command line.
- Run an experimental isolated Gmsh executable Worker with a bounded versioned handshake.
- Prepare an isolated job workspace from revision-bound BREP and geometry-map artifacts, validating both SHA-256 digests and publishing the request last.
- Launch that Worker asynchronously from a main-process client that validates control messages, job identity, geometry revision, and output digests before publishing a result.
- Convert bounded ASCII MSH 4.1 output to the neutral mesh contract using stable typed entity mappings.
- Export a committed neutral mesh to an exclusively created staged ASCII MSH 4.1 file with stable tags and region Physical Groups.
- Publish a move-only staged export only after rechecking exact mesh identity and revisions, using same-directory Windows replacement that preserves the final file on failure.
- Export the current mesh from the workbench through a save dialog, background MSH staging, owner-thread revision recheck, and typed success or failure feedback.
- Write a versioned `MeshRevision=0` neutral `result.meshbin` Worker artifact, then verify and strictly decode it in the main process.
- Validate solver-neutral mesh IDs, references, regions, geometry associations, and revision ownership.
- Evaluate neutral Triangle3 and Tetrahedron4 area, shape, angle, orientation, signed-volume, and scaled-Jacobian metrics.
- Publish a bounded canonical `quality.json` with a SHA-256 digest and independently recompute it in the main process.
- Commit a quality-valid Worker mesh through `ProjectState` with a monotonically increasing positive mesh revision.
- Reject experimental Worker results when their geometry revision or mapped output artifacts are stale or invalid.
- Submit the current revision from the Qt workbench with common/advanced typed settings, Worker stage/progress, cancellation, live elapsed-time and working-set telemetry, and committed mesh counts.
- Configure disabled-by-default wall-clock and combined-working-set limits; resource failures terminate the Worker tree, remove formal output, and preserve the current mesh.
- Prepare committed Triangle3 surfaces off the UI thread, reject stale revision results, and show or hide the OCCT mesh presentation.
- Inspect accepted quality summaries and exact issue records bound to the committed mesh revision.
- Apply surface-quality colors from the Quality panel using explicit numeric thresholds or normal consistency, with background preparation and an applied legend.
- Select a quality issue to reveal and frame its exact Triangle3 or Tetrahedron4 element in the current revision.
- Inspect the selected issue element's ordered nodes, region, geometry association, and applicable metric values without scanning the full mesh.
- Filter the displayed committed mesh to its accepted issue elements without applying implicit quality thresholds or colors.

## Build

The default preset expects these existing installations:

```text
Qt:   E:\Qt\6.8.3\msvc2022_64
OCCT: D:\Projects\OpenCASCADE-8.0.0\install
Gmsh: D:\Projects\gmsh\install\bin\gmsh.exe
```

Configure and build from a Visual Studio developer prompt or a regular terminal with CMake available:

```powershell
cmake --preset vs2022-x64
cmake --build --preset release
ctest --preset release
```

Open `build-vs2022\CCEDA.sln` in Visual Studio 2022 for normal development. The solution intentionally contains only the `Release | x64` configuration because the installed OCCT libraries are Release builds.

The build deploys the required Qt and OCCT runtime files beside `build-vs2022\Release\CCEDA.exe`, builds `cceda_gmsh_worker.exe` in the same directory, and configures the documented Gmsh executable path. The application configures OCCT resource paths itself.

## Product specifications

The product contract is in [`docs/spec`](docs/spec/README.md), and architectural decisions are in [`docs/adr`](docs/adr). The original Win32 integration spike is preserved in [`prototype/win32`](prototype/win32).

The first Gmsh Worker slice uses an explicitly bounded executable adapter because the installed Gmsh build has no linkable C++ SDK. Its process test covers protocol-major rejection, BREP/map hash and revision validation, real BREP volume meshing with the local Gmsh executable, ASCII MSH 4.1 inspection, and atomic publication of an experimental intermediate result.

The main process prepares each executable-adapter workspace from an empty directory, copies `input.brep` and `geometry-map.json` through one combined byte budget while recomputing and comparing both SHA-256 digests, validates the versioned map against the submitted geometry revision, creates only controlled directories, and publishes `request.json` last with an atomic save.

The Worker consumes the bounded geometry map before launching Gmsh. It parses MSH 4.1 entity bounds and volume-boundary Surfaces, uniquely reconciles independent-Solid multi-region snapshots without relying on import or tag order, associates retained elements with stable regions and source Solids, and atomically writes a versioned `MeshRevision=0` `result.meshbin`. Missing, extra, unmatched, or ambiguous entities fail with `MESH_TOPOLOGY_PREPARATION_FAILED`.

The main-process Worker client uses asynchronous Qt process I/O, validates every newline-delimited control message against the protocol envelope, sends the versioned handshake, and propagates intermediate mesh, neutral mesh, geometry-map, and quality digests only after a matching geometry revision and normal Worker exit. The Controller accepts BREP/map snapshot metadata rather than a STEP path, re-verifies the sealed input map and fixed result paths against their hashes, decodes `result.meshbin` under explicit format/count/string budgets, strictly parses the bounded quality artifact, independently recomputes its canonical form, and cross-checks revision, backend, counts, region, and every geometry association before atomically committing through `ProjectState`.

The Gmsh adapter now has a dependency-free ASCII MSH 4.1 reader with explicit byte, line, count, and entity-block budgets. It accepts the low-order Point, Line2, Triangle3, and Tetrahedron4 blocks written by the configured Gmsh executable, validates every tag and node reference, discards auxiliary 0D/1D elements, and converts retained elements only when stable entity and region mappings are supplied.

The domain now defines a storage-neutral `MeshModel` and validates Worker-versus-committed revision rules, IDs, references, regions, and geometry associations without Qt, OCCT, or Gmsh types.

The standard-library-only neutral mesh codec implements the versioned `meshbin` 0.1 contract with a checksummed header and block directory, deterministic GeometryId associations, explicit file/count/string budgets, Worker-versus-committed revision modes, and compatible skipping of checksum-valid unknown optional blocks. It has no Qt, OCCT, or Gmsh dependency.

The standard-library-only quality module evaluates the neutral mesh with versioned metric IDs and exact element issue indexes. It detects degenerate or non-finite elements, inverted tetrahedra, inconsistent triangle orientation, and non-manifold surface edges without Qt, OCCT, or Gmsh types.

`ProjectState` owns the current neutral mesh, preserves stale meshes for inspection after geometry changes, rejects invalid or obsolete Worker artifacts without changing project state, and assigns monotonically increasing positive mesh revisions only on successful commit. It also captures revision-bound read-only export snapshots and rechecks their shared identity and revisions before they may be treated as current results.

The domain export-settings contract requires an explicit format version, ASCII or binary encoding, and a bounded canonical region-policy ID. It deliberately supplies no product defaults and leaves supported-value whitelists to concrete exporters.

The application-layer `IMeshExporter` contract accepts only a structurally valid committed neutral mesh, explicit settings, and an application-generated normalized absolute staging path. It is a no-throw writer that reports typed outcomes and explicitly transfers staging-file ownership, but never receives or publishes the user-selected final path.

The export transaction captures a move-only current-mesh capability on the `ProjectState` owner thread, permits only staging to move to a background thread, and returns a move-only staged-file capability. Publication must return to the owner thread, recheck shared mesh identity plus geometry and mesh revisions, then consume the staging file through the Windows same-directory publisher. Geometry changes, successful remeshing, abandoned staging, locked targets, and every failure preserve the final file and trigger owned-stage cleanup.

The initial concrete Gmsh exporter supports only explicitly requested MSH 4.1 ASCII output with the `physical-groups` region policy. It preserves neutral node and element IDs, groups elements by stable region and geometry association, maps representable region IDs to dimensioned Physical Groups, creates staging files exclusively, and removes exporter-created partial output on failure. Other format combinations fail closed without creating a file.

The workbench composes that exporter with the Windows publisher through `MeshExportJobController`. The controller begins and publishes on the `ProjectState` owner thread, stages through `QtConcurrent`, rejects concurrent export requests, and returns only typed outcomes to the UI. The export action is available only for a current committed mesh and shows bounded busy, success, stale, and failure states without exposing adapter diagnostics as user-facing text.

The geometry adapter now creates protocol-0.2 independent-Solid snapshots from one atomic repository revision. It writes only valid meshable Solids to `input.brep`, assigns deterministic snapshot region IDs, records Assembly/Part/Solid/Face IDs, OCCT location matrices, and finite canonical-millimeter BREP bounds in `geometry-map.json`, bounds the combined artifacts, and returns SHA-256 for both files without mutating canonical XCAF geometry. Shape locations are preserved but explicitly marked as not baked.

Meshing now exposes a typed Delaunay/HXT algorithm whitelist and an opt-in automatic retry policy. Retry is disabled by default and bounded to two attempts. Only surface- or volume-generation failures switch to the alternate algorithm; later attempts preserve all other settings, start a fresh Worker in a separate workspace, retain failed-attempt diagnostics, and consume the remaining total time budget. Resource, input, topology, cancellation, Worker-loss, output, quality, stale-revision, and commit failures remain terminal.

This is not yet the complete product mesh pipeline. The intermediate Worker `.msh` remains diagnostic and the executable adapter is still reported as experimental. Stable Face reconciliation, project persistence, preset quality thresholds, and volume quality coloring remain future slices.
