# CCEDA Neutral Mesh Quality

Status: Draft
Version: 0.1
Updated: 2026-07-26

## 1. Scope

This document freezes the first solver-neutral quality formulas and stable
metric identifiers. The evaluator consumes `MeshModel` only and MUST NOT
depend on Qt, OCCT, Gmsh, Worker control messages, or persisted file formats.

Version 0.1 computes per-element metrics and geometric validity issues.
Threshold profiles, percentile summaries, optimization policy, and UI
color maps remain later contracts.

The first quality-coloring slice defines the dependency-free policy boundary
without selecting product thresholds. A caller supplies a named
`MeshQualityThresholdProfile` with finite, direction-aware warning and
critical boundaries. The evaluator remains unchanged and never reads this
policy implicitly.

## 2. Evaluation Contract

- Callers supply the expected `MeshValidationMode`; the input MUST first pass
  `validateMeshModel` in that mode.
- Evaluation success means all required metrics were computed. It does not mean
  the mesh is good enough to commit.
- A report is commit-valid only when it contains no commit-blocking issue.
- Results preserve the relative Triangle3 and Tetrahedron4 order from
  `MeshModel`. Issue lookup is keyed by `MeshElementId`.
- Coordinates are canonical millimeters. Area is square millimeters and volume
  is cubic millimeters.
- Version 0.1 uses exact zero and non-finite checks for geometric invalidity.
  Scale-dependent warning tolerances belong to an explicit threshold profile
  and MUST NOT be hidden in the evaluator.

## 3. Stable Metric Identifiers

| Identifier | Element | Direction | Ideal |
| --- | --- | --- | ---: |
| `triangle.area.mm2` | Triangle3 | higher is better | none |
| `triangle.aspect-ratio` | Triangle3 | lower is better | 1 |
| `triangle.minimum-angle.deg` | Triangle3 | higher is better | 60 |
| `triangle.maximum-angle.deg` | Triangle3 | lower is better | 60 |
| `triangle.normal-consistent` | Triangle3 | required boolean | true |
| `tetrahedron.signed-volume.mm3` | Tetrahedron4 | positive required | none |
| `tetrahedron.aspect-ratio` | Tetrahedron4 | lower is better | 1 |
| `tetrahedron.minimum-dihedral-angle.deg` | Tetrahedron4 | higher is better | 70.5287793655 |
| `tetrahedron.scaled-jacobian` | Tetrahedron4 | higher is better | 1 |

Stable identifiers and formulas are compatibility contracts. Changing either
requires a metric major-version change.

## 4. Triangle3 Formulas

For ordered vertices `a`, `b`, and `c`, let edge lengths be `l0`,
`l1`, and `l2`.

- Area is `0.5 * |(b-a) x (c-a)|`.
- Aspect ratio is
  `(l0^2 + l1^2 + l2^2) / (4 * sqrt(3) * area)`.
  An equilateral triangle evaluates to 1.
- Each interior angle uses the clamped normalized dot product of its incident
  edges. The minimum and maximum are reported in degrees.
- A zero-area or non-finite triangle produces a commit-blocking
  `degenerate-triangle` or `non-finite-metric` issue.

For normal consistency, an undirected edge is identified by its sorted pair of
`MeshNodeId` values. Two adjacent triangles are consistent when their ordered
node lists traverse the shared edge in opposite directions. A boundary edge
with one use is neutral. Two uses in the same direction produce an
`inconsistent-triangle-orientation` issue for both elements. More than two uses
produce a commit-blocking `non-manifold-surface-edge` issue for every incident
element.

## 5. Tetrahedron4 Formulas

For ordered vertices `a`, `b`, `c`, and `d`:

- Signed volume is
  `dot(b-a, cross(c-a, d-a)) / 6`.
- Inradius is `3 * abs(signedVolume) / totalFaceArea`.
- Aspect ratio is
  `longestEdge / (2 * sqrt(6) * inradius)`.
  A regular tetrahedron evaluates to 1.
- Minimum dihedral angle is the minimum, over all six face pairs, of
  `pi - acos(clamp(dot(n0,n1) / (|n0|*|n1|), -1, 1))`, where each face
  normal points away from the opposite vertex.
