# M4 Completion Report

**Milestone:** M4 — Sketch/Profile Foundation, Pad/Extrude & Basic 3D Viewer
**Baseline:** `f583f29` on `master` (M3 COMPLETE, independent review APPROVE 97/100)
**Final commit:** `4c84194` on `master`
**Date:** 2026-08-09

---

## Result

**M4 COMPLETE.**

Validation basis, stated precisely because that is what makes the claim
checkable (ADR-M4-016):

| | |
|---|---|
| Functional review | **Independent agent review — APPROVE 96/100** (three rounds: 74 → 89 → 96) |
| Automated tests | **322/322** in Debug and Release, including release gates A–E against real OCCT geometry |
| UI validation | **Owner manual validation — ACCEPTED**, seven of eight groups, no Critical, no Major |
| Independent UI agent review | **Not performed** according to project workflow. One round ran (REQUEST CHANGES 79/100); its four Majors were fixed, then the owner chose to validate directly instead. |

`docs/M4_UI_Design_and_Validation.md` §26/§28 required an independent UI review;
`docs/M5_UI_User_Assisted_Validation_Guide.md` §4/§18 defines owner manual
validation as an alternative and gives the wording for recording it. The two
documents conflict, the owner directed which one governs, and ADR-M4-016 records
the conflict and the resolution rather than resolving it silently.

**Nothing here claims an independent UI reviewer verified the fixes.** It did not
happen (UI spec §29, guide §18).

---

## What is done

- Kernel-neutral Sketch model in `src/Core/Sketch`: Point/Line/Circle/Arc in
  sketch-local (u,v) mm on an explicit `SketchFrame`, with `SketchEntityId`
  stable across insertion, removal and reordering.
- Semantic Profile validation: deterministic loop ordering and orientation
  independent of storage order; a documented connectivity tolerance that is
  never silently healed; open, disconnected, branched, duplicate, degenerate
  and self-intersecting outlines rejected with diagnostics.
- `extrudeProfile` on the kernel-neutral interface. `Kernel/Occt` builds
  Wire/Face/Prism internally; no OCCT type crosses the boundary.
- `PadFeature` in the M2 dependency graph: Sketch → Pad → MassProperties,
  Length → Pad, Material → MassProperties. Length-only edits do not rebuild
  sketch semantics; density-only edits do not rebuild geometry.
- Schema v4 persisting semantics only. v1–v3 files still load.
- Qt 6 application shell outside Core: model tree, property panel, toolbar,
  menus, status bar, OCCT viewport with rotate/pan/zoom/fit/select/show-hide.

## Not implemented (spec §3 non-goals, unchanged)

DXF, dimension reconstruction, constraint solver, interactive sketch editor,
Pocket/Fillet/Chamfer, Assembly, Joint, Collision, Robot, dynamics, CAM, FEA,
persistent OCCT face/edge naming, 3D sketch, multithreaded recompute.

---

## Versions and discovery

| | |
|---|---|
| OCCT | 8.0.1 (`opencascade[core,freetype]:x64-windows`, vcpkg) |
| Qt | 6.11.1 (`qtbase:x64-windows`, vcpkg) |
| Discovery | `find_package(... CONFIG)` via vcpkg toolchain, both hardened with `NO_CMAKE_(SYSTEM_)PACKAGE_REGISTRY` |
| Optionality | Without OCCT the kernel target is skipped; without Qt (or `-DPARAMCAD_BUILD_VIEWER=OFF`) the viewer is skipped. Core and its tests build in every case. |

The Qt platform plugin is deployed to `platforms/` by a post-build step. Without
it the viewer builds, links and cannot start — see Defects below.

---

## Build and test evidence

| | Debug | Release |
|---|---|---|
| Clean configure + build | exit 0 | exit 0 |
| Tests | **322 / 322** | **322 / 322** |

Independently reproduced by the functional reviewer from a build tree created
outside the repository, including `-DPARAMCAD_BUILD_VIEWER=OFF`.

Test accounting: 322 = 180 (M3) + 142 new. Every M0–M3 test still passes; three
assertions were updated for legitimate reasons (schema version bumped to 4 on
*written* output, v3 input fixtures unchanged; one M3 test had used `"Pad"` as an
arbitrary placeholder type name before `PadFeature` existed).

**Architecture boundary.** `src/Core` scanned for all 11 spec patterns
(`TopoDS_`, `BRep`, `Geom_`, `gp_`, `AIS_`, `V3d_`, `OpenCASCADE`, `QObject`,
`QWidget`, `QGraphics`, `QString`): **0 matches**. Binary check:

| Binary | OCCT | Qt |
|---|---|---|
| `ParametricCADCoreTests.exe` | **0** | **0** |
| `ParametricCADKernelOcctTests.exe` | 4 | **0** |
| `ParametricCADViewer.exe` | 7 | 3 |

---

## Mandatory release gates A–E

`tests/Kernel/M4ReleaseGateTests.cpp`, against real OCCT geometry, passing in
Debug and Release. All expected values computed independently from raw formulas.

| Gate | Result |
|---|---|
| A — rectangle: 100000 mm³ / 0.27 kg / COM (50,25,10); Pad 20→30 → 150000 / 0.405 / z=15 with sketch entities and ids untouched; width 100→120 → 180000 / 0.486 / x=60 | **PASS** |
| B — circle r10 × 30 → π·100·30 mm³, COM (0,0,15) | **PASS** |
| C — endpoint broken 5 mm → Profile invalid, Pad Failed, diagnostic, downstream not current, retained shape intact, no crash; repair recovers | **PASS** |
| D — translated and rotated frames keep volume; rotated COM (50,−10,25); arbitrary-axis frame matches world-XY volume exactly | **PASS** |
| E — save/load preserves sketch id, entity ids, frame, Pad id, parameter and material refs; recompute reproduces volume, mass, COM and the full inertia tensor; post-load edits still propagate | **PASS** |

---

## ADRs

| ADR | Decision |
|---|---|
| M4-001 | Sketch entity identity and reference model |
| M4-002 | Sketch coordinate frame; single conversion site |
| M4-003 | Neutral profile → kernel boundary; one `extrudeProfile` call |
| M4-004 | Topological naming deferral and rules |
| M4-005 | Profile connectivity and tolerance policy; Profile is computed, not cached |
| M4-006 | Viewer/Core boundary |
| M4-007 | Roadmap/spec conflict for M4 (recorded, then amended to note its own false claim) |
| M4-008 | Sketch mutation goes through the document |
| M4-009 | Removal completeness: owner step **and** referrer step |
| M4-010 | Save/load symmetry applies per feature type |
| M4-011 | Geometric predicates must be orientation-independent |
| M4-012 | Running the program is its own check |
| M4-013 | Visibility is view state |
| M4-014 | Shortcuts are application-scoped because the 3D view is native |
| M4-015 | Mouse input crosses a logical/device pixel boundary |

M4-008 through M4-015 were all written after a review or a user report found the
thing they describe.

---

## Reviews

### Functional — genuinely independent, three rounds

A separate reviewer agent was used. No claim of independent review is made
beyond what actually happened.

| Round | Decision | Score | Findings |
|---|---|---|---|
| 1 | REQUEST CHANGES | 74/100 | 1 Critical, 5 Major |
| 2 | REQUEST CHANGES | 89/100 | 4 Majors resolved; **1 new Major introduced by a round-1 fix** |
| 3 | **APPROVE** | **96/100** | 0 Critical, 0 Major open |

Final: **APPROVE, 96/100.** §34 functional gate: every line passes.

### UI — independent review: one round, not re-reviewed

| Round | Decision | Score | Findings |
|---|---|---|---|
| 1 | **REQUEST CHANGES** | **79/100** | 0 Critical, 4 Major |
| 2 | **not performed** | — | stopped by decision |

The four Majors — no viewer highlight on tree selection; a deselection dead end;
no Show/Hide command (the automatic REQUEST CHANGES trigger); the root row
permanently reading "Not computed" — are fixed, each with a regression test.
**No independent reviewer has verified those fixes.**

The reviewer also executed four of six items the self-validation report had
marked NOT EXECUTED, and measured two of its claims as false. Both corrections
are recorded in revision 2 of that report.

### UI — user-assisted validation: performed, seven of eight groups

`docs/reviews/M4_UI_UserValidation.md`, following the workflow in
`docs/M5_UI_User_Assisted_Validation_Guide.md`. The project owner operated the
application; the agent supplied deterministic samples, step-by-step
instructions and expected values, and recorded the observations.

| Group | Result |
|---|---|
| Sample A — rectangle Pad, numeric edit | PASS |
| Sample B — failed profile visibility | PASS |
| Sample D — circle dimensional ratio | PASS |
| Selection synchronization | PASS |
| Viewer — Show/Hide, rotate, pan, zoom, fit | PASS |
| Failure / recovery through the UI | PASS |
| DPI 100% and 200% | PASS |
| DPI 150% | NOT EXECUTED |

Result: **ACCEPTED.** No Critical and no Major user findings. All four of UI
spec §24's Critical conditions were tested and none reproduced. The only item
not executed is 150% display scaling; 100% and 200% both passed and bracket it,
but that is an argument rather than a measurement.

