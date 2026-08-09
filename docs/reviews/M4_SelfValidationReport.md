# M4 Self-Validation Report

Baseline: `f583f29` on `master` (M4 COMPLETE, independent review APPROVE 97/100).
The spec cites the baseline as `69c6ab1` on `m3-wip`; that commit was squashed
into `f583f29`, and the two trees are identical apart from an obsolete
machine-migration note deliberately left on the branch.

Final/Working Commit: uncommitted working tree (39 changed/added files).

All evidence below was produced by executing the commands shown. Nothing is
carried over from a previous session or inferred from documentation.

## Environment

OS: Windows 11 Home 10.0.26200 (machine `RICHARD`, 63.6 GB RAM, 32 logical cores)
Compiler: MSVC 19.44.35223.0 (VS 2022 BuildTools, x64)
CMake: 4.2.3
OCCT: 8.0.1 (`opencascade[core,freetype]:x64-windows`, vcpkg)
Qt: **6.11.1** (`qtbase:x64-windows`, vcpkg — installed during this session)
Generator: Visual Studio 17 2022, x64

Both OCCT and Qt are discovered through the vcpkg toolchain file, each hardened
with `NO_CMAKE_PACKAGE_REGISTRY NO_CMAKE_SYSTEM_PACKAGE_REGISTRY`. The stray
`HKCU\...\CMake\Packages\OpenCASCADE` entry recorded in M3 is still present on
this machine and still correctly ignored.

## Architecture Boundary

Core OCCT + Qt scan — `grep -rnE "TopoDS_|BRep|Geom_|gp_|AIS_|V3d_|OpenCASCADE|QObject|QWidget|QGraphics|QString" src/Core/` over all 11 spec-§24 patterns: **0 matches**. No false positives to explain.

Kernel: `src/Kernel/Occt` is the only OCCT-including code outside the viewer.
Viewer: `src/Viewer` is the only Qt code anywhere. `DocumentPresenter` — the
document-facing half — is free of *both*, which is what makes the viewer's
ownership rules unit-testable without a display.

**Binary-level verification** (`dumpbin /dependents`, stronger than a text scan
because it reflects what the linker actually resolved):

| Binary | OCCT (`TK*`) | Qt (`Qt6*`) |
|---|---|---|
| `ParametricCADCoreTests.exe` (links Core only) | **0** | **0** |
| `ParametricCADKernelOcctTests.exe` | 4 | **0** |
| `ParametricCADViewer.exe` | 7 | 3 |

Result: **PASS**

## Identity Audit

Stable IDs: `SketchEntityId` is a distinct type drawn from the shared
`ObjectIdGenerator` (ADR-M4-001), so it inherits M1's restore-collision safety.
Verified surviving insertion, removal of the *first* stored entity, and
save/load (`M4_SKETCH_002/003`, `M4_SER_002/008`).

Sub-element refs: `SketchElementRef{SketchEntityId, SketchSubElement}` exists
with `Whole/StartPoint/EndPoint/CenterPoint` reserved. No constraint semantics
implemented — correctly out of scope.

No index persistence: `grep -rnE '"index"|"position"|"0x' src/Core/Serialization/`
→ **0 matches**. `M4_SER_006` additionally asserts the written document contains
none of `TopoDS`, `TShape`, `BRep`, `Geom_`, `gp_`, `AIS_`, `KernelShape`,
`IShapeHandle`, `OcctShape`, or `0x`. Profile loops carry `SketchEntityId`, never
positions, so the rule holds semantically and not only at the file boundary.

Result: **PASS**

## Debug / Release Build and Tests

```
rm -rf build
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug    &&  ctest --test-dir build -C Debug
cmake --build build --config Release  &&  ctest --test-dir build -C Release
```

Both configurations built from a completely empty tree (satisfies spec-§24
items 3/5 and item 16 in one run). Configure reported OCCT 8.0.1 and Qt 6.11.1.

| | Debug | Release |
|---|---|---|
| Build | exit 0, 0 errors | exit 0, 0 errors |
| Tests | **284 / 284** | **284 / 284** |
| Failures | 0 | 0 |

Test accounting: 284 = 180 (M3 baseline) + 104 new M4 tests
(237 in the Core-only executable, 47 in the OCCT-linked one).

## Sketch / Frames

21 tests (`SketchTests.cpp`). Ids unique and stable across insertion and
removal; duplicate restored id rejected; a restored high id pushes the
generator past itself; restore accepts geometry `add*` rejects, so a
hand-edited document round-trips losslessly instead of silently losing an
entity. Point/Line/Circle/Arc semantics; zero-length lines, non-positive and
non-finite radii, NaN/Inf coordinates and degenerate arc sweeps all rejected.

