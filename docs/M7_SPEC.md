# M7 — DXF Dimension and Constraint Reconstruction

## 1. Mission

M7 converts imported 2D DXF geometry from M6 into an editable, parameter-driven constrained Sketch.

M6 proves that supported DXF geometry becomes native, stable ParametricCAD Sketch entities.

M7 adds the next layer:

> Recover useful dimensional and geometric design intent from DXF data and/or deterministic reconstruction rules, represent that intent using native `Parameter` and `SketchConstraint` objects, and prove that editing the reconstructed dimensions changes the downstream 3D model correctly.

M7 is successful when an imported 2D drawing is no longer only stable geometry.

It becomes a native parametric model.

The central release proof is:

> Import a supported dimensioned DXF rectangle, reconstruct Width and Height as native Parameters and constraints, create or use the downstream Pad, change Width from 100 mm to 120 mm through the normal ParametricCAD editing path, and verify that the constrained Sketch resolves and the 3D solid rebuilds to the analytically expected volume.

---

## 2. Baseline

M7 starts from the accepted M6 master state.

M6 is expected to provide:

- deterministic DXF import
- native `Sketch` creation
- stable `SketchEntityId`
- native LINE / CIRCLE / ARC geometry
- deterministic unit conversion
- save/load independence from the original DXF file
- transaction-safe import
- downstream use of imported closed profiles
- documented DXF parser boundary
- no DXF-library types crossing into `src/Core`

M5 remains the source of the constraint/solver model:

- stable `SketchConstraintId`
- native Parameters
- dimensional constraints
- geometric constraints
- solver status and DOF
- selective recompute
- failure/recovery
- persistence

M7 must preserve every accepted M0–M6 contract.

---

## 3. Scope

### Required M7 capability

M7 shall reconstruct useful parametric intent for imported Sketch geometry.

Required dimensional reconstruction:

- horizontal line length
- vertical line length
- arbitrary line length
- circle radius
- circle diameter where explicitly represented
- selected point-to-point distance where supported by the source representation

Required geometric reconstruction:

- horizontal
- vertical
- coincident endpoints
- equal/concentric/tangent/etc. are optional unless explicitly promoted into M7 scope by ADR

Required native output:

- `Parameter`
- `SketchConstraint`
- stable Parameter-to-constraint binding
- native dependency graph edges
- solver participation
- save/load
- downstream recompute

M7 must distinguish:

1. explicit dimensions present in the source
2. inferred geometric constraints
3. inferred dimensions created by ParametricCAD policy

These categories must not be silently mixed.

---

## 4. Explicitly Out of Scope

M7 does NOT require:

- a complete automatic CAD feature-recognition system
- arbitrary drawing-to-design-intent AI inference
- a full interactive production Sketcher
- free-form user constraint authoring UI
- DXF export
- DWG import
- Assembly
- CAM
- FEA
- automatic 3D feature recognition from arbitrary 2D views
- drawing-sheet title blocks
- BOM extraction
- GD&T reconstruction
- manufacturing tolerance reconstruction
- automatic interpretation of every DXF DIMENSION subtype
- semantic inference from text labels with no reliable geometry association

Unsupported or ambiguous source dimensions must never be silently converted into a confident native constraint.

---

## 5. Architectural Rule

M7 reconstruction is a transformation layer.

Required flow:

```text
DXF
  ↓
M6 DXF parser
  ↓
format-neutral imported geometry / annotation representation
  ↓
M6 native Sketch entities
  ↓
M7 reconstruction analysis
  ↓
ReconstructionPlan
  ↓
PartDocument facade
  ↓
Parameter + SketchConstraint
  ↓
Sketch solver
  ↓
downstream Pad / mass / viewer
```

Forbidden architecture:

```text
DXF DIMENSION object
    ↓
stored permanently inside SketchConstraint
```

or:

```text
reconstruction engine
    ↓
direct mutation of solver internal arrays
```

The reconstruction layer must create normal native document objects through documented APIs.

After reconstruction and save, the PartDocument must remain valid without the original DXF file or parser state.

---

## 6. Reconstruction Boundary

Define a format-neutral reconstruction input.

Suggested conceptual representation:

