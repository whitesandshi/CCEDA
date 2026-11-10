# Implementation Plan: Quality-Gated Mesh Commit

## Overview

The protocol-0.1 Worker path produces a solver-neutral `MeshRevision=0`
artifact and a bounded quality artifact that the main process independently
recomputes. The Controller now commits a quality-valid mesh through
`ProjectState`; the Qt workbench now exposes the bounded independent-Solid submission workflow.

## Current Progress

- STEP/XCAF import, hierarchy inspection, properties, and OCCT display are implemented.
- Revision-bound BREP snapshots and stable geometry maps are implemented.
- The executable Gmsh Worker, cancellation, MSH 4.1 conversion, neutral meshbin publication, and main-process validation are implemented.
- The executable Gmsh Worker publishes canonical `quality.json` with a SHA-256 digest after neutral mesh evaluation.
- The main process strictly parses the quality artifact and compares it with an independent evaluation of the decoded neutral mesh.
- The Controller commits the verified neutral mesh on the project-owning thread and exposes its positive mesh revision.
- The Qt workbench exposes minimum/maximum size, asynchronous snapshot preparation, Worker stage/progress, cancellation, terminal quality-valid mesh summaries, and asynchronous committed-mesh surface display.
- The workbench retains accepted quality summaries and exact issue records against the committed mesh revision and exposes them through a scalable inspection view.
- Selecting an accepted quality issue reveals and frames its exact Triangle3 or Tetrahedron4 element only when the displayed mesh revision matches.
- The Quality view shows direct-keyed metadata and applicable metric values for only the unique accepted issue elements.
- The Quality view can filter the displayed committed mesh to the bounded accepted issue set after its revision-matched presentation is ready.
- The project aggregate captures revision-bound immutable export snapshots and can recheck their shared identity and revisions before publication.
- Export settings require an explicit format version, encoding, and bounded canonical region-policy ID; no product defaults are encoded.
- The application exposes a staged neutral-mesh exporter port with committed-mesh, explicit-settings, and normalized-path validation; final publication remains application-owned.
- The first concrete exporter streams deterministic ASCII MSH 4.1 with stable neutral tags and region Physical Groups to an exclusively created staging file.
- The workbench exposes typed advanced Gmsh controls and validated live/terminal elapsed-time and combined working-set telemetry.
- Explicit wall-clock and combined-working-set limits now round-trip through the Worker protocol, terminate the contained process tree on timeout, and preserve any existing current mesh on failure.

## Architecture Decisions

- Quality evaluation consumes only `domain::MeshModel`; it does not depend on Qt, OCCT, or Gmsh.
- Metric identifiers and formulas are versioned in the product specification before implementation.
- Structural mesh validation remains owned by `domain`; the quality module reports geometric invalidity such as zero-area triangles and non-positive tetrahedra.
- Worker quality JSON is a strict Qt protocol adapter over the dependency-free report; quality visualization, thresholds, persistence, and optimization remain later slices.

## Task List

### Phase 1: Contract

- [x] Specify metric IDs, formulas, orientation rules, and invalidity semantics.
- [x] Define a small C++17 quality result contract over neutral mesh IDs.

### Phase 2: Evaluator

- [x] Add failing tests for reference elements, orientation consistency, and invalid elements.
- [x] Implement the dependency-free evaluator and make focused tests pass.

### Checkpoint: Quality Foundation

- [x] Release build succeeds.
- [x] Full Release test suite passes.
- [x] No Qt, OCCT, or Gmsh type crosses into the quality module.

### Phase 3: Worker Quality Artifact

- [x] Freeze the bounded `cceda.mesh-quality-worker/0.1` JSON contract.
- [x] Add strict codec boundary tests before implementation.
- [x] Publish canonical `quality.json` and its SHA-256 from the Worker.
- [x] Validate and independently recompute quality in the main process.
- [x] Run focused, integration, and full Release verification.

### Phase 4: Quality-Gated Project Commit

- [x] Add a failing Controller integration test for a successful project commit.
- [x] Move the decoded neutral mesh from background validation without a large copy.
- [x] Commit through `ProjectState` only after canonical quality recomputation.
- [x] Preserve the prior current mesh when output validation fails or cancellation wins.
- [x] Run code review, Release build, and full Release verification.

### Phase 5: Workbench Mesh Submission

