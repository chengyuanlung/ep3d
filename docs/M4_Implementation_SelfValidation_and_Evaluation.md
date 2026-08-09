# EP3D M4 — Sketch/Profile Foundation, Pad/Extrude & Basic 3D Viewer

**Baseline:** approved M3 implementation on `m3-wip` (`69c6ab1`)  
**Stack:** C++20, CMake, GoogleTest, OCCT, Qt 6 (UI only)  
**Execution:** Codex must read `AGENTS.md` first and use the repository orchestration/reviewer workflow.

## 1. Mission

M4 establishes the first general 2D-to-3D CAD path:

```text
Sketch semantic model
 -> stable Sketch entity IDs
 -> Profile validation
 -> kernel-neutral profile
 -> OCCT Wire/Face
 -> Pad/Extrude
 -> B-Rep solid
 -> existing MassProperties
 -> basic 3D viewer
```

This is the foundation for the product goal:

```text
DXF / 2D drawing -> dimensions/constraints -> editable sketch -> automatic 3D rebuild
```

M4 does not implement DXF or a full constraint solver.

## 2. Definition of Done

M4 is complete only when:

- Core has a kernel-neutral Sketch model.
- Point, Line, Circle and Arc are supported.
- Every Sketch entity has a stable persistent ID.
- Persistent references never rely on vector order, pointer address, or OCCT topology.
- Sketch uses local 2D `(u,v)` coordinates plus an explicit frame/support plane.
- XY, translated, and rotated Sketch frames are tested.
- A Profile layer deterministically builds and validates a closed loop.
- Open, disconnected, branched, duplicate, degenerate, non-finite and self-intersecting invalid profiles are handled deterministically.
- A full Circle can form a one-entity closed profile.
- Kernel-neutral APIs can create a planar profile/face and extrude it.
- OCCT implementation produces valid solids.
- `PadFeature` uses a Sketch/Profile and Length Parameter.
- Sketch edits rebuild dependent Pad/MassProperties.
- Pad-length-only edits do not unnecessarily rebuild Sketch semantic geometry.
- Density-only edits do not rebuild Sketch or Pad geometry.
- Unrelated branches remain untouched.
- M3 MassProperties implementation is reused.
- Failure is transactional and recoverable.
- Sketch/Profile/Pad semantic data serializes and reloads.
- No OCCT Edge/Wire/Face identity is persisted.
- Minimal Qt viewer displays solid and supports rotate, pan, zoom, fit-all, whole-object selection/highlight, and refresh.
- Core remains free of Qt and OCCT.
- All M0-M3 regression tests pass.
- All M4 tests and mandatory release gates pass.
- `docs/reviews/M4_SelfValidationReport.md` contains actual evidence.
- Independent Reviewer has no Critical or unresolved Major findings.

## 3. Non-Goals

Do not implement DXF/DWG, dimension reconstruction, full Sketch constraint solver, interactive production Sketch editor, Pocket, Fillet, Chamfer, Assembly, Joint, Collision, Robot, dynamics, CAM, FEA, persistent OCCT face/edge naming, arbitrary 3D Sketch, or multithreaded recompute.

## 4. Hard Architecture Rules

```text
Core semantic model
    |
    v
kernel-neutral geometry interface
    |
    v
Kernel/Occt
    |
    v
OCCT transient topology
```

Semantic identity must never be `TopoDS_Edge`, `TopoDS_Face`, explorer index, vector index, or pointer address.

Qt presentation objects must never own the semantic CAD model.

Forbidden real dependencies under `src/Core` include:

```text
TopoDS_  BRep  Geom_  gp_  AIS_  V3d_
QObject  QWidget  QGraphics  QString
```

## 5. Stable Sketch Entity Identity

Introduce `SketchEntityId` or an equivalently typed stable ID.

Example:

```text
Sketch001
  E17 Line
  E21 Line
  E33 Arc
```

Inserting/removing/reordering other entities must not change these identities.

