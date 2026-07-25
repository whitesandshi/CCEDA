# CCEDA Specifications

Status: Draft  
Version: 0.1  
Updated: 2026-07-23

This directory specifies the CCEDA geometry and mesh preprocessing product. The first Qt/OCCT vertical slice implements STEP import, geometry inspection, and display. Meshing and project persistence remain specified future slices.

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

## Normative Language

- MUST is required for first-release acceptance.
- SHOULD is the default expectation and requires a recorded reason to deviate.
- MAY describes an optional or future-compatible capability.
- OUT OF SCOPE means the first release must not expose a partial implementation.

When specifications conflict, the PRD takes precedence, then topic-specific invariants, then implementation and prototype behavior.