- [x] Add the meshing task panel component test before implementation.
- [x] Expose validated minimum and maximum mesh sizes.
- [x] Create a revision-bound snapshot asynchronously and submit it through the Controller.
- [x] Show Worker stage/progress, cancellation, failure, and committed mesh counts.
- [x] Configure deterministic Worker, Gmsh, and job-root paths in the application composition root.

### Phase 6: Committed Mesh Surface Visualization

- [x] Expose a stable read-only handle to the current committed neutral mesh.
- [x] Convert Triangle3 elements to an OCCT triangulation outside the UI thread.
- [x] Display the prepared mesh through the viewport port without exposing OCCT types to UI or domain.
- [x] Add a mesh-surface visibility toggle and clear stale presentations after geometry changes.
- [x] Run focused, full Release, and desktop runtime verification.

### Phase 7: Accepted Mesh Quality Inspection

- [x] Retain the bounded, independently verified quality artifact as an immutable application snapshot.
- [x] Bind the accepted quality snapshot to the positive committed mesh revision.
- [x] Add a non-editable quality inspection view for metric summaries and exact element issues.
- [x] Clear stale inspection state when geometry changes and replace it only after a successful mesh commit.
- [x] Run focused, full Release, and desktop runtime verification.

### Phase 8: Advanced Meshing Controls And Resource Reporting

- [x] Define one dependency-free mesh-generation settings contract with validated defaults.
- [x] Seal curvature sampling, tetrahedron optimization, and thread limits into the Worker request.
- [x] Add compact common/advanced controls and reset-to-default behavior to the task panel.
- [x] Report monotonic elapsed time and current/peak working set through progress and success results.
- [x] Validate telemetry in the main process and expose it in the task panel without blocking the UI.
- [x] Run focused and full Release verification.
- [ ] Complete desktop runtime verification from an interactive Windows session.

### Phase 9: Explicit Meshing Resource Limits

- [x] Define a dependency-free wall-clock and combined-working-set limit contract.
- [x] Seal configured limits into the Worker request and successful result audit trail.
- [x] Enforce wall-clock limits in the main process with cooperative Worker checkpoints.
- [x] Enforce sampled combined-working-set limits without confusing them with committed-memory limits.
- [x] Expose disabled-by-default time and memory limits in the advanced task controls.
- [x] Prove resource-limit failure preserves the existing project mesh and removes formal output.
- [x] Run focused and full Release verification.

### Phase 10: Accepted Quality Issue Location

- [x] Freeze the revision-bound issue-location contract without exposing OCCT types.
- [x] Add failing tests for exact Triangle3/Tetrahedron4 locator geometry and issue selection.
- [x] Prepare locator geometry only for accepted issue element IDs.
- [x] Highlight and fit the exact element through the viewport port.
- [x] Connect quality-row selection to revision-checked workbench location.
- [x] Run focused and full Release verification.
- [ ] Complete desktop runtime verification from an interactive Windows session.

### Phase 11: Accepted Quality Issue Properties

- [x] Freeze the revision-bound, issue-bounded property snapshot contract.
- [x] Add failing tests for direct issue-detail lookup and selected-row presentation.
- [x] Build detail records from the verified neutral mesh and recomputed quality report.
- [x] Retain details only for unique accepted issue element IDs through project commit.
- [x] Show type, nodes, region, geometry association, and applicable metric values.
- [x] Run focused and full Release verification.
- [ ] Complete desktop runtime verification from an interactive Windows session.

### Phase 12: Accepted Issue-Only Display

- [x] Freeze the revision-bound issue-only display contract without thresholds or color maps.
- [x] Add failing tests for issue-set triangulation and disabled/cleared filter controls.
- [x] Prepare a bounded aggregate presentation from accepted issue locators only.
- [x] Add the quality-panel filter and connect it through revision-checked viewport behavior.
- [x] Restore the full mesh surface when the filter is disabled or stale state is cleared.
- [x] Run focused and full Release verification.
- [ ] Complete desktop runtime verification from an interactive Windows session.

### Phase 13: Revision-Bound Export Snapshot

- [x] Freeze the two-phase current-mesh export snapshot contract.
- [x] Add failing domain tests for missing, ready, stale, replaced, and rejected-commit states.
- [x] Capture one immutable mesh with its exact geometry and mesh revisions.
- [x] Recheck shared-mesh identity and both revisions before future export publication.
- [x] Run focused and full Release verification.

### Phase 14: Explicit Mesh Export Settings