Future constraints must be able to reference a stable sub-element:

```cpp
struct SketchElementRef {
    SketchEntityId entityId;
    SketchSubElement subElement;
};
```

Reserve semantics such as `Whole`, `StartPoint`, `EndPoint`, `CenterPoint`.

Do not implement the constraint solver yet.

Required ADR:

`ADR-M4-001 Sketch Entity Identity and Reference Model`

## 6. Sketch Coordinate Frame

Store entity geometry in sketch-local `(u,v)`, not world XYZ.

```text
(u,v)
  -> SketchFrame
  -> Part local XYZ
  -> future Assembly/World
```

Reuse `ReferenceFrame` if architecturally correct; otherwise create a neutral Sketch frame type. Coordinate conversion must be centralized.

Required tests: world XY, translated frame, rotated frame.

Required ADR:

`ADR-M4-002 Sketch Coordinate Frame`

## 7. Entity Semantics

Minimum:

- Point: finite `u,v`.
- Line: addressable start/end semantics.
- Circle: center + finite radius > 0.
- Arc: robust deterministic center/radius/start/end/direction representation; radians internally.

Architect must choose whether line endpoints own coordinates or reference stable point entities. The decision must support future constraints and serialization without requiring M4 to solve constraints.

## 8. Profile Model

A Profile is semantic interpretation of Sketch entities, not OCCT topology.

M4 minimum is one outer loop; holes are deferred but architecture should not block them.

Conceptually:

```cpp
struct OrientedSketchEntityRef { ... };
struct ProfileLoop {
    std::vector<OrientedSketchEntityRef> entities;
};
struct ValidatedProfile {
    ProfileLoop outer;
};
```

Profile construction must not depend on entity storage order.

Required ADR:

`ADR-M4-003 Neutral Profile to Kernel Boundary`

## 9. Connectivity / Ordering / Tolerance

For a valid ordered loop:

```text
end(E1) ~= start(E2)
...
end(En) ~= start(E1)
```

Define a documented mm-scale connectivity tolerance. Do not use exact floating equality and do not silently heal large gaps.

Given unordered entities, construct deterministic connectivity, orient curves consistently, and reject ambiguous branching.

Required ADR:

`ADR-M4-005 Profile Connectivity and Tolerance Policy`

## 10. Mandatory Profile Cases

Validate:

- closed rectangle,
- random/reversed rectangle entity order,
- full Circle,
- valid Line+Arc loop,
- open loop,
- disconnected components,
- branch/T-junction,
- duplicate entity,
- zero-length line,
- invalid radius,
- NaN/Infinity coordinates,
- gap just inside tolerance,
- gap just outside tolerance,
- obvious self-intersection.

Recommended M4 self-intersection policy: reject rather than guess a face.

## 11. Kernel Boundary

Extend the kernel-neutral API to express planar profile/extrusion without leaking OCCT types.

Conceptually either:

```cpp
FaceResult createPlanarFace(const PlanarProfileDefinition&);
ShapeResult extrude(const KernelFace&, double distanceMm);
```

or a single:

```cpp
ShapeResult extrudeProfile(const PlanarProfileDefinition&, double distanceMm);
```

Architect chooses based on current M3 shape ownership.

Kernel/Occt may construct Edge/Wire/Face/Prism internally.

## 12. PadFeature

Introduce the first general profile-based solid Feature:

```text
PadFeature
  ObjectId
  Sketch/Profile reference
  Length Parameter reference
  direction = +Sketch normal (M4)
  computed runtime shape
```

Future Reverse/Symmetric/UpToFace are deferred.

Required graph:

```text
Sketch/Profile -----> PadFeature -----> MassProperties
PadLength ----------> PadFeature
Density --------------------------------> MassProperties
```

Required behavior:

- Sketch geometry edit: Profile/Pad/Mass affected.
- PadLength edit: Pad/Mass affected; Sketch semantic geometry not unnecessarily rebuilt.
- Density edit: Mass only.
- unrelated edit: none of this branch.