```cpp
enum class ImportedDimensionKind
{
    Linear,
    Aligned,
    Radius,
    Diameter,
    Angular
};

struct ImportedDimensionRef
{
    ImportedDimensionKind kind;
    double value;
    // semantic references to imported geometry
};

struct ReconstructionCandidate
{
    // source identity / diagnostic metadata
    // target native SketchEntityId references
    // proposed constraint kind
    // proposed parameter value
    // confidence / reason if the implementation uses such a concept
};
```

Exact type names are implementation-defined.

Important rules:

- no parser-owned pointer
- no DXF library type
- no vector index as identity
- no solver variable index
- no OCCT topology
- no Qt type
- no persistence dependency on parser runtime state

---

## 7. Native Identity Contract

Every reconstructed native object shall use the existing ParametricCAD identity system.

Required:

- every Parameter receives a normal stable object ID
- every constraint receives a stable `SketchConstraintId`
- every entity reference uses `SketchEntityId`
- every sub-element reference uses the existing semantic sub-element model
- parameter bindings use native object IDs

Forbidden identity:

- DXF entity array position
- DXF DIMENSION array position
- parser pointer
- memory address
- solver slot number
- residual index
- OCCT topology
- enum ordinal persisted as identity

DXF handles may be retained as optional source metadata, but correctness must not depend on them.

---

## 8. Source Dimension Policy

M7 must explicitly classify source dimension information.

### Class A — explicit and resolvable

The source dimension can be unambiguously associated with native imported geometry.

Expected action:

- create native Parameter
- create native dimensional SketchConstraint
- bind constraint to Parameter

### Class B — explicit but ambiguous

The source contains a dimension value but its target geometry cannot be resolved uniquely.

Expected action:

- do not guess
- record diagnostic
- leave geometry valid
- preserve the unresolved item as import/reconstruction diagnostic metadata if required by the architecture

### Class C — inferred geometric relation

The source geometry strongly satisfies a deterministic reconstruction rule, for example:

- line within tolerance of horizontal
- line within tolerance of vertical
- two endpoints within coincidence tolerance

Expected action:

- reconstruction may create geometric constraints only under a documented rule and tolerance

### Class D — inferred dimension

A useful design dimension is not explicitly present but can be deterministically promoted under a documented policy.

This class is optional in M7.

If supported, it must be visibly distinguishable in diagnostics from an explicit source dimension.

---

## 9. Parameter Naming Policy

Every reconstructed dimensional constraint must bind to a native Parameter.

Parameter naming must be deterministic and user-readable.

Recommended examples:

```text
Width
Height
Radius
Diameter
Length1
Length2
Distance1
```

When the source provides a meaningful dimension name or label that is safe and unambiguous, it may be used according to a documented sanitization policy.

Naming requirements:

- unique within the required namespace
- deterministic
- legal for the existing Parameter model
- stable across save/load
- no silent collision
- collision resolution documented

Do not derive correctness from Parameter display names.

Native IDs remain identity.

---

## 10. Dimension Value Policy

A reconstructed dimensional Parameter must use the authoritative source/reconstruction value.

Required validation:

- finite
- correct unit
- compatible with constraint kind
- above existing geometric validity floors where required
- consistent with M5 unit rules

Length-like constraints use length-compatible Parameters.

Angle constraints use angle-compatible Parameters.

`Unitless` must not be accepted as a substitute where the existing M5 contract forbids it.

All external units must be converted once at the import/reconstruction boundary.

---

## 11. Geometric Constraint Reconstruction

M7 shall support deterministic reconstruction of at least:

- `Horizontal`
- `Vertical`
- `Coincident`

### Horizontal

A candidate line may become Horizontal when its endpoints satisfy the documented angular or coordinate tolerance.

### Vertical

A candidate line may become Vertical under the corresponding policy.

### Coincident

Two semantically relevant endpoints may become Coincident when their source geometry is within the documented coincidence tolerance.

Critical rule:

> Tolerance is a recognition rule, not permission to permanently corrupt geometry.

The implementation must define whether geometry is snapped before solving, left unchanged and solved, or normalized through another explicit mechanism.

The policy must be documented in an ADR.

---

## 12. Dimensional Constraint Reconstruction

M7 shall support at least:

### Line Length

A source linear/aligned dimension that maps to one native line may become a native `Length` constraint bound to a Parameter.

### Radius

A source radius dimension maps to native `Radius`.

### Diameter

A source diameter dimension maps to native `Diameter`.