- [x] Freeze the dependency-free export settings contract without defaults.
- [x] Add failing tests for missing fields, invalid encodings, versions, and policy IDs.
- [x] Validate explicit format version, encoding, and bounded policy identifier values.
- [x] Keep exporter capability whitelists and command construction outside the domain.
- [x] Run focused and full Release verification.

### Phase 15: Staged Mesh Exporter Port

- [x] Freeze the application-layer staged writer and typed result contract.
- [x] Add failing contract tests for committed meshes, explicit settings, staging paths, and result states.
- [x] Define the dependency-free `IMeshExporter` port and boundary validation.
- [x] Keep final atomic publication and export-snapshot recheck outside the exporter.
- [x] Run focused and full Release verification.

### Phase 16: Initial Gmsh MSH 4.1 ASCII Exporter

- [x] Freeze the explicit `4.1/Ascii/physical-groups` adapter capability.
- [x] Add failing tests for deterministic sections, unsupported settings, exclusive staging creation, and Gmsh readback.
- [x] Stream region- and geometry-grouped Triangle3/Tetrahedron4 blocks with stable neutral tags.
- [x] Create staging files exclusively and clean up only exporter-created partial files.
- [x] Run focused and full Release verification with the configured Gmsh executable.

### Phase 17: Revision-Checked Final Mesh Export Publication

- [x] Freeze the move-only staged-export transaction and typed publication contract.
- [x] Add failing tests for missing/current meshes, stale/remeshed snapshots, replay, cleanup, and final-file preservation.
- [x] Separate owner-thread begin/publish from background-capable staging without repeated full-mesh validation.
- [x] Add the Windows same-directory replacement adapter with explicit staging ownership and fail-closed path checks.
- [x] Run focused and full Release verification and update implementation documentation.

### Phase 18: Workbench Current-Mesh Export

- [x] Freeze the workbench command around the approved MSH 4.1 ASCII capability.
- [x] Add asynchronous controller tests for unavailable, active, stale, and successful exports first.
- [x] Compose the Gmsh exporter and Windows publisher only at the UI composition root.
- [x] Add an accessible export action, save dialog, busy state, and typed user feedback.
- [x] Run focused and full Release verification and update implementation documentation.

### Phase 19: Bounded Automatic Meshing Retry

- [x] Freeze the Delaunay/HXT whitelist, opt-in default, two-attempt limit, and retryable error set.
- [x] Add failing protocol, controller, resource-failure, and workbench-control tests first.
- [x] Seal the selected algorithm through request, Worker execution, result, and main-process validation.
- [x] Retry only generation failures in a fresh Worker workspace with the remaining total time budget.
- [x] Expose typed algorithm/retry controls and concise attempt summaries in the workbench.
- [x] Run focused and full Release verification and update implementation documentation.

### Phase 20: Multi-Region Reconciliation And Conformal Meshing

- [x] Generalize main-process Worker artifact validation to an exact bounded set of region-to-Solid expectations.
- [x] Freeze geometry signatures that can prove Gmsh entities against snapshot regions without import-order assumptions.
- [x] Add failing multi-Solid Worker and Controller integration tests before generation changes.
- [x] Reconcile every independent Solid to its generated surface and volume entities.
- [x] Publish and independently validate the complete multi-region result map.
- [x] Add task-only Fragment/Imprint for explicit conformal mode without mutating canonical geometry.
- [x] Run focused and full Release verification.
- [ ] Complete desktop runtime verification from an interactive Windows session.

### Later Slices

- [ ] Add quality coloring and approved threshold profiles.
  - [x] Add an explicit dependency-free threshold and color-band contract.
  - [x] Connect validated bands to the OCCT viewport preparation and display port.
  - [ ] Approve metric-specific threshold values and expose the workbench control.
- [ ] Persist current mesh settings, reports, and accepted quality results.

## Risks and Mitigations

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Ambiguous metric formulas | Results differ across modules or releases | Freeze formulas and stable IDs before code |
| Scale-dependent degeneracy tolerance | Valid small geometry is rejected | Treat exact zero/non-finite geometry as invalid; thresholds remain explicit policy |
| Surface adjacency cost | Excess memory on large meshes | Index only unordered triangle edges with bounded, linear expected complexity |
| Premature project commit | Invalid mesh becomes exportable | Commit only after bounded artifact validation and independent quality recomputation |

## Open Questions Deferred By This Slice

- Default warning thresholds and percentile set.
- Project persistence encoding for accepted quality results.
- Quality evaluation time and memory budgets beyond JSON boundary limits.
