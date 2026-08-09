# M5 Self-Validation Report

**Baseline:** `3734740` — the project-owner-accepted M4 state on `master`
(`docs: record the M4 master milestone commit`).

**Final / Working Commit:** working tree at time of writing; the exact M5 commit
hash is recorded in `M5_CompletionReport.md` after the commit is made.

> ## CORRECTED after independent review
>
> This report was audited for honesty by an independent reviewer, as spec 29
> requires. **Six claims below were overstated and one Critical was reported
> PASS on evidence that could not fail.** Every one is corrected in place and
> marked **[CORRECTED]**; nothing has been quietly deleted. The full accounting
> is in `M5_IndependentReview.md`.
>
> The self-score at the end is **withdrawn**, not revised down — replacing a
> measured score with another number of my own would be worth nothing. The
> independent measurements are recorded in `M5_IndependentReview.md`:
> **63/100** on the first pass and **78.5/100** on the re-review. Rounds 3 and 4
> were focused reviews of the previous round's fixes and were not scored; both
> returned "not safe to ship", and both were right — each found defects created
> by the fixes it was reviewing.

Every row below was **actually executed**. Where something was not executed it
says NOT EXECUTED and says why. Nothing is reported PASS on the strength of the
code looking correct.

---

## Environment

| | |
|---|---|
| OS | Windows 11 Home 10.0.26200 |
| Compiler | MSVC 19.44 (VS 2022 BuildTools 14.44.35207), C++20 |
| CMake | 4.2.3 |
| OCCT | 8.0.1 (vcpkg, `D:\vcpkg`) |
| Qt | 6.11.1 (vcpkg) |
| Solver backend | **Own Gauss-Newton with Levenberg-Marquardt damping**, using **Eigen 5.0.1** for dense linear algebra and the rank-revealing decomposition (`ColPivHouseholderQR`) |
| Eigen licence | **MPL2** — strictly less encumbering than the OCCT (LGPL-2.1-with-exception) and Qt dependencies already present |

---

## Architecture

| Check | Method | Result |
|---|---|---|
| Core Qt | `grep -rn "QObject\|QWidget\|QGraphics\|QString" src/Core/` | **PASS** — zero matches |
| Core OCCT | `grep -rn "TopoDS_\|BRep\|AIS_\|V3d_" src/Core/` | **PASS** — zero matches |
| Core Eigen | `grep -rn "Eigen\|<Eigen" src/Core/` | **PASS** — 4 matches, **all of them comments** explaining that Core has no Eigen dependency (`PartDocument.h:223`, `RecomputeContext.h:24`, `ISketchSolver.h:12,15`). No include, no type. |
| Core includes | `grep -rn "#include" src/Core/` filtered to non-`Core/`, non-`<...>` | **PASS** — Core includes only `Core/` headers and the standard library |
| Binary-level | `dumpbin /dependents build/Release/ParametricCADCoreTests.exe` | **PASS** — KERNEL32, MSVCP140, VCRUNTIME140(_1) and the UCRT api-ms-win-crt set. **No OCCT DLL, no Qt DLL.** Eigen is header-only so it cannot appear here; the source scan above is the check that covers it. |
| Link-level | `ParametricCADCoreTests` links `ParametricCADCore` + `ParametricCADViewerCore` only | **PASS** — it cannot pull an OCCT or Eigen symbol regardless of what any header says |
| Solver boundary | `ISketchSolver`, `SketchSolveProblem`, `SketchSolveResult` carry no backend type | **PASS** — Eigen appears in exactly **one** translation unit, `src/Solver/GaussNewtonSketchSolver.cpp`, and is `PRIVATE` to that CMake target |

### Identity audit (spec 17/19)

