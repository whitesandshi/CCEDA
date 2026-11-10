# Neutral Mesh Quality Tasks

- [x] Freeze neutral quality metric formulas and stable IDs.
- [x] Add the dependency-free quality contract.
- [x] Add reference-element and invalidity tests first.
- [x] Implement triangle and tetrahedron metric evaluation.
- [x] Implement triangle normal-consistency detection.
- [x] Run focused quality tests.
- [x] Run the full Release build and test suite.

## Worker Quality Artifact

- [x] Freeze the 0.1 JSON schema and resource limits.
- [x] Add strict read/write boundary tests.
- [x] Emit and hash `quality.json` in the Worker.
- [x] Validate and recompute it in the main process.
- [x] Run the full Release build and test suite.

## Quality-Gated Project Commit

- [x] Add the Controller commit integration test first.
- [x] Return the decoded mesh from asynchronous artifact validation.
- [x] Commit through `ProjectState` and expose the assigned mesh revision.
- [x] Prove rejected quality output preserves the existing current mesh.
- [x] Run code review, Release build, and the full test suite.
## Workbench Mesh Submission

- [x] Add the meshing task panel test first.
- [x] Add minimum/maximum size controls and inline validation.
- [x] Prepare revision-bound geometry snapshots without blocking the UI.
- [x] Submit through `MeshingJobController` and show stage/progress.
- [x] Support cancellation during snapshot preparation and Worker execution.
- [x] Show committed revision and element counts only after quality validation.
- [x] Configure and build the Worker with the desktop application.
- [x] Expose a stable read-only handle to the current committed neutral mesh.
- [x] Convert and render committed Triangle3 surface elements without blocking the UI.
- [x] Add mesh-surface visibility and stale-presentation handling.
- [x] Retain the accepted quality result with its committed mesh revision.
- [x] Expose quality summaries and exact issue records in the workbench.
- [x] Clear stale quality inspection state when geometry changes.
- [x] Verify the quality inspection workflow in Release and the desktop runtime.

## Advanced Meshing Controls And Resource Reporting

- [x] Add the shared validated settings contract and request encoding.
- [x] Apply typed advanced settings in the executable Worker.
- [x] Add common/advanced option groups and reset defaults in the workbench.
- [x] Publish and validate elapsed-time and working-set telemetry.
- [x] Show live and terminal resource summaries in the workbench.
- [x] Run focused and full Release verification.

## Explicit Meshing Resource Limits

- [x] Add the shared validated resource-limit contract.
- [x] Round-trip configured limits through request and success artifacts.
- [x] Enforce wall-clock and sampled combined-working-set limits.
- [x] Add advanced time and memory limit controls with disabled defaults.
- [x] Prove limit failure cannot replace the current mesh.
- [x] Run focused and full Release verification.

## Accepted Quality Issue Location

- [x] Freeze revision-bound issue-location behavior.
- [x] Add locator-geometry and quality-selection tests first.
- [x] Prepare only accepted issue element locators.
- [x] Highlight and fit the exact element through the viewport port.
- [x] Connect the quality issue table to the current mesh presentation.
- [x] Run focused and full Release verification.
- [ ] Complete desktop runtime verification from an interactive Windows session.

## Accepted Quality Issue Properties

- [x] Freeze the bounded issue-property snapshot contract.
- [x] Add issue-detail lookup and selected-row UI tests first.
- [x] Build direct-keyed details from the independently recomputed report.
- [x] Carry the bounded details through accepted mesh commit.
- [x] Present element metadata and applicable metrics in the Quality view.
- [x] Run focused and full Release verification.
- [ ] Complete desktop runtime verification from an interactive Windows session.

## Accepted Issue-Only Display

- [x] Freeze revision-bound issue-only display behavior without thresholds or color maps.
- [x] Add issue-set triangulation and filter-control tests first.
- [x] Prepare an aggregate only from accepted issue element locators.
- [x] Add the Quality view filter and revision-checked viewport connection.
- [x] Reset the filter when quality or the mesh presentation is cleared.
- [x] Run focused and full Release verification.
- [ ] Complete desktop runtime verification from an interactive Windows session.

