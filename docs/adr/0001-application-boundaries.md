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

The adapter does not make Gmsh `.msh` a domain format. The bounded MSH reader now converts validated output to the neutral mesh contract when the caller supplies stable typed associations. Worker-side input-map consumption, versioned result-map ingestion, quality evaluation, and resource enforcement remain required before the main application can commit a real Worker result.

The neutral `MeshModel` and its structural validator live in the dependency-free domain layer. Binary alignment, compression, block checksums, and result-map serialization remain deliberately unfrozen and are not encoded by the domain model.

The project aggregate owns at most one current neutral mesh. It accepts only a structurally valid uncommitted Worker artifact for the exact current `GeometryRevision`, assigns the next positive `MeshRevision`, and replaces the previous mesh only after every check succeeds. Geometry changes retain that mesh for inspection but mark it stale and non-exportable.

ASCII MSH 4.1 parsing belongs to the Gmsh adapter, not the domain. The reader treats Worker output as untrusted, requires caller-provided byte/count limits, validates the complete low-order Gmsh topology, and removes Gmsh entity tags while applying typed stable entity/region mappings. Connecting those mappings to the protocol-0.1 input map still requires explicit Worker-side Gmsh tag reconciliation.

The OCCT importer treats XCAF leaves as Parts and indexes their unique Solid, Face, Edge, and Vertex topology with opaque IDs scoped to one `GeometryRevision`. The OCCT repository retains the reverse ID-to-shape association and rejects lookups for a different revision; `TopoDS_Shape` remains inside the geometry adapter. These IDs do not imply any relation to Gmsh entity tags.

`IGeometrySnapshotService` defines the standard-library-only port for protocol-0.1 independent-Solid snapshots. Its OCCT adapter captures the domain document and topology map under one repository lock, includes every valid meshable Solid exactly once in an OCCT BREP compound, excludes non-solid Parts and invalid Solids, and never mutates canonical XCAF geometry. `geometry-map.json` assigns deterministic positive region and BREP Solid indexes; retains source Assembly, Part, Solid, and Face IDs; records shared-Face candidates; and carries a row-major location matrix for each region. Locations are preserved in BREP and therefore declared `transformsBaked: false`, rather than being misrepresented as physically baked geometry.

Snapshot publication accepts only an absolute absent-or-empty output directory and a caller byte budget. BREP is written without triangulation to a same-directory temporary file, hashed by streaming SHA-256, and renamed only after the versioned JSON map fits the remaining budget. The JSON map is atomically committed last; scoped cleanup removes the BREP on every failure path. Both final byte digests are returned to the caller for later `request.json` sealing. The 0.1 input-map schema does not claim that the executable Gmsh adapter can yet recover stable source IDs from imported Gmsh tags.

The main-side Worker client is a separate QtCore adapter. It owns `QProcess`, treats standard output as untrusted bounded NDJSON, performs the versioned handshake, checks job and source-revision identity, and emits only neutral progress/result/failure DTOs. A result is published only after both a valid result message and normal process exit; process failures never mutate project state.

The current executable adapter has a matching main-side workspace builder. It accepts only an absent or empty absolute workspace, copies a bounded STEP snapshot while computing SHA-256, fixes all relative input/output paths, and atomically publishes `request.json` last. This protocol-0.1 helper is intentionally named for the executable adapter and does not freeze the final BREP, geometry-map, or meshbin formats.

The application layer owns the per-project meshing job state machine. It captures the current `GeometryRevision`, prepares and verifies large artifacts on background threads, allows at most one active job, and rechecks the authoritative project revision before Worker launch and before publishing a result. Its public DTOs contain no Gmsh tags or process handles. The current success remains explicitly experimental: the controller verifies the manifest identity and intermediate-mesh SHA-256 but does not commit `ProjectState` until the versioned geometry mapping and neutral result formats are complete.

Cancellation is an explicit application and protocol state rather than a generic failure. Main sends one bounded reason and deadline, Worker acknowledges at a safe checkpoint, terminates its Gmsh child, removes formal mesh/result output, reports `cancelled`, and exits. Main uses a single-shot deadline to force-terminate an unresponsive Worker. Preparation and result verification finish safely in the background but suppress publication after cancellation; no cancellation path mutates `ProjectState`.

## Sources

- Qt CMake project setup: https://doc.qt.io/qt-6/cmake-get-started.html
- Qt main-window framework: https://doc.qt.io/qt-6/qmainwindow.html
- Qt model/view architecture: https://doc.qt.io/qt-6/model-view-programming.html
- Qt process lifecycle and channels: https://doc.qt.io/qt-6.8/qprocess.html
- Qt single-shot cancellation deadlines: https://doc.qt.io/qt-6.8/qtimer.html
- Qt atomic file replacement: https://doc.qt.io/qt-6.8/qsavefile.html
- Qt cryptographic hashes: https://doc.qt.io/qt-6.8/qcryptographichash.html
- Qt concurrent function execution: https://doc.qt.io/qt-6.8/qtconcurrentrun.html
- Qt future completion monitoring: https://doc.qt.io/qt-6.8/qfuturewatcher.html
- OCCT overview and data-exchange architecture: https://dev.opencascade.org/doc/overview/html/
- OCCT BREP read/write tools: https://dev.opencascade.org/doc/refman/html/class_b_rep_tools.html
- Gmsh reference manual and official API sequence: https://gmsh.info/doc/texinfo/
