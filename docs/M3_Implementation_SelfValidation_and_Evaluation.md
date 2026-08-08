# EP3D M3 — Geometry Kernel Adapter & First Parametric Solid

**Baseline:** M2 commit `8245c89`  
**Stack:** C++20, CMake, GoogleTest, OpenCASCADE (OCCT)  
**Execution:** Codex using the repository `AGENTS.md` orchestration rules.

## 1. Mission

M3 introduces real CAD geometry while preserving the architectural boundary established in M0–M2.

Required pipeline:

```text
Parameter
  -> DependencyGraph / DocumentRecomputeEngine
  -> Parametric Feature
  -> kernel-neutral geometry API
  -> OCCT adapter
  -> exact B-Rep
  -> Volume / Mass / COM / Inertia
```

The primary release object is a parametric rectangular solid.

## 2. Definition of Done

M3 is complete only when:

- OCCT is integrated by CMake in a dedicated Kernel layer.
- `src/Core` neither includes nor links OCCT.
- No OCCT type appears in Core public APIs.
- A safe kernel-neutral runtime shape abstraction exists.
- OCCT creates a valid rectangular solid.
- Width, Height and Depth are Parameters participating in M2 recompute.
- Changing W/H/D rebuilds only the affected geometry branch.
- Density-only changes recompute physical properties without rebuilding geometry.
- Exact Volume, COM, Mass and inertia tensor are calculated.
- mm/m unit conversions are explicitly tested.
- Invalid dimensions fail safely and downstream current results do not falsely succeed.
- Failed geometry can recover after parameters become valid.
- Semantic inputs serialize; runtime OCCT objects do not.
- Reload reconstructs dependencies and recomputes equivalent geometry.
- All M0/M1/M2 tests still pass.
- Debug and Release builds pass where supported.
- Self-validation report exists.
- Independent review has no Critical or unresolved Major findings.

## 3. Non-Goals

Do not implement Qt viewport, DXF, full Sketch solver, Pocket, Fillet, Chamfer, Assembly, Joint, Collision, Robot, dynamics, FEA, CAM, multithreaded recompute, scripting or plugin architecture in M3.

## 4. Hard Architecture Rule

```text
Core  <---- application/domain logic
 ^
 |
Kernel-neutral interface
 ^
 |
Kernel/Occt ----> OpenCASCADE
```

Forbidden in `src/Core`:

```cpp
TopoDS_Shape
gp_Pnt
gp_Vec
BRep*
AIS_*
V3d_*
```

Core CMake target must not link OCCT.

## 5. Recommended Responsibilities

```text
src/Core/
  Feature/BoxFeature.*
  Physics/MassProperties.*
  geometry-neutral definitions only

src/Kernel/
  IGeometryKernel.*
  KernelShape.*
  KernelMassProperties.*

src/Kernel/Occt/
  OcctGeometryKernel.*
  OcctShape.*
```

Exact names may follow repository conventions. Do not create abstractions with no clear responsibility.

## 6. Required ADRs

Create at least:

- `ADR-M3-001 Geometry Kernel Boundary and Shape Ownership`
- `ADR-M3-002 Geometry and Physical Units`
- `ADR-M3-003 Kernel Service Injection`
- `ADR-M3-004 Failed Feature / Last Valid Shape Policy`
- If needed: `ADR-M3-005 Computed Geometry Persistence Policy`

### Shape identity rule

`ObjectId` is persistent identity. A `KernelShape` is runtime computed state. Never persist a pointer, address, OCCT handle identity, vector index, or raw B-Rep memory as document identity.

## 7. Kernel API

Minimum conceptual capability:

```cpp
struct BoxDefinition {
    double widthMm;
    double heightMm;
    double depthMm;
};

class IGeometryKernel {
public:
    virtual ~IGeometryKernel() = default;
    virtual ShapeResult createBox(const BoxDefinition&) = 0;
    virtual KernelMassProperties calculateMassProperties(
        const KernelShape&) = 0;
};
```

The actual API may be adapted after inspecting current ownership. Expected invalid user geometry must return controlled failure plus diagnostics rather than relying only on exceptions.