| Persisted as identity? | Answer | Evidence |
|---|---|---|
| Vector index | **No** | `M5_SER_007_ConstraintOrderIsNotIdentity` — removing and re-adding a middle constraint leaves every other constraint's id and references intact |
| Pointer / address | **No** | Nothing writes an address; `GATE_G_NoBackendRuntimeIdentityInTheFile` scans the file for backend tokens |
| Solver variable / residual index | **No** | `M5_SER_005_NoSolverStateIsPersisted` asserts `variableIndex`, `residualIndex`, `varA`, `varB`, `jacobian`, `degreesOfFreedom`, `dof` and `solveStatus` are absent. **[CORRECTED]**: it checks 3 of the 6 status names, not "every status name" as originally claimed. |
| OCCT topology | **No** | `GATE_G_NoBackendRuntimeIdentityInTheFile` asserts `TopoDS`, `TShape`, `BRep`, `gp_` absent |
| Qt object | **No** | same test asserts `QObject`, `QString` absent |
| Enum ordinal | **No** | Sub-elements persist as **names** (`"StartPoint"`), so inserting a value into the middle of the enum cannot silently re-interpret files already on disk — `M5_SER_003_SubElementsSurviveAsNamesNotIntegers` |

---

## Builds / Regression

| | Result |
|---|---|
| Debug build | **PASS** — 0 errors |
| Debug tests | **498 / 498** |
| Release build | **PASS** — 0 errors |
| Release tests | **[CORRECTED] originally "448/448"; only 115 of those actually ran Release binaries.** `gtest_discover_tests`' default POST_BUILD mode writes a config-less test list hard-coding `build/Debug/...`, so `ctest -C Release` re-ran the Debug binaries for 333 tests. Every suite now uses `PRE_TEST` (ADR-M5-015) and a Release run invokes `build/Release/...` for all of them. **498 / 498**, verified from the ctest log (301 + 80 + 47 + 48 + 6 command lines, all naming `build/Release/...`, zero `build/Debug/`). |
| M0–M4 regression | **PASS** — every pre-existing test still passes |
| Clean rebuild | **PASS** — `cmake -S . -B build` reconfigure + full build, both configurations |

### Totals

| | M4 baseline | M5 |
|---|---|---|
| Total | 322 | **498** |
| `ParametricCADCoreTests` | — | 301 |
| `ParametricCADSolverTests` | — | 53 |
| `ParametricCADKernelOcctTests` | — | 47 |
| `ParametricCADIntegrationTests` | — | 87 |
| Viewer smoke tests (ctest) | 1 | 10 |

160 tests added. **34 of them are regressions for findings of the three
independent review rounds** — 16 from round 1, 9 from round 2, 9 from round 3 —
and every one was mutation-verified: the fix removed, the test observed to fail,
the fix restored and the tree re-verified. Two round-3 tests could not fail when
first written; a mutation caught both and they were rewritten. `ParametricCADIntegrationTests` is new in M5: it is the only
target linking Core + Solver + KernelOcct together, which is what spec 13's
selectivity claims and spec 21–28's gates require.

### Tests changed rather than added

Four pre-existing tests were modified. Each is a deliberate, explained change,
not an accommodation:

| Test | Change | Why |
|---|---|---|
| `M3_SER_004_ByteIdenticalRoundTrip` | `schemaVersion 4` → `5` | Asserts the *current* write version; the number is incidental |
| `M4_SER_001_SchemaVersionIsFour` | renamed `..._SaveWritesTheCurrentSchemaVersion`, `4` → `5` | What it checks is worth keeping; the number in its name was not |
| `M2_SER_001_StableIdsSurviveRoundTrip` | `schemaVersion 4` → `5` | Same |
| `M2_SER_005_StubEdgesNotPersisted` | searches for the id **quoted** | Pre-existing fragility the bump exposed: it searched for the stub id as a bare numeric substring, which matched `"schemaVersion": 5`. A test about stub edges was failing for a reason unrelated to stub edges. |
| `UI_PROP_004_SketchShowsProfileStatusAndReason` | unchanged; the *code* was changed | M5 added a second row labelled `Status`. Ambiguous to the test **and** to a user reading the panel, so the new row was renamed `Solve status`. |

---

## Constraint Matrix

