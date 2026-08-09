# EP3D M5 — Sketch Constraints & Dimensional Parameterization

**Prerequisite:** M4 accepted by project owner  
**Stack:** C++20, CMake, GoogleTest, OCCT behind Kernel boundary, Qt only in App/UI  
**Execution:** Codex reads `AGENTS.md` first and preserves M0–M4 architecture.

## 1. Mission

M5 turns the M4 Sketch into a genuinely parametric 2D sketch:

```text
Sketch Entities + Constraints + Parameters
        ↓
Solver-neutral constraint system
        ↓
Solved Sketch + DOF/status
        ↓
Profile
        ↓
Pad
        ↓
3D rebuild
```

Central proof:

```text
change 2D dimension → solve Sketch → rebuild Profile/Pad → 3D changes correctly
```

This is the architectural bridge to future DXF dimension reconstruction.

## 2. Definition of Done

M5 requires:

- stable persistent `SketchConstraintId`;
- references use M4 `SketchEntityId` / stable sub-element references;
- no persistent vector indexes, raw pointers, Qt objects, OCCT topology, or solver variable IDs;
- solver-neutral interface;
- Coincident, Horizontal, Vertical, Distance, Length, Radius, Diameter, Angle, Fix;
- dimensional constraints bind to existing Parameters;
- explicit length/angle unit rules;
- meaningful Solved/UnderConstrained/OverConstrained/Conflicting/InvalidInput/NumericalFailure semantics, with conservative mapping if backend cannot distinguish some cases;
- Sketch-level DOF, including DOF=0 for a fully constrained reference sketch;
- transactional solve/commit;
- conflict/failure cannot silently commit corrupted geometry;
- deterministic recovery;
- solved Sketch feeds M4 Profile/Pad/MassProperties;
- Width/Height/Radius changes propagate correctly;
- PadLength does not unnecessarily solve Sketch;
- Density changes Mass only;
- unrelated branches remain untouched;
- constraints and Parameter references serialize/reload;
- runtime solver state is not persistent identity;
- minimal Qt UI exposes solve state, DOF, dimensions and useful conflicts;
- all M0–M4 regression tests pass;
- mandatory Gates A–H pass;
- SelfValidation report contains actual evidence;
- review status is recorded honestly.

## 3. Non-Goals

Do not implement DXF/DWG, automatic dimension recognition, production Sketcher UI, splines, Assembly constraints, 3D constraints, persistent OCCT subshape naming, CAM, FEA or dynamics. Tangent/equal/symmetry may only be optional extensions after mandatory gates pass.

## 4. Architecture

```text
Core semantic Sketch/Constraints
        ↓
neutral SketchSolveProblem
        ↓
ISketchSolver
        ↓
solver implementation
        ↓
neutral SketchSolveResult
        ↓
atomic solved-geometry commit
        ↓
M4 Profile → Pad → Mass
```

Backend-specific types must not leak into persistent Core semantics.

## 5. Constraint Identity / References

Introduce `SketchConstraintId`.

Future-safe references remain semantic:

```cpp
struct SketchElementRef {
    SketchEntityId entityId;
    SketchSubElement subElement;
};
```

Example:

```text
C10 Horizontal(E10)
C20 Coincident(E10.End,E20.Start)
C30 Length(E10, WidthParameter)
```

Entity/constraint insertion, deletion or reordering must not alter unrelated identity.

**ADR-M5-001:** Constraint Identity and Reference Model.

## 6. Constraint Semantics

Minimum conceptual types:

```text
Coincident
Horizontal
Vertical
Distance
Length
Radius
Diameter
Angle
Fix
```

Invalid reference cardinality/type combinations must fail validation.

Recommended type-safe semantic objects are preferred over an unvalidated generic property bag.

## 7. Parameter Binding / Units

Reuse the existing Parameter architecture; do not create a second scalar system.

```text
Width Parameter → Length constraint → Sketch solve
```

Rules:

```text
length/radius/diameter/distance: mm
angles: radians internally
UI angle display: degrees is acceptable
```

Negative/non-finite geometric dimensions are invalid. Zero-value policies must be explicit.

**ADR-M5-002:** Dimensional Constraint Units and Parameter Binding.

