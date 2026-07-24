# CCEDA First-Release Manual Acceptance

Status: Draft  
Version: 0.1  
Depends on: [00-prd.md](00-prd.md), [01-domain-model.md](01-domain-model.md), [02-project-format.md](02-project-format.md), [03-geometry-service.md](03-geometry-service.md), [04-meshing-protocol.md](04-meshing-protocol.md)

## 1. Principles

- Verification is manual and click-driven; no automation framework is required.
- Every candidate build uses the same model set and steps.
- Failures record stage, stable code, model, settings, and log path.
- "No crash" is not functional success.
- Mesh success requires topology, counts, quality, mapping, and project-state checks.

## 2. Environment Record

Freeze and record the actual versions before acceptance:

- Windows version and architecture.
- Visual Studio/MSVC toolset.
- Qt 6 kit under `E:\Qt`.
- OCCT version and build configuration.
- Gmsh version and Worker build ID.
- CPU, memory, GPU, and free disk space.

Use a clean task directory; never use the build output directory as a project workspace.

## 3. Model Set

- M-01 Valid package assembly: repeated instances, names, colors, and at least two Solids.
- M-02 Unit/scale model: explicit source unit plus an abnormal-scale file.
- M-03 Non-solid model: Shell, independent Face, Wire, or Edge.
- M-04 Safe-healing model: defects covered by automatic safe repair.
- M-05 Confirmation-healing model: sewing, solidification, small-feature, or Fragment candidates.
- M-06 Independent assembly: touching, gapped, or overlapping Solid candidates.
- M-07 Conformal assembly: valid shared-interface multi-region model.
- M-08 Quality model: low angle, high aspect, and negative-volume risk elements.
- M-09 Failure model: irreparable geometry or extreme scale.
- M-10 Scale model: near the product baseline, approximately 200 MB STEP, thousands of Solids, or millions of tetrahedra.

## 4. Acceptance Cases

### AC-001 Import and Units

Create a project, select M-01, confirm the unit dialog, and wait for validation.  
Expected: assembly, names, colors, transforms, counts, millimeter dimensions, embedded original STEP, and warnings are correct.

### AC-002 Geometry Properties

Select a Solid, Face, Edge, and Vertex from both tree and view.  
Expected: both paths highlight the same object and the property panel shows the required topology counts and derived values.

### AC-003 Non-solids

Open M-03, inspect each non-solid kind, and attempt to submit a mesh job.  
Expected: objects remain visible/inspectable, are marked non-meshable, and the job is rejected with a concise reason.

### AC-004 Issue Location

Open M-04/M-05 and click issue-list entries.  
Expected: view zooms/highlights the target; issue shows stable code, severity, summary, and repairability.

### AC-005 Safe Healing

Run the default safe repair on M-04, save, undo, and redo.  
Expected: revisions and history update, snapshots restore consistent geometry, and old mesh becomes stale.

### AC-006 Confirmation Healing

Plan a topology-changing action on M-05, cancel it, then confirm it.  
Expected: cancel leaves project unchanged; confirmation shows impact and ID mapping; failure rolls back.

### AC-007 Gmsh Settings

Browse common/advanced dialog groups, change size/algorithm, and restore defaults.  
Expected: values are typed/validated, units are clear, defaults restore, and final settings are recorded in the job.

### AC-008 Independent Meshing

Run M-06 in default independent mode for surface and volume mesh.  
Expected: Worker runs separately, every Solid has a region mapping, current mesh becomes Ready, and non-conformal interfaces are explicit.

### AC-009 Conformal Meshing

Run M-07 in conformal mode.  
Expected: task-only fragmentation does not mutate canonical geometry; interface nodes/facets are shared; source-to-region mapping is available.

### AC-010 Quality and Location

Run M-08, enable quality colors, set thresholds, show issues only, and visit previous/next issue.  
Expected: legend direction is correct; issue list and view stay linked; element properties include ID, nodes, metric, and source geometry.

### AC-011 Optimization

Optimize M-08 and compare summaries.  
Expected: geometry is unchanged, all metrics recompute, invalid or worse results roll back, and no manual node editor exists.

### AC-012 Failure and Retry

Run M-09 with automatic retry enabled.  
Expected: attempt count is bounded; final summary is concise; each attempt is traceable in logs; old mesh is not replaced.

### AC-013 Cancel and Worker Loss

Cancel an M-10 job; in a separate run terminate the Worker.  
Expected: main UI stays responsive, job becomes Cancelled/WorkerLost, project/current mesh remain valid, and incomplete output is quarantined.

### AC-014 Save and Reopen

Import, heal, mesh, inspect, save `.cceda`, close, and reopen without the source STEP path.  
Expected: assembly, metadata, geometry, current mesh, settings, reports, and revision state recover.

### AC-015 Export

Export a Ready mesh to `.msh` and read it with Gmsh or a compatible reader.  
Expected: node/triangle/tetra/region counts agree, coordinates are millimeters, and stale/failed meshes cannot export.

### AC-016 Save Failure Protection

Simulate an unwritable target or insufficient disk during save.  
Expected: previous `.cceda` remains readable, temporary file does not replace it, and UI identifies the error/log.

## 5. Evidence

Record build/dependency versions, model digest and ID, mode/settings digest, expected/actual result, screenshot path, error code/log path, elapsed time, peak memory, and node/element counts.

## 6. Release Gates

- AC-001 through AC-006 MUST pass.
- AC-007 through AC-011 MUST pass on one valid assembly and one issue model.
- AC-012 through AC-016 MUST not corrupt projects, crash main, or silently accept invalid results.
- M-10 may exceed soft performance targets, but must provide warning, cancellation, and a comprehensible failure.
- Any OCCT or Gmsh version change reruns affected cases.
- The checklist and evidence ship with the candidate build record.

## 7. Accepted Risks

- No automated tests means higher regression risk during refactoring.
- No autosave means unsaved work can be lost after abnormal exit.
- No local remeshing means a local quality defect may require full remeshing.
- Complex gaps, slivers, and non-manifold interfaces can still defeat conformal meshing.