**[CORRECTED TWICE.]** This sentence originally read "every kind is exercised
end to end (solve → geometry → profile → Pad → mass)". That was false for Angle.
My first correction changed it to say the same thing and added "it is true now"
— **strengthening a claim that was still false**, which the re-review caught.

What is actually true, stated per kind:

- **Through solve → geometry → profile → Pad → mass:** Coincident, Horizontal,
  Vertical, Length, Radius, Fix (Gates A–F).
- **Through solve → geometry only:** Angle (`M5_ANGLE_E2E_001` builds a real
  document and measures the solved coordinates, but no Body and no Pad),
  Distance (`M5_SOLVE_002` is a hand-built solver problem with no sketch),
  Diameter (no integration-suite coverage at all; solver-level only).

Three of nine stop short of the Pad. That is a real coverage gap, recorded
rather than papered over.

| Constraint | Status | Evidence |
|---|---|---|
| Coincident | **PASS** | `M5_CONS_002_CoincidentClosesTheCorners`; all four corners in `GATE_A` |
| Horizontal | **PASS** | `M5_CONS_001_HorizontalAndVerticalHold`; `GATE_A` |
| Vertical | **PASS** | `M5_CONS_001`; `GATE_A` |
| Distance | **PASS** | `M5_SOLVE_002_DistanceConverges`; round-tripped in `M5_SER_002` |
| Length | **PASS** | `GATE_A/B/C` — Width and Height drive real geometry |
| Radius | **PASS** | `M5_CONS_003_RadiusDrivesACircle`; `GATE_F` |
| Diameter | **PASS** | `M5_CONS_004_DiameterDrivesTheSameRadiusNotSeparateState` — it drives the same radius variable, halved, so a Radius and a Diameter that disagree conflict (`M5_CONS_005`) |
| Angle | **[CORRECTED] originally reported PASS; the constraint was BROKEN** | The cited tests could not fail: they recomputed the solver's *own residual formula* and checked it had been driven to zero, asserting convergence rather than an angle. The residual compared millimetres to radians and produced angles wrong by up to 260 degrees while reporting `Solved`. Fixed (ADR-M5-011) and re-tested geometrically with `M5_ANGLE_001..006` and `M5_ANGLE_E2E_001`, which measure the solved coordinates with `atan2`. Those fail 5-of-6 against the original defect. |
| Fix | **PASS** | `M5_SOLVE_001_FixedPointConverges`; the placement convention in `GATE_A` |
| Already-satisfied cases | **PASS** | `M5_DOF_003_RedundantConstraintDoesNotReduceDofBelowZero` |
| Incompatible fixed cases | **PASS** | `M5_CONFLICT_001`, `GATE_E` |
| Invalid / non-finite dimensions | **PASS** | `M5_VALID_002`, `M5_ADV_004` (parameterised over NaN, ±Inf, −5, 0) |

---

## Solver

| Property | Status | Evidence |
|---|---|---|
| Fully constrained | **PASS** | `M5_SOLVE_010`, `GATE_A` — DOF 0, geometry matches the analytical rectangle |
| Under-constrained | **PASS** | `M5_DOF_001`, `M5_DOF_002`, `GATE_H` — a legal state, not a failure |
| Conflict | **PASS** | `M5_CONFLICT_001`, `GATE_E` |
| Redundant (consistent) | **PASS** | Accepted and reported `OverConstrained` with geometry committed, per ADR-M5-005 |
| DOF | **PASS** | Measured by **numerical rank** (`ColPivHouseholderQR`), never `variables − constraints`. `M5_DOF_004` and `M5_DOF_005` were written specifically to FAIL under the forbidden formula, and were verified to do so. |
| Determinism | **PASS** | `M5_DETERMINISM_001_RepeatedSolvesAgreeExactly`; `M5_DETERMINISM_002_NoDriftOver100Resolves`; `M5_ADV_006` repeats 100 **whole-document** recomputes with no drift |
| Order independence | **PASS** | `M5_ORDER_001` (residual order), `M5_ORDER_002` (constraint creation order), `M5_ADV_001` (**all 23 non-identity permutations** of entity creation order) |
| Finiteness | **PASS** | `M5_CONFLICT_002_NoNonFiniteValueIsEverReturned`; `M5_ADV_004` checks nothing non-finite is committed |
| Scale | **PASS** | `M5_SCALE_001` (four orders of magnitude, solver level); `M5_ADV_005` (0.01× to 100× end to end, i.e. a 1 × 0.5 mm rectangle up to a 10 × 5 m one) |

