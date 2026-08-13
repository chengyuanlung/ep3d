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

## ADR-M3-006 — Currency vs retention for derived results (M3, post-review)
Status: Accepted

Raised as Major finding 1 by the M3 independent review: `MassProperties::valid`
was set `true` on first success and never cleared, so after a geometry failure
the document still exposed `valid=1` with the previous numbers — failing spec 2's
"downstream current results do not falsely succeed" at the data level even though
the recompute *report* correctly said failure.

ADR-M3-001/004 established **retention**: a failed rebuild keeps the last valid
result rather than destroying it. That policy is unchanged and still right. What
was missing is its other half: retained data must not claim to be current.

- **Retention and currency are separate properties.** For `BoxFeature` the two are
  already separated: `currentShape_` is retained, and currency is carried by
  `ComputeState`, which every consumer reaches through the graph.
  `MassProperties` has no such wrapper — `PartDocument::massProperties()` hands
  out a plain struct, so its own `valid` flag is the only currency signal a
  reader ever sees, and it must be maintained with the same discipline.
- **The clear must happen at document level.** `PartDocument::refreshMassPropertiesCurrency()` inspects the engine report after every `recompute()`/
  `recomputeFrom()` and clears `valid` if the mass node did not succeed. Doing
  this only inside `MassPropertiesNode::recompute()` cannot work: when the
  upstream box fails, the graph blocks the node with `BlockedByDependency` and
  never invokes it, so no code inside the node runs at all. The node's own
  failure paths (via `failAndMarkStale`) clear the flag as defense in depth for
  direct out-of-band calls, mirroring the existing upstream-state check.
- **A node absent from the report was not touched**, so its previous currency
  stands. Only nodes that actually ran, or were actually blocked, are affected.
- General rule for future derived results: whenever a value is retained past a
  failure, the same commit must define how a reader learns it is stale. If the
  value is not reached through something carrying `ComputeState`, it needs its
  own currency flag, and that flag needs a clearing path on every failure route
  including the ones where the producing code never executes.

## ADR-M3-007 — Cached Feature state is derived, never authoritative (M3, post-review)
Status: Accepted

Raised as Major finding 2. ADR-M3-004 declared the graph's `ComputeState` the
sole scheduling source of truth and `Feature::state_` a manually-synchronized
cache, but nothing kept the cache honest: `Feature::markDirty()` had **zero
callers** anywhere in `src/`. Two observable falsehoods followed — a restored
feature read `Valid` while holding no runtime shape (ComputeState is persisted,
`KernelShape` deliberately is not, ADR-M3-005), and a feature kept reading
`Valid` after a parameter edit had superseded its geometry.

- `PartDocument::syncFeatureStatesFromGraph()` demotes any feature whose cached
  `state()` claims `Valid` while the graph disagrees. Called from every path that
  can invalidate: `setParameterValue`, `setMaterialDensity`, `markDirty`,
  `wireBoxFeature` (covering restore), `recompute`, `recomputeFrom`.
- **Only false `Valid` is corrected, and only ever downward to `Dirty`.** `Failed`
  is never manufactured by the sync: a feature is `Failed` solely because its own
  `recompute()` ran and reported failure. This keeps ADR-M3-004's rule that a
  feature's own recompute is the only writer of `Failed` intact.
- The asymmetry is deliberate and worth stating: a cache that wrongly says
  "needs recompute" costs one redundant recompute, while a cache that wrongly
  says "valid" hands out superseded geometry. Only the second is a correctness
  bug, so the sync is one-directional.
- Persisting `ComputeState` while deliberately not persisting the runtime shape
  means a restored `Valid` is **always** unearned. Rather than special-case the
  loader, `wireBoxFeature` runs the same sync every other path uses: the fresh
  graph node starts `Dirty`, so the demotion falls out of the general rule.
- **SCOPE — the sync applies only to features that are graph nodes.** As of M3
  features are heterogeneous: `BoxFeature` is the first and so far only type that
  joins the dependency graph (via `wireBoxFeature`), while `PlaceholderFeature`
  is Body-owned and never registered. A feature outside the graph has no graph
  state that could be authoritative over it — its `ComputeState` is owned by
  whoever drives it and is persisted verbatim, so rewriting it would corrupt a
  lossless round-trip. `syncFeatureStatesFromGraph()` therefore skips any feature
  with no graph node, and that is a semantic rule, not a defensive check.
- **How this was learned (recorded because the pattern will recur).** The first
  version of the sync called `graph_.state()` for every cached-`Valid` feature.
  `DependencyGraph::state()` asserts on an unknown id, so a document holding a
  `Valid` `PlaceholderFeature` **aborted the process in Debug** — including
  inside `loadPartDocument` itself — and in Release silently demoted the
  placeholder, making Debug and Release disagree about document semantics. It
  was caught by independent re-review, not by the 174-test suite, because every
  existing test either used a Box-only document or never recomputed after
  creating a placeholder.
- This was the **second** defect from the same assumption that every `Feature`
  behaves like a `BoxFeature`; the first was the stale "Feature*: not registered"
  comment in `removeObject` (ADR-M3-008). M4 will add more feature types, so any
  code iterating `Body::features()` must state which kinds of feature it applies
  to before it touches one.

## ADR-M3-008 — Removal completeness and save/load symmetry (M3, post-review)
Status: Accepted

Raised as Major findings 3 and 4. Both are the same class of defect — an
operation that reported success while leaving the document in a state its own
invariants reject.

- **Removal must reach every owner.** `PartDocument::removeObject` cleaned up the
  graph and registry but had no case for a Body-owned `Feature`, because in M2
  features were not registered at all. Since M3 a `BoxFeature` registers under
  the `IRecomputable*` variant while still being owned by its `Body`, so the
  pre-existing "Feature*: not registered" comment had quietly become false.
  The feature survived removal: still serialized, still restored on load, and
  still the mass node's source, which then failed every subsequent recompute
  forever with "no box feature configured". Fixed with `Body::removeFeature` plus
  an `IRecomputable*` case that detaches `massPropertiesNode_` FIRST (it holds
  the id by value, so leaving it set would point it at a destroyed object).
- **Save must accept exactly what load accepts.** The loader rejects a BoxFeature
  whose W/H/D parameter ids are not parameters in the file; the save path wrote
  those ids unchecked. Removing a referenced Parameter therefore produced a file
  that saved cleanly and could never be loaded again — potentially over the only
  copy of the data. `validateSaveable()` applies the symmetric check before
  writing, so the dangling reference surfaces while the in-memory document is
  still intact and repairable.
- General rule: any validation the load path enforces is an invariant of the
  format, and the save path must enforce it too. A one-sided check does not
  prevent a bad document — it only defers the failure to the point where the
  data is already lost.

## ADR-M3-009 — Minimum accepted box dimension (M3, post-review)
Status: Accepted

`IsValidBoxDefinition` originally accepted any finite, strictly positive
dimension. That is not sufficient in practice: OCCT rejects degenerate
primitives below its own confusion tolerance by throwing, so a positive-but-
degenerate input (say 1e-12 mm) escaped the structured `InvalidDimension`
contract of spec 13 and surfaced instead as an unstructured "OCCT raised ..."
string — a kernel-internal error where the spec promises a dimension error.

