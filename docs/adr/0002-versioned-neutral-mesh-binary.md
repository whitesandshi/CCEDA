# ADR 0002: Directory-Based Versioned Neutral Mesh Binary

- Status: Accepted
- Date: 2026-07-25

## Context

The Gmsh Worker must return a neutral mesh that the main process can validate without a Gmsh SDK. The same logical mesh later becomes `mesh/current.meshbin` in a `.cceda` package after the main process assigns a positive `MeshRevision`.

The format must preserve 64-bit stable IDs, geometry associations, region mappings, backend trace metadata, and exact source `GeometryRevision`. It also crosses a process and persistence boundary, so corrupt or hostile counts and offsets must be rejected before allocation. The earlier architecture decision intentionally left alignment, compression, checksums, and optional blocks unfrozen.

## Decision

Use the format specified in `docs/spec/06-neutral-mesh-binary.md`:

- a 128-byte little-endian header with exact counts, revisions, unit, conformity, file size, and header CRC-32;
- a 40-byte block directory with required/optional flags, offsets, stored/logical lengths, and per-block CRC-32;
- 8-byte-aligned, uncompressed 0.1 blocks for metadata, deduplicated GeometryId associations, nodes, Triangle3, Tetrahedron4, and regions;
- an optional node-association block so the fixed node record remains ID plus three binary64 coordinates;
- one-based indexes into a deterministic sorted UTF-8 association table;
- caller-supplied file/count/string budgets and domain validation after decoding.

Unknown optional blocks can be skipped in a compatible minor version. Unknown required blocks, compression algorithms, flags, or record changes fail closed. Worker artifacts use `MeshRevision=0`; committed artifacts require a positive revision.

## Alternatives Considered

### Serialize C++ Structures

Rejected because padding, enum size, compiler ABI, and endianness would become accidental persistent contracts and could expose uninitialized memory.

### JSON Mesh Arrays

Rejected because coordinate and connectivity arrays would be substantially larger and slower to parse, while number representation would make exact 64-bit ID handling fragile.

### Adopt Gmsh MSH As The Project Format

Rejected because it would leak backend entity semantics into the domain, require backend-specific mapping knowledge in the main process, and violate solver neutrality.

### Add Compression In 0.1

Rejected for the first codec slice. Compression expands the attack surface and requires separate stored/logical budgets. The directory reserves a compression field so a later compatible version can add one explicitly.

## Consequences

The codec can remain standard-library-only and live outside the domain while consuming and producing `MeshModel`. Writers perform extra sorting, checksumming, and validation, and readers must implement careful overflow and range checks. The fixed directory adds modest overhead but gives the main process a bounded compatibility mechanism and lets future minor versions add optional blocks without changing known record layouts.
