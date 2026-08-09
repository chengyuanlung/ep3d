# M5 Completion Report — Parameter-Driven Constrained Sketch

> **STATUS: FUNCTIONALLY COMPLETE — one verification item open.**
>
> **Four** independent review rounds have run. Each found defects created by the
> previous round's fixes: round 2 in round-1's, round 3 in round-2's, round 4 in
> round-3's. All findings are fixed and mutation-verified, the suite is
> **498/498** in Debug and Release, and **owner manual UI validation has been
> performed and passed** (Tests A–E).
>
> **The round-4 fixes have not themselves been reviewed.** Given the record
> above that is a real gap, not a formality — but it is now the *only* one, and
> the round-4 changes are smaller and more local than any previous round's.
> **DPI scaling remains unverified at the owner's direction.**

**Baseline:** `3734740` — the project-owner-accepted M4 state on `master`.

**Final commit:** `1967238` on branch `m5-wip` — "M5: parameter-driven
constrained Sketch". Not yet squashed into `master`.

---

## Mission

Turn the M4 Sketch into a parameter-driven **constrained** Sketch whose 2D
dimension edits automatically rebuild the downstream 3D geometry.

**The central release proof (spec 22, Gate B):** editing the `Width` Parameter
from 100 mm to 120 mm solves the constrained rectangle and rebuilds the Pad, so
the volume changes 100 000 → 120 000 mm³ at Height 50 and Pad 20. This passes,
against a hand-computed oracle, with the sketch geometry deliberately seeded
off-size and skewed so that a solver doing nothing could not produce it.

---

## Solver backend

| | |
|---|---|
| Backend | **Own Gauss-Newton with Levenberg-Marquardt damping** |
| Linear algebra | **Eigen 5.0.1**, licence **MPL2** |
| Rank / DOF | `ColPivHouseholderQR` with an explicit threshold — DOF is *measured*, never `variables − constraints` |
| Confinement | Eigen appears in **exactly one translation unit** (`src/Solver/GaussNewtonSketchSolver.cpp`), `PRIVATE` to its CMake target |

Independent review verified the confinement with `dumpbin /archivemembers` and
symbol counts (Solver 12269 Eigen symbols; Core, ViewerCore and KernelOcct all
**0**) rather than taking the claim on trust.

MPL2 is strictly less encumbering than the OCCT (LGPL-2.1-with-exception) and Qt
dependencies the project already carries.

---

## ADRs

Six were required by spec 33; **twenty-five** exist. Nineteen more had to be
recorded once review found what the first six had got wrong or left unsaid — and
three of the six were themselves amended, **three times in one case**, each time
because they asserted behaviour the code did not have.

| ADR | Subject |
|---|---|
| M5-001 | Constraint identity and reference model |
| M5-002 | Dimensional constraint units and Parameter binding |
| M5-003 | Sketch solver selection and boundary |
| M5-004 | Solver commit, failure and recovery policy |
| M5-005 | DOF and constraint status semantics |
| M5-006 | Angle constraint convention — **amended twice**, both times because it asserted something the code did not do; the second amendment's own fix was then found to be a regression and was replaced |
| M5-007 | Recomputability is read from the static type |
| M5-008 | Constraint mutation goes through PartDocument |
| M5-009 | Deletion policy: cascade for entities, refuse for Parameters |
| M5-010 | Schema v5: constraints semantic, edges re-derived |
| M5-011 | A residual type must be able to express its constraint — **amended** to state the guard's two limits |
| M5-012 | DOF outranks redundancy; an unmeasured DOF is not zero |
| M5-013 | One reconciler owns the sketch's Parameter edges — **amended**: it claimed four mutation paths and there were five |
| M5-014 | A degenerate configuration is nudged, never called contradictory |
| M5-015 | Every test suite uses PRE_TEST discovery |
| M5-016 | Deleting a referenced Sketch keeps the M4 contract (open question for the owner) |
| M5-017 | A dimensional constraint must bind a Parameter, and both validators must agree |
| M5-018 | Registration failures are checked for every restored type — **amended**: it meant four of six, and two were checked in the wrong order |
| M5-019 | A reconciler must add and remove over the same set |
| M5-020 | Removing a derived node clears the result it derived |
| M5-021 | The deferred Minors, closed |
| M5-022 | A commit answers for what it writes, not for what it found |
| M5-023 | Two tolerances that meet must not meet on the same number |
| M5-024 | Replacing an owned object unhooks the one it replaces |
| M5-025 | A flag the program does not understand is never silently discarded |