## Revision-Bound Export Snapshot

- [x] Freeze the two-phase export snapshot contract.
- [x] Add missing, ready, stale, replaced, and rejected-commit tests first.
- [x] Capture the immutable current mesh and both exact revisions.
- [x] Recheck snapshot identity and revisions before future export publication.
- [x] Run focused and full Release verification.

## Explicit Mesh Export Settings

- [x] Freeze a dependency-free export settings contract with no defaults.
- [x] Add missing-field, invalid-enum, version, and policy-ID tests first.
- [x] Validate explicit format version, encoding, and bounded policy IDs.
- [x] Leave exporter capability whitelists and command construction to adapters.
- [x] Run focused and full Release verification.

## Staged Mesh Exporter Port

- [x] Freeze the staged writer and typed result contract.
- [x] Add committed-mesh, settings, path, and result-state tests first.
- [x] Add the dependency-free application port and boundary validation.
- [x] Require application-owned snapshot recheck and final atomic publication.
- [x] Run focused and full Release verification.

## Initial Gmsh MSH 4.1 ASCII Exporter

- [x] Freeze the explicit `4.1/Ascii/physical-groups` capability.
- [x] Add deterministic output, unsupported-settings, exclusive-create, and Gmsh readback tests first.
- [x] Stream stable neutral tags and region Physical Groups through the concrete exporter.
- [x] Remove exporter-created partial staging files without touching pre-existing paths.
- [x] Run focused and full Release verification with the configured Gmsh executable.

## Revision-Checked Final Mesh Export Publication

- [x] Freeze the move-only staged-export transaction and typed publication contract.
- [x] Add missing/current, stale/remeshed, replay, cleanup, and final-preservation tests first.
- [x] Keep begin and publish on the ProjectState owner thread while allowing staging in the background.
- [x] Publish only owned same-directory staging files through the fail-closed Windows adapter.
- [x] Run focused and full Release verification and update implementation documentation.

## Workbench Current-Mesh Export

- [x] Freeze the workbench command around the approved MSH 4.1 ASCII capability.
- [x] Add unavailable, active, stale, and successful asynchronous controller tests first.
- [x] Inject the Gmsh exporter and Windows publisher at the UI composition root.
- [x] Add the export action, save dialog, busy state, and typed user feedback.
- [x] Run focused and full Release verification and update implementation documentation.

## Bounded Automatic Meshing Retry

- [x] Freeze the Delaunay/HXT whitelist and opt-in two-attempt policy.
- [x] Add protocol, controller, resource-failure, and workbench RED tests.
- [x] Carry the selected algorithm through the strict Worker protocol.
- [x] Launch the approved fallback in a fresh workspace under the remaining time budget.
- [x] Add algorithm/retry controls and attempt-count summaries.
- [x] Run focused and full Release verification and update implementation documentation.

## Multi-Region Reconciliation And Conformal Meshing

- [x] Validate an exact bounded region-to-Solid expectation set in the main process.
- [x] Freeze verifiable snapshot-region geometry signatures.
- [x] Add multi-Solid Worker and Controller RED tests.
- [x] Reconcile and publish every independent-Solid region mapping.
- [ ] Add explicit task-only conformal Fragment/Imprint mode.
- [ ] Run focused, full Release, and desktop runtime verification.

## Quality Coloring And Threshold Profiles

- [x] Add an explicit dependency-free threshold and color-band contract.
- [x] Add direction-aware validation and fail-closed classification tests.
- [x] Connect validated bands to the OCCT viewport preparation and display port.
- [x] Expose surface metrics, explicit user thresholds, Apply, and the applied legend.
- [x] Verify Release build and all 24 tests with unrestricted temporary-file access.
- [ ] Complete desktop visual acceptance and approve preset threshold profiles.

## Project Persistence

- [x] Specify mesh/settings.json 0.1 with explicit complete fields and bounded parsing.
- [x] Implement deterministic serialization and validated deserialization over existing domain types.
- [x] Test full-field roundtrip and malformed, duplicate, incompatible, and oversized input.
- [ ] Integrate settings with consistent project snapshots and package Save/Open.