Frames: world XY, translated, rotated about X and Z, rotated+translated
composition, and a rigid-motion check asserting the same (u,v) separation maps
to the same world distance under every frame — which is what stops a malformed
quaternion from silently resizing a Pad. Degenerate rotation axis yields
identity rather than a division by zero.

Result: **PASS**

## Profile

25 tests (`ProfileTests.cpp`), covering every case spec §10 lists.

Accepted: closed rectangle; scrambled/reversed entity order (with orientation
normalization actually asserted, not assumed); full circle as a one-entity loop;
line+arc half-disc; concave L-shape; Points ignored as reference geometry.

Rejected with a structured diagnostic: empty sketch, open loop, disconnected
components, branch/T-junction, duplicate entity (both same-direction and
reversed), degenerate geometry on the restore path, invalid radius, NaN/Inf,
circle mixed with other curves, two circles.

Tolerance boundary: a gap at half tolerance closes; a gap 100× tolerance is
**rejected, not healed**, with the failure naming the condition.

Self-intersection: a bowtie is rejected; a valid rectangle and a concave
L-shape are not falsely flagged.

**A real defect this suite caught.** Duplicate detection originally compared
only endpoints, which rejected *every* valid two-entity loop — an arc and its
chord legitimately span the same two points. Fixed by comparing whole curves
(kind, span, and midpoint; the midpoint is required because the two arcs on
opposite sides of a chord share both endpoints). `M4_PROFILE_021/022` guard both
directions: valid two-entity loops accepted, genuinely identical arcs still
rejected.

Result: **PASS**

## Extrusion Oracles

All computed independently from raw formulas, never from the kernel's own
reported values.

Rectangle: 100 × 50 profile, Pad 20 mm → 100000 mm³, COM (50, 25, 10) — matches.
Circle: r = 10, Pad 30 → π·100·30 mm³, COM (0, 0, 15) — matches.
Half-disc: π·r²/2·h, COM_v = 4r/3π — matches.
Translated frame: volume unchanged, COM shifted by the frame origin — matches.
Rotated frame (+90° about X): volume unchanged, COM (50, −10, 25) — matches.

Cross-check worth recording: `M4_KERNEL_004` asserts that a 100×50 profile
extruded 20 mm produces the *same* volume, COM and full inertia tensor as M3's
already-reviewed `createBox(100, 50, 20)`. The new construction path is
validated against the old one rather than only against a formula.

Result: **PASS**

## Recompute

Sketch geometry edit: Profile/Pad/Mass recompute (`M4_PAD_012`).
PadLength edit: Pad + Mass recompute; sketch entities and their ids untouched
(`M4_PAD_010`, and Gate A asserts the entity count and ids are unchanged).
Density edit: Mass only — `extrudeProfile` call count **unchanged**
(`M4_PAD_011`, Gate A).
Unrelated parameter: neither recomputes (`M4_PAD_013`).
Failure/recovery: broken loop → Pad `Failed`, retained shape untouched,
diagnostic present, downstream not current; repair recovers deterministically
(`M4_PAD_020/021`, Gate C).

Adversarial (spec §25) additionally covered: four invalid Pad lengths
(0/negative/NaN/Inf), deleting a Sketch a Pad still references, and a
self-intersecting sketch.

Result: **PASS**

## Persistence

13 tests (`SerializationV4Tests.cpp`). Schema v4; v1–v3 files still load.

Sketch id, entity ids, entity geometry (including arc direction), sketch frame,
Pad id, Sketch/Length/Material references all survive. Byte-identical second
save. Removing an entity shifts every later entity's *position* without
disturbing any identity. Duplicate entity id in a file is rejected; a Pad
pointing at a non-existent sketch is rejected.

