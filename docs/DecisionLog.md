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

## ADR-010 — Object registry ownership (M2)
Status: Accepted

`ObjectRegistry` is a plain member of `PartDocument` (document-local; no singleton, no global). Ownership is UNCHANGED from M1: `PartDocument` owns Parameters (ParameterManager's `vector<unique_ptr>`) and Bodies/Frames/Connectors; `Body` owns Features; there is still no common base class.

- The registry stores a type-safe non-owning tagged handle: `std::variant<Parameter*, Body*, Feature*, IRecomputable*>` — no `void*`, no new base class. The `IRecomputable*` alternative is how externally owned recomputables (test stubs in M2) participate.
- Lifetime guarantee: a registered pointer is valid exactly while the owner holds the object. Every public mutation path that destroys an object goes through `PartDocument::removeObject`, which unhooks graph → registry BEFORE the owner erases, so no dangling reference is reachable through a public path.
- Handles are runtime-only and are NEVER serialized (persistent identity is always `ObjectId`).
- Duplicate id, `kInvalidObjectId`, null handles, and handles whose `->id()` differs from the registered id are all rejected (return `false`).
- Registered in M2: Parameters and Bodies (via the document façade) and external `IRecomputable`s (via `addRecomputableNode`). Features stay Body-owned and unregistered until they join the document graph in M3.

## ADR-011 — ParameterState vs ComputeState; recompute semantics (M2)
Status: Accepted

The spec 4.4 recommended rule is adopted verbatim:

- `ParameterState` = evaluation validity of the Parameter's value/expression, owned by `Parameter` (`setValue`/`setExpression` still set `ParameterState::Dirty`).
- `ComputeState` (graph) = execution state of a document node; it ALONE governs document recompute scheduling. `ParameterState` never schedules anything.
- Bridge: `PartDocument::setParameterValue(id, v)` sets the value (→ `ParameterState::Dirty`) AND marks the graph node dirty (propagates to dependents). When the engine visits a Dirty parameter node it auto-validates: graph → Valid, `ParameterState` → Valid, report item `Success` with message "dirty source". Scalar parameters have trivial evaluation; a non-empty expression string still auto-validates in M2 (no evaluator — documented limitation). Parameters do NOT implement `IRecomputable`; they are registered dirty-source nodes.
- Failed owner: the graph for document nodes; `Parameter` for future expression-evaluation errors (M3+). Retry: `markDirty` on a Failed node → Dirty → normal recompute (ADR-007 barrier semantics unchanged).
- `markDirty` lives once, on `PartDocument` (delegating to the graph plus the ParameterState bridge); the engine exposes only recompute entry points — one mutation path.
- Engine report statuses: `{Success, Failed, BlockedByDependency, SkippedSuppressed}`. Spec-conflict note: spec 6.3 suggested `{Success, Failed, Skipped, Suppressed}` — "Skipped" alone is ambiguous between blocked-by-failure and suppressed, and spec 10 itself invites the `BlockedByDependency` extension. `Failed` means the node's callback ran and reported failure; `BlockedByDependency` means it was never invoked because a prerequisite failed (message names the first failed direct prerequisite so a UI can explain the block without re-running). The engine report type is `DocumentRecomputeReport` because `RecomputeReport` already names the generic graph report.
- `recomputeFrom(id)` = `markDirty(id)` + full dirty-set recompute; any OTHER already-dirty nodes also run (a strict descendants-only filter would leave stale dirty nodes) — documented on the method.
- Suppression rule pinned for M2: a Suppressed node is never executed and reports `SkippedSuppressed`; dirty and failure propagation pass THROUGH it (ADR-007); a Dirty dependent of a Suppressed prerequisite DOES execute normally. Spec-conflict note: spec 9's "must not falsely report success" is not violated because M2 nodes have no output contract, so no false success is expressible; real cached-output semantics arrive with CAD features in M3. Unsuppress → Dirty.
- Known corner case (pinned by `M2_PROBE_005_SuppressedParameterEvaluationStateObservation`): suppressing a Parameter's graph node leaves `ParameterState` stuck at `Dirty` because the ADR-011 auto-validation bridge only runs from inside the graph callback, which a Suppressed node never receives; `ComputeState` correctly stays `Suppressed` throughout (still the sole scheduling source of truth) and the parameter self-heals to `ParameterState::Valid` on the next recompute after unsuppression.

## ADR-012 — Dependency persistence: hybrid, edges persisted now (M2)
Status: Accepted

Schema v2 persists explicit dependency edges (spec 14 Option A, restricted). Rationale: M2 has no semantic edge sources (stubs are test-only; features gain input references only in M3), so Option B would have nothing to rebuild from.

- Only edges whose BOTH endpoints are persisted document objects are saved; edges touching runtime-only `IRecomputable` stubs are never saved (test-scoped by construction).
- `"schemaVersion": 2` with a new optional top-level array `"dependencies": [{"prerequisite": "<id>", "dependent": "<id>"}]`, written in graph insertion order (deterministic). The loader accepts v1 (no edges) and v2; save always writes v2. New `SerializationError` enumerators: `UnknownDependencyId` (endpoint is not a graph-node object in the file), `InvalidDependency` (self-edge/cycle). Edges are fully validated on a scratch graph BEFORE document construction, preserving all-or-nothing loading and the never-advance-the-id-generator-on-failure guarantee.
- Graph node ComputeStates are NOT persisted (derived data, ADR-007/CodingRule 8): after load all nodes start Dirty, so the first recompute covers everything. Suppression is not persisted in M2 (graph-only, test-scoped; revisit when features join the graph in M3).
- Migration intent: when M3 adds semantic references, semantically derivable edges migrate to Option B (rebuilt from the model); explicitly user-created edges stay persisted.
- Edge-direction signature note (flagged spec conflict): the façade mirrors the existing graph exactly — `addDependency(dependent, prerequisite)`, "dependent consumes prerequisite". Spec 7's suggested `(dependency, dependent)` order is NOT adopted: two public APIs with opposite parameter orders would be the real hazard, and spec 5/6.5 permits adapting signatures to repository style. The semantic rule stays singular (ADR-007): an edge points prerequisite → dependent, dirtiness flows downstream. Direction-documenting tests: `M2_DIR_001/002`.
- `DependencyGraph` gained ONLY the read-only `nodes()` accessor (insertion order) so the serializer can write edges deterministically; the graph stays generic and learns no CAD types. The rejected alternative — a serializer-side edge log on `PartDocument` — would duplicate graph truth (spec 4.4).
- Breaking change note: `PartDocument::parameters()` became const-only so the document façade is the single registration path (spec 13); callers use `addParameter`/`setParameterValue`/`removeObject`. All in-repo callers were updated in the same change.
- ADR-009 follow-up (virtual `Feature::typeName()` replacing the serializer `dynamic_cast`) remains open; not in M2 scope (serializer debt, no M2 behavior depends on it).
- D14: On load, the generator is advanced past the file's maximum persisted id before document construction, so fresh ids allocated during restore (e.g. the re-created Origin frame) can never collide with restored ids. `PartDocument` gained a read-only `frames()` accessor (mirroring `bodies()`) so tests can assert document-wide id uniqueness.

Follow-up for M2 (recorded per review): replace the serializer's `dynamic_cast`-based feature type lookup with a virtual `Feature::typeName()` (or registry) so concrete feature types cannot silently serialize as `"Placeholder"`.