## 13. Transactional Failure

Recompute:

```text
resolve -> validate profile -> build temporary face/solid
-> validate -> commit successful runtime result
```

On failure: no partial invalid shape is committed, Pad is Failed, downstream current physical properties cannot falsely report success, diagnostics identify the cause, and M3 last-valid-shape policy is respected.

Repair must recover deterministically.

## 14. Topological Naming Deferral

M4 must explicitly forbid persistent references such as:

```text
Face1
Edge7
TopoDS_Edge address
explorer index
```

Persistent subshape naming is deferred. M4 only needs stable Sketch semantics and whole Pad result identity.

Required ADR:

`ADR-M4-004 Topological Naming Deferral and Rules`

## 15. Physical Properties

Reuse M3 physical property path. Do not create a second implementation.

Analytical rectangle oracle:

```text
100 x 50 mm profile, Pad 20 mm
Volume = 100000 mm^3
density 2700 kg/m^3
Mass = 0.27 kg
COM = (50,25,10) mm on XY frame
```

Analytical Circle oracle:

```text
r = 10 mm, Pad = 30 mm
Volume = pi*r^2*h
```

## 16. Serialization

Persist semantic state:

- Sketch ObjectId,
- Sketch frame/reference,
- SketchEntityIds,
- entity types and geometry,
- semantic Profile references if Profile is persisted,
- Pad ObjectId,
- Sketch/Profile reference,
- Pad Length Parameter reference,
- material reference as appropriate.

Never persist OCCT topology/runtime shape memory, viewer/AIS/Qt pointers, pointer addresses, or vector indexes as identity.

After load: rebuild registry/dependencies, mark runtime geometry dirty as required, recompute equivalent geometry.

## 17. Basic Viewer

M4 viewer scope:

```text
display solid
rotate
pan
zoom
fit all
whole-object select/highlight
refresh after recompute
```

Viewer lives outside Core. OCCT AIS/V3d types, if used, stay in viewer/UI adapter modules.

Selection may map transient display object to document `ObjectId`. Persistent face/edge selection is explicitly out of scope.

Required ADR:

`ADR-M4-006 Viewer/Core Boundary`

## 18. Required Test Matrix

### Sketch
- stable IDs survive insertion/removal/reorder,
- duplicate ID rejected,
- Point/Line/Circle/Arc semantics,
- invalid radii/non-finite values,
- XY/translated/rotated frame transforms.

### Profile
- unordered rectangle accepted,
- reversed curves normalized,
- open/disconnected/branch/duplicate/degenerate rejected,
- tolerance boundary cases,
- Circle accepted,
- Line+Arc loop accepted,
- self-intersection handled by policy.

### Kernel
- rectangle planar profile -> valid solid,
- 100x50x20 volume = 100000 mm^3,
- Circle r10 x 30 volume = `pi*100*30`,
- translated/rotated frame extrusion and COM,
- invalid profile cannot create current valid solid.

### Recompute
- Pad length: Pad+Mass only,
- Sketch geometry: Profile/Pad/Mass,
- density: Mass only,
- unrelated: none,
- invalid profile blocks downstream,
- repair recovers.

### Persistence
- Sketch/Object/Entity IDs survive,
- geometry/frame survive,
- Pad/Parameter refs survive,
- no OCCT topology serialized,
- load+recompute equivalent,
- references remain valid independent of storage order.

### Viewer
Automate ownership/association where practical; manually smoke-test display, rotate, pan, zoom, fit, select, refresh.

## 19. Mandatory Release Gate A — Rectangle

Create XY Sketch:

```text
P1=(0,0) P2=(100,0) P3=(100,50) P4=(0,50)
E1 P1->P2
E2 P2->P3
E3 P3->P4
E4 P4->P1
PadLength=20 mm
Density=2700 kg/m^3
```

Expected:

```text
valid Profile/Solid
Volume=100000 mm^3
Mass=0.27 kg
COM=(50,25,10) mm
```

