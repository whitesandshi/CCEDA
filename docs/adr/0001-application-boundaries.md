# ADR 0001: Qt Workbench With OCCT Adapters And An Isolated Gmsh Worker

- Status: Accepted
- Date: 2026-07-23

## Context

CCEDA needs a Windows desktop workbench that can inspect large STEP/XCAF models, generate meshes without binding the product to one solver, and later add mesh engines without exposing engine-specific objects throughout the codebase.

The local environment provides Qt 6.8.3 and OCCT 8.0.0. A Gmsh executable is installed, but its CMake cache has both ENABLE_BUILD_LIB and ENABLE_BUILD_SHARED disabled, so no supported linkable C++ SDK is present.

## Decision

- Use Qt Widgets with QMainWindow, dock widgets, and Qt model/view for the desktop workbench.
- Keep domain state as dependency-free C++ contracts.
- Put STEP/XCAF import and geometry queries behind IGeometryService.
- Put AIS/V3d rendering and navigation behind IViewportController.
- Run meshing in a separate Gmsh Worker process behind the versioned job/result protocol specified in docs/spec/04-meshing-protocol.md.
- Treat GeometryRevision as the ownership key for every mesh result.
- Build one complete vertical slice at a time and retain contract and adapter tests.

## Consequences

The UI can evolve without owning OCCT topology objects, and a failing mesher cannot directly corrupt in-process project state. Worker startup and protocol handling add implementation work, but they also provide cancellation, crash containment, progress reporting, and future mesher replacement points.

The first Worker slice uses the explicitly documented executable-based adapter because the local Gmsh build has no linkable SDK. It establishes process isolation, protocol-major rejection, bounded request validation, real STEP meshing, and experimental intermediate-result verification.

The adapter does not make Gmsh `.msh` a domain format. The bounded MSH reader now converts validated output to the neutral mesh contract when the caller supplies stable typed associations. Versioned association-file ingestion, quality evaluation, and cancellation/resource enforcement remain required before the main application can commit a real Worker result.

The neutral `MeshModel` and its structural validator live in the dependency-free domain layer. Binary alignment, compression, block checksums, and geometry-map serialization remain deliberately unfrozen and are not encoded by the domain model.

The project aggregate owns at most one current neutral mesh. It accepts only a structurally valid uncommitted Worker artifact for the exact current `GeometryRevision`, assigns the next positive `MeshRevision`, and replaces the previous mesh only after every check succeeds. Geometry changes retain that mesh for inspection but mark it stale and non-exportable.

ASCII MSH 4.1 parsing belongs to the Gmsh adapter, not the domain. The reader treats Worker output as untrusted, requires caller-provided byte/count limits, validates the complete low-order Gmsh topology, and removes Gmsh entity tags while applying typed stable entity/region mappings. The typed mapping contract deliberately does not freeze the pending `geometry-map.json` storage schema.

The main-side Worker client is a separate QtCore adapter. It owns `QProcess`, treats standard output as untrusted bounded NDJSON, performs the versioned handshake, checks job and source-revision identity, and emits only neutral progress/result/failure DTOs. A result is published only after both a valid result message and normal process exit; process failures never mutate project state.

## Sources

- Qt CMake project setup: https://doc.qt.io/qt-6/cmake-get-started.html
- Qt main-window framework: https://doc.qt.io/qt-6/qmainwindow.html
- Qt model/view architecture: https://doc.qt.io/qt-6/model-view-programming.html
- Qt process lifecycle and channels: https://doc.qt.io/qt-6.8/qprocess.html
- OCCT overview and data-exchange architecture: https://dev.opencascade.org/doc/overview/html/
- Gmsh reference manual and official API sequence: https://gmsh.info/doc/texinfo/