- `kMinBoxDimensionMm = 1e-6` (one nanometre), checked in `IsValidBoxDefinition`
  so it applies to every kernel implementation, real or fake, exactly like the
  finiteness and sign checks (ADR-M3-001's single-validation-site rule).
- The value sits far above OCCT's rejection threshold and far below any real CAD
  feature, so it rejects only inputs that were already unusable. Verified against
  real OCCT during review: 1e-6 mm is accepted, 9.9e-7 mm is rejected, and a
  mixed 1e-6 x 100 x 50 box builds correctly.
- Upper end: no maximum is imposed. 1e6 mm (1 km) and beyond build without
  incident, and no CAD-meaningful upper bound presented itself that would not be
  arbitrary.
- The diagnostic strings in both kernels were updated at the same time: they
  previously said "must be finite and positive", which became false the moment
  this threshold existed — 9.9e-7 is finite and positive and rejected.

## ADR-M4-001 — Sketch entity identity and reference model (M4)
Status: Accepted

Sketch entities need identities that survive insertion, removal and reordering
of *other* entities (spec 5), and that future constraints can point at
(spec 5's `SketchElementRef`) without M4 solving constraints.

- **`SketchEntityId` is a distinct type allocated from the existing
  `ObjectIdGenerator`.** Distinct so the compiler rejects passing an `ObjectId`
  where an entity id belongs — they are different id spaces semantically, and
  M4 introduces the first place where confusing them is possible. Allocated
  from the *existing* generator rather than a private counter so it inherits
  the collision-safety machinery M1 built and M3 depended on: `RestoreObjectId`
  advancing the generator past every restored id, the `kMaxObjectId` cap, and
  the no-wrap guarantee. A second independent generator would reintroduce
  exactly the restore-collision bug class that machinery exists to prevent.
- Full identity of an entity is the pair (`Sketch::id()`, `SketchEntityId`).
  Entities are NOT registered in `ObjectRegistry`: they are sub-objects of a
  Sketch, not document objects, and in M4 nothing outside their Sketch
  references an individual entity. `ObjectRegistry` keeps its M2 meaning.
- Storage is `std::vector<SketchEntity>` but **position is never identity**
  (ADR-010's rule, extended to sub-objects): every lookup is by
  `SketchEntityId`, and removal does not renumber anything.
- **Sub-element references are reserved, not implemented**:
  `SketchElementRef{SketchEntityId, SketchSubElement}` with
  `SketchSubElement{Whole, StartPoint, EndPoint, CenterPoint}`. M4 constructs
  them for profile orientation but implements no constraint semantics.
- **Line endpoints own their coordinates** rather than referencing shared Point
  entities (the choice spec 7 assigns to the Architect). Referencing shared
  points would make coincidence implicit in the storage model, and M4
  explicitly defers the constraint solver — implicit coincidence with no solver
  to maintain it is a half-built feature. Owning coordinates keeps M4 honest,
  and the `SketchElementRef` indirection means M5 can add a real Coincident
  constraint binding two endpoint refs without changing storage or the
  persisted format.

## ADR-M4-002 — Sketch coordinate frame (M4)
Status: Accepted

Entity geometry is stored in sketch-local `(u,v)` millimetres, never world XYZ
(spec 6), so that moving a sketch's plane does not rewrite its geometry.

- **A `SketchFrame` value embedded in the Sketch, not a reference to a
  `ReferenceFrame` object.** `ReferenceFrame` exists (M0) but is not registered
  in `ObjectRegistry`, not persisted, and the Origin frame is re-created fresh
  on load (ADR-009 D6). Making frames registered, persisted, graph-participating
  document objects is a real change to the M0-M2 contract, and M4 does not need
  it: a sketch's support plane is intrinsic to that sketch. Deferring keeps the
  milestone's blast radius honest rather than smuggling a frame-system redesign
  into a sketch milestone.
- `SketchFrame` carries a `Transform3D` — the same type `ReferenceFrame` uses —
  precisely so that when frames do become first-class (M5+), a Sketch can gain
  an optional `ObjectId supportFrameId` and the embedded transform becomes the
  fallback, with no change to entity storage or to the `(u,v)` convention.
- **Conversion lives in exactly one place**: `SketchFrame::toWorld(Vec2) ->
  Vec3` and `SketchFrame::normal() -> Vec3`. No other code composes the
  rotation. This is the same single-conversion-site discipline ADR-M3-002
  applied to units, for the same reason — a second conversion path is how
  frames silently disagree.
- Default is the world XY plane (identity transform), so world-XY behaviour is
  a *case* of the general path, never a shortcut around it. Translated and
  rotated frames are tested explicitly (spec 6, Gate D) because a world-XY
  hardcode is invisible until something is not at the origin.

## ADR-M4-003 — Neutral profile to kernel boundary (M4)
Status: Accepted

The kernel interface gains profile extrusion without leaking OCCT (spec 11).

- **One call: `extrudeProfile(const PlanarProfileDefinition&, double
  distanceMm) -> ShapeResult`**, not the `createPlanarFace` + `extrude(face)`
  split spec 11 offers as an alternative. A `KernelFace` would be a second
  runtime handle type with its own validity, ownership and staleness story,
  and M4 has no consumer for a bare face. M3's most expensive defects were all
  about a second copy of runtime state disagreeing with the first
  (ADR-M3-006/007); adding one speculatively, for a capability nothing uses
  yet, repeats that on purpose. The face stays internal to `Kernel/Occt`. If
  M5+ needs faces (UpToFace, shells), the interface can gain the split then,
  when there is a caller to define its semantics.
- `PlanarProfileDefinition` is pure data, zero OCCT: the sketch frame plus an
  ordered, oriented list of neutral curve segments in `(u,v)` mm —
  `ProfileSegment` covering line (start/end), arc (center, radius, start/end
  angle, CCW flag) and full circle (center, radius). Angles in radians
  (spec 7).
- The kernel receives an already-validated profile. Validation is Core's job
  (ADR-M4-005) and happens before any kernel call, so `Kernel/Occt` never
  decides what a valid loop is — it only builds what it is given, and reports a
  structured failure if OCCT still refuses.
- Reuses M3's `ShapeResult`/`KernelShape` ownership unchanged, so `PadFeature`
  gets the same transactional retention `BoxFeature` has for free.

## ADR-M4-004 — Topological naming deferral and rules (M4)
Status: Accepted

Persistent subshape naming is out of scope (spec 14), and M4 must make it
impossible to accidentally depend on it.

- **Forbidden as persistent or semantic identity**, without exception:
  `TopoDS_Shape`/`Edge`/`Face`/`Wire` and any OCCT handle, a `TopExp_Explorer`
  visit order or index, a `std::vector` index, a pointer or address, and
  string names of the form `"Face1"`/`"Edge7"`.
- **The complete set of things M4 persists as identity**: `Sketch` `ObjectId`,
  `SketchEntityId`, `PadFeature` `ObjectId`, and the `ObjectId`s of referenced
  Parameters and Material. Nothing else. If a future feature needs to name a
  face, that needs a real naming scheme, designed as its own decision.
- The rule holds *semantically*, not only at the file boundary: a runtime
  structure that maps a vector index to meaning is the same defect one step
  earlier, so profile loops carry `SketchEntityId`, never positions.
- Enforcement is a test, not a convention: the serialization suite asserts the
  written document contains no OCCT type name and no index-shaped reference,
  alongside the existing `src/Core` static scan.

## ADR-M4-005 — Profile connectivity and tolerance policy (M4)
Status: Accepted

A Profile is a semantic interpretation of Sketch entities (spec 8) and must be
deterministic and independent of storage order (spec 9).

- **Connectivity tolerance `kProfileConnectivityToleranceMm = 1e-6` mm**
  (1 nanometre), matching M3's `kLengthAbsTol` and `kMinBoxDimensionMm` so the
  project has one length-scale story rather than three. Two endpoints closer
  than this are the same point; further apart, they are not. **Gaps are never
  healed** — a gap just outside tolerance is rejected with a diagnostic naming
  the entities and the measured distance, not quietly closed. Silently healing
  is how a user's real modelling error becomes a wrong solid.
- **Deterministic traversal**: start from the entity with the lowest
  `SketchEntityId` (a stable, storage-order-independent choice), then repeatedly
  follow whichever entity has an unused endpoint within tolerance of the current
  loop end. Each step orients the entity — `OrientedSketchEntityRef
  {SketchEntityId, bool reversed}` — so the loop reads start-to-end regardless
  of how entities were drawn.
- **Ambiguity is rejected, never guessed**: if any point has more than two
  incident endpoints the profile is a branch/T-junction and fails; if entities
  remain unvisited after the loop closes, it is disconnected and fails. Both
  produce a structured error identifying the offending entities.
- A full Circle is a valid one-entity closed loop, handled as its own case
  rather than forced through endpoint matching (it has no endpoints).
- **Self-intersection is rejected** (spec 10's recommended policy) rather than
  resolved into faces. Guessing which region a self-intersecting outline means
  is a modelling decision the user has not made.
- **Profile is a computed value, not a stored node.** Validation is a pure
  function from Sketch entities to a `ValidatedProfile`, run inside
  `PadFeature::recompute`, and nothing caches it between passes. A cached
  Profile node would need its own currency flag kept coherent with the graph —
  precisely the failure mode of ADR-M3-006/007. Recompute economy is preserved
  where it is actually required (spec 12): a Pad-length-only edit does not
  dirty the Sketch node, so no Sketch semantic geometry is rebuilt; re-running a
  pure validation over a handful of entities is not the cost the requirement is
  about.
- Consequently the M4 graph is exactly:
  `Sketch -> PadFeature -> MassPropertiesNode`, `PadLength -> PadFeature`,
  `Material -> MassPropertiesNode`. `Sketch` is a dirty-source node like
  `Parameter`/`Material` (ADR-011's pattern, reused unchanged), not an
  `IRecomputable`.

## ADR-M4-006 — Viewer/Core boundary (M4)
Status: Accepted

- **A new `ParametricCADViewer` target** links Qt 6 and `ParametricCADKernelOcct`
  (for OCCT `AIS`/`V3d`, whose toolkits the existing vcpkg OCCT install already
  provides). `ParametricCADCore` links neither Qt nor OCCT and gains no new
  dependency — the M3 boundary is extended, not relaxed, and the same
  binary-level check (`dumpbin` on the Core-only test executable) still applies.
- **The viewer never owns semantic objects.** It holds a non-owning
  `PartDocument*` and, per displayed solid, an `ObjectId`. Display objects
  (`AIS_Shape`) are transient presentation state, rebuilt on refresh and owned
  solely by the viewer.
- **Selection maps presentation to identity, one way**: a viewer-local
  `AIS_InteractiveObject* -> ObjectId` map, rebuilt whenever the display is
  rebuilt, so a stale display object can never resolve to a document object.
  Whole-object selection only; persistent face/edge selection is out of scope
  (ADR-M4-004).
- **Mutation flows one way**: the viewer calls `PartDocument`'s facade
  (`setParameterValue`, `recompute`) and re-reads results. It never writes
  document state directly, and it is never on the recompute path — refresh is a
  consequence of recompute, never a participant in it.
- Qt is a hard dependency of the viewer target only. The build stays usable
  without Qt: absent Qt, the viewer target is skipped and Core, Kernel and all
  non-viewer tests configure and build normally — the same conditional pattern
  `PARAMCAD_BUILD_KERNEL_OCCT` already uses for OCCT.

## ADR-M4-007 — Roadmap/spec conflict for M4 (M4)
Status: Accepted

Recorded rather than silently resolved, per AGENTS.md hard rule 8.

`docs/Roadmap.md`'s M4 entry (written at M0) describes M4 as "Qt viewer" with a
Qt Widgets shell, **feature tree**, **property panel**, an OCC-based 3D view and
parameter-edit-triggers-recompute, with the exit criterion "edit Width 100 ->
120 and see 3D update".

`docs/M4_Implementation_SelfValidation_and_Evaluation.md` describes a
substantially different milestone: a Sketch/Profile foundation and Pad/Extrude
feature — a much deeper CAD modelling scope — with only a *basic* viewer
(display, rotate, pan, zoom, fit, whole-object select, refresh) and no feature
tree or property panel at all. Its exit criteria are release gates A-E.

**The milestone spec governs**, consistent with how M1-M3 were run (the spec is
the authoritative contract when present; the Roadmap is a planning sketch). The
Roadmap M4 entry is rewritten to match the spec, and the deferred UI items
(feature tree, property panel) move to M5+ rather than being dropped silently.

Recorded because it matters for how this log is read: the first revision of this
ADR stated the rewrite in the present tense while the Roadmap was still
untouched, and the self-validation report repeated the claim. Independent review
caught it. An ADR describes a decision; it is not evidence the decision was
carried out, and anything asserting a file was changed has to be checked against
the file.
Noting the conflict matters because the Roadmap's exit criterion mentions
`Width`, a BoxFeature parameter — a reader following the Roadmap alone would
build the wrong milestone.

## ADR-M4-008 — Sketch mutation goes through the document (M4, post-review)
Status: Accepted

Raised as Major finding 1. `addSketch`/`findSketch`/`sketches()` handed out a
non-const `Sketch&` with a full mutating API and no dirty bridge, so editing a
sketch's geometry and calling `recompute()` reported success while the Pad kept
its old solid. M2 had already removed exactly this hazard for Parameters by
making `parameters()` const-only (ADR-011); the rule was written in
`PartDocument.h` but not enforced for sketches.

- `findSketch` and `sketches()` are const-only. `PartDocument::editSketch(id,
  callback)` is the mutation path: it applies the edit and then dirties the
  sketch node, so "edited" and "dirtied" cannot be separated. A callback rather
  than a mutable reference is the whole point -- a reference can outlive the
  call and be used later without the graph ever hearing about it.
- `addSketch`/`restoreSketch` still return `Sketch&` for construction-time
  population, mirroring `addParameter`. The residual (a caller retaining that
  reference and mutating later) is identical for Parameter and Sketch, is
  pre-existing, and is deliberately not changed here: closing it means reworking
  the M1 creation contract for every object type, which is a decision of its own
  rather than a fix smuggled into a sketch milestone.

## ADR-M4-009 — Removal completeness, second pass (M4, post-review)
Status: Accepted

Raised as Major finding 2 -- and the third instance of the same defect.
ADR-M3-008 fixed `removeObject` for Body-owned Features; M4 added two more owned
types and did not extend it. `removeObject(sketchId)` returned true, unregistered
and removed the graph node, and left the Sketch in `sketches_`: still resolvable,
still serialized, fully resurrected on reload, and now impossible to dirty
because its node was gone.

- `removeObject` now has an owner step for `Sketch*` and `Material*`. The sketch
  is dirtied BEFORE removal so dependent Pads fail loudly on their next
  recompute rather than continuing to report a solid built from geometry that no
  longer exists; removing the material also detaches it from
  `massPropertiesNode_` and drops the derived result's currency.
- **The recurring rule, stated once so it stops recurring**: every alternative of
  `ObjectRegistry::ObjectRef` needs a corresponding owner step in
  `removeObject`, and adding an alternative without one is the defect. This has
  now been the finding in two consecutive milestones; the next type added is
  expected to arrive with its owner step and a removal test in the same change.

**Amended after re-review — the rule above was not sufficient.** Stating it in
terms of OWNERS missed the other half: removing an object must also reach
everything that REFERS to it. The first version of this change cleared
`material_` and left every `BoxFeature`/`PadFeature` holding the removed id, so
the document saved cleanly and its own loader rejected it forever. That is the
same data-loss shape ADR-M4-010 exists to prevent, introduced by the change that
cited it.

- **Removal has two halves: the owner step and the referrer step.** A type that
  can be referenced needs a way to be found without naming its referrers'
  concrete types -- `IMaterialReferencing` is that mechanism for Material
  (`materialId()`, `clearMaterialReference()`, `setMaterialReference()`), and it
  is what makes the check survive M5 adding another feature type.
- **A removal test that never round-trips proves half the rule.** The test
  written for the owner step asserted the owner was gone and stopped there,
  which is precisely why 297 passing tests missed the referrer half. Removal
  tests now save and load.

## ADR-M4-010 — Save/load symmetry applies per feature type (M4, post-review)
Status: Accepted

Raised as Major finding 3, and the same rule failing the same way one type
later. `validateSaveable` checked `BoxFeature`'s parameter references
(ADR-M3-008) but not `PadFeature`'s, so removing a Pad's Length Parameter
produced a file that saved cleanly and its own loader then rejected -- the exact
data-loss scenario the rule exists to prevent, potentially over the last good
copy.

- `validateSaveable` now checks a Pad's Length Parameter and its Sketch, using
  the same referential rules the loader enforces.
- General rule: any validation the load path performs is an invariant of the
  FORMAT, and every feature type that carries references owes the save path a
  matching check. A per-type check list is the wrong shape long-term.

**Amended after re-review.** The "M5+ should" above was already overdue: the very
next finding was this same rule failing again, for Material rather than Sketch or
Parameter. The capability approach is now built rather than planned --
`validateSaveable` asks features for `IMaterialReferencing` instead of
enumerating `BoxFeature` and `PadFeature`, so a new referencing feature type is
checked without anyone remembering to extend a list. Parameter and Sketch
references are still checked per type; converting them to the same shape is the
remaining half, and belongs with M5's constraint references rather than as a
late change here.

## ADR-M4-011 — Geometric predicates must be orientation-independent (M4, post-review)
Status: Accepted

Raised as the Critical finding, and the most instructive defect of the
milestone. The self-intersection check's collinear-overlap branch used a
bounding box that shrank its x bounds by the tolerance while GROWING its y
bounds. For any segment narrower than the tolerance in x -- every vertical
segment -- the test could never fire. An outline whose vertical members
overlapped was accepted as a valid profile, OCCT built a degenerate face from
it, and the document reported `success`, `Valid`, and a volume of **0 mm³** for
a region of real area 100 mm²: silent wrong geometry, spec §27's own Critical
example.

- Replaced with a parametric test: project the point onto the segment, compare
  the normalized perpendicular distance against the tolerance, and require the
  parameter to lie strictly inside (0, 1). This is axis-independent by
  construction.
- The normalization matters independently: the raw cross product scales with
  segment length, so comparing it against a length tolerance made the effective
  perpendicular tolerance depend on how long the segment happened to be.
- **The rule this milestone earns**: a geometric predicate whose result depends
  on the orientation of its input is wrong even where it happens to give the
  right answer. The same figure rotated 90 degrees WAS correctly rejected, which
  is why 284 passing tests missed it -- the suite tested the predicate, not its
  invariance. Predicates now get an explicit invariance test
  (`CRITICAL1_DetectionIsOrientationIndependent`), and axis-aligned geometry is
  the first case to try, not the last.

## ADR-M4-012 — Running the program is its own check (M4, post-review)
Status: Accepted

Four user-facing defects survived two complete review rounds while 302 tests
passed, for one reason: nothing ever started the executable.

- the Qt platform plugin was never deployed, so the viewer **could not launch at
  all** -- it built, linked, and aborted on startup;
- `WA_NativeWindow` was missing, so OCCT received the top-level window handle and
  painted over the toolbar, property panel and status bar;
- `AIS_Shape` displayed in its default wireframe, so a solid appeared as edges;
- `resizeEvent` updated the view's size without redrawing, leaving stale pixels
  and undrawn regions after any resize.

None of these is reachable from a unit test, and a successful build says nothing
about any of them. The milestone's own self-validation report had honestly
recorded the viewer as "builds and links" with interactive behaviour UNVERIFIED
-- which was true, and which is exactly the point: **an item marked unverified is
not evidence of anything, and treating it as probably-fine is how four defects
reached two reviewers.**

- `ParametricCADViewer --selftest` builds the real window, lets it lay out and
  paint, then asserts the things a user notices first: the window is visible and
  properly sized, exactly one solid is displayable, mass properties are current
  and match the analytical oracle, and selection round-trips by `ObjectId`. It
  exits non-zero on any failure.
- Registered as the CTest case `ViewerSmokeTest`, so it runs with everything
  else. Verified to actually catch the original defect: with `platforms/`
  removed the run exits 139 (crash); restored, it exits 0.
- **General rule**: any executable a user launches gets a smoke check that
  launches it. The check does not need to be thorough -- three of these four
  defects would have been caught by "does the window appear", which is the
  cheapest assertion in the suite.

## ADR-M4-013 — Visibility is view state (M4 UI)
Status: Accepted

Show/Hide was missing entirely and was the automatic REQUEST CHANGES trigger in
the first UI review ("required viewer/property workflow cannot be completed").

- **Hidden lives in `DocumentPresenter`, never in `PartDocument`.** UI spec 11
  separates Hidden from Suppressed for exactly this reason: hiding changes what
  is DRAWN, while suppression changes what is COMPUTED. A hidden solid is still
  recomputed, still contributes its mass, and still serializes; nothing about
  the document differs. Putting visibility in the document would have made a
  presentation preference part of the saved model.
- `DocumentOutline::build()` takes the hidden id set as a parameter rather than
  reaching for a presenter, so the outline stays free of any notion of a viewer
  and remains unit-testable.
- **Hidden never masks Failed.** The tree reports Hidden only when the object is
  otherwise Valid; a hidden object that fails still reports Failed. Hiding
  something must not conceal that it is broken (`M4_VIEW_012`).

## ADR-M4-014 — Shortcuts are application-scoped because the 3D view is native (M4 UI)
Status: Accepted

Every shortcut the menus advertised silently did nothing whenever the 3D view
held focus -- which, in a CAD application, is most of the time.

The cause is structural rather than incidental: OCCT needs to own its drawing
surface, so `OcctViewWidget` sets `WA_NativeWindow` (itself a fix from earlier in
this milestone -- without it OCCT painted over every Qt control). A native child
window changes how key events reach Qt, and Qt's default `WindowShortcut`
context did not deliver them.

- All menu/toolbar actions use `Qt::ApplicationShortcut`.
- Verified by driving the application with the viewport deliberately focused:
  Ctrl+H toggled the solid (viewport coverage 69.4% -> 0% -> 69.4%),
  Ctrl+Shift+F refitted a wrecked camera, Ctrl+R recomputed without disturbing
  the selection. All three had done nothing before the change.
- **How it was found, and the rule it earns**: the Show/Hide command was tested
  first with Ctrl+H, which appeared to prove the command broken. Clicking the
  toolbar button worked, which separated "the command is broken" from "the key
  never arrived". A UI check that exercises only one input path cannot tell
  those apart -- so a command reachable by both a button and a shortcut is
  verified through both, or it is verified through neither.

## ADR-M4-015 — Mouse input crosses a logical/device pixel boundary (M4 UI)
Status: Accepted

Reported by the user from ordinary use: clicking beside the solid selected it
anyway.

Qt reports mouse positions in LOGICAL pixels; OCCT's view is sized in DEVICE
pixels. `OcctViewWidget` passed Qt's coordinates straight to
`AIS_InteractiveContext::MoveTo`, `V3d_View::StartRotation`, `Rotation` and
`Pan`. On a display scaled at 200% the two differ by a factor of two, so every
hit test, rotation pivot and pan delta was applied at half the true distance
from the top-left corner -- the pick happened up and to the left of the cursor,
and picked whatever was there.

- `OcctViewWidget::toDevicePixels()` is the single conversion site; every mouse
  handler goes through it, so no call site can forget the multiplication. Same
  discipline as ADR-M3-002 for units and ADR-M4-002 for sketch frames: a second
  conversion path is how two coordinate systems silently disagree.
- Verified on the 200%-scaled display after the fix: a click in empty space
  above the solid gives `No selection` with an empty property panel; a click on
  the solid gives `Selected: Pad001 (Pad)`.

**Why it survived every check.** The defect is invisible at 100% scaling, where
logical and device pixels are equal. Every screenshot I captured, and every
interaction the independent UI reviewer drove, ran on the 1920x1080 secondary
display at 100%. The primary display -- 2560x1600 at 200% -- is where the user
actually works, and one click there was enough.

- **The rule this earns**: a UI verified on one display configuration has been
  verified on one display configuration. Scaling, not resolution, is the axis
  that changes behaviour, and it must be varied deliberately rather than
  inherited from whichever monitor the automation happened to target.
- Corollary already visible: `setMinimumSize(1000, 640)` is 2000x1280 DEVICE
  pixels at 200%. It fits 2560x1600, but the margin is small, and a minimum
  expressed in logical pixels has a physical consequence that has to be checked
  on the scaled display rather than reasoned about.

## ADR-M4-016 — UI validation basis: owner manual validation in place of independent UI review (M4)
Status: Accepted

Two project documents prescribe different ways to validate M4's UI, and they
disagree. Recorded here rather than silently resolved (AGENTS.md hard rule 8).

- `docs/M4_UI_Design_and_Validation.md` §26/§28 require an **independent UI
  reviewer** scoring >= 80 with no unresolved Major before `M4 UI = READY`.
- `docs/M5_UI_User_Assisted_Validation_Guide.md` §4/§18 define a different
  workflow — the agent builds deterministic samples and instructions, the
  **project owner operates the application** and reports observations — and
  gives the exact wording for recording it, including
  `Independent UI agent review: not required / not performed according to
  project workflow`.

**Resolution: the owner's manual validation is M4's UI validation basis.** The
project owner directed this explicitly: an independent UI review was run once
(REQUEST CHANGES 79/100), its four Majors were fixed, and the owner then chose
to stop reviewing and validate directly instead. That is a workflow decision the
owner is entitled to make, and the M5 guide sanctions recording it.

What this does and does not claim:

- **Claimed**: the owner exercised seven of eight validation groups and all
  passed, including every Critical condition UI spec §24 defines, the
  failure/recovery input path, and picking accuracy at 200% display scaling.
- **Claimed**: the four independent-review Majors concerned behaviours the owner
  has now confirmed correct by direct observation.
- **NOT claimed**: that an independent reviewer verified the fixes. It did not
  happen, and no document in this project says it did (UI spec §29, guide §18).

**Why this is defensible rather than a downgrade.** Owner validation is weaker
than independent review in one specific way — the person validating knows what
the software is supposed to do, so they are less likely to be confused by it and
less likely to try what a stranger would. It is *stronger* in another: the owner
uses the real machine. The single Critical-class defect of this milestone
(ADR-M4-015, picking offset at 200% scaling) was found by the owner in ordinary
use and was structurally unreachable by both the agent and the independent
reviewer, because every automated interaction ran on the 100%-scaled secondary
display. Neither form of validation dominates the other; this milestone had
both, in sequence.

**Forward rule**: when a later milestone's guide supersedes an earlier
milestone's spec, the completion report states which document governed and why.
Recording "PASS" without naming the basis is what makes a validation claim
unfalsifiable.

## ADR-M5-001 — Constraint identity and reference model (M5)
Status: Accepted

Constraints need identities as durable as the entities they reference, and
references that survive insertion, deletion and reordering of anything else
(spec 5).

- **`SketchConstraintId` is a distinct type allocated from the existing
  `ObjectIdGenerator`**, exactly as `SketchEntityId` is (ADR-M4-001). Distinct so
  the compiler rejects passing an entity id where a constraint id belongs;
  drawn from the shared generator so it inherits M1's restore-collision safety
  (`AdvancePast` on restore, the `kMaxObjectId` cap, the no-wrap guarantee)
  rather than a private counter that would reintroduce that bug class.
- Full identity of a constraint is the pair (`Sketch::id()`,
  `SketchConstraintId`). Constraints are sub-objects of a Sketch, like entities,
  and are not registered in `ObjectRegistry`.
- **Targets are `SketchElementRef`**, the type M4 reserved and left unused for
  exactly this purpose: `{SketchEntityId, SketchSubElement}` with
  `Whole/StartPoint/EndPoint/CenterPoint`. M5 is where those sub-elements
  acquire meaning. Nothing else changes about the type, which is the point of
  having reserved it.
- **Storage is a vector, position is never identity.** Every lookup is by
  `SketchConstraintId`; removal renumbers nothing. Constraint ORDER must not
  affect the solved result either -- order independence is a tested property
  (spec 19), not an assumption.
- **Never persisted as identity**: solver variable indices, Jacobian row or
  column numbers, backend handles, pointers, vector positions. A solver variable
  index is the M5-shaped version of the topology-index mistake ADR-M4-004
  forbids, and it is easy to make because the solver genuinely does number
  things internally -- those numbers are rebuilt on every solve and are never
  written down.

## ADR-M5-002 — Dimensional constraint units and Parameter binding (M5)
Status: Accepted

- **Dimensional constraints bind to existing `Parameter` objects by `ObjectId`.**
  No second scalar system (spec 7). A `Length` constraint stores the parameter's
  id, reads its value at solve time through the registry, and participates in the
  dependency graph as an ordinary prerequisite -- which is what makes
  `Width 100 -> 120` propagate through the existing M2 machinery rather than
  through anything new.
- **Units**: lengths, distances, radii and diameters in **mm**, matching
  `SketchTypes`' `(u,v)` convention and ADR-M3-002's project-wide length story.
  **Angles in radians internally**; the UI may display degrees, and the
  conversion lives at the display boundary only.
- **A dimensional constraint whose bound Parameter carries an incompatible
  `UnitType` is invalid input**, not a silent reinterpretation. A `Length`
  constraint bound to a `Kilogram` parameter fails validation; spec 32 lists
  "unit mismatch changes physical geometry" as a Critical example, and the way
  to not have that defect is to refuse the binding.
- **Value policy**: lengths, distances and radii must be finite and strictly
  positive -- `kMinSketchDimensionMm = 1e-6`, the same floor ADR-M3-009 and
  ADR-M4-005 use, so the project keeps one length-scale story. Zero is invalid
  for these (a zero-length line has no direction; a zero-radius circle is a
  point). Angles must be finite; any value is geometrically meaningful.
- `Diameter` is **not separate state**: it drives the same underlying radius,
  as `radius = diameter / 2`, so a Radius and a Diameter constraint on the same
  circle are two views of one quantity and conflict if they disagree (spec 11).

## ADR-M5-003 — Sketch solver selection and boundary (M5)
Status: Accepted

**Backend: a project-owned Gauss-Newton / Levenberg-Marquardt solver, with
Eigen 5.0.1 (MPL2, header-only, via vcpkg) used only for dense linear algebra
and rank determination.**

Chosen over PlaneGCS (FreeCAD, LGPL) and SolveSpace (GPL). Rationale, recorded
because the alternatives are reasonable:

- M5's systems are small -- a constrained rectangle is 8 variables and ~10
  residuals -- so a general-purpose geometric constraint library is not
  load-bearing here. Its value would be the advanced constraints (tangent,
  equal, symmetry) that spec 3 explicitly excludes from M5.
- Vendoring a third-party solver's source is a substantial, permanent import
  with a viral-or-weak-copyleft licence attached, made for capability M5 does
  not use.
- **Eigen earns its place on one specific requirement.** Spec 10 forbids faking
  DOF as `variables - constraints` and demands rank. Computing the numerical
  rank of a constraint Jacobian requires a rank-revealing decomposition;
  `ColPivHouseholderQR` with an explicit threshold is exactly that, and it is
  the part of this milestone where a hand-rolled implementation would produce
  subtle wrong answers that survive review -- a failure mode this project has
  already demonstrated twice.
- Eigen is header-only and MPL2, strictly less encumbering than the OCCT (LGPL)
  and Qt (LGPL) dependencies the project already carries, and it enters through
  the same vcpkg toolchain path.

**Numerical characteristics** (spec 12, all documented and none to be enlarged
merely to make a test pass):

```
residual tolerance (length)     1e-9 mm
residual tolerance (angle)      1e-9 rad
convergence: max |residual|     < 1e-9
step tolerance                  1e-12
iteration limit                 100
LM damping: initial 1e-6, x10 on rejection, /10 on acceptance
rank threshold                  1e-9 relative to the largest pivot
scale assumption                CAD-scale sketches, 1e-6 .. 1e6 mm
```

**Replacement boundary.** `ISketchSolver::solve(const SketchSolveProblem&) ->
SketchSolveResult` is the only surface. Both types are Core-side, free of Eigen
and of any backend type: the problem carries variables, residual definitions and
parameter values; the result carries solved variable values, a status and a DOF.
Eigen appears in exactly one translation unit. Swapping in PlaneGCS later means
writing a second `ISketchSolver` and changing nothing else -- the same shape as
`IGeometryKernel` and its OCCT implementation (ADR-M3-001).

## ADR-M5-004 — Solver commit, failure and recovery policy (M5)
Status: Accepted

Solving is transactional in the same sense as M3's geometry build
(ADR-M3-001/004), and for the same reason.

- Order: validate semantic references and parameters -> build a temporary
  `SketchSolveProblem` -> solve -> check every output is finite and the status
  is acceptable -> compute DOF -> **only then** write solved coordinates back
  into the sketch's entities.
- **A failed solve writes nothing.** The sketch keeps its previous geometry
  byte-for-byte; no partial, NaN or infinite coordinate is ever committed. Spec
  32 lists "NaN/Inf committed" and "conflict silently produces wrong solid"
  among its Critical examples, and not writing on failure is what makes both
  unreachable rather than defended against.
- **Retention and currency are separate** (ADR-M3-006, which this milestone
  inherits rather than re-derives): the retained geometry stays, and the
  sketch's solve status plus the graph's `ComputeState` carry the fact that it
  is stale. Downstream `Profile`/`Pad`/`MassProperties` must not report a
  current success after a failed solve.
- **Recovery is deterministic**: correcting the input and recomputing follows
  ADR-007's barrier-clear semantics unchanged. Re-solving the same problem twice
  yields the same answer; solving from the retained geometry after a failure
  yields the same answer as solving from scratch. Both are tested (spec 30's
  "repeat solve 100x for drift").
- **Under-constrained geometry is committed** when the solve otherwise
  succeeded. Spec 16 recommends this: valid geometry with remaining DOF is a
  normal editing state, not a failure, and blocking it would make a sketch
  unusable until fully constrained. The status and DOF carry the caveat.

## ADR-M5-005 — DOF and constraint status semantics (M5)
Status: Accepted

- **DOF is computed as `variables - rank(Jacobian)`**, with the rank taken
  numerically at the solved configuration via `ColPivHouseholderQR` and an
  explicit threshold. Not `variables - constraint_count`: that formula reports a
  fully constrained rectangle with one redundant-but-consistent constraint as
  having negative DOF, and reports a sketch constrained twice in the same
  direction as fully constrained when it is not. Spec 10 names this specifically.
- Statuses, with the mapping stated so it can be checked:

| Status | Meaning |
|---|---|
| `Solved` | converged, all residuals within tolerance, DOF = 0 |
| `UnderConstrained` | converged, residuals within tolerance, DOF > 0 |
| `OverConstrained` | rank < number of constraint rows, and the redundant rows are CONSISTENT (residuals converge) |
| `Conflicting` | residuals do not converge and the Jacobian is rank-deficient in the direction of the violation |
| `InvalidInput` | a reference does not resolve, a parameter is missing, or a dimension is non-finite or non-positive -- detected before any solve |
| `NumericalFailure` | iteration limit reached, or a non-finite value appeared during iteration |

- **Conservative mapping where the distinction is not reliable.** Separating
  `OverConstrained` from `Conflicting` depends on whether inconsistent redundant
  rows can be told from merely redundant ones, which is a tolerance judgement.
  The rule: if residuals converge, redundancy is reported as `OverConstrained`
  (benign); if they do not, it is `Conflicting` (an error). Borderline cases
  therefore resolve toward `Conflicting`, because reporting a real conflict as
  benign is the more damaging error.
- **Redundant-but-consistent constraints are accepted, not rejected**, and
  reported as `OverConstrained` with the geometry committed. Rejecting them
  would make ordinary modelling (e.g. Horizontal on both a line and its
  already-horizontal neighbour) fail for no user-visible reason. Tests lock this
  behaviour (spec 16).
- Diagnostics name the offending `SketchConstraintId`s, not just a status --
  spec 25's Gate E requires a "useful constraint diagnostic", and a status alone
  does not tell a user which constraint to remove.

## ADR-M5-006 — Angle constraint convention (M5)
Status: Accepted

- `Angle(lineA, lineB, parameter)` constrains the angle **from lineA to lineB**,
  measured **counter-clockwise**, in **radians**, normalized to `[0, 2*pi)`.
- Each line's direction is its stored `start -> end`, so reversing how a line
  was drawn reverses its direction and changes the measured angle by `pi`. This
  is deliberate: the alternative -- an undirected angle in `[0, pi)` -- cannot
  express the difference between a 60-degree corner and a 120-degree corner,
  which is a distinction a CAD user makes constantly.
- **Degenerate cases are `InvalidInput`, not silently solved**: an angle
  constraint on a zero-length line has no defined direction. Lines shorter than
  `kMinSketchDimensionMm` are rejected at validation.
- **Angles near 0 and near pi are the numerically delicate cases** and are
  tested explicitly (spec 30).

### SUPERSEDED clause, and why (M5 independent review)

This ADR originally specified the residual as `sin(theta_actual -
theta_target)`, "so that it is smooth across the wrap point". **That clause
contradicted this same ADR.** `sin` has period `pi`, so it is satisfied by
`theta` and by `theta + pi` alike -- it cannot distinguish a 60-degree corner
from a 120-degree one, which is the exact distinction the paragraph above gives
as the reason for a directed angle. The ADR asked for two incompatible things
and the implementation followed the wrong one.

**The residual is now the WRAPPED angular difference:**

```
d = atan2(dvB, duB) - atan2(dvA, duA) - target,  wrapped into (-pi, pi]
```

Smooth everywhere except exactly a half-turn from the target. In exchange it is
zero exactly at the requested angle (mod `2*pi`) and has derivative 1 there,
which is what makes the Jacobian rank, and therefore the DOF, come out right. A
residual that is smooth but measures the wrong quantity is worse than one with a
single removable discontinuity.

**Correction (M5 re-review).** This paragraph first claimed the half-turn was
"a measure-zero starting configuration the solve converges through from either
side". Measured, it did not converge at all: the solver stalled at **iteration
1**, reported "did not converge within the iteration limit" after refusing to
take a single step, and in one variant falsely reported `Conflicting`. 13 of 576
(target, start) grid configurations landed on it, and two lines drawn exactly
parallel or antiparallel is an everyday result of axis snapping -- not measure
zero in practice.

An ADR that asserts a property the code does not have is the same failure as a
comment that does, and this ADR had already produced one Critical that way.

**Second correction (M5 round-3 review): the first correction was wrong too, and
its fix was a regression.**

The first correction said the half-turn was handled by rotating the starting
guess off the antipode, and that "only the exact point was unreachable". Both
halves were false when measured:

- It was never a point. Before the nudge, offsets of 1e-15, 1e-12, 1e-9 and
  1e-8 rad all failed -- a band.
- The nudge used a fixed 1e-6 rad guard and a fixed 1e-4 rad rotation, while the
  residual's usable angular resolution is `1e-7 * |coordinate| / length` --
  **scale-dependent**. Over a 760-case grid the nudge improved 131
  configurations and **broke 42**, turning correct `Solved` results into false
  "did not converge" failures for short lines far from the sketch origin. It
  produced, from its own fix, the exact symptom it was written to remove.

**The nudge is deleted.** The real defect was never the starting point: wrapping
makes the residual's VALUE jump by `2*pi` at the antipode, but its GRADIENT is
continuous there. Central differences that wrap each evaluation independently
straddle the jump and produce a Jacobian entry of about `2*pi/(2h)` -- enormous
and wrong -- so Levenberg-Marquardt rejects every step and the solve stalls at
iteration 1.

`ComputeJacobian` now **wraps the DIFFERENCE** of the two evaluations for
angular residuals, which removes the discontinuity from the derivative rather
than steering around it.

**Third correction (M5 round-4 review): "needs no tuning constant, is
scale-free" was false, and this is the third clause in this ADR to be corrected
for asserting a property the code does not have.**

The tuning constant did not disappear; it moved from the deleted nudge into the
finite-difference step. That step is `1e-7 * max(1, |x_j|)` -- relative to the
COORDINATE -- while an angle residual's sensitivity is `1/L`, relative to the
LINE LENGTH. Wrapping caps any angular Jacobian entry at `pi/(2h)`, so the
recovered derivative is wrong once `|coordinate| / length` exceeds about
`1.6e7`: measured, 132 of 336 swept Jacobians were wrong by more than 1e-4
relative and 75 of 336 solves returned a **false** `NumericalFailure`.

**Measured envelope, stated instead of a claim:** correct to ~1e-9 relative for
lines down to `1e-3` mm at coordinates up to `1e5` mm, i.e. a coordinate-to-length
ratio up to about `1e7`. Beyond that -- a 1 micrometre line 10 metres from the
sketch origin -- the angular Jacobian degrades. That is outside spec 30's
"reasonable geometry", which is why this is documented rather than engineered
around, and spec 12 requires scale assumptions to be documented rather than
assumed.

`M5_REV3_001` sweeps to ratio `1e6`, one order inside the boundary. Locked by `M5_REV2_001` and by `M5_REV3_001`, which sweeps
140 combinations of line length, distance from origin, and offset from the
antipode -- the space that exposed the nudge.

Wrapping is now `std::remainder`, not a `while` loop: `while (d > pi) d -= 2*pi`
is unbounded, `DimensionValueValid` accepts any finite angle, and an `Angle`
Parameter of `1e300` rad made `recompute()` **never return** (`2*pi` is below
the ULP of `1e300`). `1e9` rad took 21.8 seconds. Locked by `M5_REV3_002`.
- The UI may display degrees; conversion happens at the display boundary only,
  and the persisted and solved values are always radians (ADR-M5-002).

## ADR-M5-007 — Recomputability is read from the static type, not from the registered handle (M5)
Status: Accepted

`ObjectRegistry` stores a `std::variant` of concrete handle types plus an
`IRecomputable*` alternative. Until M5, `findRecomputable` matched **only** the
`IRecomputable*` alternative, so whether an object was recomputable depended on
which alternative the registering call site happened to pick.

Making `Sketch` recomputable exposed that as a trap with no correct answer:

- Register the sketch via `addRecomputableNode` (as `IRecomputable*`), and
  `PadFeature::resolveSketch`, which does `std::get_if<Sketch*>`, silently
  returns `nullptr` -- every Pad loses its profile.
- Register it as `Sketch*`, and `findRecomputable` reports "not recomputable"
  -- the engine never invokes the solver and the sketch silently never solves.

Both failures are silent, and both would have been introduced by an ordinary,
locally reasonable one-line choice.

**Decision**: `findRecomputable` visits the variant and upcasts from whichever
concrete alternative is stored, using `if constexpr (std::is_base_of_v<...>)`.
The registry now answers a question about the object's **type**, which is what
the question actually is. Objects stay registered under their own concrete
alternative, so one handle serves both the recompute engine and every
type-specific lookup, and the choice that had no correct answer no longer exists.

This generalizes ADR-M3-007 (capability over type enumeration) from feature
iteration to registry lookup: ask what an object *can do*, and derive the answer
from the type system rather than from a bookkeeping convention a call site must
remember.

## ADR-M5-008 — Constraint mutation goes through PartDocument (M5)
Status: Accepted

`Sketch::addConstraint` is not the path callers use. `PartDocument::
addSketchConstraint` / `removeSketchConstraint` are, for the same reason
`editSketch` exists (ADR-M4-008): a dimensional constraint binds a `Parameter`,
and that binding is only real once the `Parameter -> Sketch` **graph edge**
exists. A constraint added straight on the sketch compiles, persists, solves
once, and then never re-solves when its parameter changes -- the sketch would
silently freeze at whatever the parameter was when it was first solved.

- `addSketchConstraint` adds the constraint **and** wires the edge, so
  "edit Width, the sketch re-solves" falls out of M2's existing propagation
  rather than needing a mechanism of its own (spec 13).
- `removeSketchConstraint` drops the edge **only when no remaining constraint on
  that sketch still binds the same Parameter**. Two dimensions legitimately
  share one; removing the edge on the first removal would silently stop the
  second from ever updating.
- Both are verified by mutation: deleting the `addDependency` call fails
  `M5_RECT_003` and `M5_CIRCLE_001`; deleting the `removeDependency` call fails
  `M5_FACADE_001`. A selective-recompute test that only asserts "did not
  re-solve" passes trivially when the edge was never wired, so each such test is
  paired with a positive control that asserts the solve *did* happen.

`SolveStatusName` also moved from the solver backend into Core
(`src/Core/Sketch/ISketchSolver.cpp`): the names describe Core's status enum, so
every Core caller can use them whether or not a backend is linked, and a second
backend cannot rename `Conflicting` behind the first one's back.

## ADR-M5-009 — Deletion policy: cascade for entities, refuse for Parameters (M5)
Status: Accepted

Spec 17 requires a deterministic policy and no dangling references. The two
deletion cases get **different** answers, and the asymmetry is the decision:

**Deleting a sketch entity CASCADES** to every constraint referencing it.
A constraint whose geometry is gone has nothing left to constrain. Keeping it
would produce a reference the solver reports as `InvalidInput` on every
subsequent recompute, forever, with no way for a user to reach the offending
constraint -- the geometry they would click to find it no longer exists.

**Deleting a Parameter is REFUSED** while any sketch constraint binds it, and
`PartDocument::constraintsBindingParameter` names the constraints so the refusal
is actionable. A Parameter is a named, shared, document-level object the user
can see and re-point; silently deleting their dimensional constraints as a side
effect of deleting a parameter destroys more than was asked for. The refusal is
checked BEFORE anything is unhooked, so it leaves the document unchanged rather
than half-removed.

Both branches are deterministic and both leave zero dangling references, which
is what spec 17 actually demands -- it does not demand that both use the same
mechanism.

Consequences:
- `Sketch::removeEntityCascading` returns the removed constraints and the
  Parameters they released, so `PartDocument::removeSketchEntity` can drop the
  graph edges the sketch no longer needs. It **re-checks** each released
  Parameter against the surviving constraints rather than treating the released
  list as a removal list: another constraint may still bind the same one.
- `Sketch::removeEntity` cascades too. The lower-level path cannot touch the
  graph, but it must not be able to create a dangling reference either --
  otherwise the invariant would hold only for callers who remembered the facade.
- The save-side validator rejects a constraint with an unresolvable reference
  (ADR-M3-008: a file the loader would reject must never be writable over the
  last good copy). With the policy in place this should never fire; if it does,
  a mutation path bypassed both removal functions, and failing the save is how
  that surfaces.

## ADR-M5-010 — Schema v5: constraints are semantic, edges are re-derived (M5)
Status: Accepted

`kSchemaVersion` is 5. Per sketch, a `constraints` array persists constraint id,
kind name, entity/sub-element references and the bound Parameter's `ObjectId`.

- **Sub-elements are written as names** (`"StartPoint"`), never as the enum's
  underlying integer. An integer changes meaning the day a sub-element is
  inserted into the middle of the enum, and every file already on disk would
  then load as the wrong sub-element with no error at all. This is ADR-M4-004's
  "identity is semantic, never positional" applied to an enum.
- **Nothing derived is persisted**: no solver variable index, residual index,
  Jacobian layout, DOF or solve status. A reloaded sketch re-solves before
  anything reads it. A test asserts each of those names is absent from the file.
- **`Parameter -> Sketch` edges are Option B**: re-derived on load from the
  constraints, never written. Deriving them means a hand-written file gets the
  same graph as a saved one, and removes the possibility of a file whose edge
  list disagrees with its own constraints. Without the re-derivation the
  document loads, solves once, and then silently freezes -- editing the
  Parameter would never dirty the sketch again (locked by `M5_SER_006`,
  verified by mutation).
- **v4 files load unchanged**: the `constraints` array is optional, and its
  absence means a sketch of free geometry, which is exactly what a v4 sketch was.

Three version-pinning tests were updated from 4 to 5, and
`M4_SER_001_SchemaVersionIsFour` was renamed to
`M4_SER_001_SaveWritesTheCurrentSchemaVersion` -- what it checks (that save
writes the *current* version) is worth keeping; the number in its name was not.

The bump also exposed a pre-existing fragile test:
`M2_SER_005_StubEdgesNotPersisted` searched for the stub's id as a **bare
numeric substring**, which matched `"schemaVersion": 5` the moment the version
became 5. It now searches for the id in its persisted form -- a quoted decimal
string -- so a test about stub edges can no longer fail for a reason that has
nothing to do with stub edges.


## ADR-M5-011 - A residual type must be able to express its constraint (M5)
Status: Accepted. Supersedes the four-slot `SolveResidual` of ADR-M5-003.

`SolveResidual` carried four variable slots. A line-to-line angle is a function
of **eight** scalars. Unable to say what the constraint meant, the code said
something else: it packed the two lines' `v` components and evaluated
`sin(dvB - dvA - target)` -- subtracting a millimetre difference from a radian
target. It converged, reported `Solved` with a residual of 4e-11, produced
angles wrong by up to 260 degrees, was not rotation-invariant, and silently
stretched already-correct geometry by about 1 mm.

Three decisions follow:

1. **Eight slots** (`std::array<int, 8> vars`), so the type can express every
   constraint M5 has.
2. **`SlotsRequired(kind)` is part of the interface**, declared next to the enum
   rather than assumed privately by each backend.
3. **The solver REJECTS an under-packed residual** as `InvalidInput`. This is
   the part that matters: a future kind whose slots are not all filled fails
   loudly instead of reading `vars[-1]` and computing plausible nonsense.

**Two limits of that guard, stated rather than left to be discovered.**

*It is arity-only.* A residual whose slots are all filled but MIS-ORDERED --
a `Distance` packed `(a.u, b.u, a.v, b.v)` instead of `(a.u, a.v, b.u, b.v)` --
passes, and then reports `Solved` with a residual of 2.7e-12 while the geometry
is wrong. That is character-for-character the C1 failure mode. The problem
already carries a `SolveVariable::Component` for every variable, so a
cross-check is cheap and is recorded as follow-up. What stands in for it today
is that the geometric tests DO catch it: an injected `Distance`/`Length` slot
swap fails 49 tests, and an `Angle` slot swap fails `M5_ANGLE_E2E_001`.

*It only protects the code that runs after it.* The ADR-M5-014 degeneracy nudge
was first placed ABOVE the guard and indexed `x[]` raw -- so an under-packed
`Distance`, the exact thing the guard refuses, was dereferenced first: an
assertion abort in Debug and an out-of-bounds read AND WRITE in Release, through
an interface whose header promises it never throws. A fix for one finding
re-opened the hole this guard had just closed, and the guard's own test could
not see it because that test uses `Angle`, which the nudge loop skips.
**Everything that indexes `x[]` by slot now sits below the validation loop, and
the code says so.** Locked by `M5_REV2_002`.

**Why it survived review and 444 passing tests.** No test built an
`AngleConstraint` and measured the resulting geometry. The two tests named for
angles worked on bare scalars and finished by recomputing *the solver's own
residual formula*, checking the solver had driven it to zero -- a tautology that
asserts convergence, not an angle. Substituting `startU/endU` for
`startV/endV`, which changes the constraint's meaning entirely, left the whole
suite green.

**The rule this establishes:** a constraint test measures the SOLVED GEOMETRY
with an independent formula (`atan2` over the committed coordinates), never by
re-evaluating the residual under test. The replacement tests fail 5-of-6 against
the original defect; the ones they replaced failed 0-of-2.

## ADR-M5-012 - DOF outranks redundancy, and an unmeasured DOF is not zero (M5)
Status: Accepted. Refines ADR-M5-005.

Two status defects, both found by independent review, both reachable in ordinary
modelling:

- **A redundant constraint masked free degrees.** The solver tested `redundant`
  before it tested DOF, so a sketch still short of fully constrained that
  happened to carry one duplicate reported `OverConstrained` -- telling the user
  there are *too many* constraints on a sketch that needs *more*, and hiding the
  free degrees behind a status implying none remain. **DOF > 0 now wins**:
  redundancy becomes the headline only once there is no freedom left, and the
  message says both when both are true.
- **`degreesOfFreedom` defaulted to 0**, and 0 is this project's signal for
  FULLY CONSTRAINED. A sketch whose *first* solve failed therefore read as
  finished work, and every constraint-free sketch -- i.e. every M4 document --
  read "Under-constrained, DOF 0", which is self-contradictory. There is now
  `kUnknownDegreesOfFreedom = -1`, the UI renders it "not measured", and a
  constraint-free sketch reports its actual free-variable count.

## ADR-M5-013 - One reconciler owns the sketch's Parameter edges (M5)
Status: Accepted. Refines ADR-M5-008.

ADR-M5-008 said `Sketch::addConstraint` "is not the path callers use". Nothing
enforced that: `editSketch` hands out a mutable `Sketch&`, and the shipped
viewer uses exactly that path. Independent review found both holes:

- a constraint added through `editSketch` wired **no** edge, so the document
  behaved differently before and after a save/load -- the loader re-derives
  edges from the constraints, so one appeared from nowhere;
- a cascaded entity removal through the same path left a **phantom** edge, so a
  Parameter kept re-solving a sketch that no longer read it, violating spec 13's
  "unrelated Parameter: none of branch" through a public API.

`PartDocument::reconcileSketchParameterEdges(sketchId)` now makes the graph
match the constraint set exactly, and every facade path calls it:
`addSketchConstraint`, `removeSketchConstraint`, `removeSketchEntity` and
`editSketch`. Per-path edge bookkeeping produced two opposite bugs from one
rule; one reconciler cannot disagree with itself.

**Correction (M5 re-review): there was a FIFTH path, and this ADR said there
were four.** `PartDocument::addSketch` returns a mutable `Sketch&`, so
`Sketch::addConstraint` is reachable without any facade at all -- while
`PartDocument.h` two screens above states the opposite invariant ("Const-only
reads... Editing goes through `editSketch()`"). Through that reference a
dimension edit silently did nothing, and the document behaved differently before
and after a save/load, because the loader re-derives edges and the live document
had none: verbatim the symptom this ADR claims to have fixed.

`reconcileAllSketchParameterEdges()` now runs at the start of every recompute
pass as a **net**, not as the primary mechanism, so the graph agrees with the
constraint set whatever route a caller took. The cost is one walk over the
sketches per pass. Locked by `M5_REV2_012`.

The lesson is narrower than "add a net": an ADR that enumerates "every path"
is a claim about a whole API surface, and it was written from the paths I had
just edited rather than from the ones that exist.

## ADR-M5-014 - A degenerate configuration is nudged, never called contradictory (M5)
Status: Accepted

`sqrt(du^2 + dv^2) - target` has an all-zero central-difference row at
`du = dv = 0`, because the probe evaluates `|+h| - |-h| = 0`. Gauss-Newton then
has no descent direction, the rank test sees deficiency, and a `Distance`
between two coincident points -- a system with an **infinite** solution set --
was reported `Conflicting` with the message "no configuration satisfies them".
A false accusation of contradiction is the damaging kind of wrong answer: no
constraint is in conflict, so the user has nothing to act on. Coincident points
arrive by ordinary means (snapping, duplication, a collapsed edit, an import).

The solver now perturbs its **starting guess** for such a residual. Initial
values are the solver's input, not the model; any direction is as good as any
other for a configuration that constrains none, and stored geometry is untouched.

This also makes true a claim `SketchSolveSession` was already asserting as fact
-- that a `Length` on a zero-length line is solvable because its direction is
merely a free degree of freedom. It was not, until now.

**Extension (M5 round-4 review): the same principle, violated by this ADR's own
sibling fix.** `WrapToPi(atan2B - atan2A - target)` subtracted the RAW target
before wrapping, so once the target was large its ULP exceeded the angular
signal and the residual stopped depending on the geometry at all. At `1e9` rad
the solver reported `Conflicting` -- "constraints are contradictory: no
configuration satisfies them" -- for a system that solves perfectly with the
mathematically identical pre-wrapped target. The round-3 fix for the unbounded
`while` loop converted a hang into a wrong answer rather than a right one, and a
false accusation of contradiction is exactly what this ADR calls the damaging
kind. The target is now wrapped before it is subtracted. Locked by
`M5_REV4_001`.

## ADR-M5-015 - Every test suite uses PRE_TEST discovery (M5)
Status: Accepted

`gtest_discover_tests`' default POST_BUILD mode writes one **config-less**
`<target>_tests.cmake` whose `add_test` lines hard-code an absolute path to
whichever configuration was linked last. Under a multi-config generator,
`ctest -C Release` then silently runs the **Debug** binaries. Independent review
measured 333 of 448 tests doing exactly that, which made this project's "Release
tests pass" claim a second Debug run for those suites -- and ADR-M5-005 puts DOF
on a numerical rank threshold, precisely the kind of thing that can differ
between configurations.

Every suite now uses `DISCOVERY_MODE PRE_TEST`, which emits per-config include
files so `-C` selects the binary it names. Verified from the ctest log: a
Release run invokes `build/Release/...` for all 464 tests.

The earlier note claiming only DLL-carrying targets needed PRE_TEST is corrected
in place: the DLL argument is why the OCCT suite needed it *first*, not why the
others could go without it.

## ADR-M5-016 - Deleting a referenced Sketch keeps the M4 contract (M5)
Status: Accepted, with an open question for the owner.

Independent review recommended REFUSING to delete a Sketch that a `PadFeature`
reads, by analogy with ADR-M5-009's rule for a bound Parameter. That change was
made, broke three accepted M4 tests, and was **reverted**.

M4's own independent review took this exact case (its MAJOR2 and MAJOR3
findings) and settled it the other way: deletion is allowed, the Pad fails
LOUDLY, and `savePartDocument` refuses to write a document with a dangling Pad
reference -- so a broken document can never overwrite a good file, and the user
recovers by deleting the Pad. Three tests encode that contract.

The reviewer's underlying complaint is real: until the Pad is removed, every
save fails, and `PadFeature` exposes no way to re-point its sketch. But that is
the accepted design behaving as designed, not a defect M5 introduced, and
reversing a reviewed milestone decision is the owner's call -- not a side effect
of fixing something else. `M5_REV_008` now pins the M4 contract explicitly,
including the recovery path, so a future change cannot drift away from it
silently.

**Open question for the owner:** should M6 add a way to re-point a Pad at
another Sketch, which would remove the sharp edge without overturning anything?


## ADR-M5-017 - A dimensional constraint must bind a Parameter, and both validators must agree (M5)
Status: Accepted

Rejecting a dimension bound to something that is not a Parameter left the other
half of the same finding open -- a dimension bound to **nothing** -- and that
half was worse:

- `BuildSolveProblem` validated the Parameter only when the id was non-invalid,
  so an unbound `Length` was translated with `target = 0.0` and no complaint.
  The solver was asked to drive a line to zero length and a circle to zero
  radius, values ADR-M5-002 declares invalid.
- `validateSaveable` skipped its check for an invalid id, while the LOADER
  **requires** `parameterId` for all five dimensional kinds. The document
  therefore **saved cleanly and could never be loaded back** -- precisely what
  ADR-M3-008 exists to prevent, reached through the facade that had just been
  hardened for this very finding.

Three changes, because one was not enough:

1. `PartDocument::addSketchConstraint` rejects an unbound dimensional
   constraint.
2. `BuildSolveProblem` rejects it as `InvalidInput` and names it, instead of
   defaulting the target to 0.
3. `validateSaveable` mirrors the loader exactly.

`IsDimensional(data)` answers "does this kind read a Parameter at all", which is
the question all three sites need. `BoundParameterId(data) != kInvalidObjectId`
cannot distinguish "needs none" from "needs one and has none" -- a distinction
three separate call sites got wrong in the same way, which is what a capability
question exists to prevent (ADR-M3-007).

Locked by `M5_REV2_010` and `M5_REV2_011`.

## ADR-M5-018 - Registration failures are checked for every restored type (M5)
Status: Accepted. Completes ADR-M5-007 / the C2 fix.

The C2 fix checked `registerObject`'s result in `restoreSketch` only. A reviewer
removed *parameter* ids from `maxPersistedId` -- C2's exact shape, one type over
-- and reproduced the identical silent symptom: **load reports success**, the id
resolves to the MassPropertiesNode instead of the Parameter, and the feature
downstream is blocked forever with no error anywhere.

Fixing the instance is not fixing the class. `restoreParameter`, `restoreBody`
and `restoreMaterial` now check too, and `loadPartDocument` already converts the
throw into a clean load failure.

**Correction (M5 round-3 review): "every restored type" meant four of six, and
two of the four were checked in the wrong order.**

- `restoreBoxFeature` and `restorePadFeature` discarded `addRecomputableNode`'s
  result. A duplicate feature id threw nothing, left two features sharing one id
  in the same Body, and the document then **saved cleanly and reloaded with
  "duplicate ObjectId"** -- C2's symptom on precisely the types this ADR's title
  claimed to have covered. Both now check.
- `restoreMaterial` assigned `material_` **before** the check, so the throw
  destroyed the previous `Material` (its `shared_ptr` use_count was 1) while the
  registry still held its address -- and the next recompute read the density out
  of freed memory. `restoreParameter` stored the duplicate before throwing, so
  the document saved cleanly and could never be loaded back. **Both now validate
  before mutating any owner state.** Adding a check without adding the rollback
  replaced one silent failure with another.

Locked by `M5_REV3_010`, `M5_REV3_011`, `M5_REV3_012`.

Related, same class: `rewireMassPropertiesSource` re-added the
MassPropertiesNode's **graph node** without re-registering it, producing a node
the engine can schedule but not resolve -- "missing registry object" forever, in
violation of the invariant `ObjectRegistry`'s own header states. It now restores
both together.

**Scope correction to the never-advance-on-failure guarantee.** It holds for
every validation failure, all of which return before `AdvancePast`. It does NOT
hold for the two failures that can occur after it -- a throwing restore, and the
defensive edge re-apply. Both leave no document and ids only move forward, so
nothing is corrupted, but the comment claimed the guarantee was unconditional
and it is not. Stated in place rather than quietly relied upon.


## ADR-M5-019 - A reconciler must add and remove over the same set (M5)
Status: Accepted. Refines ADR-M5-013.

`reconcileSketchParameterEdges` **added** an edge for any bound id but only
**removed** prerequisites that resolve to a Parameter. A dimensional constraint
bound to the Material id -- reachable through `editSketch` -- therefore wired a
`Material -> Sketch` edge the function could never take away: after the
offending constraint was deleted the edge survived two recompute passes, and a
density edit kept re-solving a sketch that read nothing from it.

That is verbatim the phantom-edge defect this reconciler was written to
eliminate (ADR-M5-013's M7), re-created by the reconciler itself.

**Add and remove now range over the same set**: only ids that resolve to a
Parameter are wired at all. A reconciler whose two halves disagree is not a
reconciler.

It also **owns** every `Parameter -> Sketch` edge, which the header now states:
one added by hand through `addDependency` is revoked by the next recompute pass.
That is correct -- such an edge has no constraint behind it and the loader would
not re-derive it, so keeping it would make the document behave differently
before and after a save/load -- but the call succeeds and the edge later
vanishes, which is surprising enough to write down.

Locked by `M5_REV3_014`.

## ADR-M5-020 - Removing a derived node clears the result it derived (M5)
Status: Accepted

`removeObject(massPropertiesNodeId)` unhooked the node but left
`massProperties_.valid == true`, so the status bar went on reporting a stale
volume as **current** through every later edit -- 100 000 mm3 after a change
that made the true value 200 000. `syncFeatureStatesFromGraph` could not correct
it, because it skips a node the graph no longer has.

Removing the only thing that could keep a derived result current must clear that
result. Retention and currency are separate properties (ADR-M3-006), and this
was the case where retaining a value while claiming currency is exactly wrong.

Locked by `M5_REV3_013` -- which, in its first draft, could not fail: it used a
fixture with no solver whose mass had never been valid, so "expect invalid" held
for the wrong reason. A mutation caught that, and the test now establishes a
CURRENT mass before removing the node.

## ADR-M5-021 — The deferred Minors, closed (M5)
Status: Accepted

Six findings had been recorded as "open, not fixed" rather than closed. They are
now closed, each with a mutation-verified test. Recorded together because the
common thread matters more than any one of them: **each was a guard that
defended against a case rather than a class.**

1. **The residual slot guard was arity-only.** A `Distance` packed
   `(a.u, b.u, a.v, b.v)` filled all four slots in range, passed, and reported
   `Solved` with a residual of 2.7e-12 while the geometry was wrong by
   millimetres — character-for-character the C1 failure. Every `SolveVariable`
   already carries its `Component`, so `SlotComponent(kind, slot)` now states
   what each slot must be and the solver checks it. Arity said "enough
   variables"; this says "the right ones". `M5_DEF_001`.

2. **`CommitSolvedGeometry` did not re-validate.** `replaceGeometry` does not
   check either, so a `Coincident` between a line's own endpoints solved to a
   zero-length line and committed it with status `Solved` — geometry `addEntity`
   has always refused. The sketch could hold state its own invariant forbids.
   The commit is now two-phase: build every entity, validate all of them, write
   only if all pass, so a rejection leaves the sketch exactly as it was.
   `M5_DEF_010`.

3. **`UnitType::Unitless` satisfied both the length and the angle check**, so
   one unitless Parameter could drive a `Length` and an `Angle`
   interchangeably — the door ADR-M5-002's unit rule exists to close, left ajar
   by a convenience. A dimension states a physical quantity; "no unit" is not
   one. `M5_DEF_011`.

4. **`OutlineState::Blocked` was never assigned.** The graph stores `Failed`
   for both "this failed" and "a prerequisite failed so it never ran"; the
   engine distinguishes them and the display threw the distinction away. A Pad
   blocked by a conflicting sketch read exactly like a Pad that broke on its
   own, with no diagnostic — pointing the user at the wrong object. Blocked is
   now derived in the outline using the engine's own rule, and names the
   prerequisite that failed. `M5_DEF_012`.

5. **`result.iterations` was not the iteration count.** `iteration =
   kSolveMaxIterations` was used to break out of the loop, so a solve that
   settled in three steps reported 100, and the failure message blamed an
   iteration limit the solver had never reached. The exit reason is now tracked
   separately and the message says which actually happened. `M5_DEF_002`.

6. **`editSketch` bypasses the facade's Parameter-binding validation.** Left
   open deliberately: the solver rejects the result as `InvalidInput` and names
   the constraint, the panel shows it, and the save validator refuses the file.
   Three independent catches downstream make this defence-in-depth rather than a
   hole, and closing it at `Sketch::addConstraint` would require the sketch to
   know about the document's Parameters — the coupling ADR-M5-001 avoided on
   purpose.

**DPI scaling remains NOT EXECUTED at the owner's direction.** A fix was
prototyped — sizing the window from `availableGeometry()` and asserting in
physical pixels — and it did make the selftest fail at 200% where the old
logical-pixel assertion could not. It also revealed that the shell's 1000×640
minimum cannot fit a 200%-scaled 1280×800 desktop at all, which is a layout
change rather than an assertion change. It was reverted whole rather than left
half-applied.


## ADR-M5-022 - A commit answers for what it writes, not for what it found (M5)
Status: Accepted. Corrects ADR-M5-021 item 2.

Validating the solved geometry before committing was right. Validating **every
entity in the sketch** was not.

`Sketch::restoreEntity` deliberately does not validate -- a hand-edited file must
round-trip, and the code says so -- and the loader calls it unguarded. So one
degenerate entity anywhere in a loaded document made the entire sketch
**permanently unsolvable**: every recompute returned `NumericalFailure`, the
diagnostic named no constraint (there was none to name), and `degreesOfFreedom`
kept its previous value of 0, which this project reads as FULLY CONSTRAINED.
A bad entity that M4 tolerated -- the profile rejected it, the sketch still
solved -- became fatal.

`CommitSolvedGeometry` now validates only the entities this solve actually
CHANGED. The rule generalises: a write is answerable for what it writes. Making
it answerable for pre-existing state turns every legacy defect into a new one.

Locked by `M5_REV4_010`, which uses no Pad on purpose -- a stray entity breaks
the PROFILE as well, and asserting document-level success would fail for the
profile's reason and prove nothing about the commit.

## ADR-M5-023 - Two tolerances that meet must not meet on the same number (M5)
Status: Accepted

`kMinSketchDimensionMm` (the smallest ACCEPTED dimension, `value >= floor`) and
`kSketchToleranceMm` (the largest separation still called COINCIDENT,
`<= tolerance`) were both `1e-6`, with inclusive bounds from opposite sides. The
smallest legal dimension was therefore, by definition, degenerate geometry: a
`Length` of exactly `1e-6` was accepted as a dimension and its solved line
rejected as a line. The solver's absolute residual tolerance (`1e-9`) widened the
dead band further, and 41 of 144 swept configurations had a converged solve
refused at commit.

The floor is now `1e-5`, ten times the coincidence tolerance -- 10 nanometres,
far below anything a CAD user models, so nothing real is excluded.

**The relationship is enforced by `static_assert`, not by a test.** Reverting the
constant now fails the BUILD. A behavioural test could not do this reliably: the
first attempt passed under the reverted constant, because whether a solve at the
floor lands above or below the tolerance depends on where it started, and that
test's one configuration was among the 103 that pass. Where a constraint between
two constants can be checked at compile time, checking it at run time on one
sample is the weaker choice.

## ADR-M5-024 - Replacing an owned object unhooks the one it replaces (M5)
Status: Accepted. Completes ADR-M5-018.

ADR-M5-018 made the duplicate-id THROW path safe in `restoreMaterial` by moving
the check above the mutation. It left the line below untouched, and that line is
the SUCCESS path: `material_ = std::move(item)` destroys the previous `Material`
(its `shared_ptr` use_count is 1) while `registry_` still resolves its id to the
freed address and `graph_` still holds its node.

`MassPropertiesNode::resolveMaterial` then reads density through that pointer.
In Debug the value was garbage; **in Release the freed memory still read the old
density, so the document reported a plausible but WRONG mass as CURRENT, with
`RecomputeStatus::Success` and no diagnostic anywhere** -- the worst available
outcome, and the one hardest to notice.

`detachCurrentMaterial()` unhooks the outgoing Material from the registry, the
graph, and the mass-properties source before the assignment destroys it, on both
`addMaterial` and `restoreMaterial`. It runs BEFORE the assignment on purpose:
afterwards it would unregister the id the new object had just been given in the
id-reuse case.

The general rule, which is what ADR-M5-018 should have said: fixing the path a
finding names is not fixing the class. Ask which OTHER path reaches the same
line.

## ADR-M5-025 - A flag the program does not understand is never silently discarded (M5)
Status: Accepted

`--sample` rejection was added twice and was wrong three times: an unknown NAME
(closed in round 2), a MISSING value (closed in round 3), and `--sample=value`
-- the most commonly typed form of the three -- which failed `strcmp` outright,
so the flag was dropped, the M4 rectangle was built, and `SELFTEST OK` printed.
A CI job with an M5 gate silently ran an M4 smoke test that passes.

The parser now accepts both `--flag value` and `--flag=value`, and treats an
unknown name, a missing value and an empty value alike as errors. Five ctest
cases pin it -- four rejections and, deliberately, **one acceptance of the `=`
form with a valid name**, so the parser cannot "pass" by rejecting everything
containing an `=`.

Twice was a coincidence. Three times is a category: an argument the program does
not understand must fail, never default.

---

# M6 — DXF Import to Stable Sketch Entities

## ADR-M6-001 — DXF parser selection: libdxfrw, and what its licence costs (M6)
Status: Accepted by explicit owner decision.

| | |
|---|---|
| Library | **libdxfrw** |
| Version | `2025-09-25` (vcpkg port) |
| Licence | **GPL-2.0-only** |
| Linkage | vcpkg `x64-windows` triplet — dynamic |
| Target | `ParametricCADImportDxf` **only** |
| Crosses into Core? | **No** — see ADR-M6-003 |

**The owner was asked and chose this**, which is what spec 20 requires: "No new
GPL dependency may be introduced into the commercial application without an
explicit owner decision."

**What the alternatives were**, recorded so the decision can be revisited on its
merits rather than re-argued from memory:

| Option | Licence | Why not chosen |
|---|---|---|
| **libdxfrw** | GPL-2.0-only | **Chosen.** Closest fit to the requirement; mature; reads ASCII and binary DXF. |
| `dime` (Coin3D) | BSD-3-Clause | Permissive and would have carried no licence consequence; a larger surface than M6 needs. |
| Own minimal ASCII reader | n/a | Zero dependency and zero licence risk. M6 requires only LINE, CIRCLE and ARC, and DXF group-code parsing for those is small — the same reasoning that produced the project's own solver (ADR-M5-003). |

**The consequence, stated once and not softened.** GPL-2.0-only is a copyleft
licence. Linking it — statically or dynamically — generally makes the combined
work a derivative, which must then be distributed under GPL-2.0-compatible
terms, including corresponding source. If EP3D is ever to be distributed as
closed-source commercial software, this dependency has to be removed or replaced
first. That is a distribution question, not a build question, and nothing in the
code will warn about it.

**What limits the damage, and it is required anyway.** libdxfrw is confined to a
single CMake target that Core never links, and it reaches the document only
through a format-neutral representation (ADR-M6-003). Swapping it for `dime` or
for an own reader is therefore an exercise in replacing one translation unit —
the same replaceability ADR-M5-003 gave the sketch solver, for the same reason.
A binary-level check asserts Core carries no libdxfrw symbol, so the boundary is
measured rather than asserted.

## ADR-M6-002 — DXF unit policy (M6)
Status: **PARTLY SUPERSEDED.** The policy stands; two of its statements do not.

> **Superseded by ADR-M6-010** (timing): this ADR says conversion happens "as
> DXF units enter the format-neutral representation". It now happens ONCE after
> the whole file is read, because libdxfrw dispatches sections in file order and
> the old timing produced 25.4x-wrong geometry while reporting success.
>
> **Corrected by ADR-M6-011** (coverage): this ADR's claim that the mapping
> covers "the remaining ISO units libdxfrw exposes" was false. It covered six
> values; fifteen are now mapped.
>
> These markers were added after a reviewer pointed out that the cross-
> references existed only in the superseding ADRs, so anyone reading this one
> would take its false statements at face value. **That is the third time on
> this project an ADR has been left asserting behaviour the code does not have**
> -- ADR-M5-006 needed three corrections for the same reason.

DXF stores lengths as unitless numbers plus a header variable, `$INSUNITS`,
which says what those numbers mean. It is frequently absent or `0`
("unitless"), so a policy is required rather than a lookup.

- **One conversion boundary.** Conversion happens exactly once, in the importer,
  as DXF units enter the format-neutral representation. Nothing downstream
  converts again, and no caller may guess (spec 8).
- **Internal geometry stays in millimetres**, unchanged from M4/M5.
- **`$INSUNITS` is honoured when present and recognised.** The mapping covers
  the values a real file carries: unitless(0), inches(1), feet(2), millimetres(4),
  centimetres(5), metres(6), and the remaining ISO units libdxfrw exposes.
- **Absent, zero, or unrecognised `$INSUNITS` means MILLIMETRES**, and the
  import diagnostics say so explicitly. Silently assuming a unit and not
  reporting it is how a drawing arrives 25.4x wrong with nothing to point at.
- **The assumption is reported, not hidden**: the import result carries the unit
  actually used and whether it was read from the file or defaulted.

Chosen over "reject unitless files" because a large share of real DXF output is
unitless and rejecting them would make the importer useless for the files it
exists to read; chosen over "ask the user" because M6's UI scope is a file
picker (spec 19), and a policy that depends on a dialog cannot be tested.

## ADR-M6-003 — Import architecture and the Core boundary (M6)
Status: Accepted

```
DXF file -> libdxfrw -> ImportedSketchGeometry (format-neutral) -> importer -> PartDocument API -> Sketch entities
```

- `src/Core` gains **no** DXF type, header or link dependency. The check is the
  same one M5 used for Eigen and M3 used for OCCT: a source scan plus
  `dumpbin` on the Core test binary, which links neither the importer nor
  libdxfrw and therefore cannot pull a symbol from either.
- The format-neutral representation carries **semantic geometry and import
  metadata only**: coordinates already converted to millimetres, radii, angles,
  and a source description. No parser pointer, no `DRW_*` type, no file offset,
  no library handle (spec 6).
- The importer writes through the ordinary `PartDocument` / `Sketch` API, so an
  imported entity is indistinguishable from one created natively — which is the
  property spec 3 requires and Gate G proves by driving a Pad from it.

The layering follows M4's viewer split exactly: the document-facing half is free
of the third-party type and is unit-testable without it; only the adapter names
`libdxfrw`.

## ADR-M6-004 — Imported entity identity (M6)
Status: Accepted

**DXF order is not identity, and a DXF handle is not identity.**

Every imported entity receives an ordinary `SketchEntityId` from the shared
generator, exactly as `Sketch::addLine` produces. After import there is nothing
about the entity that says it came from DXF, and that is the point: the M5
identity, persistence and solving contracts apply unchanged (spec 7).

- Vector position is not identity, file offset is not identity, memory address
  is not identity, DXF handle is not identity.
- A DXF handle **may** be kept as informational metadata, but no correctness
  claim may depend on it. Nothing in M6.1 reads one.
- Save/load preserves ParametricCAD identity. It does **not** re-derive identity
  from DXF ordering, which is what Gate E and the shuffled-order adversarial
  test exist to prove.

Two separate imports of the same file are not required to produce equal ids
(spec 13); identity stability is required across save/load of **one** imported
document.

## ADR-M6-005 — Unsupported entity policy (M6)
Status: Accepted

An entity kind M6 does not import is **reported and skipped**, never
reinterpreted as another kind (spec 4), and never fatal.

- The reader records `ImportSkipReason::UnsupportedEntity` with the kind as the
  file named it, so the diagnostic can say `SPLINE` rather than "something".
- Skipping is **not** silent: the count reaches the import result and the status
  bar. A user who cannot see that something was dropped will assume nothing was.
- **Unsupported** is distinct from **invalid**: `CIRCLE` is supported, and a
  circle with a zero radius is a supported kind carrying values the model
  refuses. Spec 11 wants those told apart because they call for different user
  actions — one means "M6 does not do this yet", the other means "your drawing
  has a degenerate entity".
- A file consisting **only** of skipped entities is a FAILED import with a
  message saying so, distinct from an empty file. Returning an empty sketch
  would make the user discover the emptiness themselves.

The fixtures place unsupported and degenerate entities **between** valid ones,
because a trailing one cannot distinguish "skipped it and carried on" from
"stopped there".

## ADR-M6-006 — Transactional import (M6)
Status: Accepted

Import is transactional at the **sketch** level (spec 10). Any failure removes
the sketch again, leaving no half-built sketch, no orphan registry entry, no
stray graph node and no dangling id.

Returning a partially-populated sketch would be worse than failing: the user
would have to work out which half arrived, and a drawing that is half-imported
looks like a drawing that was half-drawn.

Entities the READER rejected (unsupported kinds, degenerate geometry) arrive as
skip records and do **not** fail the import — the file is legitimate, it simply
contains more than M6 handles. Entities the reader passed but the model refuses
DO fail it: the two layers disagreeing is a fault worth surfacing, not an entity
to drop quietly.

`M6_GATE_H_AFailedImportLeavesNoTraceInTheDocument` checks sketch count,
registry size and graph node count, because "the sketch is gone" is a weaker
claim than "nothing of the attempt remains".

## ADR-M6-007 — ARC orientation and angle convention (M6)
Status: Accepted

- DXF stores arc angles as **degrees**, counter-clockwise from +X, in group
  codes 50 (start) and 51 (end). libdxfrw converts them to radians before the
  reader sees them.
- **That conversion is verified, not trusted.** `M6_ARC_001` measures it and
  fails in either direction. libdxfrw's header comment asserts it; this project
  has already shipped one Critical because two comments described an algorithm
  the code did not implement.
- A DXF arc always sweeps **counter-clockwise from start to end**, so
  350° → 40° is a 50° sweep through zero, not 310° the other way. An importer
  assuming `end > start` gets it backwards, which is why a fixture that crosses
  zero exists — an arc that does not cross it cannot reveal the mistake.
- `SketchArc::counterClockwise` is therefore always `true` for an imported arc.
- **Arcs are tested by measuring their endpoints**, computed from the model's
  stored centre, radius and angles and compared with hand-computed coordinates.
  Re-deriving the importer's own conversion would prove only that it agrees with
  itself — the failure mode that let a broken Angle constraint ship in M5 with a
  green test beside it (spec 16 says the same thing).

## ADR-M6-008 — What M6 does NOT read, and the one that can be silently wrong (M6)
Status: **SUPERSEDED by ADR-M6-013.**

> The assessment below is wrong in both directions and is kept only as a record
> of what was believed. It says the reader "ignores... the extrusion direction
> (group code 210)" and therefore "imports at the wrong orientation --
> silently". Measured: for the common `(0,0,-1)` the correct result is a
> **mirror**, which no analytical oracle in this project could detect; and
> CIRCLE/ARC entities carrying a non-default extrusion are now **refused and
> reported**, not imported. LINE is unaffected because DXF stores it in world
> coordinates. See ADR-M6-013.

M6 imports LINE, CIRCLE and ARC. Everything else is reported and skipped
(ADR-M6-005). Most of those omissions are loud: a drawing made of SPLINEs
imports as nothing plus a diagnostic naming SPLINE.

**One omission is not loud, and it is recorded here so it is not discovered as a
surprise:** the reader takes DXF X and Y and **ignores Z and the extrusion
direction (group code 210)**. A drawing whose entities sit on a non-XY plane, or
that carries a non-default extrusion vector, therefore imports at the wrong
orientation — silently, because nothing in the file marks it as unusual and the
resulting geometry is perfectly valid.

It is left this way for M6 deliberately: applying the extrusion direction
correctly means deciding how an arbitrary OCS plane maps onto a sketch's frame,
which is a modelling decision rather than a parsing one and is out of scope for
"import LINE, CIRCLE and ARC". Detecting and REPORTING a non-default extrusion
would be the cheap half of the fix and is the first thing to add if M6 is
extended.

Recorded as a known limitation in `M6_SelfValidationReport.md` rather than left
in the code, because a limitation only in the code is one nobody reads.

## ADR-M6-009 — Block definitions are not model geometry (M6)
Status: Accepted. Corrects a Critical found by independent review.

libdxfrw dispatches a BLOCK's contents through the same `addLine`/`addCircle`/
`addArc` callbacks it uses for model space. Without tracking the block state the
collector could not tell them apart, and it did not: a file whose ENTITIES
section held only an `INSERT` imported the block's geometry at **definition**
coordinates — losing placement, scale, rotation and multiplicity — while
reporting the `INSERT` that places it as "skipped".

The realistic case is worse. **Every DXF carrying a DIMENSION also carries an
anonymous block holding its dimension and extension lines.** A drawing with one
real line and one dimension imported **four** lines. Spec 4 states that
dimensions and annotations must not silently become model content; they were.

`addBlock` / `endBlock` now bracket block definitions and everything inside is
skipped, reported once per block rather than once per entity.

**Correction (M6.9 re-review): "once per block" was false when written** -- the
flag was set once and never reset, so a drawing with forty dimension blocks
reported "skipped 1". It is reset in `addBlock` now, and `M6_RR_005` checks that
a two-block file produces two reports.

**And the EXIT half of this fix was entirely unguarded.** Making `endBlock()` a
no-op left all 45 tests green, while a file with a BLOCKS section followed by
real model geometry imported **nothing** -- a worse defect than the one this ADR
fixed. The only block fixture had an ENTITIES section containing a single
INSERT, so no test could observe whether the block state was ever cleared.
`M6_RR_002` covers it.

Unsupported entities *inside* a block are also no longer reported as model-level
entities: doing so told the user their drawing contained an INSERT and a TEXT it
does not contain, and inflated the skip count by the contents of every block.

`INSERT` is still not expanded, so a drawing built from blocks imports as
nothing plus a diagnostic. That is now true — it was claimed in the M6.1
self-validation report while being false in both halves.

## ADR-M6-010 — Units are applied once, after the whole file is read (M6)
Status: Accepted. Supersedes the parse-time scaling of ADR-M6-002.

ADR-M6-002 got the policy right and the timing wrong. Scaling each entity as it
arrived assumed the HEADER reached the reader before the ENTITIES section.
libdxfrw dispatches sections in **file order**, so a file with entities first
was scaled by the 1.0 default and only then learned the unit — 25.4x wrong,
while the result reported `unit = inches, unitWasDefaulted = false`. The one
signal ADR-M6-002 relies on to make that error visible was asserting the
opposite. A file with two HEADER sections split one sketch across two scales.

The reader now collects **raw** values and applies the factor once, after the
read completes. That removes the ordering assumption rather than checking it,
which is the only version a file we have not seen cannot defeat. The
degenerate-geometry checks moved with it, because "shorter than
`kMinSketchDimensionMm`" is a statement about millimetres.

The comment that used to assert the ordering is gone. An assumption stated in a
comment and nowhere else is the failure mode this project hit twice in M5 and
once here.

## ADR-M6-011 — Every $INSUNITS value the format defines (M6)
Status: Accepted. Corrects ADR-M6-002.

ADR-M6-002 claimed the mapping covered "the remaining ISO units libdxfrw
exposes". It covered 0, 1, 2, 4, 5, 6. Values 3 and 7–20 fell through to
`Unrecognized` and took the millimetre default **with a message saying the file
had not stated a usable unit** — which it had. Mils (9) made PCB geometry 39.4x
too large; microns (13), 1000x; kilometres (7), 1e6x too small.

Now mapped: micrometre, millimetre, centimetre, decimetre, metre, decametre,
hectometre, kilometre, microinch, mil, inch, foot, yard, mile. Imperial values
are multiples of the exact 1959 definition, so nothing drifts.

Angstrom, nanometre, gigametre, astronomical unit, light year and parsec are
deliberately left unmapped: the format defines them, but they are not units a
CAD part is modelled in, and mapping them would let a typo produce a 1e16 scale
factor. They report as unrecognised.

## ADR-M6-012 — A degenerate sweep is skipped, not fatal (M6)
Status: Accepted. Closes a hole in ADR-M6-005.

The reader validated an arc's radius and finiteness but never its **sweep**. A
0 to 360 degree arc — legal DXF, and what several exporters emit instead of a
CIRCLE — reached the importer, failed the model's validation, and rolled back
the **whole file**. A drawing with 500 good lines and one such arc imported as
nothing.

ADR-M6-005 says a degenerate entity is skipped and reported; the sweep was the
one hole in that rule, and it turned a skippable entity into a fatal one. The
reader now applies the same sweep test the model uses.

**Correction (M6.9 re-review): it did not, and the difference froze the
application.** The reader normalised with `while (sweep >= 2*pi) sweep -= 2*pi`
while the model uses `std::fmod`. Subtracting in a loop never terminates once
the value exceeds about `2^53 * 2*pi`, and libdxfrw applies no range check to
codes 50/51 -- so an ARC with end angle `1e20` made `ReadDxfFile` never return,
synchronously on the Qt GUI thread, with no cancel.

**This project had already fixed that exact bug in M5** and recorded the reason
in ADR-M5-006. Writing it again, on the one code path that reads untrusted
external files, is the plainest evidence yet that a lesson written into an ADR
is not a lesson applied. The reader uses `fmod` now, and `M6_RR_001` fails -- by
hanging -- if the loop returns.

**Second correction: the guard has two clauses and only one was tested.** A
sweep of `2*pi - 1.7e-7` (an arc of 359.99999 degrees) is inside the model's
rejection band but is not caught by the lower bound, so removing the upper
clause left all 45 import tests green while restoring the whole-file abort this
ADR exists to prevent. `arc_full_turn.dxf` is exactly 0->360, which normalises
to 0, so it could never exercise the other clause. `M6_RR_003` does.

## ADR-M6-013 — A non-default extrusion is refused, not guessed (M6)
Status: Accepted. Replaces ADR-M6-008's assessment, which understated the risk.

ADR-M6-008 described ignoring group code 210 as producing "the wrong
orientation". Independent review measured it and found two things worse:

1. For the common `210 = (0,0,-1)` the correct result is a **mirror**, not a
   rotation. A mirrored part has identical area, volume, mass and centre of
   mass, so **no analytical oracle of the kind every gate in this project uses
   can detect it.**
2. DXF stores **LINE in world coordinates and CIRCLE/ARC in the entity's own
   system**, so ignoring 210 mixes two frames inside one file. A plate with a
   hole imported the hole at (-25, 30) instead of (25, 30), reported success,
   and produced a part wrong in a way nothing downstream can see.

Applying an arbitrary OCS plane means deciding how it maps onto a sketch frame,
which is a modelling decision beyond M6's scope. Entities carrying a non-default
extrusion are therefore **skipped and reported**.

The rule this turns on: when the choice is between refusing and guessing, and a
wrong guess is undetectable, refuse.

## ADR-M6-014 — Finiteness is checked on both sides of the unit multiply (M6)
Status: Accepted. Records a fix that had no ADR at all.

ADR-M6-010 moved unit scaling to the end of the read. It recorded that "the
degenerate-geometry checks moved with it" and stopped there. What it did not
record is that the move put every `AllFinite` check **before** the multiply, so
a coordinate that was finite in the file and infinite after conversion — 1e305
in a drawing measured in miles — passed the reader untouched and was refused by
the model, aborting the whole file. That is exactly the failure ADR-M6-012
exists to eliminate, reintroduced through the door ADR-M6-010 opened.

The rule: **a value is checked for finiteness both as it arrives and after it is
scaled.** `tooSmall` cannot stand in for the second check — `inf < 1e-5` is
false, and `hypot` of two infinities is NaN.

Angles are the deliberate exception. They are never scaled, so conversion cannot
make a finite angle infinite; the raw check in `addArc` is their only guard. That
is a reason to test that one line harder, not to add a second check that can
never fire — `M6_R3_003` is that test.

**This ADR is written three rounds late.** The fix shipped in M6.10 with a test
(`M6_RR_004`) and no entry here, and a reviewer found it by grepping the log for
"overflow" and getting nothing. A fix with no ADR is a fix the next person will
undo.

**And the fix itself covered one third of its own case.** `overflow_on_scale.dxf`
held only LINEs, so deleting either the CIRCLE or the ARC re-check left all fifty
tests green. `M6_R3_002` covers all three, centre and radius separately.

## ADR-M6-015 — Two structural facts are read outside the parser (M6)
Status: Accepted. Extended after the scan it introduced made a second, older
finding fixable.

A `BLOCK` with no `ENDBLK` makes libdxfrw read **the entire rest of the file** as
block content. The whole ENTITIES section disappears, `read()` returns true, and
the user is told "every entity in the file was skipped; nothing to import" —
which aims them at their geometry when the fault is one missing group code in a
section they never open. Unbounded silent data loss with a misleading diagnostic.

The callback interface cannot see this. `DRW_Interface` has no
section-transition hook (`setBlock` is never called), and libdxfrw calls
`endBlock()` at EOF, so the obvious check — "are we still inside a block when the
read finishes?" — is **always false**. I wrote that check first and measured it
doing nothing before replacing it; it is recorded here because it is the check
anyone would reach for.

So the file is scanned separately for one structural fact: does the BLOCKS
section close every BLOCK it opens? This is not a second DXF parser. It reads
group code 0 values, is bounded to the BLOCKS section, answers one question, and
returns `Unknown` for binary DXF rather than guessing. `BLOCK_RECORD` in TABLES
does not collide because the comparison is for equality.

The geometry is not recoverable — once the callbacks have been dispatched there
is nothing left to re-attribute. **The diagnostic is.** The rule this turns on is
the same one as ADR-M6-013: when a fault cannot be repaired, it must at least be
named, and it must be named after the thing that is actually broken.


**Extension — a LINE with no end point is no longer invented.**

libdxfrw default-initialises a missing second point to (0,0,0) and exposes no
group-code-presence flag, so a LINE with codes 10/20 and no 11/21 arrived
indistinguishable from a real line drawn to the origin. It imported silently: a
phantom entity, `IMPORT OK`, and a profile that opens or branches. Spec 4 says
nothing may be silently misinterpreted; this was the last place in M6 where
something was.

It was accepted as a limitation for two review rounds, on the recorded grounds
that "detecting it needs a group-code-presence hook the library does not offer".
**That reason expired the moment this ADR forced a group-code scan to exist.**
The hook is not needed — the file states the absence directly. Two rounds of
"cannot be fixed" turned out to mean "cannot be fixed with the tools we had
then", and nothing re-examined it when the tools changed.

Two things about the identification, both found by mutating this code rather
than by reading it:

- The phantom is matched by **both** endpoints. A real line may share a start
  point with a phantom, and erasing genuine geometry to remove an invented
  entity would be worse than the bug.
- A LINE with code 11 and **no 21** becomes `(x11, 0)`, not the origin. Half a
  second point is still no second point, and the phantom to look for is exactly
  what the library will have built — so the scan records the synthesised end,
  it does not assume it.

Failing to match removes nothing and reports nothing, which is precisely the
behaviour this replaces: the change cannot make any file worse.

## ADR-M6-016 — An unclosed profile is a failure only when something needs it (M6)
Status: Accepted. Narrows the scope of an M4 rule without reversing it.

The Model Tree marked every sketch whose profile does not close as **Failed**,
with a red `!` and the message "profile is not closed".

That rule is right when a Pad is asking for the profile — UI spec 12 requires a
failed feature to be identifiable from the tree with a useful reason, and
`UI_TREE_005` has pinned it since M4. It is wrong when nothing is asking.

**It was invisible until M6.** Every sketch the viewer could previously produce
was built by a flow that closed its own profile, so the two cases never
separated. DXF import is the first way to put arbitrary open geometry into a
document, and the owner's *first* manual import — `line.dxf`, one line, drawn
correctly — showed `[Skt]! line`. Nothing had failed.

The cost is not cosmetic. A marker that fires after a successful operation
teaches the user to ignore it, and then it cannot do its job when something has
genuinely broken. A state channel is only worth having while it is trusted.

So: the diagnostic is **always** offered, because a user who wonders why they
cannot pad a sketch must be able to find out without opening a log. The
**state** becomes Failed only when the dependency graph shows something
downstream waiting for that profile.

Both directions are pinned, by different tests: reverting to "always fail" is
caught by `M6_UI_001`, and dropping the failure entirely is caught by M4's
`UI_TREE_005`.

The generalisable point: **this was a scope error, not a logic error.** The rule
was correct for every case that existed when it was written. A new input path
created a case it had never been asked about, and nothing re-derived it. That is
the same shape as the truncated-LINE limitation retired in M6.12 — a statement
that was true when written and was never re-read when the surroundings changed.

**And no automated test could have found it.** The tree state was correct for
every document the suite builds. It took someone opening the application and
importing a file.

---

## ADR-M7-001 — A dimension is annotation, not geometry (M7)
Status: Accepted.

M6 read a DXF as geometry and reported every DIMENSION as an unsupported
entity. M7 needs those dimensions, and the obvious cheap route — import the
dimension's own lines and infer intent from their arrangement — is the one this
project must not take. Every dimensioned DXF carries an anonymous `*D` block
holding the arrows and witness lines, and M6.9 already had to fix exactly that:
those lines were being imported as model geometry, turning a four-line
rectangle into a twelve-line drawing.

So a DIMENSION enters through a separate channel. `ImportedDimension2D` carries
the two DEFINITION points (group codes 13 and 14), the direction the
measurement is taken along, any text override, and the optional stated
measurement. It never becomes a Sketch entity. The `*D` blocks stay skipped by
`addBlock`, unchanged.

The definition points are what make this work: they sit ON the measured
geometry, not on the annotation, so a dimension can be matched to a native line
without the annotation being read at all.

---

## ADR-M7-002 — Parameter names: Width, Height, then Length-N (M7)
Status: Accepted.

Naming has to be deterministic (spec 9) and it must not depend on the order
entities or dimensions arrived in, because the same drawing exported twice can
list them either way.

The rule: candidates are sorted GEOMETRICALLY — horizontal targets first, then
by location — and the first horizontal becomes `Width`, the first vertical
`Height`, and anything further `Length1`, `Length2`, ... A name already taken,
in the document or elsewhere in the same plan, gains `_2`, `_3`, ... up to 999.

Sorting by geometry rather than by entity id or vector position is the whole
point. An ordering by id looks perfectly deterministic and fails the
shuffled-order test, because ids are handed out in file order.

Names are LABELS. Identity is the ObjectId, and nothing in the reconstruction
path resolves anything by name.

---

## ADR-M7-003 — Provenance is carried, not inferred (M7)
Status: Accepted.

Spec 3 requires explicit source dimensions, inferred geometric relations and
inferred placement never to be silently mixed. After application they are all
just native constraints — a `Length` the source stated and a `Length` M7 chose
are the same object — so provenance cannot be recovered later. It is therefore
recorded on every planned item at the moment the decision is made:
`ExplicitSource`, `InferredGeometric`, `InferredPlacement`.

Provenance is never runtime identity, and nothing resolves by it.

---

## ADR-M7-004 — Four tolerances, and the invariants between them (M7)
Status: Accepted.

Spec 19 forbids one magic constant for all purposes. M7.1 defines:

| constant | value | answers |
|---|---|---|
| `kReconstructionCoincidenceToleranceMm` | 1e-3 mm | are these two points the same point? |
| `kReconstructionAxisAngleToleranceRad` | 1e-3 rad | is this line axis-aligned? |
| `kMinReconstructedDimensionMm` | 1e-2 mm | is this a usable dimension? |
| `kReconstructionValueAgreementFraction` | 0.05 | do these two numbers describe the same feature? |

The angular and distance tolerances were briefly one constant, which is
nonsense in both directions: an angular tolerance used as a distance is
scale-dependent, and a distance used as an angle is not dimensionally sound.

The relationships are asserted at COMPILE time, not described in prose, because
M5 already learned that a behavioural test for a tolerance relationship passes
or fails depending on where the geometry started — 41 of 144 swept
configurations, and the test's one configuration was among the other 103. The
load-bearing one:

> `kMinReconstructedDimensionMm > kReconstructionCoincidenceToleranceMm`

without which a reconstructed `Length` could describe a line whose own
endpoints this same analysis calls coincident — two contradictory statements
about one pair of points.

Note that the reconstruction floor sits ABOVE the model's own
`kMinSketchDimensionMm` (1e-5). The model's floor rejects degenerate geometry
and knows nothing about recognition; it cannot express this constraint.

The agreement fraction was 1% for one revision. That put ordinary export
rounding inside the refusal band — a drawing 0.6% out is entirely normal — and
would have begun rejecting correct files as self-contradictory. 5% still
separates "same feature, drawn slightly off" from "this dimension points at
something else".

---

## ADR-M7-005 — Analyze, validate, apply, roll back (M7)
Status: Accepted.

Reconstruction is staged (spec 15). `AnalyzeForReconstruction` reads the
document and changes nothing; `ValidatePlan` rejects the whole proposal before
any object exists; `ApplyReconstruction` creates everything or nothing.

Interpreting and mutating in one pass would make "no half-created Parameters
remain" (spec 16) a property of error handling scattered through the analysis
instead of a property of the design — and by the time such a pass discovered
the seventh dimension was unusable it would already have created six
Parameters.

Rollback runs in reverse creation order: constraints first, so they release
their Parameter graph edges, then the Parameters. Removing a Parameter while a
constraint still bound it would be the dangling reference the ordering exists
to prevent.

A planned dimensional constraint refers to its not-yet-created Parameter by
`PlanParameterSlot`, a distinct type over an index. It exists only while the
plan does, is never persisted, and never becomes native identity — spec 7's
whole point is that one of those must never turn into the other, so the
compiler is made to enforce it rather than a comment.

---

## ADR-M7-006 — Ambiguity is reported, never resolved by preference (M7)
Status: Accepted.

A DXF linear dimension names no entity — the format gives it no way to. M7
associates it by geometry: the native line whose two endpoints are its two
definition points, within the coincidence tolerance.

- exactly one match: reconstruct
- more than one: **skip**, `AmbiguousTarget`, with a diagnostic
- none: **skip**, `NoTargetGeometry`

"More than one" is a real case, not a theoretical one: a duplicated export puts
two identical lines on top of each other. Choosing the first, the lowest id or
the nearest is a preference dressed as a rule, and spec 18 is explicit that
"not reconstructed" beats "confidently wrong".

A text override (code 1) that is neither empty nor `<>` is also refused: the
drawing displays a number the geometry does not encode, and which one the user
meant is not knowable. `<>` is the format's own "show the measurement" and is
NOT an override — treating it as one would silently stop reconstructing most
real files, since nearly every CAD package writes it.

---

## ADR-M7-007 — Repeated reconstruction is refused, not merged (M7)
Status: Accepted for M7.1.

`ReconstructSketch` refuses a sketch that already carries a dimensional
constraint bound to a Parameter. Spec 25's forbidden outcome is `Width`,
`Width_2`, `Width_3` accumulating from repeated identical runs, and refusing is
the smallest thing that cannot produce it.

The test is NATIVE STATE — dimensional constraints with bindings — not a
provenance flag, so it survives save/load and works on a document that never
recorded provenance.

A replacement mode is deferred. Refusing is recoverable (remove the constraints
and re-run); a merge policy that got it wrong would not be.

---

## ADR-M7-008 — The Fix goes on the lexicographically smallest corner (M7)
Status: Accepted.

Without a Fix a sketch keeps its two global translation freedoms and can never
reach DOF 0, however many dimensions are reconstructed. Something must choose
where to pin it, and the choice must be a fact about the geometry rather than
about the order it was read in.

The corner with the smallest (u, v) lexicographically. For the release fixture
that is (0,0), which matches M5's own placement convention, so the analytical
centre of mass is unchanged from M5's gates.

Recorded as `InferredPlacement`, which is neither a measurement nor a
recognised relation, and must never be presented to a user as either.

---

## ADR-M7-009 — The stated measurement wins; the definition points locate (M7)
Status: Accepted.

A linear dimension carries two kinds of evidence, and they do different jobs:

- The DEFINITION points (codes 13/14) say WHICH geometry is measured. They are
  mandatory for the supported kinds and are the only association evidence
  available.
- The stated measurement (code 42) says WHAT the answer should be.

When both are present and agree within the band, the stated value is used. That
is deliberate and it is what makes Fixture B work: a drawing exported at 99.5
with a dimension reading 100 is a drawing whose designer meant 100, and the
solver is what moves the geometry. Using the definition-point separation
instead would reconstruct 99.5, the solve would be a no-op, and a gate that
"passes" while proving nothing is worse than one that fails.

When they disagree materially the pair is **refused** with
`ValueDisagreesWithGeometry`. The drawing contradicts itself and M7 does not
pick a side.

Code 42 is optional, documented read-only, and defaulted to 0 by libdxfrw when
absent — indistinguishable from a genuine zero. Since zero is not a usable
measurement either way, "absent" and "zero" are treated alike, which avoids
inventing a stated value the file never carried.

---

## ADR-M7-010 — The rectangle is a named shape, not an inference engine (M7)
Status: Accepted. Resolves a conflict inside spec 37.

Spec 37 says M7.1 delivers explicit dimensions only, with "no general inference
engine yet", and spec 38 puts Horizontal/Vertical/Coincident recognition in
M7.2. But spec 37's own required M7.1 gate demands `Solved, DOF 0` for the
rectangle — and two Length constraints cannot reach it. A four-line rectangle
has 16 degrees of freedom and needs 4 Coincident, 2 Horizontal, 2 Vertical, 1
Fix and 2 Length to remove them all, which is exactly the list spec 13 gives.

The gate is release-critical and the sequencing note is guidance, so the gate
wins. M7.1 implements ONE deterministic named shape: four lines, each joined
end-to-end to exactly two others, forming one closed loop, every side within
the axis tolerance of an axis, two horizontal and two vertical. Anything else
gets no constraints from this rule — a sloped quadrilateral gets nothing, and
forcing its sides onto the axes would be the "confidently wrong" spec 18
forbids.

Corners are found by CLUSTERING the eight endpoints, not by pairwise matching.
The shapes that must be rejected are the ones pairwise matching accepts: three
lines meeting at a point, two coincident segments, a figure of eight. Each has
every endpoint "matched" and none is a rectangle.

M7.2 still owns general recognition over arbitrary geometry.

Each recogniser is separately switchable (`ReconstructionOptions`). That is not
a convenience: spec 23 Gate F requires proving each is load-bearing by turning
it off and watching a gate fail, and AGENTS.md records that source-editing
mutation runs in this project have repeatedly produced false "guarded" claims.