---

## Test totals

| | M4 baseline | M5 |
|---|---|---|
| Total | 322 | **498** |
| `ParametricCADCoreTests` | — | 301 |
| `ParametricCADSolverTests` | — | 53 |
| `ParametricCADKernelOcctTests` | — | 47 |
| `ParametricCADIntegrationTests` | — | 87 |
| Viewer smoke tests (ctest) | 1 | 10 |

**498 / 498 in Debug and 498 / 498 in Release.** The Release figure is
independently verified from the ctest log: every command line names
`build/Release/...`, none names `build/Debug/`. That distinction matters here —
an earlier "Release 448/448" in this project's own self-validation was false for
333 of those tests, because default gtest discovery had hard-coded the Debug
binaries.

`ParametricCADIntegrationTests` is new in M5: the only target linking Core +
Solver + KernelOcct, which is what spec 13's selectivity claims and spec 21–28's
gates require.

**48 of the added tests are regressions for review findings** (16 from round 1,
9 from round 2, 9 from round 3, 14 from round 4), and every one was
mutation-verified: the fix removed, the test observed to fail, the fix restored,
the tree re-verified byte-identical.

One relationship is enforced at **compile time** rather than by a test:
`static_assert(kMinSketchDimensionMm > 10 * kSketchToleranceMm)`. A behavioural
test for it passed under the reverted constant, because whether a solve at the
floor lands above or below the tolerance depends on where it started.

---

## Gates A–H

| Gate | Result |
|---|---|
| A — fully constrained rectangle: solved, DOF 0, V 100 000 mm³, m 0.27 kg, COM (50, 25, 10) | **PASS** |
| B — Width 100 → 120 (release-critical): V 120 000 mm³, m 0.324 kg | **PASS** |
| C — Height 50 → 80 at Width 120: V 192 000 mm³, m 0.5184 kg | **PASS** |
| D — selective recompute: PadLength does not re-solve the sketch; Density touches neither sketch nor Pad; unrelated Parameter touches nothing | **PASS** |
| E — conflict: documented status, no corrupt commit, downstream blocked, offending ids named, full recovery proven | **PASS** |
| F — circle R 10 → 20: solves, Pad length unchanged, volume exactly ×4 | **PASS** |
| G — save / load / re-solve on a fresh backend, still parametric afterwards | **PASS** |
| H — under-constrained (DOF > 0, valid geometry, UI says so) → fully constrained | **PASS** |

Every expected number is an independent analytical result computed by hand from
the parameter values. No production helper produced any of them.

---

## Architecture

| Check | Result |
|---|---|
| `src/Core` free of Qt | **PASS** — zero matches |
| `src/Core` free of OCCT | **PASS** — zero matches |
| `src/Core` free of Eigen | **PASS** — 4 matches, all comments stating the absence |
| Binary level (`dumpbin /dependents`) | **PASS** — KERNEL32, MSVCP140, VCRUNTIME140(_1), UCRT only |
| Solver boundary | **PASS** — `ISketchSolver`, `SketchSolveProblem`, `SketchSolveResult` carry no backend type |
| Identity audit | **PASS** — no index, pointer, solver variable, OCCT topology or enum ordinal reaches the file |

---

## Review

Four rounds, all recorded in full in `M5_IndependentReview.md`.

| Round | Scope | Score | Verdict |
|---|---|---|---|
| 1 | Full scorecard, three reviewers | **63 / 100** | REQUEST CHANGES — 3 Critical, 9 Major |
| 2 | Full scorecard, re-review of the fixed tree | **78.5 / 100** | REQUEST CHANGES — 3 Major, **2 caused by round-1 fixes** |
| 3 | Focused on the round-2 deltas | not scored | Not safe to ship — 5 Major, incl. an out-of-bounds write **created by a round-2 fix** |
| 4 | Focused on the round-3 deltas | not scored | Not safe to ship — 5 Major, **3 traceable to round-3 fixes** |

