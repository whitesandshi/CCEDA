# ADR 0004: Bounded Gmsh Retry Profile

- Status: Accepted
- Date: 2026-07-29

## Context

The meshing protocol permits a failed first attempt to use approved fallback
algorithms, but did not previously freeze an algorithm whitelist, attempt
default, retryable error set, or cross-attempt resource behavior. Unbounded or
implicit retries would make failures expensive and unpredictable. Silently
changing element sizes would also make the effective mesh profile harder to
explain and reproduce.

Gmsh 4.15 documents Delaunay as its most robust 3D algorithm and HXT as an
efficient parallel Delaunay implementation. Both support the general sizing
and embedded-entity capabilities that later CCEDA slices may require.

## Decision

- The domain exposes only `Delaunay` and `Hxt`; the executable adapter maps
  them to Gmsh `del3d` and `hxt` identifiers.
- Delaunay is the default algorithm. Automatic retry is disabled by default.
- When enabled, retry is limited to two attempts. Attempt two uses the other
  approved algorithm and preserves size, curvature, optimization, and thread
  settings.
- Only surface- and volume-generation failures are automatically retryable.
  Input, topology, resource, cancellation, Worker-loss, output, quality,
  stale-revision, and commit failures remain terminal.
- Each attempt starts a fresh Worker process and uses a separate workspace.
  The logical job ID, geometry snapshot, and exact source geometry revision
  remain unchanged.
- Attempts share one total elapsed-time budget. The remaining time is sealed
  into the later attempt request; the per-process working-set limit is not
  accumulated across processes.
- The successful effective algorithm and attempt count are returned to the
  application. Failed-attempt logs remain in their workspaces while formal
  output is removed.

## Alternatives Considered

### Include Frontal/Netgen As A Third Attempt

Rejected for this profile because it expands resource use and does not share
the same general support for embedded entities and mesh-size fields as
Delaunay/HXT.

### Change Size Guards Automatically

Rejected until a geometry-derived, unit-aware sizing policy is approved.
Hidden scale changes would make results difficult to reproduce.

### Retry Worker Loss

Rejected for the initial profile. A crash may indicate a deterministic input,
protocol, or resource problem; automatically repeating it would spend the same
budget without a changed meshing profile.

## Consequences

The retry behavior is deterministic, bounded, traceable, and opt-in. It adds
one alternative Worker launch at most and keeps Gmsh-specific strings outside
the domain. A model that fails both approved algorithms still requires an
explicit user settings change or a future approved profile.

## Sources

- Gmsh algorithm guidance: https://gmsh.info/doc/texinfo/#Choosing-the-right-unstructured-algorithm
- Gmsh mesh options: https://gmsh.info/doc/texinfo/#Mesh-options
- Gmsh command-line interface: https://gmsh.info/doc/texinfo/#Gmsh-command_002dline-interface