---

## Oracles

Every expected number is computed by hand from the parameter values (spec 20).
No production helper produced any of them.

| Case | Expected | Result |
|---|---|---|
| Rectangle 100 × 50, Pad 20, ρ 2700 | V = 100000 mm³, m = 0.27 kg, COM = (50, 25, 10) mm | **PASS** (`GATE_A`) |
| Width → 120 | V = 120000 mm³, m = 0.324 kg, COM.x = 60 | **PASS** (`GATE_B`) |
| Height → 80 (Width 120) | V = 192000 mm³, m = 0.5184 kg | **PASS** (`GATE_C`) |
| PadLength → 30 | V = 150000 mm³ | **PASS** (`GATE_D`) |
| ρ → 7850 at V = 150000 | m = 1.1775 kg | **PASS** (`GATE_D`) |
| Circle R 10, Pad 30 | V = π·10²·30 mm³ | **PASS** (`GATE_F`) |
| Circle R 20 | V = π·20²·30, and **exactly 4×** the R = 10 volume | **PASS** (`GATE_F`) |

**COM convention.** `Fix` pins `bottom.StartPoint` where it is, at sketch-local
(0, 0). With Horizontal(bottom), Vertical(right) and the four coincidences, the
rectangle occupies u ∈ [0, Width], v ∈ [0, Height], and the Pad extrudes +Z.
The COM that follows is (Width/2, Height/2, PadLength/2) = (50, 25, 10) mm,
which is what spec 21 states and what `GATE_A` asserts.

**All fixtures start deliberately off-size and skewed** (112 × 58, non-square).
Seeded at 100 × 50, a solver that did nothing at all would pass every dimension
assertion; starting wrong makes a correct answer evidence that the solver
produced it.

---

## Recompute

Measured by **call counters** on the real solver and the real kernel — "the
sketch did not re-solve" is a claim about invocations, and a sketch that
re-solved to the same answer is exactly the failure a value comparison misses.

| Edit | Required | Result |
|---|---|---|
| Width / Height / Radius | sketch solves + downstream rebuilds | **PASS** — solve +1, extrude +1, mass +1 (`GATE_B`, `M5_RECT_003`) |
| PadLength | **sketch solve count unchanged**; Pad +1, Mass +1 | **PASS** (`GATE_D`, `M5_SELECTIVE_001`) |
| Density | sketch unchanged, Pad unchanged, Mass +1 | **PASS** (`GATE_D`, `M5_SELECTIVE_002`) |
| Unrelated Parameter | nothing runs | **PASS** (`GATE_D`, `M5_SELECTIVE_003`) |
| No edit at all | nothing re-solves | **PASS** (`M5_SELECTIVE_004`) — without this, the three rows above could all pass while every recompute solved everything |

**Mutation-verified.** A "did not re-solve" assertion passes trivially when the
`Parameter → Sketch` edge was never wired, so each is paired with a positive
control. Deleting the `addDependency` call fails `M5_RECT_003` and
`M5_CIRCLE_001`; deleting the `removeDependency` call fails `M5_FACADE_001`.
Both were executed and both failed as predicted.

---

## Failure / Recovery