## 8. Solver Boundary

Conceptually:

```cpp
class ISketchSolver {
public:
    virtual ~ISketchSolver() = default;
    virtual SketchSolveResult solve(const SketchSolveProblem&) = 0;
};
```

Architect must document backend choice, version, license, numerical characteristics, dependency impact and replacement boundary.

**ADR-M5-003:** Sketch Solver Selection and Boundary.

## 9. Transactional Solve

```text
validate semantic refs/Parameters
→ create temporary solve problem
→ solve
→ finite/result/status validation
→ calculate DOF
→ atomically commit solved geometry
→ dirty Profile/Pad downstream
```

Never commit NaN/Inf/partial geometry. On failure, downstream current results cannot falsely report success. Recovery after correction must be deterministic.

**ADR-M5-004:** Solver Commit, Failure and Recovery Policy.

## 10. DOF / Status

At minimum expose Sketch DOF.

Examples:

```text
free 2D point ≈ 2 DOF
fixed point = 0
fully constrained reference rectangle = 0
```

Do not fake DOF using only `variables - constraint count`; rank/dependency matters.

Statuses:

```text
Solved
UnderConstrained
OverConstrained
Conflicting
InvalidInput
NumericalFailure
```

If backend cannot reliably separate conflict/overconstraint, document conservative semantics.

**ADR-M5-005:** DOF and Constraint Status Semantics.

## 11. Mandatory Constraint Behavior

- **Coincident:** point/endpoint to point/endpoint.
- **Horizontal:** line endpoints have equal v within tolerance.
- **Vertical:** line endpoints have equal u.
- **Distance:** point-to-point dimension.
- **Length:** line length dimension.
- **Radius:** Circle mandatory; Arc strongly recommended.
- **Diameter:** drives the same underlying radius geometry, not duplicate state.
- **Angle:** Line-to-Line; document sign/range/orientation, radians internally.
- **Fix:** stable semantic point/entity fixing according to ADR.

**ADR-M5-006:** Angle Constraint Convention.

## 12. Numerical Policy

Document:

```text
length residual tolerance
angular residual tolerance
convergence tolerance
iteration limit
finite-value checks
scale assumptions
```

Do not enlarge tolerances merely to pass tests.

## 13. Recompute Graph

```text
Width/Height/etc Parameters
        ↓
Sketch solve → Profile → Pad → Mass

PadLength ----------------> Pad → Mass
Density ------------------------> Mass
```

Required:

- Width/Height/Radius edit: Sketch + downstream affected branch.
- PadLength: Sketch solve count unchanged; Pad + Mass.
- Density: Sketch/Pad unchanged; Mass only.
- unrelated Parameter: none of branch.

Use counters/logs in tests.

## 14. Fully Constrained Rectangle Reference

Conceptually:

```text
E1 bottom
E2 right
E3 top
E4 left

Coincident all four corners
Horizontal(E1,E3)
Vertical(E2,E4)
Fix(E1.Start)
Length(E1, Width)
Length(E2, Height)
```

For Width=100, Height=50:

```text
DOF=0
valid closed Profile
```

It must feed M4 Pad.

## 15. Circle Reference

```text
Circle E10
Fix(center)
Radius(E10, RadiusParameter)
```

Changing Radius must solve and rebuild downstream cylinder-like Pad.

## 16. Under-Constrained / Conflict / Redundancy

Under-constrained:

```text
finite valid geometry
status UnderConstrained
DOF > 0
```

Recommended: valid under-constrained geometry may feed downstream Profile while UI clearly reports remaining DOF.

Conflict examples:

```text
Length(E1)=100 and Length(E1)=120
Radius=10 and Diameter=30
incompatible fixed endpoints + Horizontal
```

Must not corrupt/commit invalid geometry.

Redundant but consistent constraints are distinct from contradictory constraints. Architect must document whether backend accepts, reports or rejects redundancy; tests lock behavior.

## 17. Persistence / Deletion

Persist:

```text
ConstraintId
type
SketchElementRef targets
Parameter ObjectId
semantic metadata/state
```

Never persist solver variable/Jacobian indexes, backend pointers/caches, Qt objects or OCCT topology as identity.