All findings from all four rounds are fixed except one Major declined with
reasons (ADR-M5-016, an open question for the owner) and the items under
Limitations.

**No score is claimed for the current tree.** The last measurement, 78.5/100,
predates the round-3 and round-4 fixes; a number I awarded myself to replace it
would be worth nothing, and this milestone has already produced one
self-validation that reported "Critical: 0" over a broken mandatory constraint.

**Six findings across the four rounds were defects in my own TESTS rather than
in the product**, recorded because each changed what the evidence could prove: a
test that compared angles without treating 0 and 2π as the same direction; two
round-3 tests that could not fail when first written; a floor test that depended
on where the solve happened to land; a message test that skipped its own
assertion; and a fix shipped with no ctest case at all.

---

## Owner manual validation

**Not performed for M5.** By ADR-M4-016 this is the owner's to carry out, and it
must never be described as an independent agent review.

---

## Limitations and deferred work

- **DPI scaling is NOT EXECUTED.** A reviewer measured the shell at
  `QT_SCALE_FACTOR=2` overflowing a 1280×800 screen by roughly a third while
  `--selftest` still reported OK, because the only size assertion is in logical
  pixels and is therefore scale-invariant. M4-era behaviour, not an M5
  regression — but this project's single owner-found Critical was exactly a
  200%-scaling defect.
- **Five of the six previously-deferred Minors are now CLOSED** (ADR-M5-021),
  each with a mutation-verified test: the slot guard now checks each slot's
  `Component` and not just the arity; `CommitSolvedGeometry` validates before
  writing, two-phase; `Unitless` no longer satisfies both unit checks;
  `OutlineState::Blocked` is derived and names the prerequisite that failed;
  and `result.iterations` reports the real count with a message that says which
  exit actually happened.
- `editSketch` bypasses the facade's Parameter-binding validation. Left open
  deliberately: the solver rejects it and names the constraint, the panel shows
  it, and the save validator refuses the file — three catches downstream — and
  closing it at `Sketch::addConstraint` would give the sketch a dependency on
  the document's Parameters that ADR-M5-001 avoided on purpose.
- **Deferred by scope, as the spec requires:** DXF import, automatic dimension
  reconstruction, a full production Sketcher, Assembly, persistent OCCT
  subshape naming, CAM, FEA, dynamics.

---

## M5 status and M6 readiness

**M5 is functionally complete. One verification item remains open.**

What is done and measured: Gates A–H pass against hand-computed oracles; the
central release proof (Width 100 → 120 rebuilding the solid to 120 000 mm³)
holds; 482/482 in Debug and Release; the architecture boundary is clean at
source, include, link and binary level; and every review finding is either fixed
with a mutation-verified regression test or explicitly declined or deferred.

Also done since the draft: **owner manual UI validation passed** (Tests A–E,
`M5_UI_UserValidation.md`), including the release-critical Width 100 → 120 edit
and the 3D picking accuracy that produced this project's only owner-found
Critical in M4.

What stands between that and "complete":

1. **The round-4 fixes are unreviewed.** Four consecutive rounds each found
   defects created by the previous round's fixes; assuming the fifth broke the
   pattern is the assumption that has failed four times. Against that: the
   round-4 changes are the smallest of any round, all are mutation-verified, and
   one is enforced by the compiler.
2. **DPI scaling is unverified**, at the owner's direction. The shell's
   1000 × 640 logical minimum cannot fit a 200%-scaled 1280 × 800 desktop, and
   the selftest's only size assertion is in logical pixels — scale-invariant by
   construction, so no automated check in this project can fail on a scaling
   defect. A fix was prototyped and reverted whole rather than left half-applied.
3. **The owner UI validation did not record the display configuration used.** By
   ADR-M4-015 that limits what it proves to that one unrecorded configuration.

**M6 readiness:** the M6 dependency — a Sketch whose constraints drive geometry
through a stable, semantic, persistable identity model — is in place and
exercised end to end, and the owner has now used it. M6 (DXF import → stable
Sketch entities) can be planned against it.

My recommendation on sequencing, for the owner to weigh: one more focused review
of the round-4 deltas before M6 implementation begins. Four rounds of evidence
say the marginal round still finds something, and each round's findings have
been cheaper to fix than to inherit.
