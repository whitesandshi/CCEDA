# ADR 0003: Revision-Bound Asynchronous Mesh Presentations

- Status: Accepted
- Date: 2026-07-26

## Context

A committed neutral mesh can contain millions of elements. Converting its surface connectivity into an OCCT presentation on the Qt UI thread would violate the non-blocking UI requirement. Passing `Poly_Triangulation`, `AIS_Triangulation`, or another OCCT type through the UI or domain would violate the existing adapter boundaries.

A presentation also has to remain tied to one exact `MeshRevision` and source `GeometryRevision`. Remeshing replaces the current mesh, while geometry changes make the prior mesh stale. An in-flight presentation task therefore needs stable input lifetime and must not publish a result for a revision that is no longer current.

## Decision

- `ProjectState` owns the current mesh through a shared immutable snapshot and retains the existing raw-pointer inspection API for compatible synchronous callers.
- `IViewportController` accepts the neutral `MeshModel` for background preparation and returns an immutable opaque `PreparedMeshPresentation`.
- The prepared presentation exposes only mesh revision, source geometry revision, surface triangle count, and locatable-element count. OCCT handles remain inside `cceda_visualization_occ`.
- The OCCT adapter validates a committed neutral mesh, extracts only nodes referenced by `Triangle3` elements, remaps sparse 64-bit IDs to OCCT one-based indexes, and computes normals in the background.
- The Qt workbench rechecks both revisions on the UI thread before asking the viewport adapter to display the prepared object.
- A successful geometry import clears the prior mesh presentation. A successful remesh clears the old presentation before preparing its replacement.
- Presentation failure degrades independently: the committed neutral mesh remains valid even when its surface cannot be prepared or displayed.

The initial slice used `AIS_Triangulation` only for whole-surface display. The
accepted-quality issue-location extension prepares separate locator
triangulations only for reported issue IDs, remains revision-bound, and does
not claim general per-element picking, quality coloring, or threshold
filtering.

The main process also derives a direct-keyed property record for each unique
accepted issue element from the already recomputed quality report and verified
neutral mesh. These records remain in the immutable accepted-quality snapshot;
the full per-element metric arrays are discarded and the Worker protocol is not
extended.

The quality view may switch between the full mesh surface and an aggregate of
the already prepared accepted-issue locators. That switch carries the positive
mesh revision through the viewport port and fails closed when the presentation
is stale or unavailable. It is display filtering only, not a metric threshold
or color-map policy.

## Alternatives Considered

### Convert Synchronously In The Success Callback

Rejected because conversion cost scales with mesh size and would block the Qt event loop for large meshes.

### Pass OCCT Triangulation Types Through The UI

Rejected because it would make UI composition depend on OCCT lifetime and representation details, contrary to the established port and adapter boundary.

### Copy The Current Mesh Into Every Background Task

Rejected because a full copy creates avoidable peak memory pressure. A shared read-only snapshot keeps the exact committed revision alive until the task finishes.

### Build Selection And Quality Presentation Data Immediately

Rejected because `AIS_Triangulation` does not provide triangle selection, and quality coloring and element location require their own versioned data and interaction contracts.

## Consequences

Mesh preparation can temporarily overlap the old and new mesh/presentation memory, but it does not duplicate the neutral mesh solely for task lifetime. Prepared objects are adapter-specific and can only be displayed by the adapter that created them; incompatible objects fail closed.

The UI remains responsive during surface conversion and cannot publish stale output after geometry or mesh revision changes. Later quality visualization can extend or replace the prepared presentation contract without exposing OCCT objects to domain or UI code.

Issue-location memory scales with the accepted issue set instead of every mesh
element. Triangle issues retain one locator face and tetrahedron issues retain
four locator faces. The locator uses OCCT's independent-depth topmost 3D layer
so an interior element remains visible through the retained mesh surface. The
viewport rejects a location request unless its mesh revision matches the
currently displayed prepared presentation.

Issue-property lookup does not scan the neutral mesh on selection. Its memory
scales with unique accepted issue elements and retains ordered node IDs, region
and geometry associations, and only the applicable Triangle3 or Tetrahedron4
metric record.

Issue-only display memory likewise scales with the accepted locator set. The
aggregate reuses those locator triangulations, is enabled only after the exact
presentation is displayed, and resets when the quality snapshot or mesh
presentation is cleared.

## Sources

- OCCT `AIS_Triangulation`: https://dev.opencascade.org/doc/refman/html/class_a_i_s___triangulation.html
- OCCT `Poly_Triangulation`: https://dev.opencascade.org/doc/refman/html/class_poly___triangulation.html
- OCCT predefined Z layers: https://dev.opencascade.org/doc/refman/html/_graphic3d___z_layer_id_8hxx.html
- Qt concurrent execution: https://doc.qt.io/qt-6.8/qtconcurrentrun.html
- Qt future completion monitoring: https://doc.qt.io/qt-6.8/qfuturewatcher.html
