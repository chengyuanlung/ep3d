# Architecture Decision Log

## ADR-001 — Modern C++ Core
Status: Accepted

Use C++20 for the new CAD core. Legacy application code may remain in its existing toolchain, but this project does not target legacy compilers.

## ADR-002 — Core independent of UI and CAD kernel
Status: Accepted

`src/Core` may not depend on Qt or OpenCASCADE. Adapters isolate external frameworks.

## ADR-003 — Part physical properties are first-class
Status: Accepted

Material assignment and derived Mass/COM/Inertia are included from the initial Part data model to support future mechanism, robot, and physics workflows.

## ADR-004 — Motion belongs to Assembly joints
Status: Accepted

A Part can expose Connectors/Frames, but revolute/prismatic behavior is established by Assembly-level Joints.

## ADR-005 — ReferenceFrame is general infrastructure
Status: Accepted

User/Tool/Camera/Fixture/Robot frames use one common frame graph rather than robot-specific coordinate classes.

## ADR-006 — Collision is separate from Joint limits
Status: Accepted

Joint limits constrain allowed generalized coordinates; collision checks whether spatial configurations are valid.

## ADR-007 — DependencyGraph semantics
Status: Accepted

The Core `DependencyGraph` (M1) is a standalone DAG over `ObjectId` nodes; document integration is deferred to M2/M3.

- Edges point prerequisite → dependent; `addDependency(dependent, prerequisite)` reads "dependent consumes prerequisite". Cycles are rejected with a structured error and leave the graph unchanged.
- `markDirty` propagates transitively downstream, passing through Suppressed nodes without changing their state.
- Actual edge insertion/removal marks the dependent Dirty (transitively); duplicate/absent-edge no-op paths dirty nothing.
- Recompute is topological, deterministic (insertion-order tie-breaking, per-session only), and visits only Dirty nodes, each at most once.
- A Failed node persists as a downstream barrier across recompute passes: a Dirty dependent of a Failed prerequisite is set Failed without invoking its callback, until the failed node is re-marked Dirty and recomputes successfully. Idle passes do not re-report long-standing failures.
- `ComputeState` is shared with `Feature` via `Core/Feature/ComputeState.h` (single enum, no drift).

## ADR-008 — Known discrepancy: GoogleTest not yet wired
Status: Resolved

Resolved with ADR-009: GoogleTest v1.17.0 is wired via find_package-then-FetchContent, both legacy assert suites were converted to GTest 1:1, and the suite is authoritative in Debug and Release.

## ADR-009 — Native JSON serialization (schema v1) and GoogleTest adoption
Status: Accepted

PartDocument gains versioned native JSON save/load (header, scalar parameters, bodies, feature metadata) built on a minimal in-Core JSON DOM with a hand-rolled recursive-descent parser and deterministic pretty writer. Decisions:

- D1: ObjectIds are serialized as decimal strings (uint64 exceeds double's 2^53 integer range), parsed with `std::from_chars`.
- D2: Enums are serialized as their exact enumerator-name strings; unrecognized names are rejected with `InvalidEnumValue`. The serializer owns the to/from-string helpers; enum headers stay minimal.
- D3: Id-collision safety lives in the restore constructors: every one calls `ObjectIdGenerator::AdvancePast(id)` (via the shared `RestoreObjectId` helper), so safety holds by construction at every injection site.
- D4: Features are restored as `PlaceholderFeature` carrying the persisted `"type"` string; concrete geometry feature types arrive with later schemas.
- D5: The document id itself is serialized and preserved.
- D6: Frames, connectors, material, and mass properties are excluded from schema v1 (the Origin frame is re-created fresh on load).
- D7: DependencyGraph contents are derived data and are not serialized.
- D8: The canonical API is stream-based (`savePartDocument`/`loadPartDocument`); file-path functions are thin wrappers.
- D9: Tolerance policy: unknown object keys are ignored; missing required fields and wrong JSON types are structured errors.
- D10: GoogleTest is resolved by `find_package(GTest CONFIG)` first, falling back to FetchContent pinned to the v1.17.0 release archive by SHA256.
- D11: All test sources build into one consolidated executable (`ParametricCADCoreTests`) registered through `gtest_discover_tests`.
- D12: Persisted ids are capped at `kMaxObjectId` (2^63 - 1) in schema v1; larger id strings are rejected with `InvalidFieldType`, and `ObjectIdGenerator::AdvancePast` clamps to the cap so the counter can never wrap to `kInvalidObjectId` (2^63 organic allocations of headroom remain). Duplicate persistent ids within one document (across document/parameters/bodies/features) are rejected with `DuplicateId` per the stable-identity rule.
- D13: Non-finite parameter values (NaN/Inf) are not representable in JSON and serialize as `0` (documented JsonValue writer contract); the parser stays lenient on leading zeros and skips an optional UTF-8 BOM on read.
- D14: On load, the generator is advanced past the file's maximum persisted id before document construction, so fresh ids allocated during restore (e.g. the re-created Origin frame) can never collide with restored ids. `PartDocument` gained a read-only `frames()` accessor (mirroring `bodies()`) so tests can assert document-wide id uniqueness.

Follow-up for M2 (recorded per review): replace the serializer's `dynamic_cast`-based feature type lookup with a virtual `Feature::typeName()` (or registry) so concrete feature types cannot silently serialize as `"Placeholder"`.
