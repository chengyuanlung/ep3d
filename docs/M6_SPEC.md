# M6 — DXF Import to Stable Sketch Entities

## 1. Mission

M6 adds deterministic DXF import into the existing ParametricCAD document model.

The imported result must become normal, stable `Sketch` entities using the identity and persistence model established by M5.

M6 is not a DXF viewer.

M6 is not a second geometry model.

M6 is not an automatic constraint or dimension reconstruction milestone.

The core release proof is:

> Import a supported DXF file, create a Sketch containing stable semantic entities, save the PartDocument, reload it in a fresh document/backend, and obtain the same Sketch geometry and entity identity without depending on DXF parser runtime state, array position, pointer identity, or OCCT topology.

---

## 2. Baseline

M6 starts from the accepted M5 master state.

M5 provides:

- stable Sketch identity
- stable `SketchEntityId`
- stable `SketchConstraintId`
- semantic sub-element references
- persistence and reload
- parameter-driven constrained Sketch solving
- downstream Pad/recompute support
- Core isolation from Qt, OCCT and Eigen

M6 must preserve these contracts.

---

## 3. Scope

### Required

M6 shall import at least these DXF geometry types:

- `LINE`
- `CIRCLE`
- `ARC`

Optional additional entity support may be added only after the required entities are complete and independently verified.

DXF geometry shall be converted into existing ParametricCAD Sketch entities.

Each imported Sketch entity shall receive a normal stable `SketchEntityId`.

Imported entities shall behave exactly like entities created through the native document API after import.

Imported data shall survive:

1. import
2. recompute
3. save
4. close
5. fresh load
6. subsequent edit/recompute

---

## 4. Explicitly Out of Scope

The following are NOT required by M6:

- automatic dimensional constraint reconstruction
- automatic geometric constraint reconstruction
- full interactive production Sketcher
- DXF export
- DWG import
- Assembly
- CAM
- FEA
- persistent OCCT subshape naming

DXF dimensions, annotations, text, hatches, blocks or splines must not silently become constraints unless explicitly added to this specification later.

Unsupported DXF entities must be reported, skipped according to a documented policy, or cause a documented import failure.

They must never be silently misinterpreted as another entity type.

---

## 5. Architectural Rule

DXF is an input format, not part of the permanent document model.

Required flow:

```text
DXF file
    ↓
DXF parser / reader
    ↓
format-neutral import representation
    ↓
M6 importer
    ↓
PartDocument / Sketch API
    ↓
Sketch entities with stable IDs
```

Forbidden architecture:

```text
DXF object
    ↓
stored directly inside Sketch
```

The persisted PartDocument must not require:

- DXF parser objects
- DXF handles as runtime identity
- parser pointers
- parser array indices
- DXF library-specific types
- Qt types
- OCCT topology

DXF-related third-party types must not cross into `src/Core`.

---

## 6. Import Boundary

Define an explicit import boundary.

Suggested shape:

```cpp
struct ImportedLine2D
{
    double x1;
    double y1;
    double x2;
    double y2;
};

struct ImportedCircle2D
{
    double cx;
    double cy;
    double radius;
};

struct ImportedArc2D
{
    double cx;
    double cy;
    double radius;
    double startAngle;
    double endAngle;
};
```

The exact type names are implementation-defined.

The important requirement is that the boundary contains only semantic geometry and import metadata required by M6.

It must contain no parser-owned pointer or backend-owned identity.

---

## 7. Identity Contract

DXF entity order is not ParametricCAD identity.

After import:

- every Sketch receives a normal document object ID
- every entity receives a `SketchEntityId`
- references use those IDs
- vector position is not identity
- DXF file offset is not identity
- DXF handle is not the runtime identity of a Sketch entity
- memory address is not identity

A DXF handle may optionally be preserved as informational import metadata if useful, but correctness must not depend on it.

Save/load must preserve ParametricCAD identity, not recreate identity from DXF ordering.

---

## 8. Coordinate and Unit Policy

M6 must define explicitly:

- DXF unit interpretation
- conversion into ParametricCAD millimetres
- XY axis convention
- angle direction
- angle zero direction
- ARC start/end semantics
- handling of non-finite values
- handling of extremely small geometry

There must be one unit-conversion boundary.

Internal Sketch geometry remains in the existing ParametricCAD unit convention.

No caller may independently guess or duplicate DXF unit conversion.

Unknown or unitless DXF files must follow one documented deterministic policy.

---

## 9. Geometry Semantics

### LINE

DXF start/end coordinates shall map to one native Sketch line entity.

### CIRCLE

DXF centre and radius shall map to one native Sketch circle entity.

Radius must be finite and strictly valid under existing Sketch invariants.

### ARC

DXF centre, radius, start angle and end angle shall map to one native Sketch arc entity.

ARC orientation and wrap-around behaviour must be explicitly defined and geometrically tested.

Tests must measure resulting geometry.

A test that merely reproduces the importer's own conversion formula is insufficient evidence.

---

## 10. Transaction Policy