- Scaled Jacobian is the minimum of the four oriented corner determinants,
  divided by the product of the three incident edge lengths and multiplied by
  `sqrt(2)`. Corner edge order is chosen so a positively oriented regular
  tetrahedron evaluates to 1. The value is clamped to `[-1, 1]` only after
  finite division.
- Zero volume produces a commit-blocking `degenerate-tetrahedron` issue.
  Negative volume produces a commit-blocking `inverted-tetrahedron` issue.
  A non-finite required metric produces `non-finite-metric`.

## 6. Issue Semantics

| Issue | Commit-blocking |
| --- | --- |
| `degenerate-triangle` | yes |
| `inconsistent-triangle-orientation` | no |
| `non-manifold-surface-edge` | yes |
| `degenerate-tetrahedron` | yes |
| `inverted-tetrahedron` | yes |
| `non-finite-metric` | yes |

Issue records contain a stable code and exact `MeshElementId`. The evaluator
MUST emit at most one issue of a given code for one element and MUST return
issues ordered by element ID, then code.

## 7. Worker Quality Artifact

The executable Worker MUST publish `output/quality.json` only after neutral
mesh conversion and quality evaluation succeed. The artifact schema is
`cceda.mesh-quality-worker/0.1` and contains exactly:

- `schema`: `name`, `major`, and `minor`.
- `jobId`, `meshRevision`, and `sourceGeometryRevision`.
- `metricVersion` and `validForCommit`.
- `counts`: triangle, tetrahedron, and issue counts.
- `summaries`: one entry for each stable metric identifier.
- `issues`: ordered records containing `elementId` and stable issue `code`.

Numeric summary entries contain `metricId`, `kind: "numeric"`, and
`sampleCount`. When `sampleCount` is positive they also contain finite
`minimum`, `maximum`, and `mean` values satisfying
`minimum <= mean <= maximum`. Optional metrics may have zero samples when
every element of that kind is geometrically invalid.

The `triangle.normal-consistent` summary contains
`kind: "boolean"`, `sampleCount`, and `failedElementCount`. Version 0.1
does not encode warning thresholds or percentiles because those policies are
not yet frozen.

Worker artifacts MUST use `meshRevision=0` and a positive
`sourceGeometryRevision`. Metric summaries are ordered by the stable metric
table above. Issues are ordered by element ID, then code, contain no duplicate
`(elementId, code)` pair, and target an element in the neutral mesh.
`validForCommit` is derived from the issue list and MUST NOT be accepted as
an independent assertion.

The default reader limits are 16 MiB of JSON input, 100,000 issue records,
and a maximum JSON nesting depth of 16.
Readers MUST reject unknown object members, duplicate or missing metric
summaries, unknown identifiers, non-integral or out-of-range integer values,
non-finite numeric values, count mismatches, invalid ordering, and any limit
violation before project state changes.

The Worker result manifest and control result contain `qualitySha256`. The
main process MUST verify the job ID, geometry revision, artifact hash, bounded
schema, and counts. It MUST then evaluate the decoded neutral mesh itself and
compare the canonical 0.1 quality artifact before treating the Worker result
as quality-valid.

After canonical comparison succeeds, the main process MUST commit the decoded
Worker mesh through `ProjectState` on the project-owning thread.
`ProjectState` MUST recheck structural validity and the current geometry
revision while assigning a positive mesh revision. A rejected commit MUST
preserve the prior mesh and mesh revision.

The application MUST retain the bounded summaries and issue records only after
that commit succeeds, replace the artifact `meshRevision=0` with the assigned
positive mesh revision, and expose the result as an immutable inspection
snapshot. The UI MUST clear that snapshot when a new geometry revision is
accepted. Full per-element metric arrays, threshold profiles, and color maps
are not part of this inspection snapshot contract. Section 10 extends the
snapshot only with issue-bounded element details.

## 8. Acceptance

1. Equilateral Triangle3 and regular Tetrahedron4 reference elements match the
   ideal values within floating-point tolerance.
2. Reversing tetrahedron orientation makes signed volume and scaled Jacobian
   negative and blocks commit.
3. Shared triangle edges detect consistent, inconsistent, and non-manifold
   orientation without geometry-library types.
4. Structurally invalid input is rejected before metric evaluation.
5. The Worker emits a bounded `quality.json` and publishes its SHA-256 in both
   result channels.
6. The main process rejects malformed, oversized, mismatched, or non-canonical
   quality artifacts.