`BoxFeature` must not instantiate `OcctGeometryKernel` directly. Kernel service access must be injected explicitly through an architecture-compatible service/context boundary.

## 8. Geometry Convention

Recommended M3 convention:

```text
Box origin: (0,0,0)
Width  along +X
Height along +Y
Depth  along +Z
```

For `100 x 50 x 20 mm`, expected geometric center is `(50,25,10) mm`.

If Architect chooses another convention, document and test it before implementation.

## 9. Units — Release Critical

Document these explicitly:

```text
CAD/kernel length: mm
Geometric volume: mm^3 or clearly normalized neutral value
Density: kg/m^3
Mass: kg
COM exposed to CAD: mm
Physical inertia: kg*m^2
```

Mandatory analytical case:

```text
100 mm * 50 mm * 20 mm = 100000 mm^3
                         = 0.0001 m^3

density = 2700 kg/m^3
mass    = 0.27 kg
COM     = (50,25,10) mm
```

Production code must never multiply `kg/m^3` directly by `mm^3` without conversion.

## 10. Inertia Oracle

For uniform rectangular solid, about COM and local XYZ:

```text
Ixx = m/12 * (h^2 + d^2)
Iyy = m/12 * (w^2 + d^2)
Izz = m/12 * (w^2 + h^2)
```

Use meters in these equations to obtain `kg*m^2`.

For an axis-aligned box about COM, `Ixy`, `Ixz`, `Iyz` should be approximately zero.

Tests must calculate expected values independently; do not call production helpers to create the expected result.

## 11. Required Dependency Graph

```text
Width  ----Height -----+--> BoxFeature ---> MassProperties
Depth  ----/                        ^
                                  /
Density -------------------------/
```

Required behavior:

- Width/Height/Depth change: BoxFeature + MassProperties recompute.
- Density-only change: MassProperties recomputes; BoxFeature does NOT.
- Unrelated parameter change: neither recomputes.

This is a release gate.

## 12. Transactional Feature Recompute

Recommended behavior:

```text
read inputs
 -> validate
 -> build temporary shape
 -> validate shape
 -> calculate/prepare output
 -> commit successful result
```

Never destroy the last valid runtime result before knowing the new computation succeeded.

Architect must document whether a failed feature retains a stale last-valid shape or clears it. Recommended: retain it for future CAD UX but mark it unequivocally stale/failed so downstream consumers cannot treat it as current.

## 13. Invalid Geometry

Mandatory cases:

- Width/Height/Depth = 0
- negative dimension
- NaN
- infinity

Expected:

- feature reports Failed,
- invalid new shape is not committed,
- downstream current physical properties do not report success,
- diagnostic exists,
- no crash,
- returning to valid values permits deterministic recovery.

Define density policy too; non-finite density must fail. Decide explicitly whether zero density is valid.

## 14. Mass Properties

OCCT-specific geometric calls stay in `Kernel/Occt`. Core receives neutral numeric results.

Required:

- Volume
- Mass
- Center of Mass
- 3x3 inertia tensor

Material/density changes must not alter geometry or volume. Uniform-density material changes must not alter COM, but must alter mass and physical inertia.

## 15. Persistence

Persist semantic state:

- BoxFeature stable ObjectId
- parameter references
- material reference/assignment
- feature inputs required to reconstruct semantics

Do not persist:

- `TopoDS_Shape`
- OCCT runtime pointers/addresses
- tessellation cache
- runtime kernel handles as persistent identity

After load, rebuild runtime registry/dependencies and recompute geometry.

## 16. CMake / OCCT

OCCT discovery must be configurable; never hard-code a developer-specific absolute path.

Acceptable patterns include `find_package(OpenCASCADE ...)`, `OpenCASCADE_DIR`, `CMAKE_PREFIX_PATH`, or an explicitly documented package-manager/toolchain approach.

Document:

- tested OCCT version,
- discovery method,
- Windows runtime DLL strategy,
- Debug/Release behavior.

## 17. Mandatory Tests

### Kernel
- `M3-KERNEL-001`: valid 100x50x20 box.
- `M3-KERNEL-002`: volume matches analytical value.
- `M3-KERNEL-003`: COM matches `(50,25,10)` under corner-origin convention.
- `M3-KERNEL-004`: second asymmetric box prevents hardcoding.