Change Pad `20 -> 30`:

```text
Sketch not unnecessarily rebuilt
Pad + Mass recompute
Volume=150000 mm^3
Mass=0.405 kg
COM=(50,25,15)
```

Change width `100 -> 120` by editing appropriate sketch geometry:

```text
Profile/Pad/Mass affected
Volume=180000 mm^3
Mass=0.486 kg
COM=(60,25,15)
```

## 20. Mandatory Release Gate B — Circle

```text
Circle center=(0,0)
radius=10 mm
Pad=30 mm
```

Expect valid solid, `Volume=pi*10^2*30 mm^3`, and COM `(0,0,15)` in sketch frame.

## 21. Mandatory Release Gate C — Failure/Recovery

Break rectangle endpoint beyond tolerance.

Expect Profile invalid, Pad Failed, downstream not-current, useful diagnostic, no crash.

Repair endpoint. Expect Profile/Pad/Mass success and viewer refresh.

## 22. Mandatory Release Gate D — Frame

Extrude the same rectangle on translated and rotated frames.

Verify local dimensions and volume unchanged, world orientation correct, COM transformed correctly. This prevents world-XY hardcoding.

## 23. Mandatory Release Gate E — Save/Load

Save/reload rectangle Pad. Verify Sketch ID, entity IDs, frame, geometry, Pad ID, Parameter refs, material, dependencies. Recompute equivalent Volume/Mass/COM/Inertia without requiring OCCT edge/wire/face identity.

## 24. Self-Validation Protocol

Before independent review create:

`docs/reviews/M4_SelfValidationReport.md`

Actually execute and record:

1. architecture boundary scan,
2. stable-ID audit,
3. clean Debug build,
4. complete Debug tests,
5. clean Release build/tests where supported,
6. Sketch tests,
7. Profile adversarial tests,
8. analytical rectangle oracle,
9. analytical Circle oracle,
10. translated/rotated frame tests,
11. incremental recompute tests,
12. failure injection and recovery,
13. serialization round trip,
14. M0-M3 regression,
15. viewer smoke test,
16. clean rebuild from scratch,
17. self-review score.

Static scan Core for real dependencies containing:
`TopoDS_`, `BRep`, `Geom_`, `gp_`, `AIS_`, `V3d_`, `OpenCASCADE`, `QObject`, `QWidget`, `QGraphics`, `QString`.

Also audit serialization for edge/face/wire/vector indexes or pointer addresses used as identity.

## 25. Adversarial Self-Validation

Intentionally try:

```text
duplicate SketchEntityId
missing/deleted entity reference
zero/tiny line
NaN/Infinity coordinate
zero/negative/non-finite radius
invalid Arc
open loop
disconnected loops
branch
duplicate edge
reversed/random entity order
self-intersection
gap just below/above tolerance
Pad length 0/negative/NaN/Infinity
delete Sketch referenced by Pad
failed Pad then recovery
load then recompute
```

All behavior must be deterministic and documented.

## 26. Self-Validation Report Format

```text
# M4 Self-Validation Report
Baseline:
Final/Working Commit:

## Environment
OS:
Compiler:
CMake:
OCCT:
Qt:
Generator:

## Architecture Boundary
Core OCCT:
Core Qt:
Kernel:
Viewer:
Result:

## Identity Audit
Stable IDs:
Sub-element refs:
No index persistence:
Result:

## Debug / Release Build and Tests
Commands:
Totals:
Passed:
Failed:

## Sketch / Frames
...

## Profile
...

## Extrusion Oracles
Rectangle:
Circle:
Translated:
Rotated:

## Recompute
Sketch:
PadLength:
Density:
Unrelated:
Failure/Recovery:

## Persistence
...

## Viewer
Display/Rotate/Pan/Zoom/Fit/Selection/Refresh:

## Regression M0-M3
...

## Release Gates
A:
B:
C:
D:
E:

## Findings
Critical:
Major:
Minor:

## Self Score
XX/100
## Ready for Independent Review
YES/NO
```