This confirms by observation the behaviours the four independent-review Majors
concerned, and — uniquely — picking accuracy at **200% display scaling**, which
no agent-driven test on the 100% secondary display could have reached.

**This is owner manual validation and is NOT an independent agent review**
(guide §4, §18). It does not substitute for the UI re-review.

---

## Defects found, and how

Recorded because the *how* is the useful part.

**Found by the functional review (7):** a Critical where the self-intersection
predicate's bounding box was orientation-dependent, so an overlapping outline
was accepted and OCCT reported a volume of 0 mm³ as a success — the same figure
rotated 90° was correctly rejected, which is why 284 passing tests missed it;
plus six Majors including two the developer introduced while fixing others.

**Found by the UI review (4):** listed above.

**Found by running the executable for the first time (4):** the Qt platform
plugin was never deployed, so the viewer built, linked, and could not start;
`WA_NativeWindow` was missing, so OCCT painted over every Qt control;
`AIS_Shape` displayed in wireframe; `resizeEvent` never redrew. None of these is
visible to a unit test or to a successful build. `--selftest` now runs in CTest
and was verified to catch the plugin defect.

**Found by the user, in ordinary use (1):** clicking beside the solid selected
it. Qt reports mouse positions in logical pixels; OCCT's view is in device
pixels; on the 200%-scaled display these differ by 2×. **The defect cannot occur
at 100% scaling** — and every screenshot taken and every reviewer interaction
driven had run on the 100% secondary display, because the self-validation report
had wrongly declared DPI scaling untestable in this environment. The
configuration ruled out was the only one that exposes it.

**Found while verifying another fix (1):** every shortcut the menus advertised
did nothing while the 3D viewport held focus. Testing Show/Hide with Ctrl+H
suggested the command was broken; the toolbar button worked, which separated
"command broken" from "key never arrived".

---

## Limitations and deferred work

1. **The UI review is not re-run.** The primary limitation, stated first.
2. Self-intersection covers straight segments only; arc-involved pairs are not
   tested, because approximating an arc by its chord would reject valid
   profiles (ADR-M4-005).
3. `FakeGeometryKernel::extrudeProfile` models an axis-aligned rectangle and a
   full circle; anything else returns a structured failure.
4. `MassPropertiesNode` remains a document singleton bound to the most recently
   wired solid.
5. Holes/inner loops are not implemented.
6. Profile validation is O(n³) — fine at M4 scale, a real concern once DXF
   import arrives at M6.
7. No sketch editor, so `editSketch` has no UI path.
8. `Active` state is not modelled; `Suppressed` is modelled but unreachable from
   the UI.
9. Toolbar is text-only; no icon set ships in M4.
10. DPI 125/150/200% were verified by the UI reviewer but are not in any
    automated check.

---

## Commit

**`master` = `4c84194`** — "M4: sketch/profile foundation, Pad/Extrude, and a Qt
CAD shell". 73 files, +12205 / −89. Squashed from `m4-wip` so `master` keeps one
clean commit per milestone:

```
4c84194  M4: sketch/profile foundation, Pad/Extrude, and a Qt CAD shell
cc48eaa  docs: record the master milestone commit in the M3 completion report
f583f29  M3: geometry kernel adapter and first parametric solid
8245c89  M2: document recompute infrastructure
34f67cf  M1: native JSON serialization (schema v1) + GoogleTest adoption
2f9768a  Initial commit
```

The squashed result was re-verified rather than assumed equivalent: clean
configure and build from an empty tree, **322/322 in Debug and Release**, Core
boundary scan 0 matches. The tree is byte-identical to `m4-wip` at `ceb56ac`.

`m4-wip` retains the development history — `d9e2f75` (implementation) →
`fcfd0b4` (completion report) → `6ad5b3b` / `db5a1a9` (user validation) →
`ceb56ac` (M4 complete).

---

## Remaining work, carried forward

None blocking. Two items are open and tracked rather than closed:

1. **Display scaling at 150% is NOT EXECUTED.** 100% and 200% both passed and
   bracket it, but that is an argument, not a measurement.
2. **Independent UI re-review was not performed.** Available at any time; the
   fixes and their regression tests are in place, and the behaviours concerned
   have been confirmed by owner observation.

## M5 readiness

**NOT READY**, by process rather than architecture.

The functional reviewer judged the architecture ready: `SketchElementRef` is
reserved and unused as intended, entity identity held under every attack
constructed, the frame is a single conversion site, `editSketch` gives a future
solver one enforced mutation path, and the two capability interfaces mean a
constrained-sketch feature can join the document without touching
`removeObject`, `validateSaveable`, `MassPropertiesNode` or the viewer.

Two things to watch going into M5: the O(n³) validator, and the
document-scope question for sketch entity ids, which M5's constraint references
will force.