### Negative
- zero dimension fails.
- negative dimension fails.
- NaN fails.
- infinity fails.
- downstream node cannot report current success after geometry failure.

### Mass
- 100x50x20, density 2700 -> `0.27 kg`.
- same box, density 7850 -> `0.785 kg`.
- volume unchanged by density.
- COM unchanged by uniform density.

### Inertia
- analytical `Ixx`, `Iyy`, `Izz`.
- off-diagonal terms approximately zero.

### Recompute
- Width change recomputes Box + Mass.
- Height change recomputes Box + Mass.
- Depth change recomputes Box + Mass.
- Density-only recomputes Mass only.
- unrelated parameter recomputes neither.
- invalid -> failure -> corrected value -> successful recovery.

### Serialization
- stable IDs survive.
- parameter/material references survive.
- no runtime OCCT state serialized.
- loaded document recomputes equivalent geometry.

### Architecture
- Core does not link OCCT.
- Core public headers do not include OCCT.
- OCCT implementation is isolated.
- BoxFeature does not construct concrete OCCT implementation.

### Regression
All existing M0/M1/M2 tests must pass. Record old baseline and new totals.

## 18. Numerical Tolerance

Do not use exact floating-point equality. Choose tight, justified absolute/relative tolerances appropriate for exact primitive geometry. Document tolerances. Do not enlarge tolerances merely to pass a failing implementation.

## 19. Mandatory Integration Release Gate

Initial:

```text
Width   = 100 mm
Height  = 50 mm
Depth   = 20 mm
Density = 2700 kg/m^3
```

Expect:

```text
Volume = 100000 mm^3
Mass   = 0.27 kg
COM    = (50,25,10) mm
```

Record recompute counters.

### A — Width -> 120 mm

Expect:

```text
BoxFeature +1
MassProperties +1
Volume = 120000 mm^3
Mass = 0.324 kg
COM = (60,25,10) mm
```

### B — Density -> 7850

Expect:

```text
BoxFeature count unchanged
MassProperties +1
Volume unchanged
Mass = 0.942 kg
COM unchanged
physical inertia changes
```

### C — Width -> 0

Expect feature failure, downstream blocked/not-current, diagnostic, no crash.

### D — Width -> 80

Expect recovery:

```text
Volume = 80000 mm^3
Mass = 0.628 kg at density 7850
```

### E — Save / load / recompute

Expect equivalent dimensions, material, Volume, Mass, COM, inertia, stable IDs and dependency behavior.

M3 cannot be declared complete if this scenario fails.

## 20. Self-Validation Protocol

Before independent review, Developer must create:

`docs/reviews/M3_SelfValidationReport.md`

Execute and record:

1. static architecture scan,
2. clean Debug configure,
3. clean Debug build,
4. full Debug tests,
5. clean Release build where supported,
6. full Release tests,
7. analytical geometry oracle,
8. analytical mass/COM/inertia oracle,
9. incremental recompute tests,
10. negative/failure injection,
11. failure recovery,
12. serialization round trip,
13. clean rebuild from scratch,
14. all M0/M1/M2 regression tests,
15. source/CMake boundary inspection,
16. mandatory release gate,
17. self-review score.

### Static scan

Actively search `src/Core` for real dependencies involving:

```text
TopoDS_
BRep
gp_
AIS_
V3d_
OpenCASCADE
QObject
QWidget
QString
```

Inspect CMake too. Explain harmless textual false positives.

## 21. Self-Validation Report Format

```text
# M3 Self-Validation Report
Baseline:
Final/Working Commit:

## Environment
OS:
Compiler:
CMake:
OCCT:
Generator:

## Architecture Boundary
Core OCCT scan:
Core Qt scan:
Core link dependencies:
Kernel link dependencies:
Result:

## Debug Build
Commands:
Result:

## Debug Tests
Total:
Passed:
Failed:

## Release Build / Tests
Commands:
Total:
Passed:
Failed:

## Analytical Oracle
Volume:
Mass:
COM:
Ixx:
Iyy:
Izz:
Result:

## Incremental Recompute
Dimension change:
Density-only:
Unrelated:
Result:

## Negative / Recovery
Zero:
Negative:
NaN:
Infinity:
Downstream:
Recovery:
Result:

## Serialization
Round trip:
Stable IDs:
Runtime shape not persisted:
Result:

## Regression
Result:

## Mandatory Release Gate
PASS/FAIL

## Self Findings
Critical:
Major:
Minor:

## Self Score
XX/100

## Ready for Independent Review
YES/NO
```