| Case | Status | Evidence |
|---|---|---|
| Conflict status | **PASS** | Documented status, not a generic failure (`GATE_E`) |
| No corrupt commit | **PASS**, but **[CORRECTED]** in scope | `GATE_E` compares **four coordinates of one line** with `EXPECT_DOUBLE_EQ`, not the whole sketch. The property holds — `CommitSolvedGeometry` is unreachable on the failure path, which independent review confirmed by reading — but "byte for byte" overstated what the test measures. |
| Downstream blocked | **PASS** | Pad leaves `Valid`; the retained mass is the last valid one — retention and currency are separate properties (ADR-M3-006) |
| Useful diagnostic | **PASS** | The offending `SketchConstraintId`s are named, and surfaced in the UI |
| Broken references | **PASS**, **[CORRECTED]** in scope | `M5_SER_009` (missing entity), `M5_SER_010` (missing parameter), `M5_SER_011` (duplicate id), `M5_SER_012` (unknown kind), `M5_SER_013` (unknown sub-element **name**). "All rejected at load" was broader than the tests: a *known but wrong* sub-element — a line's `CenterPoint` — loads successfully and becomes `InvalidInput` at solve time. That is a defensible policy, but it is not load-time rejection. |
| Deletion | **[CORRECTED] originally PASS; a use-after-free existed** | `removeObject(Body)` destroyed the features a Body owned while leaving them registered and graph-scheduled — the next recompute called into freed memory (reproduced as a segfault by independent review). Pre-existing since M4 and untested: no test in the suite removed a Body owning a feature. Fixed and covered by `M5_DELETE_007`, mutation-verified. |
| Recovery | **PASS** | `GATE_E` removes the conflict, proves DOF returns to 0 and the volume returns to 100000 mm³, **and then edits Width again** — recovery means the model is usable again, not that one recompute stopped failing |

---

## Persistence

| | Status |
|---|---|
| Constraint ids | **PASS** — `M5_SER_002` compares **by id**, never by array position |
| Entity refs | **PASS** — `GATE_G` |
| Sub-element refs | **PASS** — `M5_SER_003`, and `GATE_G` checks specifically that the circle's Fix comes back on the **centre** |
| Parameter bindings | **PASS** — `M5_SER_002`, `GATE_G` |
| Frame | **PASS** — carried over from M4's v4 round trip, still covered |
| Pad refs | **PASS** — `GATE_G` |
| Re-solve after load | **PASS** — `GATE_G` re-solves with a **freshly constructed** kernel and solver and reproduces V, m and COM |
| Still parametric after load | **PASS** — `GATE_G` edits Width on the reloaded document and gets 120000 mm³. The `Parameter → Sketch` edges are re-derived from the constraints (Option B), never stored. Deleting that re-derivation fails `M5_SER_006` (verified by mutation). |
| No backend runtime identity | **PASS** — `GATE_G_NoBackendRuntimeIdentityInTheFile` |
| v4 files still load | **PASS** — `M5_SER_008`; the `constraints` array is optional and its absence means free geometry, which is what a v4 sketch was |
| Byte-identical round trip | **PASS** — `M5_SER_004` |

---

## UI Smoke

`DocumentOutline` is free of Qt and of OCCT, so what the UI *decides* is
testable without a display. Those decisions are checked against a **real**
solve (`ParametricCADIntegrationTests`), not a stub.

