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
