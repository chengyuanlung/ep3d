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

Follow-up for M2 (recorded per review): replace the serializer's `dynamic_cast`-based feature type lookup with a virtual `Feature::typeName()` (or registry) so concrete feature types cannot silently serialize as `"Placeholder"`. **Closed in M3, see ADR-M3-005.**

## ADR-M3-001 — Geometry kernel boundary and shape ownership (M3)
Status: Accepted

Three physical layers: a kernel-neutral interface INSIDE `src/Core` (not a separate linked target), a runtime shape value type, and an OCCT adapter under `src/Kernel/Occt` that is the only place OCCT is ever included or linked.

- `src/Core/Kernel/KernelTypes.h` (zero OCCT): `BoxDefinition` (mm), `KernelError`, and the free function `IsValidBoxDefinition` — the ONE place numeric dimension validation lives; every `IGeometryKernel` implementation, real or fake, calls it first. `KernelMassProperties`/`KernelMassPropertiesResult` carry density-independent geometric results (volume, COM, second moment of volume about COM).
- `src/Core/Kernel/KernelShape.h/.cpp` (zero OCCT): `IShapeHandle` is a pure opaque marker; `KernelShape` is a copyable value type wrapping `shared_ptr<IShapeHandle>` (CodingRule 6 — no owning raw pointer), default-invalid. It carries no `ObjectId` and no comparable identity — `BoxFeature::id()` (from `Feature`) remains the only persistent identity for a feature's shape (shape identity rule). `KernelShape`/`IShapeHandle` are never serialized.
- `src/Core/Kernel/IGeometryKernel.h` (zero OCCT): `ShapeResult` and the `IGeometryKernel` interface (`createBox`, `calculateMassProperties`). Expected invalid input returns a controlled result with a `KernelError` and diagnostic message — never throws, never UB.
- `src/Kernel/Occt/OcctShape.h` wraps a `TopoDS_Shape` as an `IShapeHandle`; `src/Kernel/Occt/OcctGeometryKernel` implements `createBox` via `BRepPrimAPI_MakeBox` and `calculateMassProperties` via `BRepGProp`/`GProp_GProps`, using `dynamic_cast` on the incoming `KernelShape`'s handle — null/wrong-type cast returns a structured `GeometryConstructionFailed`, never UB.
- **Transactional shape ownership** (spec 12, scorecard "no dangling/partial shape"): `BoxFeature` holds `KernelShape currentShape_` as a member (default invalid). `recompute()` builds a LOCAL `ShapeResult` first; `currentShape_` is reassigned ONLY on success. A failed build leaves `currentShape_` byte-for-byte unchanged — "transactional" falls out of this ordering with zero extra bookkeeping. Staleness is communicated exclusively through `ComputeState` (graph + `Feature::state()`, see ADR-M3-004), never by mutating or nulling the shape.
- **Flagged deviation from spec 5**: the spec suggests a standalone top-level `src/Kernel/` for `IGeometryKernel`/`KernelShape`. This project places them under `src/Core/Kernel/` instead — the smallest change that satisfies both "Core injects a kernel service" (ADR-M3-003) and "Core links/includes nothing beyond std + Core" without amending CodingRule 2. `src/Kernel/Occt/` remains exactly where the spec puts it.

## ADR-M3-002 — Geometry and physical units (M3)
Status: Accepted

