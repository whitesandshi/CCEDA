# Implementation Plan: Worker Protocol Completion

## Overview

Continue the protocol-0.1 meshing path from the proven BREP snapshot flow toward a neutral Worker artifact. Retire the unused STEP compatibility path first so later protocol work has one request shape and one validation path.

## Architecture Decisions

- The BREP plus geometry-map request is the only supported Worker input; direct STEP input belongs to import/provenance, not meshing.
- Removal is isolated from neutral mesh serialization so each change remains independently reviewable and revertible.
- `result.meshbin` will use domain types and a versioned storage adapter; the domain will not depend on Qt or Gmsh.

## Task List

### Phase 1: Contract Cleanup

- [x] Remove the STEP workspace request API, its tests, and the Worker legacy request branch.
- [x] Verify no production compatibility references remain; retain only the rejection fixture for the removed request shape.

### Checkpoint: One Worker Input Contract

- [x] Release build succeeds.
- [x] All tests pass, including the real local Gmsh process test.

### Phase 2: Neutral Worker Artifact

- [ ] Freeze a versioned, bounded `result.meshbin` codec contract against `MeshModel`.
- [ ] Convert validated MSH 4.1 output and geometry mapping into `MeshRevision=0` meshbin.
- [ ] Validate meshbin in the main process before assigning a committed revision.

### Checkpoint: Protocol Success Output

- [ ] Worker output requires no Gmsh API to interpret.
- [ ] Corrupt, oversized, stale, or unmapped meshbin is rejected.
- [ ] Full Release verification passes.

## Risks and Mitigations

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Hidden legacy consumer | Build or runtime regression | Repository-wide reference scan plus full integration suite before removal commit |
| Binary format freezes too early | Expensive compatibility burden | Define codec acceptance tests and explicit version/count budgets before Worker integration |
| Gmsh Face mapping remains incomplete | Invalid neutral associations | Keep meshbin integration limited to associations proven by the result map; fail closed otherwise |

## Open Questions

- Exact meshbin alignment and optional-block rules remain to be frozen before Phase 2 implementation, as required by `docs/spec/02-project-format.md`.
