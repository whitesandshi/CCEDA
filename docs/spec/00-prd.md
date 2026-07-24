# CCEDA First-Release Product Requirements

Status: Draft  
Version: 0.1  
Target: Windows 10/11 x64, Visual Studio 2022  
Audience: Personal use first; possible open-source or commercial distribution later

## 1. Product Definition

CCEDA is a solver-neutral CAD and mesh preprocessing desktop application for chip-package-scale 3D models. OCCT provides STEP/XCAF geometry, assembly metadata, visualization, validation, and healing. An isolated Gmsh Worker generates triangular surface meshes and tetrahedral volume meshes.

The first release ends with a quality-checked current mesh and Gmsh `.msh` export. Materials, loads, boundary conditions, solver setup, and simulation are not included.

## 2. Goals

- G-01: Reliably import one chip-package STEP model while retaining assembly hierarchy, names, colors, instances, and multi-solid relationships.
- G-02: Inspect Assembly, Part, Solid, Face, Edge, and Vertex information and locate each object in the 3D view.
- G-03: Provide a high meshing success rate for valid or safely repairable complex solids.
- G-04: Store a Gmsh-independent neutral mesh and export it as `.msh`.
- G-05: Produce concise, actionable, and locatable geometry and mesh diagnostics.
- G-06: Keep module boundaries suitable for future meshers, exporters, and platforms.
- G-07: Reopen a saved project without needing the external STEP file.

## 3. Out of Scope

- NG-01: Multiple STEP inputs in one project.
- NG-02: Reimporting or synchronizing an updated source STEP.
- NG-03: Meshing Shell, Face, Wire, or Edge objects that are not valid Solids.
- NG-04: User-authored named groups or Physical Groups.
- NG-05: Local mesh sizing controls.
- NG-06: Manual node movement or element creation/deletion.
- NG-07: True local remeshing.
- NG-08: Multiple saved mesh alternatives or old mesh versions.
- NG-09: Autosave, crash recovery, installer, or automatic updates.
- NG-10: Automated tests; first release uses a fixed manual acceptance suite.

## 4. Primary Workflow

1. Create a project and select one `.step` or `.stp` file.
2. Detect source units and show source scale, model dimensions, and conversion to millimeters.
3. Import XCAF assembly, names, colors, transforms, and B-Rep; embed the original STEP.
4. Run geometry validation automatically.
5. Apply safe repairs automatically and request confirmation for topology-changing repairs.
6. Inspect objects and issues through the model tree, 3D view, and property panel.
7. Configure common and advanced Gmsh parameters and select a meshing mode.
8. Submit the job to an isolated Worker and show stage, progress, elapsed time, and memory.
9. Generate the current mesh, compute quality metrics, and show quality colors and issue elements.
10. Run automatic optimization or change parameters and replace the current mesh by remeshing.
11. Export `.msh` and explicitly save the `.cceda` project.
12. Reopen `.cceda` directly without parsing the external STEP.

## 5. Functional Requirements

### 5.1 Project and Import

- FR-PROJ-001: A project MUST contain exactly one original STEP source.
- FR-PROJ-002: The project MUST embed the original STEP and a directly restorable XCAF geometry document.
- FR-PROJ-003: The project MUST store one current mesh, current settings, reports, and geometry healing history.
- FR-PROJ-004: Successful remeshing MUST replace the old mesh; lightweight job summaries MAY remain.
- FR-PROJ-005: A geometry revision change MUST mark the existing mesh stale and prevent misleading export.
- FR-IMP-001: Import MUST retain assembly hierarchy, definitions, instances, names, colors, and transforms supported by the STEP/XCAF path.
- FR-IMP-002: Import MUST detect the STEP length unit; canonical internal length is millimeters.
- FR-IMP-003: Import MUST record the source unit, scale factor, model bounds, and abnormal-scale warnings.

### 5.2 Geometry Inspection