7. A quality-valid Worker mesh is committed with a positive mesh revision;
   rejected artifacts preserve the prior current mesh.
8. The module builds with C++17 and depends only on `cceda_domain`.
9. Accepted issue details are direct-keyed, revision-bound, and retain only
   unique issue elements with their applicable metric values.
10. The accepted issue set can be displayed without the full mesh, only while
    its committed mesh revision remains current.

## 9. Accepted-Issue Element Location

- The quality view emits only the selected `MeshElementId`; it never owns or
  exposes OCCT presentation objects.
- Location requests MUST include the exact positive `MeshRevision` associated
  with the accepted quality result. A stale revision, missing element, or
  unavailable presentation fails closed without changing selection.
- Mesh-presentation preparation MAY precompute locator geometry only for the
  bounded set of accepted issue element IDs. It MUST NOT build an additional
  full-mesh element index solely for issue navigation.
- A `Triangle3` issue highlights its exact face. A `Tetrahedron4` issue
  highlights the four faces of the exact volume element.
- Successful location makes the mesh visible, replaces the prior element
  highlight, and fits the view to the highlighted element. Clearing or
  replacing the mesh presentation also clears its element highlight.

## 10. Accepted-Issue Element Properties

- After independent main-process recomputation succeeds, the application MAY
  retain element properties and metric values only for the bounded set of
  unique element IDs referenced by the accepted issue list. It MUST NOT retain
  the full per-element metric arrays solely for UI inspection.
- The retained detail index MUST be keyed by `MeshElementId`, bound to the same
  positive `MeshRevision` and source `GeometryRevision` as the accepted quality
  snapshot, and built before the recomputed report is discarded. Selecting an
  issue MUST NOT scan the full neutral mesh or metric arrays.
- Each detail contains the exact element type, ordered node IDs, region ID,
  geometry association, and all metric values applicable to that element type.
  Unavailable values remain explicitly unavailable rather than being replaced
  by numeric defaults.
- The quality view MUST show the selected issue element ID, type, nodes,
  region, geometry association, and applicable stable metric IDs and values.
  Replacing or clearing the accepted quality snapshot clears the detail view.
- Worker protocol `0.1` remains unchanged. Detail properties are derived from
  the already verified neutral mesh and the independently recomputed quality
  report in the main process.

## 11. Accepted-Issue-Only Display

- The quality view MAY expose an issue-only display control only when the
  accepted snapshot contains at least one issue and its mesh presentation is
  available. The control is disabled otherwise.
- Enabling the control displays exactly the bounded, unique accepted issue
  element set for the snapshot's positive `MeshRevision`; it does not derive
  new issues, scan the full mesh from the UI, or apply implicit thresholds.
- The viewport port MUST reject an issue-only request whose mesh revision does
  not match the displayed committed mesh presentation. Replacing or clearing
  that presentation clears the issue-only display state.
- The prepared OCCT adapter MAY combine already prepared issue locator
  triangulations into one bounded presentation. It MUST NOT build a full-mesh
  element index solely for this filter.
- Disabling the control restores the complete committed mesh surface. Issue
  selection continues to highlight and fit the exact selected element in both
  display modes.
- This filter is distinct from quality coloring: metric thresholds, color
  direction, and color maps remain later contracts. Worker protocol `0.1`
  remains unchanged.

## 12. Quality Coloring Policy Boundary

- A threshold profile MUST have a non-empty stable caller-supplied ID.
- Each metric MAY occur at most once in a profile. Unknown metric identifiers,
  non-finite boundaries, and directionally reversed boundaries MUST invalidate
  the profile.
- For higher-is-better metrics, values at or above the warning boundary are
  `Good`, values between the critical and warning boundaries are `Warning`,
  and values below the critical boundary are `Critical`.
- For lower-is-better metrics, values at or below the warning boundary are
  `Good`, values between the warning and critical boundaries are `Warning`,
  and values above the critical boundary are `Critical`.
- Positive-required metrics classify finite positive values as `Good` and all
  other finite values as `Critical`. True-required metrics classify `true` as
  `Good` and `false` as `Critical`.
- Missing or non-finite metric values, and metrics without an explicit profile
  entry, classify as `Unavailable`.
- The profile contract does not establish approved product threshold values,
  percentile policies, or a viewport presentation format. Those remain the
  next integration slice.