## 22. Independent Reviewer Scorecard

### Architecture Boundary — 20
- Core has no OCCT dependency: 7
- Core public API has no OCCT type: 5
- adapter isolated: 4
- Feature does not instantiate concrete OCCT kernel: 4

### Shape Ownership — 10
- safe explicit ownership: 4
- no persistent pointer identity: 3
- failed build cannot expose dangling/partial shape: 3

### Geometry Correctness — 15
- valid solid: 5
- dimensions/volume: 5
- COM: 5

### Physical Correctness — 20
- unit conversion: 6
- mass: 4
- COM convention: 3
- inertia: 5
- tensor units/reference documented: 2

### Parametric/Recompute — 15
- W/H/D behavior: 5
- density-only optimization/correctness: 4
- unrelated branch: 3
- failure/recovery: 3

### Persistence — 5
- semantics persist: 2
- runtime shape does not: 1
- reload/recompute equivalent: 2

### Tests/Self-validation — 10
- matrix coverage: 4
- independent analytical oracle: 2
- release gate: 2
- actual evidence: 2

### Documentation — 5
- ADRs: 3
- README/Roadmap/AGENTS updated: 2

Total: 100.

Decision:
- 90–100: APPROVE
- 80–89: APPROVE WITH MINOR FOLLOW-UP
- below 80: REQUEST CHANGES

Overrides requiring REQUEST CHANGES:
- any Critical finding,
- unresolved Major architectural finding,
- Core links/includes OCCT,
- build failure,
- regression failure,
- mandatory release-gate failure,
- claimed tests were not actually executed.

## 23. Adversarial Reviewer Checks

Reviewer should intentionally test/inspect:

- zero/negative/NaN/infinite dimensions,
- very small positive dimension,
- large reasonable dimension,
- zero/negative/NaN density according to documented policy,
- failed feature recovery,
- delete referenced Parameter,
- reload then recompute,
- density-only recompute counts,
- unrelated branch counts,
- no pointer/index persistence.

## 24. Reviewer Output

```text
# M3 Independent Review
Baseline:
Reviewed Commit:

Decision:
APPROVE | APPROVE WITH MINOR FOLLOW-UP | REQUEST CHANGES
Score:
XX/100

## Build Evidence
...

## Critical Findings
...

## Major Findings
...

## Minor Findings
...

## Architecture
Core free of OCCT:
Public headers:
Kernel isolation:
Injection:
Shape ownership:

## Geometry
Validity:
Volume:
COM:

## Physics
Units:
Mass:
Inertia:
Density-only:

## Recompute
Dimensions:
Unrelated:
Failure:
Recovery:

## Persistence
...

## Self-Validation Audit
...

## Required Changes
...

## M4 Readiness
READY | NOT READY
Reason:
...
```

## 25. Completion Report

Create `docs/reviews/M3_CompletionReport.md` containing:

- baseline and final commit,
- implemented items,
- files added/modified,
- OCCT version/discovery/runtime handling,
- ADR decisions,
- Debug/Release build evidence,
- test baseline/new total/passed/failed,
- mandatory release gate,
- SelfValidation report result,
- independent reviewer availability/decision/score,
- limitations and deferred work,
- final `M3 COMPLETE` or `M3 NOT COMPLETE`.

Never claim independent sub-agent review when runtime did not provide one.

## 26. Implementation Order