- FR-GEO-001: The model tree MUST represent Assembly, Part, Solid, Shell, Face, Wire, Edge, and Vertex.
- FR-GEO-002: Tree selection, 3D selection, properties, and issue lists MUST stay synchronized.
- FR-GEO-003: Solid properties MUST include Face/Edge/Vertex counts, bounds, volume, closure, and validity.
- FR-GEO-004: Face properties MUST include Edge/Vertex counts, area, surface type, orientation, and bounds.
- FR-GEO-005: Edge properties MUST include endpoint count, length, curve type, and degeneration state.
- FR-GEO-006: Non-solid objects MUST remain visible and inspectable but MUST NOT enter first-release mesh jobs.
- FR-GEO-007: Validation MUST cover invalid B-Rep, open shells, abnormal tolerances, small edges/faces, degenerate topology, and reliably detectable intersections.

### 5.3 Geometry Healing

- FR-HEAL-001: Repairs proven not to change model meaning SHOULD run automatically.
- FR-HEAL-002: Feature removal, entity merging, sewing/solidification, fragmentation, and other topology changes MUST require confirmation.
- FR-HEAL-003: Unreliable repairs MUST preserve the original geometry and report a locatable issue.
- FR-HEAL-004: Committed geometry healing MUST support undo and redo.
- FR-HEAL-005: Every repair MUST record its issues, targets, action, result, and geometry revisions.

### 5.4 Meshing

- FR-MESH-001: The first release MUST generate Triangle3 surface and Tetrahedron4 volume elements.
- FR-MESH-002: Independent per-Solid meshing MUST be the default mode.
- FR-MESH-003: Conformal multi-region meshing MUST be available, using shared topology first and confirmed imprint/sewing/fragmentation when needed.
- FR-MESH-004: Simple distance-based node merging MUST NOT be the primary conformal strategy.
- FR-MESH-005: The Gmsh dialog MUST expose well-organized common and advanced options, validated defaults, and reset-to-default behavior.
- FR-MESH-006: A failed first attempt MAY trigger bounded fallback algorithms and conservative settings.
- FR-MESH-007: Meshing MUST run in an isolated Worker and support progress, cancellation, and resource limits.
- FR-MESH-008: The domain mesh MUST NOT depend on Gmsh tags, API classes, or in-process state.

### 5.5 Mesh Quality and Repair

- FR-QUAL-001: Triangle metrics MUST include area, aspect ratio, minimum/maximum angle, and normal consistency.
- FR-QUAL-002: Tetrahedron metrics MUST include signed volume, aspect ratio, minimum dihedral angle, and scaled Jacobian.
- FR-QUAL-003: The UI MUST support quality coloring, thresholds, and issue-only display.
- FR-QUAL-004: Selecting an issue MUST zoom to and highlight the exact mesh element.
- FR-QUAL-005: Issue properties MUST show element ID, type, nodes, values, and associated geometry.
- FR-REPAIR-001: First-release mesh repair is limited to automatic optimization and full remeshing with changed settings.
- FR-REPAIR-002: Optimization MUST recompute all metrics and roll back if the mesh becomes invalid or materially worse.

### 5.6 Export

- FR-EXP-001: The first release exports only Gmsh `.msh`.
- FR-EXP-002: Export MUST read the neutral mesh through an `IMeshExporter` boundary.
- FR-EXP-003: Missing, stale, failed, or invalid meshes MUST NOT be exportable as current results.
- FR-EXP-004: MSH version and ASCII/binary form belong to export settings; implementation defaults remain an open decision.

## 6. User Interface

- UI-001: Use Qt 6 Widgets with a dense engineering-tool interaction style inspired by HyperMesh.
- UI-002: Place the assembly/model tree left, OCCT view center, properties right, and issues/tasks/logs at the bottom.
- UI-003: Panels SHOULD be dockable, hideable, and resizable.
- UI-004: Provide geometry shading, mesh surface, wireframe, transparency, and quality-color modes.
- UI-005: No explicit selection filter is required initially, but supported objects must be selectable from tree and view.
- UI-006: Chinese MAY be the first UI language; all user text MUST use Qt internationalization.
- UI-007: Long operations MUST NOT block the UI; default error text is concise and detailed logs are secondary.

## 7. Architecture Constraints