The native circle still has one geometric radius state; Radius and Diameter must not create duplicated geometry state.

### Distance

Supported point-to-point dimensions may map to native `Distance` when the source reference can be resolved semantically.

---

## 13. Rectangle Reconstruction Contract

A release-critical rectangle fixture shall reconstruct a stable constrained model.

Minimum expected design intent:

- 4 LINE entities
- 4 Coincident corner constraints
- 2 Horizontal constraints
- 2 Vertical constraints
- 1 Fix or equivalent deterministic placement policy
- Width Parameter
- Height Parameter
- Width dimensional constraint
- Height dimensional constraint

Expected status after solve:

```text
Solved
DOF = 0
```

The exact entity ordering must not be part of the contract.

The exact numerical IDs are not required to match a separate import operation.

They must survive save/load of the same document.

---

## 14. Circle Reconstruction Contract

A release fixture shall support:

- imported circle
- native stable circle entity
- radius or diameter source dimension
- reconstructed native Parameter
- reconstructed native Radius or Diameter constraint
- deterministic placement policy if required for DOF 0

Changing the Parameter must modify the native circle geometry through the solver.

If the circle drives a Pad, volume must change analytically as expected.

---

## 15. Reconstruction Plan

Reconstruction should be staged rather than immediately mutating the document while interpreting source data.

Preferred conceptual workflow:

```text
Analyze
  ↓
Build ReconstructionPlan
  ↓
Validate plan
  ↓
Apply transaction
  ↓
Recompute
  ↓
Commit or rollback
```

The plan should make it possible to inspect:

- Parameters to create
- constraints to create
- source-to-native associations
- skipped/ambiguous items
- expected warnings

before irreversible document mutation.

---

## 16. Transaction Policy

Applying one reconstruction operation must be transactional.

If a fatal error occurs while applying the reconstruction plan:

- no half-created Parameters remain
- no half-created constraints remain
- no phantom graph edges remain
- no dangling Parameter references remain
- no duplicate IDs remain
- no partially rewritten Sketch geometry remains unless the documented transaction model explicitly supports it

The document must return to the pre-reconstruction state.

Mutation testing must demonstrate rollback correctness.

---

## 17. Solver Failure Policy

Reconstruction may produce:

- `Solved`
- `UnderConstrained`
- `OverConstrained`
- `Conflicting`
- `InvalidInput`
- other existing documented solver states

M7 must not force a Sketch into `Solved` by deleting or weakening constraints silently.

If reconstructed intent conflicts:

- preserve diagnostic information
- identify offending reconstructed constraints where possible
- do not commit corrupt solved geometry
- block downstream rebuild according to existing M5 contracts
- allow recovery by removing or editing the offending reconstructed item

---

## 18. Ambiguity Policy

M7 must prefer "not reconstructed" over "confidently wrong."

Examples requiring caution:

- one dimension could refer to two parallel lines
- text is near geometry but has no reliable reference
- duplicate source handles
- overlapping identical entities
- multiple candidate endpoints inside tolerance
- broken DIMENSION references
- source dimension value disagrees materially with referenced geometry

For ambiguous cases:

- do not guess silently
- emit structured diagnostic
- continue with other unambiguous reconstruction candidates when policy allows

---

## 19. Tolerance Policy

M7 must define separate tolerances for separate meanings.

At minimum distinguish:

- import numeric tolerance
- coincidence recognition tolerance
- horizontal/vertical angular tolerance
- geometry validity floor
- solver tolerance

Do not reuse one magic constant for all purposes.

Required compile-time or runtime invariants shall ensure tolerance relationships cannot collapse into contradictory values.

Tests must include cases just below and just above reconstruction thresholds.

---

## 20. Persistence Rule

After M7 reconstruction and save:

- Parameters persist as native Parameters
- constraints persist as native constraints
- entity references persist semantically
- Parameter bindings persist
- reconstruction diagnostics that are required for user-visible provenance persist according to documented policy
- DXF parser state does not persist as authoritative model state

Required sequence:

```text
DXF
→ M6 import
→ M7 reconstruct
→ solve
→ save PartDocument
→ remove original DXF
→ fresh load
→ solve
→ edit Width
→ recompute
→ downstream 3D updates
```

This must succeed without consulting the original DXF.

---

## 21. Recompute Contract

M7 must preserve M5 selective recompute.