Import must have a documented failure policy.

Preferred M6 contract:

> Import is transactional at the Sketch creation/import-operation level.

If a fatal import error occurs:

- no half-created imported Sketch may remain
- no orphan registry entries may remain
- no graph nodes may remain
- no dangling entity IDs may remain

For unsupported but non-fatal entities, the importer may continue only if the policy is explicit and diagnostics identify what was skipped.

---

## 11. Diagnostics

Import result must provide useful structured information.

At minimum:

- success/failure
- source filename
- number of entities imported
- number skipped
- unsupported entity kinds encountered
- invalid entities encountered
- useful error message

Diagnostics must distinguish:

- malformed DXF
- unsupported entity
- invalid geometry
- unsupported/unknown units
- document insertion failure

No generic `Import failed` should be the only available diagnostic when the actual cause is known.

---

## 12. Persistence Rule

After successful import and PartDocument save:

The resulting document must be self-contained.

Reload must not require the original DXF file.

This sequence must pass:

```text
DXF
→ Import
→ Sketch
→ Save
→ delete/rename original DXF
→ Load
→ Sketch remains valid
```

Persisted geometry must use the existing PartDocument/Sketch schema rather than embedding the DXF as the authoritative model.

---

## 13. Determinism

Importing identical DXF input under identical policy must produce geometrically equivalent Sketches.

Tests shall verify:

- same entity count
- same entity kinds
- same geometry
- deterministic ordering policy where ordering is externally observable

Stable document IDs do not need to numerically equal those from a separate import operation unless the architecture explicitly guarantees that.

Identity stability is required across save/load of one imported document, not necessarily between two unrelated import operations.

---

## 14. Existing M5 Contracts Must Survive

M6 must not regress:

- stable Sketch entity identity
- stable constraint references
- save/load
- recompute
- solver operation
- Pad generation
- mass properties
- deletion safety
- Core dependency isolation
- Debug/Release test correctness

Existing M0–M5 tests remain mandatory.

---

## 15. Required Test Fixtures

Do not rely only on large real-world DXF files.

Create small hand-verifiable fixtures.

### Fixture A — line

One known line:

- start `(0, 0)`
- end `(100, 50)`

### Fixture B — circle

One known circle:

- centre `(25, 30)`
- radius `10`

### Fixture C — arc

One known arc with deliberately nontrivial start/end angles.

Avoid only using 0°, 90° or 180° cases.

### Fixture D — mixed

At least:

- multiple lines
- one circle
- one arc

Entity ordering must be deliberately interleaved.

### Fixture E — unsupported entity

Contains at least one unsupported entity kind.

Expected policy must be tested.

### Fixture F — malformed/invalid geometry

Exercise parser/import error handling.

### Fixture G — units

At least two unit interpretations producing analytically known millimetre geometry.

---

## 16. Release Gates

### Gate A — LINE import

Import a known DXF LINE.

Verify native Sketch coordinates against a hand-computed oracle.

PASS requires exact/tolerance-correct geometry.

---

### Gate B — CIRCLE import

Import known centre/radius.

Verify centre and radius geometrically.

---

### Gate C — ARC import

Import a nontrivial arc.

Verify:

- centre
- radius
- start direction
- end direction
- orientation

using independent geometric measurement.

---

### Gate D — Mixed file

Import a DXF containing LINE + CIRCLE + ARC.

Verify:

- correct entity count
- correct entity kinds
- unique stable IDs
- no accidental dependence on entity array position

---

### Gate E — Save/load identity

Import mixed file.

Record all resulting `SketchEntityId`s and geometry.

Save.

Load into a fresh document/backend.

Verify:

- same IDs
- same entity references
- same geometry
- same entity kinds

---

### Gate F — Source independence

Import DXF.

Save PartDocument.

Remove original DXF from availability.

Reload PartDocument successfully.

The imported Sketch remains complete and usable.

---

### Gate G — Downstream geometry

Import a supported closed profile composed of DXF entities.

Use that Sketch through the existing downstream profile/Pad path.

Verify the resulting solid against an independent analytical oracle.

This gate proves the importer creates real native Sketch geometry rather than display-only DXF geometry.

---

### Gate H — Invalid/unsupported input

Exercise unsupported and malformed input.

Verify:

- documented result
- useful diagnostics
- no corrupt partial document
- no dangling registry/graph state

---

### Gate I — Regression

All pre-M6 tests pass in:

- Debug
- Release

Release test execution must be proven to invoke Release binaries.

---

## 17. Adversarial Tests

At minimum test:

- zero-length LINE
- zero/negative CIRCLE radius if representable by malformed input
- invalid ARC radius
- ARC crossing 0°
- ARC near 360°
- duplicate DXF handles
- missing DXF handles
- shuffled DXF entity order
- very small coordinates
- very large coordinates
- NaN / Infinity if parser layer can expose them
- unsupported entity between supported entities
- import failure after several valid entities were already parsed
- save/load after import
- repeated import into the same document
- deleting the imported Sketch
- recompute after deletion
- fresh-process load

