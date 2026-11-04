# CCEDA Specifications

Status: Draft  
Version: 0.1  
Updated: 2026-07-30

This directory specifies the CCEDA geometry and mesh preprocessing product. The current implementation covers STEP/XCAF inspection, OCCT display, an experimental neutral Gmsh Worker path, bounded Worker quality artifacts with main-process recomputation, quality-gated current-mesh commit, typed common/advanced meshing controls, validated resource telemetry and explicit limits, an opt-in two-attempt Delaunay/HXT fallback profile with fresh Worker workspaces and a shared time budget, bounded independent-Solid multi-region reconciliation, asynchronous committed-mesh surface display, accepted-quality inspection, revision-checked issue-element location, issue-bounded element properties and metric values, revision-checked issue-only mesh display, two-phase revision-bound current-mesh export snapshots, explicit default-free export settings validation, a staged neutral-mesh exporter port, an exclusively created ASCII MSH 4.1 writer with region Physical Groups, move-only same-directory final export publication after exact revision recheck, and a workbench command that stages current-mesh export off the UI thread before owner-thread publication. Project persistence, task-only conformal Fragment/Imprint meshing, and mesh quality coloring remain future slices.

The sealed `cceda.geometry-map/0.2` input records finite canonical-millimeter bounds for every located snapshot Solid. The executable Worker reconciles multi-region independent Solids against MSH 4.1 volume bounds without relying on import or tag order.

## Documents

| Document | Purpose |
| --- | --- |
| [00-prd.md](00-prd.md) | Product goals, first-release scope, workflows, and non-functional requirements |
| [01-domain-model.md](01-domain-model.md) | Domain entities, identifiers, states, invariants, and module boundaries |
| [02-project-format.md](02-project-format.md) | `.cceda` package layout, versioning, checksums, and compatibility |
| [03-geometry-service.md](03-geometry-service.md) | STEP/XCAF import, geometry queries, validation, healing, and undo |
| [04-meshing-protocol.md](04-meshing-protocol.md) | Main application to Gmsh Worker job and result protocol |
| [05-manual-acceptance.md](05-manual-acceptance.md) | Manual acceptance models, procedures, evidence, and release gates |
| [06-neutral-mesh-binary.md](06-neutral-mesh-binary.md) | Versioned neutral mesh binary layout, limits, and compatibility |
| [07-mesh-quality.md](07-mesh-quality.md) | Solver-neutral metric formulas, identifiers, orientation rules, and validity gates |

## Normative Language

- MUST is required for first-release acceptance.
- SHOULD is the default expectation and requires a recorded reason to deviate.
- MAY describes an optional or future-compatible capability.
- OUT OF SCOPE means the first release must not expose a partial implementation.

When specifications conflict, the PRD takes precedence, then topic-specific invariants, then implementation and prototype behavior.