- AR-001: Use Qt Widgets, an application layer, a framework-neutral domain, and ports/adapters.
- AR-002: The domain MUST NOT depend on Qt, OCCT, Gmsh, or Win32.
- AR-003: UI and domain MUST NOT own `TopoDS_Shape`, Gmsh tags, or native lifetime handles.
- AR-004: OCCT/XCAF and AIS MAY live in the main process; expensive geometry work runs through background task scheduling.
- AR-005: Gmsh MUST be encapsulated by a separate `MeshingWorker` process.
- AR-006: Main and Worker MUST use a versioned control protocol and file-based bulk data exchange.
- AR-007: First release uses statically registered adapters and does not promise a dynamic plugin ABI.
- AR-008: Build with CMake and VS2022. Discover Qt under `E:\Qt` without embedding machine paths in source.
- AR-009: Windows is the supported first platform, while core and protocol designs must not prevent Linux later.

## 8. Performance and Reliability

- NFR-PERF-001: Baseline hardware is 4-8 CPU cores, 16 GB RAM, SSD, and modern OpenGL; 32 GB is recommended.
- NFR-PERF-002: Target approximately 200 MB STEP, thousands of Solids, and about 100,000 Faces.
- NFR-PERF-003: Target roughly 3-5 million tetrahedra on 16 GB and up to about 10 million on 32 GB.
- NFR-PERF-004: These are soft goals. Runtime memory budgets MUST control warnings, retries, and cancellation.
- NFR-REL-001: Import, validation, healing, or meshing failures MUST NOT crash the main application.
- NFR-REL-002: Worker loss MUST become a recoverable failed job and MUST NOT replace the current mesh.
- NFR-REL-003: Explicit project save MUST use a temporary file, verification, and same-volume atomic replacement.
- NFR-OBS-001: User reports remain concise; detailed OCCT/Gmsh logs are retained for diagnosis.

## 9. Test and Delivery

- TEST-001: First-release verification is manual and click-driven; no automated test framework is required.
- TEST-002: The model suite MUST cover valid packages, non-solids, geometry defects, independent/conformal meshing, retries, and large meshes.
- TEST-003: Every release candidate MUST complete `05-manual-acceptance.md` with recorded results.
- DELIV-001: Deliver a CMake project and VS2022 solution; no installer is required.
- DELIV-002: Build documentation MUST pin compatible Qt, OCCT, Gmsh, MSVC, and runtime configurations.

## 10. Licensing

- LIC-001: Personal development may use GPL Gmsh.
- LIC-002: Closed-source distribution requires appropriate Gmsh commercial licensing or a replacement mesher.
- LIC-003: OCCT distribution must comply with LGPL 2.1 plus the Open CASCADE exception, or use a commercial license.
- LIC-004: Process isolation is an engineering boundary, not an automatic way around license obligations.
- LIC-005: Release output MUST include a third-party component and license inventory.

Official references: [Gmsh](https://gmsh.info/) and [OCCT](https://dev.opencascade.org/doc/overview/html/).

## 11. First-Release Completion

1. Fixed package STEP models preserve assembly metadata and survive project save/reopen.
2. Required Solid, Face, Edge, and Vertex properties are inspectable.
3. Issues are locatable; repair confirmation, rollback, undo, and redo follow policy.
4. Independent meshing and at least one conformal multi-region case pass.
5. Quality coloring, filtering, and issue-element location work.
6. The current mesh exports and can be read back by Gmsh.
7. Cancel, failure, Worker loss, and memory warnings do not corrupt the project.
8. All manual acceptance cases have recorded outcomes.

## 12. Decisions Required Before Coding

- Exact Qt 6/MSVC kit and CMake discovery path under `E:\Qt`.
- Gmsh version, build, Worker launch configuration, and licensing path.
- Default MSH version, ASCII/binary mode, and region-writing policy.
- Initial OCCT validation and healing algorithm whitelist.
- Exact metric formulas, default quality thresholds, and color direction.
- Worker control transport: `QLocalSocket` or length-prefixed standard streams.
- Neutral mesh block alignment, compression, and byte-order details.
- Actual manual acceptance files, redistribution rights, and baseline machine.

These decisions do not change product scope, but they must be frozen before implementation defaults are written into UI or adapters.
