# Neutral Mesh Binary

Status: Draft  
Version: 0.1  
Depends on: [01-domain-model.md](01-domain-model.md), [02-project-format.md](02-project-format.md), [04-meshing-protocol.md](04-meshing-protocol.md)

## 1. Purpose

`mesh/current.meshbin` and Worker `output/result.meshbin` store the solver-neutral `MeshModel`. The format MUST NOT contain C++ object memory, Qt types, OCCT handles, or Gmsh entity objects. A reader MUST validate structure, budgets, checksums, references, and geometry ownership without loading Gmsh.

Format 0.1 stores uncompressed little-endian blocks behind a fixed header and directory. The directory is the compatibility boundary: a newer minor version may add optional blocks that an older reader skips by length, while unknown required blocks fail closed.

## 2. Integer And Alignment Rules

- All integers are unsigned little-endian unless stated otherwise.
- Floating-point coordinates are IEEE 754 binary64 written as their 64-bit bit pattern.
- The byte-order marker is `0x01020304`.
- Header, directory, and every block start at an 8-byte-aligned offset.
- Inter-block padding bytes MUST be zero and are not part of either block checksum.
- All reserved fields and writer flags not defined by this version MUST be zero.
- Readers MUST check every addition, multiplication, offset, and length before allocation or access.

## 3. Header

The 0.1 writer emits a 128-byte header. Magic occupies eight bytes: ASCII `CCMESH` followed by two zero bytes.

| Offset | Size | Field | 0.1 rule |
| ---: | ---: | --- | --- |
| 0 | 8 | magic | `43 43 4d 45 53 48 00 00` |
| 8 | 2 | format major | `0` |
| 10 | 2 | format minor | `1` |
| 12 | 4 | header size | `128` |
| 16 | 4 | byte-order marker | `0x01020304` |
| 20 | 4 | header flags | `0` |
| 24 | 8 | mesh revision | `0` for Worker artifact, positive after commit |
| 32 | 8 | source geometry revision | positive |
| 40 | 4 | canonical unit | `1` means millimeters |
| 44 | 4 | conformity | `1` non-conformal, `2` conformal |
| 48 | 8 | node count | exact decoded count |
| 56 | 8 | Triangle3 count | exact decoded count |
| 64 | 8 | Tetrahedron4 count | exact decoded count |
| 72 | 8 | region count | exact decoded count |
| 80 | 8 | association count | exact string-table count |
| 88 | 8 | directory offset | 8-byte aligned, at least header size |
| 96 | 4 | directory entry count | bounded, at least six |
| 100 | 4 | directory entry size | `40` for 0.1 |
| 104 | 8 | file size | exact file byte count |
| 112 | 4 | header CRC-32 | CRC with this field zeroed |
| 116 | 12 | reserved | zero |

CRC-32 uses IEEE polynomial `0xedb88320`, initialized and finalized with `0xffffffff`. A reader rejects a bad header checksum before trusting counts or directory locations.

## 4. Block Directory

Each 0.1 directory entry is 40 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | block type |
| 4 | 4 | flags |
| 8 | 8 | stored offset |
| 16 | 8 | stored length |
| 24 | 8 | logical length |
| 32 | 4 | logical-data CRC-32 |
| 36 | 4 | reserved |

Flag bit 0 means required. Bits 8 through 15 are the compression algorithm; `0` is uncompressed and is the only algorithm emitted or accepted by 0.1. Other flag bits are zero.

Entries MUST be ordered by increasing block type, unique, aligned, in range, and non-overlapping. For uncompressed blocks, stored and logical lengths are equal. The file contains exactly the header-declared size; trailing bytes are rejected.

| Type | Name | Requirement |
| ---: | --- | --- |
| 1 | metadata | required |
| 2 | associations | required |
| 3 | nodes | required |
| 4 | node associations | optional; omitted when empty |
| 5 | Triangle3 | required |
| 6 | Tetrahedron4 | required |
| 7 | regions | required |

An unknown required block is unsupported. An unknown optional block in a compatible minor is checksum-validated and skipped. Duplicate known or unknown block types are malformed.

## 5. Strings And Association Indices

Strings are strict UTF-8 without BOM or NUL and are caller-budgeted. Empty metadata and association strings are invalid.

The association block begins with `entryCount:u64`, followed by that many `byteLength:u32` plus raw UTF-8 byte entries. Entries are unique and sorted by byte value for deterministic encoding. Indexes are one-based; zero means no association and is valid only where the domain permits an empty association.

