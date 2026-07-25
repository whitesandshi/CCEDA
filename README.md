# CCEDA Preprocessor

CCEDA is a solver-neutral geometry and mesh preprocessing desktop application. The current vertical slice uses Qt 6 Widgets for the workbench UI and Open CASCADE Technology (OCCT) 8.0.0 for STEP/XCAF import and 3D visualization.

## Features

- Open `.step` and `.stp` files asynchronously.
- Preserve the XCAF assembly hierarchy, names, colors, and placements.
- Inspect solid, face, edge, vertex, volume, validity, and meshability counts.
- Create a revision-bound independent-Solid OCCT BREP snapshot with a versioned geometry map and SHA-256 digests.
- Select tree entities and highlight the corresponding shape.
- Pan, rotate, zoom, fit, and restore an isometric view.
- Open a STEP file directly from the command line.
- Run an experimental isolated Gmsh executable Worker with a bounded versioned handshake.
- Prepare an isolated job workspace from revision-bound BREP and geometry-map artifacts, validating both SHA-256 digests and publishing the request last.
- Launch that Worker asynchronously from a main-process client that validates control messages, job identity, geometry revision, and output digests before publishing a result.
- Convert bounded ASCII MSH 4.1 output to the neutral mesh contract using stable typed entity mappings.
- Validate solver-neutral mesh IDs, references, regions, geometry associations, and revision ownership.
- Reject experimental Worker results when their geometry revision or mapped output artifacts are stale or invalid.

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

The build deploys the required Qt and OCCT runtime files beside `build-vs2022\Release\CCEDA.exe`. The application configures OCCT resource paths itself.

## Product specifications

The product contract is in [`docs/spec`](docs/spec/README.md), and architectural decisions are in [`docs/adr`](docs/adr). The original Win32 integration spike is preserved in [`prototype/win32`](prototype/win32).

The first Gmsh Worker slice uses an explicitly bounded executable adapter because the installed Gmsh build has no linkable C++ SDK. Its process test covers protocol-major rejection, BREP/map hash and revision validation, real BREP volume meshing with the local Gmsh executable, ASCII MSH 4.1 inspection, and atomic publication of an experimental intermediate result.

The main process prepares each executable-adapter workspace from an empty directory, copies `input.brep` and `geometry-map.json` through one combined byte budget while recomputing and comparing both SHA-256 digests, validates the versioned map against the submitted geometry revision, creates only controlled directories, and publishes `request.json` last with an atomic save.

The Worker consumes the bounded geometry map before launching Gmsh. For the currently supported single-region case it extracts the actual tetrahedron volume entity tags from MSH 4.1 and writes `geometry-map-result.json` using the stable snapshot region and Solid identity. It rejects multi-region snapshots with `MESH_TOPOLOGY_PREPARATION_FAILED` because the current map does not yet contain enough geometric signatures to prove a tag reconciliation without relying on import order.

The main-process Worker client uses asynchronous Qt process I/O, validates every newline-delimited control message against the protocol envelope, sends the versioned handshake, and propagates mesh and geometry-map digests only after a matching geometry revision and normal Worker exit. The Controller accepts BREP/map snapshot metadata rather than a STEP path, verifies both result files and their hashes, and checks the returned region/Solid identity and unique positive Gmsh volume tags against the submitted map before publishing experimental success.

The Gmsh adapter now has a dependency-free ASCII MSH 4.1 reader with explicit byte, line, count, and entity-block budgets. It accepts the low-order Point, Line2, Triangle3, and Tetrahedron4 blocks written by the configured Gmsh executable, validates every tag and node reference, discards auxiliary 0D/1D elements, and converts retained elements only when stable entity and region mappings are supplied.

The domain now defines a storage-neutral `MeshModel` and validates Worker-versus-committed revision rules, IDs, references, regions, and geometry associations without Qt, OCCT, or Gmsh types.

`ProjectState` owns the current neutral mesh, preserves stale meshes for inspection after geometry changes, rejects invalid or obsolete Worker artifacts without changing project state, and assigns monotonically increasing positive mesh revisions only on successful commit.

The geometry adapter now creates protocol-0.1 independent-Solid snapshots from one atomic repository revision. It writes only valid meshable Solids to `input.brep`, assigns deterministic snapshot region IDs, records Assembly/Part/Solid/Face IDs and OCCT location matrices in `geometry-map.json`, bounds the combined artifacts, and returns SHA-256 for both files without mutating canonical XCAF geometry. Shape locations are preserved but explicitly marked as not baked.

This is not yet the product mesh pipeline. The intermediate `.msh` is marked experimental and is not committed as a domain mesh. Proven multi-region Solid/Face reconciliation, neutral `result.meshbin` serialization, quality evaluation, resource enforcement, and UI task submission remain future slices.