After load: resolve refs → rebuild dependencies → solve → rebuild Profile/Pad.

Mandatory safety tests:

- delete referenced entity;
- delete referenced Parameter;
- delete constraint;
- missing reference on load;
- reorder entities/constraints.

Choose deterministic reject-or-mark-invalid deletion policy. No dangling references.

## 18. Minimal UI

M5 UI must at least show:

```text
Sketch solve status
DOF
constraint list
dimension value/unit
failed/conflicting constraint IDs
```

Editing a Parameter-backed dimension through the existing property mechanism must update the 3D result. Viewer refreshes after successful recompute. State must not rely on color alone.

## 19. Mandatory Tests

### Identity
- stable ConstraintId;
- duplicate ID rejected;
- refs survive reorder;
- invalid/missing entity/sub-element/Parameter refs handled.

### Constraints
- Coincident, Horizontal, Vertical, Distance, Length, Radius, Diameter, Angle, Fix;
- already-satisfied cases;
- incompatible fixed cases;
- invalid/non-finite dimensions.

### Solver
- simple solve;
- fully constrained rectangle;
- under-constrained;
- redundant case per policy;
- conflict;
- finite checks;
- repeated deterministic solve;
- random entity/constraint order equivalence;
- failure/recovery;
- save/load/re-solve.

### Architecture
Scan Core for real dependencies:

```text
QObject QWidget QGraphics QString
TopoDS_ BRep AIS_ V3d_
```

Audit persistence for pointer/index/backend-variable/topology identity.

## 20. Independent Analytical Oracles

Do not use production solver helpers to create expected values.

Rectangle:

```text
Width=100, Height=50, Pad=20
Volume=100000 mm^3
COM=(50,25,10) for reference placement
```

Width=120:

```text
Volume=120000 mm^3
COM=(60,25,10)
```

Circle:

```text
R=10, Pad=30
Volume=pi*10^2*30
```

R=20 produces four times the volume.

## 21. Release Gate A — Fully Constrained Rectangle

Parameters:

```text
Width=100 mm
Height=50 mm
PadLength=20 mm
Density=2700 kg/m^3
```

Expected:

```text
Sketch solved
DOF=0
Profile valid
Volume=100000 mm^3
Mass=0.27 kg
COM=(50,25,10) mm
```

If Fix convention changes placement, analytical COM must follow the documented convention.

## 22. Gate B — Core Product Proof

Change:

```text
Width 100 → 120 mm
```

Expected:

```text
Sketch solver recomputes
Width constraint satisfied
DOF=0
Profile valid
Pad recomputes
Volume=120000 mm^3
Mass=0.324 kg
Viewer refreshes
```

This gate is release-critical.

## 23. Gate C — Height Edit

With Width=120, Pad=20:

```text
Height 50 → 80
Volume=192000 mm^3
Mass=0.5184 kg at density 2700
```

Profile remains valid.

## 24. Gate D — Selective Recompute

PadLength 20→30:

```text
Sketch solve unchanged
Pad +1
Mass +1
```

Density 2700→7850:

```text
Sketch unchanged
Pad unchanged
Mass +1
```

Unrelated Parameter: all unchanged.

## 25. Gate E — Conflict / Recovery

Add contradictory dimensions to same line.

Expected:

```text
documented conflict/overconstraint status
no corrupt geometry commit
downstream current result blocked
useful constraint diagnostic
```

Remove/correct conflict and prove full recovery.

## 26. Gate F — Circle Dimension

Fixed-center Circle, R=10, Pad=30; validate analytical volume.

Change R 10→20:

```text
Sketch solves
Pad length unchanged
volume ×4
downstream recompute correct
```

## 27. Gate G — Save / Load / Re-solve

Persist/reload rectangle and Circle.

Verify Sketch/Entity/Constraint IDs, types, sub-element refs, Parameter bindings, frame and Pad refs. Re-solve to equivalent geometry/Volume/Mass/COM without backend runtime identity.

## 28. Gate H — Under-Constrained

Create intentional free DOF:

```text
UnderConstrained
DOF > 0
finite valid geometry
UI exposes status/DOF
```

Add constraints until:

```text
DOF=0
fully constrained/solved
```

## 29. Self-Validation

Create:

`docs/reviews/M5_SelfValidationReport.md`

Actually execute:

1. baseline/architecture inspection;
2. Core Qt/OCCT scan;
3. identity/backend-leak audit;
4. clean Debug build + full tests;
5. clean Release build/tests where supported;
6. complete constraint matrix;
7. deterministic/order-randomization solver tests;
8. DOF/status tests;
9. analytical rectangle/circle oracles;
10. recompute selectivity;
11. conflict injection/recovery;
12. deletion/broken refs;
13. serialization/re-solve;
14. M0–M4 regression;
15. minimal UI status/dimension smoke;
16. clean rebuild;
17. self-score.

Unexecuted tests must never be reported PASS.

## 30. Adversarial Tests

```text
duplicate ConstraintId
missing EntityId/ParameterId
wrong sub-element
NaN/Infinity
negative length/radius
zero degeneracy
contradictory dimensions
redundancy
random entity order
random constraint order
delete constrained entity
delete dimension Parameter
failure then recovery
very small / large reasonable geometry
angle near 0 / pi
repeat solve 100x for drift
save/load/re-solve
```

## 31. Self-Validation Report

```text
# M5 Self-Validation Report
Baseline:
Final/Working Commit:

## Environment
OS/Compiler/CMake/OCCT/Qt/Solver:

## Architecture
Core Qt:
Core OCCT:
Backend leakage:
Identity audit:

## Builds / Regression
Debug:
Release:
M0-M4:
Totals:

## Constraint Matrix
Coincident:
Horizontal:
Vertical:
Distance:
Length:
Radius:
Diameter:
Angle:
Fix:

## Solver
Fully constrained:
Under constrained:
Conflict:
Redundant:
DOF:
Determinism:
Order independence:

## Oracles
Rectangle:
Circle:

## Recompute
Dimension:
PadLength:
Density:
Unrelated:

## Failure / Recovery
Conflict:
Broken refs:
Deletion:
Recovery:

## Persistence
IDs/refs/Parameters/re-solve/backend identity:

## UI Smoke
Status/DOF/edit/conflict/view refresh:

## Gates
A:
B:
C:
D:
E:
F:
G:
H:

## Findings
Critical:
Major:
Minor:

## Self Score
XX/100
## Ready for Review
YES/NO
```

## 32. Independent Review Scorecard

```text
Architecture / solver boundary       15
Stable identity / references         12
Constraint semantic correctness      15
Numerical solver correctness         15
DOF / status / diagnostics           10
Parametric recompute                 12
Failure / recovery                    7
Persistence                           6
UI functional validation              3
Tests / docs / evidence               5
                                    ---
                                    100
```

90–100 APPROVE; 80–89 APPROVE WITH MINOR FOLLOW-UP; below 80 REQUEST CHANGES.

Automatic REQUEST CHANGES for Critical findings, unresolved Major architecture issues, Core Qt/OCCT leakage, persistent index/pointer/backend identity, incorrect 2D→3D dimension propagation, invalid conflict commit, broken constraint persistence, regression failure, failed Gate A–H, or fabricated evidence.

Critical examples:

```text
constraint targets wrong entity after reorder/load
dimension edit changes wrong entity
NaN/Inf committed
conflict silently produces wrong solid
dangling reference/use-after-free
unit mismatch changes physical geometry
```

## 33. Required ADRs

```text
ADR-M5-001 Constraint Identity and Reference Model
ADR-M5-002 Dimensional Constraint Units and Parameter Binding
ADR-M5-003 Sketch Solver Selection and Boundary
ADR-M5-004 Solver Commit, Failure and Recovery Policy
ADR-M5-005 DOF and Constraint Status Semantics
ADR-M5-006 Angle Constraint Convention
```

## 34. Completion Report

Create `docs/reviews/M5_CompletionReport.md` containing baseline/final commit, solver/version/license, ADRs, changed files, old/new test totals, Debug/Release evidence, Gates A–H, SelfValidation, review availability/decision/score, owner manual validation if any, limitations/deferred constraints, exact M5 status and M6 readiness.

Never call owner/manual validation an independent agent review.

## 35. Implementation Order