- CAD/kernel length: mm (`BoxDefinition` fields). OCCT is built directly in mm by convention.
- Kernel-computed geometric quantities are density-independent: `volumeMm3` (mm³), `centerOfMassMm` (mm, exposed to CAD unconverted), `secondMomentMm5` = geometric second moment of volume about COM, `∫r²dV`, in mm⁵ (OCCT's `GProp_GProps` yields this natively in the shape's build unit, which is mm, so no extra scaling happens inside `Kernel/Occt`).
- **Single traceable conversion site: `MassPropertiesNode::recompute()`.** Named constants `kMm3ToM3 = 1e-9` and `kMm5ToM5 = 1e-15` (both `(1e-3)^n`): `volumeM3 = volumeMm3 * kMm3ToM3`; `massKg = densityKgPerM3 * volumeM3`; `centerOfMassMm` passes through unconverted; `inertiaTensorKgM2[i] = densityKgPerM3 * secondMomentMm5[i] * kMm5ToM5` — dimensionally `(kg/m³)·(m⁵) = kg·m²`, matching the physical inertia convention. Production code never multiplies a density by a raw mm³/mm⁵ value without going through these two constants (mechanically greppable).
- `MassProperties.h` breaking rename: `inertiaTensorKgMm2` → `inertiaTensorKgM2` — the old name was both mis-unit-labeled and never populated by real data; fixed before any real producer existed.
- Numeric type: `double` throughout (not `long double`) — consistent with every existing Core numeric field; double's ~15-17 significant digits vastly exceed the tolerances below for CAD-scale numbers.
- Tolerances (spec 18; a box is an exact analytic primitive, so tight tolerances are justified, not aspirational): length/COM components — absolute `1e-6` mm. Volume/mass — relative `1e-9`. Diagonal inertia terms (Ixx/Iyy/Izz) — relative `1e-9`. Off-diagonal terms (Ixy/Ixz/Iyz, expected ≈0, so relative tolerance is meaningless) — absolute `1e-9` kg·m² (the analytical diagonal magnitudes in the mandatory case are ~1e-3 to 1e-1 kg·m², so this floor sits 6+ orders of magnitude below signal). Documented once in the Kernel test files; never enlarged to make a failing implementation pass.
- The mandatory analytical case (100×50×20 mm, density 2700 kg/m³ → 0.27 kg, COM (50,25,10) mm) and the inertia oracle (`Ixx = m/12·(h²+d²)` etc., meters) are computed independently in test files using raw formulas, never via production conversion helpers.

## ADR-M3-003 — Kernel service injection (M3)
Status: Accepted

Reconciling "Core neither includes nor links OCCT" with "Core needs to receive an `IGeometryKernel&`": `IGeometryKernel`/`KernelShape`/`KernelTypes` are Core headers (zero OCCT, ADR-M3-001), so referencing them from `RecomputeContext` does not violate CodingRule 2. `src/Kernel/Occt/` is the only place that ever includes OCCT; it depends on (includes and links) Core, never the reverse.