---

## 18. Mutation Verification

For release-critical claims, tests must demonstrate that they discriminate.

Examples:

- swap ARC start/end → Gate C must fail
- omit unit conversion → unit test must fail
- assign duplicate `SketchEntityId` → identity test must fail
- skip one imported entity → mixed-file gate must fail
- retain parser ordering as identity → reorder test must fail
- break transaction rollback → failure-state test must fail

A green test whose corresponding production defect can be restored without causing failure is not release evidence.

---

## 19. UI Scope

Minimal UI integration only.

Required user workflow:

1. invoke DXF import
2. select a DXF file
3. import into a Sketch
4. imported Sketch appears in the existing model tree
5. imported geometry is visible using the normal Sketch/document presentation path
6. useful failure diagnostic is visible when import fails

M6 does not require a full Sketch editing UI.

---

## 20. Dependency / Licence Rule

Any DXF parser dependency must be reviewed before adoption.

Record:

- library
- exact version
- licence
- static/dynamic/header-only usage
- source files/targets in which it appears
- whether it crosses the Core boundary

Prefer permissive dependencies where practical.

No new GPL dependency may be introduced into the commercial application without an explicit owner decision.

---

## 21. Required ADRs Before Completion

At minimum:

- ADR-M6-001 — DXF parser / dependency selection
- ADR-M6-002 — DXF unit policy
- ADR-M6-003 — import architecture and Core boundary
- ADR-M6-004 — entity identity policy
- ADR-M6-005 — unsupported entity policy
- ADR-M6-006 — transactional failure/rollback policy
- ADR-M6-007 — ARC orientation and angle convention

Additional ADRs shall be created when implementation discovers a design decision that would otherwise live only in code.

---

## 22. Self-Validation Rules

The implementation agent must not claim completion based only on unit tests.

Before requesting independent review it must provide:

- clean Debug build
- clean Release build
- complete Debug test total
- complete Release test total
- proof Release tests actually executed Release binaries
- Gates A–I results
- regression results
- mutation verification evidence
- dependency/licence audit
- Core boundary audit
- persistence/fresh-load evidence
- list of known limitations
- list of unsupported DXF entities
- exact commit hash

Any test not executed must say `NOT EXECUTED`.

No unexecuted item may be reported as PASS.

---

## 23. Independent Review

Independent reviewers must treat the self-validation report as claims to verify.

At least these areas require adversarial review:

1. DXF semantic correctness and units
2. identity / persistence / fresh-load behaviour
3. ARC semantics
4. rollback and malformed input
5. architecture / dependency boundary
6. downstream Pad integration
7. tests that may merely duplicate production formulas

A suspected defect should be demonstrated by execution where practical.

A fix introduced by review must receive a regression test, and that test should be mutation-verified.

---

## 24. Definition of Done

M6 is complete only when:

- required DXF entities import correctly
- imported entities are native Sketch entities
- stable identity survives save/load
- the original DXF is no longer required after save
- mixed import works
- units are deterministic
- ARC semantics are verified geometrically
- invalid input cannot corrupt the document
- an imported closed profile drives downstream 3D geometry
- all M0–M5 regressions pass
- Debug and Release are independently verified
- dependency licences are recorded
- independent review has no unresolved Critical or accepted Major findings
- owner manual UI validation passes the M6 import workflow

---

## 25. First Implementation Slice

Do not implement the entire milestone at once.

Start with:

**M6.1 — Parser boundary + LINE only**

Deliver:

- DXF reader abstraction
- format-neutral import representation
- LINE conversion
- stable Sketch entity creation
- unit-policy skeleton
- one tiny DXF fixture
- import → Sketch test
- import → save → fresh-load test
- Core dependency boundary test

Only after M6.1 is green should CIRCLE and ARC be added.

Recommended sequence:

- M6.1 — infrastructure + LINE
- M6.2 — CIRCLE
- M6.3 — ARC
- M6.4 — mixed files + units + unsupported entities
- M6.5 — transaction/error handling
- M6.6 — downstream Pad gate
- M6.7 — UI import workflow
- M6.8 — adversarial/self-validation
- M6.9 — independent review and close

---

## 26. Branch / Commit Strategy

Create the milestone branch from the accepted M5 master state:

```bash
git checkout master
git pull
git checkout -b m6-wip
```

The first implementation commit should remain narrow:

```text
M6.1: establish DXF import boundary and LINE import
```

Do not combine parser selection, all entity types, UI, and transaction handling into one initial commit.

The DXF parser decision shall be recorded first in `ADR-M6-001`.

---

## 27. Initial M6 Status

At milestone start:

- M5 implementation baseline: available
- M6 branch: to be created
- DXF parser: not yet selected
- DXF licence decision: OPEN
- M6.1 implementation: NOT STARTED
- M6 release gates: NOT EXECUTED
- M6 owner UI validation: NOT EXECUTED
- M6 independent review: NOT EXECUTED

No M6 gate may be marked PASS until it has actually been executed and its evidence recorded.