```text
M5-A inspect accepted M4 contracts
M5-B ConstraintId + semantic model
M5-C Parameter binding/units
M5-D neutral solver problem/result
M5-E solver backend + ADR
M5-F Coincident/Horizontal/Vertical/Fix
M5-G Distance/Length/Radius/Diameter/Angle
M5-H DOF/status/diagnostics
M5-I transactional solve
M5-J M2 recompute integration
M5-K M4 Profile/Pad integration
M5-L persistence
M5-M minimal UI
M5-N adversarial/self-validation
M5-O review/fixes
M5-P docs/final commit
```

## 36. Recommended M6 / M7

```text
M6 — DXF Import & Semantic Entity Mapping
LINE/ARC/CIRCLE → stable Sketch entities
units/layers/source metadata → profile candidates

M7 — Dimension / Constraint Reconstruction
DXF/drawing dimensions → M5 constraints/Parameters
```

Target product flow:

```text
DXF 2D dimension edit
→ constrained Sketch solve
→ Profile
→ feature rebuild
→ updated 3D
```

## 37. Codex Master Prompt

```text
Implement EP3D Milestone M5 using the project-owner-accepted M4 state as baseline.

Read AGENTS.md first and all M0-M4 architecture/ADR/review/completion documents, then:
docs/M5_Implementation_SelfValidation_and_Evaluation.md

Mission:
Turn M4 Sketch into a parameter-driven constrained Sketch whose 2D dimension edits automatically rebuild downstream 3D geometry.

Mandatory:
stable SketchConstraintId; stable SketchEntityId/SketchElementRef references; Coincident, Horizontal, Vertical, Distance, Length, Radius, Diameter, Angle, Fix; existing Parameter binding; solver-neutral interface and documented backend/license; transactional solve; meaningful status and DOF; fully/under-constrained cases; deterministic conflict/recovery; dimension→Sketch→Profile→Pad→3D; PadLength/density/unrelated recompute selectivity; constrained Circle radius edit; semantic persistence/re-solve; Core free of Qt/OCCT/backend semantic leakage; all M0-M4 regressions; Gates A-H; actual SelfValidation evidence; honest review status; CompletionReport.

Do not implement DXF, automatic dimension reconstruction, full production Sketcher, Assembly, persistent OCCT subshape naming, CAM, FEA or dynamics.

Before deep implementation Architect must document ADR-M5-001..006.

Never persist vector indexes, pointers, solver variable IDs, Qt objects or OCCT topology as constraint identity.

Central release proof:
Width Parameter 100→120 mm must solve the constrained rectangle and automatically rebuild Pad so Volume changes 100000→120000 mm^3 for Height=50 and Pad=20.

Do not declare M5 complete unless regressions and Gates A-H pass and no Critical/unresolved Major remains.

Finally report exact commit, test totals, review status and M6 readiness.
```

## 38. Final Gate

```text
[ ] stable ConstraintId / SketchElementRef
[ ] no index/pointer/backend identity persistence
[ ] Coincident / Horizontal / Vertical
[ ] Distance / Length
[ ] Radius / Diameter
[ ] Angle / Fix
[ ] existing Parameter binding
[ ] units documented
[ ] neutral solver API
[ ] backend/version/license documented
[ ] transactional finite solve
[ ] UnderConstrained + DOF>0
[ ] fully constrained + DOF=0
[ ] conflict/redundancy policy
[ ] constrained rectangle
[ ] constrained Circle
[ ] Width/Height/Radius edit updates 3D
[ ] PadLength does not solve Sketch
[ ] Density does not solve Sketch/Pad
[ ] unrelated branch untouched
[ ] conflict blocks invalid downstream
[ ] recovery
[ ] broken-reference/deletion safety
[ ] semantic serialization
[ ] load/re-solve equivalent
[ ] Core free Qt/OCCT/backend leakage
[ ] UI status/DOF/dimension/conflict
[ ] M0-M4 regression
[ ] Debug + Release tests where supported
[ ] Gates A-H
[ ] ADR-M5-001..006
[ ] M5_SelfValidationReport
[ ] review status honestly recorded
[ ] no Critical / unresolved Major
[ ] M5_CompletionReport
[ ] exact final commit

Only then:
M5 = COMPLETE
M6 = READY
```