Examples:

### Width change

Expected:

```text
Width Parameter
  ↓
Sketch solve
  ↓
Pad rebuild
  ↓
MassProperties rebuild
```

### Density change

Expected:

```text
Density
  ↓
MassProperties only
```

Sketch must not re-solve.

### Unrelated Parameter

Expected:

```text
no unrelated Sketch solve
no unrelated Pad rebuild
```

Tests shall use invocation counters or equally strong evidence, not only equal final values.

---

## 22. Required Test Fixtures

Use small hand-verifiable fixtures.

### Fixture A — dimensioned rectangle

Geometry:

```text
100 mm × 50 mm rectangle
```

Required reconstructed native design intent:

- Width = 100 mm
- Height = 50 mm
- horizontal/vertical/coincident constraints
- fully constrained result

### Fixture B — skewed source rectangle

Start geometry deliberately imperfect within reconstruction tolerance.

Purpose:

- prove reconstruction rules
- prove solver changes geometry
- avoid a no-op implementation passing

### Fixture C — dimensioned circle

Example:

```text
center = (25, 30)
radius = 10 mm
```

or explicit diameter equivalent.

### Fixture D — ambiguous dimension

One source dimension with more than one plausible geometry target.

Expected:

- diagnostic
- no silent guessed constraint

### Fixture E — conflicting dimensions

Expected:

- conflict
- no corrupt commit
- downstream blocked
- recovery possible

### Fixture F — tolerance boundary

Cases just inside and outside:

- horizontal tolerance
- vertical tolerance
- coincidence tolerance

### Fixture G — naming collision

Two reconstructed parameters that would naturally request the same display name.

Expected deterministic collision handling.

### Fixture H — source independence

Reconstruct, save, remove original DXF, reload, edit Parameter.

---

## 23. Release Gates

### Gate A — Explicit Width reconstruction

Import a dimensioned line/rectangle.

Verify:

- native Parameter created
- native dimensional constraint created
- stable Parameter binding
- source value interpreted correctly
- solver uses the constraint

PASS requires measuring native geometry, not merely inspecting metadata.

---

### Gate B — Rectangle fully constrained

Reconstruct the release rectangle.

Verify:

- expected native geometric constraints
- Width and Height Parameters
- `Solved`
- DOF = 0
- geometry = 100 × 50 mm within tolerance

Expected geometry must be independently calculated.

---

### Gate C — Parametric edit

Starting from Gate B:

```text
Width 100 → 120
```

Verify:

- sketch solves
- DOF remains 0
- geometry becomes 120 × 50
- downstream Pad rebuilds
- volume becomes analytically correct

For Pad length 20 mm:

```text
V = 120 × 50 × 20
  = 120000 mm³
```

This is the central M7 release gate.

---

### Gate D — Height edit

Starting from Width 120:

```text
Height 50 → 80
```

Expected Pad volume at length 20:

```text
V = 120 × 80 × 20
  = 192000 mm³
```

---

### Gate E — Circle dimension

Reconstruct Radius or Diameter.

Edit the controlling Parameter.

Verify actual circle geometry and downstream volume.

For a Pad with unchanged extrusion length, doubling radius must produce exactly 4× volume.

---

### Gate F — Geometric reconstruction

Verify Horizontal, Vertical and Coincident reconstruction using independent geometric measurements.

Mutation controls:

- disable Horizontal recognition → test fails
- disable Vertical recognition → test fails
- disable Coincident recognition → test fails

---

### Gate G — Ambiguity

Present ambiguous source dimension/reference data.

Verify:

- no guessed native constraint
- useful diagnostic
- unaffected valid candidates may still reconstruct according to policy
- document remains valid

---

### Gate H — Conflict and recovery

Create or import conflicting reconstruction intent.

Verify:

- explicit conflict state
- offending reconstructed IDs identified where possible
- no corrupt solved geometry committed
- downstream stale/current state is correct
- edit/remove conflict
- recompute recovers
- subsequent Parameter edit still works

---

### Gate I — Save/load/re-solve

Reconstruct the rectangle and circle.

Save.

Load into a fresh document/backend.

Verify:

- Parameter IDs survive
- constraint IDs survive
- entity references survive
- bindings survive
- solver result survives semantically
- editing reconstructed Parameters still rebuilds geometry

---

### Gate J — Source independence