**A second real defect, caught by the M3 regression suite.** A
`PlaceholderFeature` preserves an unrecognized type string losslessly
(ADR-009 D4) — which becomes a save/load asymmetry the moment a milestone adds
a concrete type with the same name. A placeholder carrying `"Pad"` would be
written as a Pad record with none of a Pad's fields, and the loader — which now
knows Pad — would reject the file forever. Fixed on the **save** side
(ADR-M3-008's rule): rejecting is safer than degrading on load, because the
opposite choice would let a real Pad with lost fields reload as an inert
placeholder, silently dropping the solid. `M4_SER_012/013` guard both sides.

Result: **PASS**

## Viewer

Automated (7 tests, `ViewerPresenterTests.cpp`): `DocumentPresenter` is free of
Qt and OCCT, so ownership and association are unit-testable. Verified: nothing
displayable before recompute; a valid solid is offered by `ObjectId`; the
presenter does not own the document; parameter edits flow through the
`PartDocument` facade; non-solid features are simply absent rather than special
-cased.

`M4_VIEW_003` is the one worth naming: a failed rebuild **retains** the last
valid shape, so a viewer checking only "is there a shape?" would keep drawing
superseded geometry — the display-layer form of the defect ADR-M3-006 fixed for
mass properties. The test asserts the shape is still there *and* that it is not
offered for display.

Manual smoke test (the widget layer genuinely needs a window):
`ParametricCADViewer.exe` builds and links (1.18 MB, Debug). Display, rotate
(left drag), pan (middle drag), zoom (wheel), fit-all, whole-object selection
and refresh-after-recompute are implemented per spec §17.

**Stated honestly: the interactive behaviours above have NOT been exercised by a
human in this session.** The viewer was built and its document-facing half is
covered automatically; the on-screen interactions are unverified. This is the
one item in this report resting on code inspection rather than execution, and it
is flagged for the reviewer rather than presented as tested.

Result: **PASS (automated) / UNVERIFIED (interactive)**

## Regression M0-M3

All 180 M3-era tests still pass in both configurations. Three assertions were
updated for legitimate reasons, none to hide a failure:

- two `schemaVersion: 3` assertions on *written* output became `4` (the schema
  was intentionally bumped); the v3 *input* fixtures still say 3 and still load,
  which is the backward-compatibility check;
- one M3 test constructed a placeholder with the arbitrary type name `"Pad"`,
  chosen before `PadFeature` existed; renamed to `"Revolve"`, and the collision
  it exposed is now covered deliberately by `M4_SER_012`.

Result: **PASS**

## Release Gates

Executed against real OCCT geometry (`tests/Kernel/M4ReleaseGateTests.cpp`),
9 tests, passing in Debug **and** Release.

A: initial 100000 mm³ / 0.27 kg / COM (50,25,10); Pad 20→30 gives 150000 mm³,
0.405 kg, COM (50,25,15) with the sketch's entity count and ids unchanged;
width 100→120 gives 180000 mm³, 0.486 kg, COM (60,25,15). Density-only edit
leaves `extrudeProfile` call count unchanged. **PASS**

B: circle r10 × 30 → π·100·30 mm³, COM (0,0,15). **PASS**

C: endpoint broken by 5 mm → Profile invalid, Pad `Failed`, diagnostic present,
downstream not current, retained shape intact, no crash; repair recovers to the
original volume. **PASS**

D: translated frame keeps volume and shifts COM correctly; rotated frame keeps
volume with COM (50,−10,25); a frame rotated about an arbitrary axis and
translated yields *exactly* the world-XY volume and mass. **PASS**

E: save/load preserves sketch id, all entity ids, frame, Pad id, parameter and
material references; recompute through real OCCT reproduces volume, mass, COM
and the full inertia tensor; a post-load parameter edit still propagates.
Also verified for a rotated frame. **PASS**

## Findings

Critical: none.

Major: none.

Minor:
1. **Self-intersection covers straight segments only.** Pairs involving an arc
   are not tested, because approximating an arc by its chord would reject valid
   profiles — a false rejection being worse than a deferred check. Stated in the
   code and in ADR-M4-005; OCCT still refuses to build a face it cannot make, so
   the failure stays structured rather than becoming a wrong solid.
2. **`FakeGeometryKernel::extrudeProfile` models two cross-sections** (axis-
   aligned rectangle, full circle) and returns a structured failure otherwise.
   Deliberate: a half-correct analytical model would make Core-only tests agree
   with a formula rather than with geometry.
3. **Interactive viewer behaviour is unverified**, as stated above.
4. **`MassPropertiesNode` is still a document singleton** bound to the most
   recently wired solid feature — carried over from ADR-M3-005, now reached by
   both Box and Pad. Multi-solid documents remain out of scope.
5. **Holes/inner loops are not implemented.** `ValidatedProfile` has room for
   them; spec §8 defers them.
6. Roadmap's M4 entry disagreed with the M4 spec; recorded rather than silently
   resolved (ADR-M4-007) and the Roadmap updated to match the spec.

## Self Score

Withheld deliberately.

At M3 I scored myself 94/100 and certified "no Major self-findings" for code
that turned out to have five, then introduced a Critical while fixing them. A
self-score from the same author, by the same reasoning, is not evidence — the
independent review is. What this report asserts is narrower and checkable: the
commands above were run, and they produced the results shown.

## Ready for Independent Review

**YES** — clean Debug and Release builds from an empty tree, 284/284 in both,
Core boundary clean at source and binary level for OCCT *and* Qt, identity audit
clean, and all five mandatory release gates passing against real geometry.
