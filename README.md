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
- Prepare an isolated executable-adapter job workspace with bounded STEP copying, streaming SHA-256, fixed output paths, and request publication last.
- Launch that Worker asynchronously from a main-process client that validates control messages, job identity, and geometry revision before publishing a result.
- Convert bounded ASCII MSH 4.1 output to the neutral mesh contract using stable typed entity mappings.
- Validate solver-neutral mesh IDs, references, regions, geometry associations, and revision ownership.
- Atomically commit a validated Worker mesh to the exact current geometry revision.

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

The first Gmsh Worker slice uses an explicitly bounded executable adapter because the installed Gmsh build has no linkable C++ SDK. Its process test covers protocol-major rejection, workspace and input-hash validation, real STEP volume meshing, ASCII MSH 4.1 inspection, and atomic publication of an experimental intermediate result.

The main process prepares each executable-adapter workspace from an empty directory, copies the STEP snapshot through a caller-provided byte budget while computing SHA-256, creates only the controlled geometry/output/log directories, and publishes `request.json` last with an atomic save.

The main-process Worker client uses asynchronous Qt process I/O, validates every newline-delimited control message against the protocol envelope, sends the versioned handshake, and publishes success only after a matching geometry revision and normal Worker exit. Its integration test exercises the real local Worker/Gmsh path and rejects a stale result revision.

The Gmsh adapter now has a dependency-free ASCII MSH 4.1 reader with explicit byte, line, count, and entity-block budgets. It accepts the low-order Point, Line2, Triangle3, and Tetrahedron4 blocks written by the configured Gmsh executable, validates every tag and node reference, discards auxiliary 0D/1D elements, and converts retained elements only when stable entity and region mappings are supplied.

The domain now defines a storage-neutral `MeshModel` and validates Worker-versus-committed revision rules, IDs, references, regions, and geometry associations without Qt, OCCT, or Gmsh types.

`ProjectState` owns the current neutral mesh, preserves stale meshes for inspection after geometry changes, rejects invalid or obsolete Worker artifacts without changing project state, and assigns monotonically increasing positive mesh revisions only on successful commit.

The geometry adapter now creates protocol-0.1 independent-Solid snapshots from one atomic repository revision. It writes only valid meshable Solids to `input.brep`, assigns deterministic snapshot region IDs, records Assembly/Part/Solid/Face IDs and OCCT location matrices in `geometry-map.json`, bounds the combined artifacts, and returns SHA-256 for both files without mutating canonical XCAF geometry. Shape locations are preserved but explicitly marked as not baked.

This is not yet the product mesh pipeline. The intermediate `.msh` is marked experimental and is not committed as a domain mesh. Worker-side `geometry-map.json` consumption and Gmsh-tag reconciliation, neutral `result.meshbin` serialization, quality evaluation, resource enforcement, and UI task submission remain future slices.