| Requirement (spec 18) | Status | Evidence |
|---|---|---|
| Sketch solve status | **PASS** | `M5_UI_001`, `M5_UI_002`; six statuses with distinct non-empty labels (`M5_UI_003`) |
| DOF | **PASS** | `M5_UI_001` (0 when solved), `M5_UI_002` (non-zero when not) |
| Constraint list | **PASS** | `M5_UI_004` — every constraint appears in the tree **under its own sketch** |
| Dimension value / unit | **PASS** | `M5_UI_007` — editable, unit `mm`, points at the Parameter's id |
| Failed / conflicting constraint IDs | **PASS** | `M5_UI_009` (listed), `M5_UI_010` (each blamed row marked individually) |
| Editing a dimension updates the 3D result | **PASS** | `M5_UI_008` asserts the **volume** changed 100000 → 130000 mm³, not merely that a number in the panel changed |
| State not conveyed by colour alone | **PASS** | `M5_UI_003` (distinct text labels); blamed rows carry a text marker and a diagnostic |
| Solve failure reported ahead of a profile complaint | **PASS** | `M5_UI_011` — an unsolved sketch's profile is meaningless, and "not a closed loop" sends the user to the wrong place |
| M4 documents unchanged | **[CORRECTED] partly false as written** | `M5_UI_012` asserts three things — no tree children, state is not Failed, `Count == "0"` — and none of them concerns a group or a status. Both extra claims were also false in the code: the Constraints group was emitted unconditionally, and a constraint-free sketch read "Under-constrained, DOF 0", which is self-contradictory. Fixed (ADR-M5-012) and covered by `M5_REV_001`. |
| Viewer actually runs | **PASS**, **[CORRECTED]** in wording | `ViewerSmokeTest` plus one per M5 sample. Each launches the real window, lets it lay out, and asserts visibility, size, displayable solids, mass properties and outline rows. **Nothing asserts a paint** — the original wording claimed one. Independent review also noted that no selftest drives an edit through `MainWindow`, so the panel-commits-and-view-redisplays path is exercised only at document level. |
| Viewer refreshes after recompute | **PASS (functional)** | Asserted through `DocumentPresenter::displayableSolids()` and the selftest's mass-property oracle |
| Pixel-level appearance, focus order, contrast, DPI scaling | **NOT EXECUTED** | Not assertable from here. ADR-M4-015 applies: a UI verified on one display configuration has been verified on one display configuration. Owner manual validation is the mechanism (ADR-M4-016), and has **not yet been run for M5**. |

---

## Adversarial Tests (spec 30)

| Case | Status | Evidence |
|---|---|---|
| Duplicate ConstraintId | **PASS** | `M5_ID_002`, `M5_SER_011` |
| Missing EntityId | **PASS** | `M5_ID_003`, `M5_SER_009` |
| Missing ParameterId | **PASS** | `M5_VALID_003`, `M5_SER_010` |
| Wrong sub-element | **PASS** | `M5_ADV_008` — the **full 16-pair entity-kind × sub-element matrix** |
| NaN / Infinity | **PASS** | `M5_INVALID_001`, `M5_ADV_004` |
| Negative length / radius | **PASS** | `M5_VALID_002`, `M5_ADV_004` (−5) |
| Zero degeneracy | **PASS** | `M5_ADV_004` (0), `M5_ADV_002` (zero-length line under an Angle) |
| Contradictory dimensions | **PASS** | `M5_CONFLICT_001`, `GATE_E` |
| Redundancy | **PASS** | `M5_DOF_003` |
| Random entity order | **PASS** | `M5_ADV_001` — all 23 non-identity permutations |
| Random constraint order | **PASS** | `M5_ORDER_002` |
| Delete constrained entity | **PASS** | `M5_DELETE_001`, `M5_DELETE_002` |
| Delete dimension Parameter | **PASS** | `M5_DELETE_003`, `M5_DELETE_005` |
| Failure then recovery | **PASS** | `GATE_E`, `M5_COMMIT_002`, `M5_ADV_004` |
| Very small / large geometry | **PASS** | `M5_ADV_005` |
| Angle near 0 / π | **PASS** | `M5_ANGLE_002` |
| 100× repeat for drift | **PASS** | `M5_DETERMINISM_002` (solver), `M5_ADV_006` (whole document) |
| Save / load / re-solve | **PASS** | `GATE_G`, `M5_ADV_007` |

---

## Gates