## 27. Independent Reviewer Scorecard

```text
Architecture boundaries              15
Identity/reference model             15
Sketch/frame correctness             10
Profile validation                   15
Kernel Pad/Extrude                   15
Parametric recompute                 10
Persistence                           8
Viewer                                5
Testing/self-validation/docs          7
                                     ---
                                     100
```

Decision:
- 90-100 APPROVE
- 80-89 APPROVE WITH MINOR FOLLOW-UP
- <80 REQUEST CHANGES

Automatic REQUEST CHANGES for any Critical finding, unresolved Major architecture issue, Core Qt/OCCT dependency, semantic identity based on transient OCCT topology or vector index, build/regression failure, failed mandatory release gate, or tests claimed without execution.

Critical examples: dangling Sketch references/use-after-free, corrupted semantic identity on save/load, silent wrong geometry.

Major examples: Core Qt/OCCT leak, Profile uses OCCT topology as semantics, world-XY hardcoding, Pad bypasses M2 incremental recompute, density rebuilds geometry because graph is wrong, open Profile silently accepted, viewer owns semantic objects.

## 28. Reviewer Output

```text
# M4 Independent Review
Baseline:
Reviewed Commit:
Decision:
Score:

## Build Evidence
## Critical Findings
## Major Findings
## Minor Findings
## Architecture
## Identity
## Sketch / Frame
## Profile
## Pad / Physics
## Recompute
## Persistence
## Viewer
## Self-Validation Audit
## Mandatory Gates A-E
## Required Changes
## M5 Readiness
READY | NOT READY
Reason:
```

## 29. Completion Report

Create `docs/reviews/M4_CompletionReport.md` with baseline/final commit, files, ADRs, OCCT/Qt versions, build/test evidence, previous/new test totals, Gates A-E, self-validation, whether a genuinely independent sub-agent was available, Reviewer decision/score, limitations/deferred work, final M4 status and M5 readiness.

Never claim independent sub-agent review if only sequential role simulation occurred.

## 30. Implementation Order

```text
M4-A baseline/contract inspection
M4-B Sketch stable identity + entities
M4-C Sketch frame
M4-D semantic Profile validator
M4-E kernel-neutral Profile API
M4-F OCCT Face/Extrude
M4-G PadFeature + M2 recompute
M4-H reuse M3 MassProperties
M4-I semantic persistence
M4-J minimal viewer
M4-K self-validation
M4-L independent review + fixes
M4-M docs + final commit
```

Do not code before Architect inspects current Feature, BoxFeature, Kernel interfaces, RecomputeContext, PartDocument, serializer, MassProperties, CMake and UI targets.

## 31. Required ADRs

At minimum:

```text
ADR-M4-001 Sketch Entity Identity and Reference Model
ADR-M4-002 Sketch Coordinate Frame
ADR-M4-003 Neutral Profile to Kernel Boundary
ADR-M4-004 Topological Naming Deferral and Rules
ADR-M4-005 Profile Connectivity and Tolerance Policy
ADR-M4-006 Viewer/Core Boundary
```

## 32. Recommended Next Milestones

If M4 passes:

```text
M5 Sketch Constraints & Dimensional Parameterization
   Coincident / Horizontal / Vertical
   Distance / Length / Radius / Diameter / Angle / Fix
   DOF and solver abstraction
   Parameter-linked dimensions
   over/under-constrained diagnostics

M6 DXF Import + Entity Mapping
M7 Dimension/Constraint Reconstruction
```

This leads directly toward:

```text
2D DXF dimension edit -> constrained Sketch -> Pad/Feature rebuild -> updated 3D
```

## 33. Codex Master Prompt