After reconstruction and save:

- rename/delete original DXF
- load PartDocument
- edit Width
- recompute successfully

The original DXF must not be required.

---

### Gate K — Selective recompute

Use counters to verify:

- Width changes solve the Sketch and rebuild downstream
- PadLength does not re-run reconstruction
- Density does not re-run reconstruction or solve unrelated Sketches
- unrelated Parameters touch nothing unrelated
- no edit performs no reconstruction work

---

### Gate L — Regression

All M0–M6 tests pass in:

- Debug
- Release

Release test execution must prove Release binaries were actually invoked.

---

## 24. Adversarial Tests

At minimum:

- duplicate source dimension references
- missing source reference
- missing native entity target
- deleted target entity before reconstruction
- duplicate requested Parameter names
- NaN / Infinity dimension
- zero length
- negative length
- zero radius
- negative radius
- very small valid dimensions
- very large dimensions
- dimension exactly at validity floor
- dimension just below validity floor
- horizontal line just inside tolerance
- horizontal line just outside tolerance
- vertical line just inside tolerance
- vertical line just outside tolerance
- coincident endpoints just inside tolerance
- endpoints just outside tolerance
- dimension disagrees with current geometry
- redundant geometric constraints
- over-constrained reconstruction
- conflicting reconstruction
- random entity order
- random source dimension order
- repeated reconstruction request
- reconstruction after save/load
- reconstruction after entity deletion
- failure halfway through plan application
- rollback followed by successful reconstruction
- fresh-process load
- edit after fresh-process load
- unknown reconstruction flag / mode

---

## 25. Idempotence

M7 must define what happens if reconstruction is requested twice.

Preferred contract:

> Re-running reconstruction on a Sketch already reconstructed from the same source must not silently duplicate equivalent Parameters and constraints.

Possible implementations:

- detect existing native bindings
- detect reconstruction provenance
- require explicit replacement mode

The exact mechanism is an ADR decision.

Tests must prevent accidental creation of:

```text
Width
Width_2
Width_3
...
```

from repeated identical reconstruction unless explicitly requested.

---

## 26. Provenance

M7 should provide enough provenance to explain reconstructed design intent.

At minimum the diagnostic/report layer should be able to answer:

- was this constraint explicit in source or inferred?
- which source item produced it?
- which native entity/entities does it target?
- which Parameter controls it?
- was anything skipped or ambiguous?

Provenance must not become runtime identity.

Native IDs remain authoritative.

---

## 27. UI Scope

M7 requires user-visible access to reconstructed parameters and constraints using the existing UI architecture.

Required workflow:

1. import supported DXF
2. run reconstruction, automatically or through the documented command
3. imported Sketch appears
4. reconstructed constraints appear under the Sketch
5. reconstructed dimensional Parameters are visible
6. user selects Width
7. user changes Width
8. Sketch updates
9. 3D solid updates
10. mass/volume status updates
11. ambiguous/skipped reconstruction is visible as diagnostic information

M7 does not require a full graphical dimension-placement editor.

---

## 28. Owner Manual UI Validation

Automation cannot fully validate:

- whether reconstructed dimensions are easy to find
- whether source-vs-inferred status is understandable
- whether parameter names are readable
- whether warnings are discoverable
- whether editing Width feels direct
- whether stale geometry is ever visually presented as current
- whether selection between tree, parameter and geometry is understandable

Provide:

```text
docs/reviews/M7_UI_UserValidation.md
```

Owner validation must include at least:

### Test A — reconstructed rectangle

- Width visible
- Height visible
- expected constraints visible
- status = Solved
- DOF = 0

### Test B — Width edit

- Width 100 → 120
- solid visibly changes
- volume becomes 120000 mm³ for the standard fixture
- no misleading stale display

### Test C — ambiguity

- ambiguous reconstruction warning is visible and understandable
- no fake confident constraint appears

### Test D — conflict/recovery

- conflict identifiable
- offending reconstructed constraint findable
- recovery understandable

### Test E — dark theme / normal interaction

- text readable
- markers distinguishable
- normal selection still works

Display scaling remains a separately recorded validation dimension according to the existing project policy.

Owner manual validation must never be described as independent agent review.

---

## 29. Dependency / Licence Rule

M7 should introduce no new third-party dependency unless required.