Elements and regions reference this table. Node mappings use a separate optional block, avoiding repeated long `GeometryId` strings while preserving fixed-size node and element blocks.

## 6. Metadata Block

The block starts with four `u32` values: schema `1`, creation-time length, backend-ID length, and backend-version length. The three UTF-8 strings follow without terminators. All three strings are non-empty.

## 7. Nodes And Node Associations

Each node record is 32 bytes:

```text
nodeId:u64, x:f64, y:f64, z:f64
```

The node block length equals `nodeCount * 32`.

The optional node-association block starts with `recordCount:u64`. Each 16-byte record contains `nodeId:u64`, `associationIndex:u32`, and `reserved:u32`. It contains only nodes with non-empty geometry associations, in node-block order. Node IDs occur at most once and exist in the node block.

## 8. Elements

Each Triangle3 record is 48 bytes:

```text
elementId:u64, nodeIds[3]:u64, regionId:u64,
associationIndex:u32, reserved:u32
```

Each Tetrahedron4 record is 56 bytes:

```text
elementId:u64, nodeIds[4]:u64, regionId:u64,
associationIndex:u32, reserved:u32
```

Element blocks preserve relative `MeshModel` order. Lengths exactly match header counts and fixed record sizes. Element association indexes are positive.

## 9. Regions

Regions preserve `MeshModel` order. Each record begins with `regionId:u64`, `mappingCount:u32`, and `reserved:u32`, followed by positive `associationIndex:u32` values. Mapping count is positive. A zero 4-byte pad follows an odd number of indexes so the next record stays 8-byte aligned.

## 10. Read And Write Validation

Writers validate `MeshModel` in the caller-selected mode, apply caller budgets, then emit canonical directory and association ordering.

Readers treat bytes as untrusted and validate in this order:

1. Minimum size, magic, version, byte order, header size, header CRC, and exact file size.
2. File, count, directory-entry, and string limits before allocation.
3. Directory flags, ordering, bounds, overlap, required blocks, zero padding, and block CRC.
4. Exact records, reserved zeros, UTF-8, association indexes, and node targets.
5. Reconstructed `MeshModel` under caller-selected Worker or committed validation mode.

A Worker reader requires `MeshRevision=0`; a committed reader requires a positive revision. Both require a positive exact source `GeometryRevision`.

## 11. Compatibility

- A different major is incompatible; minor `0` is unsupported.
- A newer minor is readable only when header flags are understood, compression is supported, unknown blocks are optional, and 0.1 invariants hold.
- Adding a required block, changing a known record, checksum semantics, or association-index meaning requires a major change.
- Compression may be added only with an explicit algorithm and decompression budget; 0.1 fails instead of guessing.

## 12. Initial Resource Limits

Every call receives explicit limits. Recommended first-release defaults are 512 MiB file bytes, 10,000,000 nodes, 20,000,000 of each element type, 100,000 regions, 2,000,000 association strings, 64 directory entries, and 4096 bytes per geometry or metadata string.

Applications MAY choose smaller limits from the job memory budget. They MUST NOT raise limits based on untrusted declarations.

## 13. Implementation Contract

- Source: standard-library-only adapter under `src/meshing`; `src/domain` does not depend on it.
- Tests: dependency-free codec tests under `tests`, including byte corruption and budget cases.
- Configure: `cmake --preset vs2022-x64`.
- Build: `cmake --build --preset release`.
- Test: `ctest --preset release`.
- Style: C++17, explicit fixed-width integer types, structured result/error values, no exceptions for untrusted input.
- Always: validate at the binary boundary and check overflow before allocation.
- Ask first: add compression or change a known block layout.
- Never: serialize object memory, accept arbitrary paths, or couple the codec to Qt, OCCT, or Gmsh.

## 14. Acceptance

1. Worker and committed models round-trip without losing IDs, coordinates, per-type element order, metadata, conformity, or mappings; decoded elements use canonical Triangle3-then-Tetrahedron4 order.
2. Re-encoding one model produces identical bytes.
3. Bad magic, versions/features/compression, reserved fields, overflow, overlap, truncation, trailing bytes, UTF-8, indexes, or checksums are rejected.
4. Limits are enforced before allocations based on declared values.
5. A compatible unknown optional block is skipped; an unknown required block is rejected.
6. Decoded output passes the dependency-free domain validator in the requested mode.