1. Inspect baseline `8245c89` and all existing docs/ADRs.
2. Freeze current Feature, PartDocument, RecomputeContext, Material, MassProperties, serializer and CMake contracts.
3. Integrate OCCT into isolated Kernel target.
4. Add kernel-neutral shape/result ownership.
5. Implement OCCT box.
6. Validate box geometry analytically.
7. Implement physical property conversion.
8. Validate mass/COM/inertia analytically.
9. Integrate real BoxFeature with M2 recompute.
10. Prove density-only update does not rebuild geometry.
11. Implement invalid-input/failure/recovery behavior.
12. Implement semantic persistence and load/recompute.
13. Add all tests.
14. Create ADRs.
15. Run Self-Validation Protocol.
16. Run independent Reviewer if supported.
17. Fix all Critical/Major findings and rerun affected/full tests.
18. Update README, Roadmap and AGENTS.
19. Create Completion Report.
20. Commit and report final hash.

## 27. Codex Master Prompt

```text
Implement EP3D Milestone M3 using commit 8245c89 as the approved M2 baseline.

Read AGENTS.md first and follow its orchestration rules. Then read all architecture, ADR, reviewer, and milestone documents, especially:
docs/M3_Implementation_SelfValidation_and_Evaluation.md

M3 mission is to introduce an OpenCASCADE-backed Geometry Kernel and first real parametric solid while preserving strict Core/kernel separation.

Mandatory outcomes:
- OCCT isolated from src/Core,
- safe kernel-neutral runtime shape abstraction,
- OCCT box primitive,
- Volume / Mass / COM / inertia with correct units,
- BoxFeature integrated with M2 DependencyGraph/RecomputeEngine,
- W/H/D rebuild geometry,
- density-only updates physical properties without geometry rebuild,
- unrelated branches remain untouched,
- invalid geometry fails transactionally and recovers,
- semantic persistence only; no runtime OCCT persistence,
- reload/recompute equivalence,
- all previous tests remain green,
- mandatory M3 release-gate scenario passes,
- required ADRs are created,
- docs/reviews/M3_SelfValidationReport.md contains actual evidence,
- independent Reviewer is run if supported,
- docs/reviews/M3_CompletionReport.md is created.

Do not implement Qt viewport, DXF, full Sketch solver, Assembly, Collision, Robot, dynamics, CAM, FEA, or multithreaded recompute.

Before coding, Architect must explicitly decide and document:
1. kernel boundary and shape ownership,
2. kernel service injection,
3. geometry/physical units,
4. inertia tensor convention,
5. failed-feature last-valid-shape policy,
6. computed geometry persistence policy.

Use analytical formulas independent of OCCT to validate box Volume, Mass, COM and inertia.

Do not declare M3 complete unless clean builds and all tests pass, the Core boundary scan passes, the mandatory release gate passes, no Critical finding remains, and no unresolved Major finding remains.

If sub-agents are unavailable, use the documented sequential fallback and explicitly state that independent sub-agent review was unavailable.

Finally report the exact final commit hash and M4 readiness.
```

## 28. Final Gate

Before `M3 = COMPLETE`, verify every item:

```text
[ ] OCCT integrated and version documented
[ ] Core does not link OCCT
[ ] Core headers contain no OCCT types
[ ] shape ownership ADR
[ ] units ADR
[ ] kernel injection ADR
[ ] failure policy ADR
[ ] valid Box B-Rep
[ ] analytical Volume
[ ] analytical COM
[ ] analytical Mass
[ ] analytical Ixx/Iyy/Izz
[ ] off-diagonal inertia checked
[ ] W/H/D incremental recompute
[ ] density-only does not rebuild geometry
[ ] unrelated branch untouched
[ ] invalid dimensions handled
[ ] downstream failure semantics correct
[ ] recovery works
[ ] semantic persistence works
[ ] runtime OCCT state not persisted
[ ] load/recompute equivalent
[ ] stable ObjectIds survive
[ ] all previous tests pass
[ ] all M3 tests pass
[ ] Debug clean build
[ ] Release clean build where supported
[ ] static Core boundary scan
[ ] mandatory release gate
[ ] SelfValidationReport
[ ] independent review where supported
[ ] no Critical findings
[ ] no unresolved Major findings
[ ] reviewer score >= 80
[ ] README/Roadmap/AGENTS updated
[ ] CompletionReport created
```

Only then:

```text
M3 = COMPLETE
M4 = READY
```