If a new reconstruction or DXF-dimension library is proposed, record before adoption:

- exact library
- exact version
- licence
- static / dynamic / header-only use
- source files or targets using it
- whether any type crosses into Core
- commercial redistribution obligations
- source-code obligations
- replacement/relink obligations if applicable

No new GPL dependency may enter the commercial application without explicit owner approval.

Prefer use of the existing M6 parser data and native M5 constraint engine.

---

## 30. Required ADRs Before Completion

At minimum:

- ADR-M7-001 — source dimension classification and reconstruction policy
- ADR-M7-002 — Parameter naming and collision policy
- ADR-M7-003 — explicit vs inferred constraint provenance
- ADR-M7-004 — geometric recognition tolerances
- ADR-M7-005 — reconstruction transaction and rollback
- ADR-M7-006 — ambiguity policy
- ADR-M7-007 — repeated reconstruction / idempotence
- ADR-M7-008 — rectangle placement / Fix convention
- ADR-M7-009 — source-value vs current-geometry disagreement policy

Additional ADRs shall be created whenever implementation discovers a durable architectural decision.

---

## 31. Self-Validation Rules

Required report:

```text
docs/reviews/M7_SelfValidationReport.md
```

It must contain:

- baseline commit
- final/working commit
- environment
- dependency/licence state
- Debug build result
- Release build result
- exact Debug test count
- exact Release test count
- proof Release tests invoke Release binaries
- M0–M6 regression result
- reconstruction matrix
- Gates A–L
- adversarial tests
- mutation verification
- persistence tests
- fresh-process tests
- architecture audit
- Core dependency audit
- known limitations
- unsupported source dimension types
- NOT EXECUTED items
- owner validation status

No unexecuted test may be reported PASS.

No test may prove correctness merely by calling the same production formula that is under test.

Geometry claims must be checked using independent geometric or analytical oracles.

---

## 32. Mutation Verification

Release-critical reconstruction tests must discriminate.

Required mutation examples include:

- disable Parameter binding → Gate A/C fails
- bind Width constraint to wrong Parameter → Gate C fails
- remove Horizontal reconstruction → Gate F fails
- remove one Coincident constraint → rectangle DOF/status gate fails
- swap Width/Height source mapping → analytical geometry gate fails
- duplicate reconstruction on second run → idempotence test fails
- break rollback after half-applied plan → transaction test fails
- omit unit conversion → dimension oracle fails
- replace semantic entity reference with vector position → shuffled-order test fails
- discard provenance classification → provenance test fails if provenance is required

For every review fix marked release-critical:

1. revert the fix
2. demonstrate the regression test fails
3. restore the fix
4. rerun relevant suite
5. record evidence

---

## 33. Independent Review

Provide:

```text
docs/reviews/M7_IndependentReview.md
```

Reviewers must treat the self-validation report as claims, not facts.

At minimum partition review across:

### Reviewer 1 — reconstruction semantics

- source dimension mapping
- Width/Height correctness
- Radius/Diameter correctness
- geometric recognition
- tolerance boundaries
- ambiguity

### Reviewer 2 — identity / persistence / transactions

- stable IDs
- source/native association
- save/load
- fresh process
- rollback
- repeated reconstruction
- deletion/recovery

### Reviewer 3 — recompute / UI evidence / test quality

- Parameter → Sketch → Pad path
- selective recompute
- stale/current state
- diagnostic presentation
- mutation claims
- false-positive tests
- Debug vs Release evidence

Suspected defects should be demonstrated by execution where practical.

Independent reviewers should deliberately search for defects introduced by previous review fixes.

---

## 34. Severity Rules

### Critical examples

- reconstructed dimension silently targets wrong geometry
- unit mismatch changes physical geometry
- Parameter edit reports success but updates wrong dimension
- solver reports Solved while required reconstructed geometry is wrong
- save/load changes reconstructed identity or meaning silently
- dangling pointer/use-after-free
- rollback leaves corrupted document
- stale 3D geometry displayed as current after reconstruction failure
- parser/runtime identity becomes persisted document identity

### Major examples

- valid source dimension not reconstructed under required scope
- deterministic ambiguity handled inconsistently
- duplicate constraints created by repeated reconstruction
- wrong DOF/status
- missing useful diagnostics
- Release test evidence invalid
- selective recompute materially broken

### Minor examples