- `RecomputeContext` gains `IGeometryKernel* kernel = nullptr;` (forward-declared only, no new `#include`).
- `PartDocument::setGeometryKernel(IGeometryKernel*)` / `geometryKernel() const` — a plain non-owning pointer, defaulting to `nullptr`. `DocumentRecomputeEngine::run()` populates `RecomputeContext{document_, registry, document_.geometryKernel()}` before invoking any callback.
- Lifetime contract (documented, not enforced by new machinery — mirrors ADR-010's externally-owned `IRecomputable` pattern): whoever calls `setGeometryKernel` owns the concrete kernel and must keep it alive for every subsequent `recompute()`/`recomputeFrom()` call. `PartDocument` never constructs a kernel itself.
- Only `src/App/main.cpp` (owns an `OcctGeometryKernel`, calls `setGeometryKernel(&kernel)` once at startup) and test files that link `ParametricCADKernelOcct` ever construct a concrete kernel. `BoxFeature` never names `OcctGeometryKernel` — it only ever sees `context.kernel` (an `IGeometryKernel*`).
- `context.kernel == nullptr` is a normal, tested failure path: `BoxFeature::recompute`/`MassPropertiesNode::recompute` return `{Failed, "no geometry kernel configured"}` — no crash, no UB. This is also how Core-only tests exercise `BoxFeature`/`MassPropertiesNode` without linking OCCT at all (`tests/Fakes/FakeGeometryKernel.h`).

## ADR-M3-004 — Failed feature / last-valid-shape policy (M3)
Status: Accepted

Recommended policy adopted verbatim: retain the last valid shape, mark it unequivocally stale. Mechanics: `currentShape_` is untouched on failure (ADR-M3-001); staleness is carried entirely by `ComputeState`, never by shape mutation.

- **`Feature::state_` vs graph `ComputeState`** — flagged architectural finding, resolved by extending ADR-011's rule rather than inventing a new one: once `BoxFeature` is also registered as a graph node (via `IRecomputable*`), there are two `ComputeState` values for the same `ObjectId`. **The graph's `ComputeState` is the sole scheduling source of truth; `Feature::state()` is a manually-synchronized cache**, kept current ONLY by the feature's own `IRecomputable::recompute()` body calling `setState(...)` at the same point it returns its `RecomputeResult` — no new infrastructure pushes graph state into `Feature::state_`. `BoxFeature::recompute(context)`: success → `setState(Valid)`, return `{Success, ""}`; failure → `setState(Failed)`, return `{Failed, message}`.
- `Feature::recompute()` (the old M1 pure-virtual, no-context, bool-returning method) is kept AS-IS — zero change, zero regression risk to `PlaceholderFeature`/existing tests. `BoxFeature` implements it trivially and it is documented as vestigial/inert (`return state() != ComputeState::Failed;`), never called by the M3 engine path. Accepted debt (hard rule 8); a candidate M4 cleanup is collapsing `Feature` to inherit `IRecomputable` directly.
- Downstream blocking is automatic, not new: `MassPropertiesNode` depends on `BoxFeature`'s graph node, so M2's existing failure-propagation (ADR-007/ADR-011, unmodified) makes `MassPropertiesNode` receive `BlockedByDependency` and never even get invoked whenever `BoxFeature` is `Failed`. Defense in depth only: `MassPropertiesNode::recompute` ALSO explicitly checks `boxFeature->state() == ComputeState::Valid` before reading `currentShape_`/calling the kernel, returning `Failed` itself ("upstream BoxFeature is not valid") if not — this guards a direct/out-of-band call (e.g. a unit test invoking `recompute(context)` outside a graph pass) beyond what the graph alone guarantees.
- Density policy (spec 13): **zero density is valid** — COM is computed purely from geometry (`∫r dV / V`) and never divides by mass, so it is well-defined at density = 0 exactly as at any other uniform density (mass = 0 kg, inertia = zero matrix, COM unchanged); this is a real, useful CAD state (an unassigned/reference part), not degenerate. **Negative and non-finite density (NaN/Inf) fail**, validated in `MassPropertiesNode::recompute()` via `std::isfinite(density) && density >= 0.0` before any kernel call. A `BoxFeature` with no material assigned behaves like density = 0 (same code path, no separate failure mode).
- Recovery: fixing the input Parameter and calling `markDirty`/`recompute` follows the existing ADR-007 barrier-clear semantics unchanged.

## ADR-M3-005 — Computed geometry persistence policy (M3)
Status: Accepted

Schema v3. Persists semantics only (spec 15); extends ADR-012's hybrid policy with a concrete rule for WHICH edges are Option A vs Option B.

- **Closes the ADR-009/012 `dynamic_cast` follow-up**: `Feature` gains `virtual std::string_view typeName() const noexcept = 0;`. `PlaceholderFeature::typeName()` becomes the override of its existing accessor; `BoxFeature::typeName()` returns `"Box"`. The serializer's feature-type dispatch (which concrete type to construct on load) is keyed by this string, not `dynamic_cast` probing across candidate types — a `dynamic_cast` is still used once on save, only to reach a type it already knows the name of (`BoxFeature`'s extra accessors), which is not the debt this closes. An unrecognized `"type"` string on load still falls back losslessly to `PlaceholderFeature`, preserving the string.
- **BoxFeature JSON record** (inside a body's `"features"` array, `"type":"Box"`): `{"id","name","state","type","widthParameterId","heightParameterId","depthParameterId","materialId"}` — id fields as decimal strings; `materialId` may be `"0"` (`kInvalidObjectId`) meaning "no material assigned", validated against the document's material record if present.
- **Material JSON record** (new top-level `"material"` object, `null` when unassigned): `{"id","name","densityKgPerM3","elasticModulusPa","poissonRatio","yieldStrengthPa","contact":{"staticFriction","dynamicFriction","restitution"}}` — the full record is persisted. `Material` gained `setDensity(double)` and a restore constructor (via the shared `RestoreObjectId` helper, mirroring every other restore ctor). `ObjectRegistry::ObjectRef` extended with `Material*`; a `Material` is registered as a dirty-source graph node exactly like `Parameter` (ADR-011's pattern reused verbatim): `PartDocument::setMaterialDensity(double)` calls `Material::setDensity` and `graph_.markDirty(materialId)`, mirroring `setParameterValue`.
- **Edge persistence split** (extends ADR-012's migration-intent note precisely):
  - **Option B (re-derived, NEVER written to the generic `"dependencies"` array)**: every edge whose dependent is a `Feature` (`BoxFeature`'s Width/Height/Depth prerequisites) or is the document's singleton `MassPropertiesNode` (never persisted, exactly like the Origin frame — its id is never a parameter/body/feature id, so it is automatically excluded from the persisted-id set on save; feature-owned edges are additionally excluded explicitly). These are always reconstructed by calling the SAME facade methods used for fresh creation (`addBoxFeature`/`restoreBoxFeature`), driven by the semantic id fields — never by replaying a persisted edge list. General rule for future milestones: an edge is Option B whenever at least one endpoint does not carry an independently stable, persisted identity across every load, or whenever the relationship is already fully implied by other persisted semantic fields on that object.
  - **Option A (persisted, unchanged from ADR-012)**: everything else — in M3 this concretely means the generic `"dependencies"` array is expected empty in practice, but the mechanism stays live and correct for any future non-Feature edge type.
- **Facade methods** (single registration path, spec 13): `PartDocument::addMaterial`/`restoreMaterial` mirror `addParameter`/`restoreParameter`. `PartDocument::addBoxFeature(Body&, name, widthId, heightId, depthId)` creates via `body.addFeature<BoxFeature>`, registers it as `IRecomputable*` (reusing `addRecomputableNode` via the `BoxFeature&`→`IRecomputable&` upcast, since `BoxFeature` inherits both `Feature` and `IRecomputable`), wires the 3 W/H/D edges, and (re)wires `massPropertiesNode_`'s 2 edges to (this box, the document's currently assigned material, if any) — detaching any previous box/material source's edges first so the graph never accumulates stale prerequisites. `restoreBoxFeature` is the restore-ctor equivalent with the same wiring, driven by the persisted `materialId` field.
- **`massPropertiesNode_` scoping note (explicit, not silent)**: M3 hard-wires the document's singleton `MassPropertiesNode` to whichever `BoxFeature` was added/restored most recently — a genuine simplification appropriate to "first parametric solid," not a general multi-feature mass-properties model. `massPropertiesNode_` is a `PartDocument` value member (RAII, not `unique_ptr`), auto-constructed with a fresh `ObjectId` and auto-registered in the registry by BOTH constructors, but it only JOINS THE GRAPH the first time a `BoxFeature` is wired to it — a document that never adds a `BoxFeature` must not carry a permanently Dirty, permanently failing, edge-less recompute node (a real defect caught during Phase 3/4 testing and fixed before this ADR was written). Its own id is never persisted. Flagged for M4+: Pad/Pocket chains will need real shape-source selection instead of a hard-wired singleton.
- Do not persist: `TopoDS_Shape`, `KernelShape`, `IShapeHandle`, any `Kernel/Occt` runtime state, `MassProperties` computed values (still purely derived, ADR-009 D6, now finally populated by real computation instead of a stub).
- After load: `restoreBoxFeature`'s wiring means the graph is fully reconstructed and every node starts Dirty — the very next `recompute()` call rebuilds equivalent geometry from the restored Parameters/Material, satisfying spec 15's "recompute geometry after load" without any special-cased load-time geometry rebuild.
- Schema version bumped to 3 (written on save); the loader still accepts v1 and v2 files.