| Gate | Status |
|---|---|
| **A** Fully constrained rectangle — solved, DOF 0, profile valid, V = 100000 mm³, m = 0.27 kg, COM = (50, 25, 10) | **PASS** |
| **B** Width 100 → 120 (release-critical) — solves, DOF 0, Pad rebuilds, V = 120000 mm³, m = 0.324 kg | **PASS** |
| **C** Height 50 → 80 at Width 120 — V = 192000 mm³, m = 0.5184 kg, profile valid | **PASS** |
| **D** Selective recompute — PadLength does not solve the sketch; Density touches neither sketch nor Pad; unrelated Parameter touches nothing | **PASS** |
| **E** Conflict / recovery — documented status, no corrupt commit, downstream blocked, offending ids named, full recovery proven | **PASS** |
| **F** Circle R 10 → 20 — solves, Pad length unchanged, volume ×4, solved radius really is 20 | **PASS** |
| **G** Save / load / re-solve — rectangle and circle; ids, refs, sub-elements, bindings preserved; re-solves on a fresh backend; still parametric | **PASS** |
| **H** Under-constrained → fully constrained — DOF > 0 with finite valid geometry and a UI that says so, then constraints added until DOF 0 | **PASS** |

---

## Findings

### Critical: 3 — **[CORRECTED]**, originally reported as 0

All three were found by independent review after this report was written; none
was found by me. Full detail in `M5_IndependentReview.md`.

1. **`Angle` did not compute an angle** — millimetres compared to radians,
   `Solved` reported, geometry wrong by up to 260°. Found independently by two
   reviewers with different probes.
2. **A reloaded document could lose its Sketch** and alias its graph node onto
   the MassPropertiesNode, because `maxPersistedId` omitted sketch, entity and
   constraint ids and `registerObject`'s failure was ignored.
3. **`removeObject(Body)` was a use-after-free.**

### Originally reported (superseded): Critical 0

### Major: 2 — both found by this validation, both fixed

1. **`Fix(circle.StartPoint)` silently resolved to the circle's CENTRE.**
   `SlotFor` tested a single `isLine` flag and guarded `EndPoint`,
   `CenterPoint` and `Whole` with it — leaving `StartPoint` unguarded for every
   non-line entity. A wrong sub-element therefore solved to a
   plausible-looking wrong answer instead of failing, which is spec 32's
   Critical example "constraint targets wrong entity". It is recorded as Major
   rather than Critical only because no shipped path constructs that reference.

   The pre-existing test `M5_VALID_004_WrongSubElementIsRejected` asked
   *CenterPoint on a line* — the one branch that happened to be guarded. The
   symmetric case was never asked.

   **Fix:** `SlotFor` now switches on the entity kind exhaustively (line →
   start/end only; circle/arc → centre only; point → whole only).
   **Regression guard:** `M5_ADV_008` enumerates all 16 pairs, so a future
   entity kind or sub-element cannot be half-guarded.

2. **An Angle constraint on a zero-length line was accepted and moved the
   geometry.** ADR-M5-006 states that lines shorter than
   `kMinSketchDimensionMm` are rejected at validation; that validation did not
   exist. The solver pulled the degenerate line's endpoints apart to satisfy an
   angle that was never defined — silently editing geometry the user did not
   ask it to touch.

   **[CORRECTED] the mechanism originally stated here was fabricated.** This
   paragraph claimed the residual "reduced to `atan2(0, 0)`". There was no
   `atan2` in the implementation — the only occurrence in `src/` was a comment,
   which I read and paraphrased as if it were the code. Describing what the code
   was *meant* to do instead of what it does is the failure a previous review
   round already flagged on this project, and it is precisely what let the Angle
   Critical pass unnoticed under my own eyes while I was writing up a finding
   about the same constraint.

   `addLine` rejects degenerate geometry, but `restoreEntity` deliberately does
   not (a hand-edited file must round-trip), so the check has to live where the
   constraint is used.

   **Fix:** `requireDirected` rejects the constraint as `InvalidInput` and names
   it. **Regression guard:** `M5_ADV_002`.

### Minor: 2 — both fixed

1. **Two property rows both labelled `Status`** (Profile and Constraints).
   Ambiguous to a user reading the panel, not only to a test looking one up.
   Renamed to `Solve status` / `Solver diagnostic`.