- diagnostic wording
- non-critical ordering
- cosmetic provenance display
- optional unsupported dimension type

---

## 35. Completion Report

Before M7 can close, create:

```text
docs/reviews/M7_CompletionReport.md
```

The report must contain:

- mission
- baseline
- final commit
- implementation summary
- constraint/reconstruction matrix
- test totals
- Gates A–L
- architecture audit
- dependency/licence audit
- review rounds
- owner UI validation result
- known limitations
- deferred work
- M8 readiness

Current-tree facts must not be copied from stale earlier drafts without re-verification.

---

## 36. Definition of Done

M7 is complete only when:

- required source dimensions reconstruct into native Parameters and constraints
- required geometric relations reconstruct
- rectangle reconstruction is fully constrained
- Width/Height edits drive native Sketch solve
- downstream Pad rebuild is analytically correct
- circle Radius/Diameter reconstruction works
- ambiguous source data is not silently guessed
- conflict/recovery works
- repeated reconstruction follows documented idempotence policy
- transaction rollback is proven
- stable identity survives save/load
- original DXF is not required after save
- selective recompute remains correct
- all M0–M6 regressions pass
- Debug and Release are independently verified
- release-critical tests are mutation-verified
- dependency licences are recorded
- independent review has no unresolved Critical or accepted Major findings
- owner manual UI validation passes

---

## 37. First Implementation Slice

Do not implement all reconstruction rules at once.

Start with:

# M7.1 — Explicit Rectangle Dimensions

Deliver only:

- reconstruction analysis boundary
- `ReconstructionPlan`
- explicit source linear dimension → native Parameter
- explicit source dimension → native Length constraint
- deterministic Parameter naming
- rectangle fixture with Width and Height
- native Width/Height binding
- import → reconstruct → solve
- save/load
- Width edit after reload
- mutation tests for wrong Parameter binding
- no Circle reconstruction yet
- no general inference engine yet

Required M7.1 gate:

```text
DXF rectangle
→ M6 import
→ M7 reconstruct Width/Height
→ Solved, DOF 0
→ save
→ fresh load
→ Width 100 → 120
→ Pad volume = 120000 mm³
```

Only after M7.1 is independently green should broader reconstruction rules be added.

---

## 38. Recommended Implementation Sequence

```text
M7.1 — explicit rectangle Width/Height reconstruction
M7.2 — Horizontal / Vertical / Coincident recognition
M7.3 — Radius / Diameter reconstruction
M7.4 — ambiguity + provenance
M7.5 — idempotence + repeated reconstruction
M7.6 — transaction / rollback / adversarial cases
M7.7 — persistence + fresh-process + source independence
M7.8 — UI reconstruction workflow
M7.9 — self-validation + mutation audit
M7.10 — independent review + close
```

Each slice should be independently buildable and testable.

Avoid combining several unverified semantic changes into one large fix.

---

## 39. Suggested Branch / Commit Strategy

Start from the accepted M6 master commit:

```bash
git checkout master
git pull
git checkout -b m7-wip
```

Recommended first commit:

```text
M7.1: reconstruct explicit rectangle dimensions
```

Later commits should remain narrow.

---

## 40. M8 Readiness Contract

M7 should leave the project with:

- imported native 2D geometry
- stable identities
- reconstructed dimensional Parameters
- reconstructed geometric constraints
- editable parametric Sketches
- verified 2D dimension → 3D rebuild path
- save/load independence from source DXF

A natural M8 may then build on this foundation for a broader interactive Sketch editing workflow, multi-view/drawing association, or another owner-selected milestone.

M7 must not silently pre-decide M8 scope.

---

## 41. Initial M7 Status

At milestone start:

- M6 baseline: required and assumed accepted before M7 implementation
- M7 branch: to be created
- reconstruction architecture: NOT IMPLEMENTED
- explicit dimension mapping: NOT IMPLEMENTED
- geometric reconstruction: NOT IMPLEMENTED
- rectangle release proof: NOT EXECUTED
- circle reconstruction: NOT EXECUTED
- ambiguity handling: NOT EXECUTED
- idempotence: NOT EXECUTED
- transaction rollback: NOT EXECUTED
- M7 owner UI validation: NOT EXECUTED
- M7 independent review: NOT EXECUTED

No M7 gate may be marked PASS until it has actually been executed and evidence is recorded.