```text
Implement EP3D Milestone M4 using the approved M3 implementation on branch m3-wip, reviewed implementation 69c6ab1.

Read AGENTS.md first and follow its orchestration rules. Then read all current architecture, ADR, reviewer and completion documents, especially:
docs/M4_Implementation_SelfValidation_and_Evaluation.md

Mission:
Create the kernel-neutral 2D Sketch/Profile foundation, profile-based Pad/Extrude feature, and minimal 3D viewer while preserving M0-M3 invariants.

Mandatory:
- stable SketchEntityId and future-compatible sub-element references,
- Point/Line/Circle/Arc,
- sketch-local 2D coordinates and explicit frame,
- deterministic Profile ordering/validation independent of storage order,
- no semantic dependency on transient OCCT topology,
- kernel-neutral planar profile/extrusion API,
- OCCT Face/Solid implementation,
- PadFeature using existing M2 recompute,
- reuse M3 MassProperties,
- PadLength-only, density-only and unrelated-branch incremental behavior,
- rectangle and circle analytical oracles,
- translated/rotated frame tests,
- transactional failure/recovery,
- semantic persistence and load/recompute equivalence,
- no OCCT topology identity persisted,
- minimal Qt viewer rotate/pan/zoom/fit/select/refresh outside Core,
- all M0-M3 regression green,
- all mandatory release gates A-E,
- required ADRs,
- actual M4 SelfValidation report,
- independent Reviewer if supported,
- M4 CompletionReport.

Do NOT implement DXF, full Sketch solver, Pocket, Fillet, Chamfer, Assembly, Collision, Robot, dynamics, CAM, FEA, or persistent OCCT face/edge naming.

Before coding Architect must decide/document:
1. Sketch entity stable ID model,
2. endpoint/sub-element reference model,
3. Sketch frame convention,
4. Profile connectivity tolerance,
5. deterministic loop ordering/orientation,
6. neutral Profile/Kernel boundary,
7. topological naming deferral,
8. Viewer/Core boundary.

Never use vector indexes, raw pointers, OCCT Edge/Wire/Face identity, or explorer order as persistent semantic references.

Do not declare M4 complete unless clean builds/tests pass, all previous regressions remain green, Core boundary and identity audits pass, Gates A-E pass, self-validation evidence is complete, and no Critical or unresolved Major finding remains.

If independent sub-agents are unavailable, use the documented sequential fallback and explicitly say independent sub-agent review was unavailable.

Finally report exact final commit hash, test totals, Reviewer decision/score and M5 readiness.
```

## 34. Final Gate Checklist

```text
[ ] stable SketchEntityId
[ ] no vector-index/pointer/topology persistence
[ ] Point / Line / Circle / Arc
[ ] viable future sub-element references
[ ] Sketch frame
[ ] XY / translated / rotated tests
[ ] semantic Profile
[ ] deterministic ordering/orientation
[ ] tolerance policy
[ ] rectangle / Circle / Line+Arc
[ ] open/disconnected/branch/duplicate/degenerate rejected
[ ] self-intersection policy tested
[ ] Core free of Qt/OCCT
[ ] neutral Profile kernel API
[ ] OCCT planar Face + Extrude
[ ] rectangle/circle analytical volume
[ ] transformed-frame extrusion
[ ] PadFeature + Length Parameter
[ ] correct Sketch/Pad/Density/Unrelated recompute
[ ] failure + recovery
[ ] M3 MassProperties reused
[ ] semantic persistence
[ ] IDs/references survive load
[ ] no OCCT topology serialized
[ ] load/recompute equivalent
[ ] viewer display/rotate/pan/zoom/fit/select/refresh
[ ] viewer does not own semantic objects
[ ] all M0-M3 regression
[ ] all M4 tests
[ ] Debug clean build
[ ] Release clean build where supported
[ ] Gates A-E
[ ] ADR-M4-001..006
[ ] M4_SelfValidationReport
[ ] independent review where supported
[ ] no Critical
[ ] no unresolved Major
[ ] Reviewer >= 80
[ ] README/Roadmap/AGENTS updated
[ ] M4_CompletionReport
[ ] exact final commit recorded

Only then:
M4 = COMPLETE
M5 = READY
```