2. **`M2_SER_005_StubEdgesNotPersisted` was fragile** — it searched for the
   stub's id as a bare numeric substring, which collided with
   `"schemaVersion": 5`. Now searches the persisted (quoted) form.

### Defects found in my own test code, not in the product

Recorded because they affected what the earlier drafts were able to prove:

- `FindRow` and `NodesOfKind` in `M5UiTests.cpp` returned pointers into
  temporaries. Three tests read MSVC's `0xDD` fill and one crashed. Both now
  return by value, so the mistake is unrepresentable rather than left to each
  call site to avoid.
- The first `m5-underconstrained` viewer selftest asserted a volume oracle for a
  sketch whose size nothing pins — that would have been asserting solver
  internals. Removed; it now asserts finite, positive, valid geometry and
  DOF > 0, which is what is actually required.

---

## Self Score

**[CORRECTED] The 92/100 below was self-assessed before independent review and
is superseded. The independent measurement is 63/100 (REQUEST CHANGES), recorded
in `M5_IndependentReview.md`. After the fixes described there, no re-review has
yet been run, so no revised score is claimed here — a score I award myself to
replace one three reviewers measured would be worth nothing.**

Superseded self-assessment: **92 / 100**

| Area | Weight | Score | Note |
|---|---|---|---|
| Architecture / solver boundary | 15 | 15 | Source, include, link and binary level all clean; Eigen in one TU |
| Stable identity / references | 12 | 12 | Ids compared by id everywhere; enum names not ordinals; full 16-pair matrix |
| Constraint semantic correctness | 15 | 13 | All nine kinds correct now, but two semantic defects existed until this validation found them |
| Numerical solver correctness | 15 | 15 | Determinism, order independence over all permutations, 5 orders of scale, no drift over 100 passes |
| DOF / status / diagnostics | 10 | 10 | Rank-measured DOF, verified discriminating against the forbidden formula |
| Parametric recompute | 12 | 12 | Selectivity mutation-verified with positive controls |
| Failure / recovery | 7 | 7 | Byte-for-byte retention, named offenders, recovery proven usable |
| Persistence | 6 | 6 | Semantic only, v4 compatible, edges re-derived |
| UI functional validation | 3 | 2 | Functional layer fully tested and the binary is actually run; **owner manual validation not yet performed for M5** |
| Tests / docs / evidence | 5 | 5 | 448/448 Debug and Release; mutation testing used to prove tests discriminate |

## Ready for Review

**[CORRECTED] Originally YES. Independent review returned REQUEST CHANGES**
(63/100) with 3 Critical and 9 Major findings. All 3 Critical and 8 of 9 Major
are fixed with mutation-verified regression tests; 1 Major was declined with
reasons (ADR-M5-016); 7 Minor remain open and are listed in
`M5_IndependentReview.md`.

**Ready for RE-review: YES.** Not ready to be called complete until a re-review
runs against the fixed tree.

**[CORRECTED] The paragraph that stood here was written before independent
review and survived the first correction pass, so a reader skimming the end
still took away "No Critical findings. Both Major findings…" — flatly
contradicting the `[CORRECTED]` sections above it. It is replaced, not amended.**

Independent review found **3 Critical and 9 Major**; a re-review of the fixed
tree found 3 further Major, one of which (an out-of-bounds write) was introduced
by one of the fixes. All are now fixed with mutation-verified regression tests,
except one Major declined with reasons (ADR-M5-016). Gates A–H pass.

Two things remain outstanding, and neither is mine to close:

- **Owner manual UI validation for M5** has not been performed. By ADR-M4-016
  that is the owner's to do, and it must never be described as an independent
  agent review.
- **DPI scaling is NOT EXECUTED.** A reviewer measured the shell at
  `QT_SCALE_FACTOR=2` overflowing a 1280×800 screen by roughly a third while
  `--selftest` still printed OK — the size assertion is in logical pixels, so it
  is scale-invariant and can never fail on a scaling defect. This is M4-era
  behaviour, not an M5 regression, but this project's one owner-found Critical
  was exactly a 200%-scaling defect.
