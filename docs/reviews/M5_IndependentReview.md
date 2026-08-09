# M5 Independent Review — findings, responses, and corrections

Three independent reviewer agents reviewed M5 in parallel, partitioned by the
spec 32 scorecard. None wrote the code under review. Each was instructed to
verify claims by execution, to write a test demonstrating any suspected defect,
and to treat the self-validation report as **claims to verify**, not as fact.

| Reviewer | Areas | Weight | Score | Verdict |
|---|---|---|---|---|
| 1 | Constraint semantics, numerical solver, DOF/status/diagnostics | 40 | **22** | REQUEST CHANGES |
| 2 | Architecture/solver boundary, stable identity, persistence | 33 | **22** | REQUEST CHANGES |
| 3 | Parametric recompute, failure/recovery, UI, tests/evidence | 27 | **19** | REQUEST CHANGES |
| | **Total** | **100** | **63** | **REQUEST CHANGES** |

Every finding below was reproduced by the reviewer running code, not by reading
it. Reviewers 1 and 3 found the Critical **independently, with different probe
programs**.

---

## Critical

### C1 — `Angle` did not compute an angle *(found twice, independently)*

`SketchSolveSession.cpp` packed only the two lines' **v components**, and the
solver evaluated `sin(dvB - dvA - target)` — subtracting a millimetre difference
from a radian target. No `atan2`, no direction vector; the u components never
entered. The only `atan2` anywhere in `src/` was in a **comment** describing what
the code was supposed to do.

Measured: asking for 90° produced **350.96°**; asking for 30° produced **21.50°**;
the result was not rotation-invariant; and an *already exactly 90°* pair was
"corrected" by stretching one line ~1 mm. Every one of these reported `Solved`
with DOF 0 and a residual around 1e-11.

Spec 32 lists both "unit mismatch changes physical geometry" and "conflict
silently produces wrong solid" as Critical examples. This is the first.

**Root cause, not just the symptom.** `SolveResidual` had four variable slots. A
line-to-line angle needs eight. The type could not express the constraint, so the
implementation packed something that fit.

**Fixed** — ADR-M5-011. Eight slots; `atan2`-based wrapped angular difference;
`SlotsRequired(kind)` published on the interface; and the solver now **rejects**
an under-packed residual instead of reading `vars[-1]`.

**Why 444 tests missed it.** Reviewer 1 mutated `startV/endV` → `startU/endU`,
changing the constraint's meaning completely, and the full suite stayed green.
No test built an `AngleConstraint` and measured geometry. The two tests named for
angles ended by recomputing *the solver's own residual formula* and checking the
solver had driven it to zero — asserting convergence, not an angle.

**Replaced with six geometric tests** that measure the solved coordinates with
`atan2`, plus an end-to-end test on a real document. Against the original defect
the new tests fail **5 of 6**; the ones they replaced failed **0 of 2**.

### C2 — A reloaded document could lose its Sketch and alias the MassPropertiesNode

`maxPersistedId` was built from document, parameter, material, body and feature
ids only. **Sketch, entity and constraint ids were omitted** — and in a v5 file
those are typically the largest ids present. The generator was left below the
file's true maximum, `PartDocument`'s constructor then allocated
`massPropertiesNode_` and the Origin frame from it, and one of them took an id a
sketch in the same file already owned. `restoreSketch`'s `registerObject`
returned `false` (duplicate) and **the return value was ignored**, while
`graph_.addNode` succeeded — so the two objects shared one node.

Consequence: the Pad loses its profile *and* the sketch is never invoked — the
two silent failures ADR-M5-007 claimed to have eliminated. Re-saving still
produced a loadable file, so nothing ever reported an error. Reproduced through
the public API alone.

**Fixed.** All three id kinds join `maxPersistedId`; `registerObject`'s result is
checked; and the loader converts an invariant violation into a clean load
failure rather than letting it escape.

**Why the suite missed it:** every serialization test saves and loads *in one
process*, where the generator is already past every id. The regression test
(`M5_SER_015`) rewrites the sketch-side ids to exactly the next id the generator
will hand out, which is the collision a fresh process sees.

### C3 — `removeObject(Body)` was a use-after-free

The `Body*` branch erased the body — destroying every `Feature` it owned — but
never unregistered those features or removed their graph nodes. The next
`recompute()` called `recompute()` on freed memory. Reviewer 2 reproduced the
segfault. `savePartDocument` succeeded in between, so the crash landed later and
elsewhere. `ObjectRegistry`'s own header asserts the opposite invariant.

Pre-existing since M4; spec 32 makes "dangling reference/use-after-free" an
automatic Critical, and the M5 self-validation reported Deletion as PASS.

**Fixed**, with `M5_DELETE_007` — mutation-verified.

---

## Major — fixed

| # | Finding | Fix |
|---|---|---|
| M1 | `Distance`/`Length` between exactly coincident points reported `Conflicting` — "no configuration satisfies them" — for a system with an **infinite** solution set. The `sqrt` residual's central difference is exactly zero there, so the Jacobian row vanished. | Perturb the solver's **starting guess**, never stored geometry — ADR-M5-014 |
| M2 | A redundant constraint on an under-constrained sketch reported `OverConstrained`, telling the user there are too many constraints on a sketch that needs more, and hiding the free degrees. | DOF outranks redundancy — ADR-M5-012 |
| M3 | A constraint-free sketch reported `DOF = 0` — and 0 is this project's signal for *fully constrained*. Every M4 document displayed "Under-constrained, DOF 0". | Report the real free-variable count — ADR-M5-012 |
| M4 | A sketch whose **first** solve failed also reported `DOF = 0`, for the same reason the code's own comment says to avoid. | `kUnknownDegreesOfFreedom`; UI shows "not measured" — ADR-M5-012 |
| M5 | `addSketchConstraint` accepted a dimension bound to a Body id, or to nothing: the edge silently failed to wire and **every subsequent save failed forever**. | Validate that the bound id is a Parameter |
| M6 | A constraint added through `editSketch` wired no edge, so the document behaved **differently before and after a save/load** (the loader re-derives edges). | One reconciler — ADR-M5-013 |
| M7 | A cascaded entity removal through `editSketch` left a **phantom** edge, so a Parameter kept re-solving a sketch that no longer read it. | One reconciler — ADR-M5-013 |
| M8 | `setSketchSolver` was a bare assignment that dirtied nothing, and the graph never re-invokes a `Failed` node — so **correcting a missing solver never recovered**, contradicting ADR-M5-004's explicit promise. Same class for `setGeometryKernel`. | Both setters dirty what depends on them |
| M9 | **"Release tests 448/448" was not measured.** Default POST_BUILD discovery writes a config-less test list hard-coding `build/Debug/...`, so `ctest -C Release` ran the Debug binaries for 333 of 448 tests. | `PRE_TEST` for every suite — ADR-M5-015. Verified from the ctest log: all 464 now invoke `build/Release/...` |

## Major — declined, with reasons

**Deleting a Sketch a Pad reads should be refused** (Reviewer 2). Implemented,
then **reverted**: it broke three accepted M4 tests. M4's own independent review
settled this exact case the other way — deletion allowed, Pad fails loudly, save
refuses a dangling reference. The reviewer's underlying complaint (every save
fails until the Pad is deleted) is real, but that is the accepted design
behaving as designed, and reversing a reviewed milestone decision is the owner's
call, not a side effect of fixing something else. `M5_REV_008` now pins the M4
contract and its recovery path. See ADR-M5-016, which carries the open question
to the owner.

## Minor — fixed

- `Diameter`'s value floor was checked against the diameter but committed as
  the radius, so `Diameter = 1e-6` wrote a radius the sketch's own validator
  rejects. The floor now follows the halving.
- The UI renders an unmeasured DOF as "not measured" rather than "0".

## Minor — accepted, not yet fixed

Recorded honestly rather than closed:

- `CommitSolvedGeometry` writes through `replaceGeometry`, which does not
  re-run `IsValidSketchGeometry`, so `Coincident(l.start, l.end)` can commit a
  zero-length line with status `Solved`. Not NaN/Inf, so ADR-M5-004's letter
  holds; the sketch's own invariant does not.
- `UnitType::Unitless` satisfies both the length and the angle unit check, so
  one unitless Parameter can drive a `Length` and an `Angle` interchangeably.
- `result.iterations` reports `kSolveMaxIterations` on the step-size exit path,
  so a solve that converged in 3 steps can report 100. Diagnostic only.
- The non-convergence message says "did not converge within the iteration
  limit" on the step-rejection path, where the loop actually stalled.
- `findRecomputable`'s type-based upcast (ADR-M5-007) is complete for `Sketch`,
  but `Feature` has no `IRecomputable` base, so a feature registered under the
  `Feature*` alternative would still report "not recomputable". Nothing
  registers that way today; the trap is latent, not closed.
- `OutlineState::Blocked` is never assigned, so "blocked by a failed sketch"
  renders identically to "this Pad itself failed" — pointing the user at the
  wrong object. The engine *does* compute `BlockedByDependency`; the
  information is discarded at the display boundary.
- `--sample <unknown>` silently falls back to the M4 rectangle and still prints
  `SELFTEST OK`, so a typo in CI downgrades an M5 gate to an M4 smoke test.

---

## Corrections to the M5 self-validation report

Reviewer 3 was tasked explicitly with auditing the report for honesty. It found
six overstatements. All are corrected in `M5_SelfValidationReport.md`; they are
listed here because the corrections matter more than the fixes.

1. **"Angle | PASS"** was the worst. The cited evidence was a test that cannot
   fail — it re-evaluated the solver's own residual formula. The section
   claimed every constraint kind is "exercised end to end (solve → geometry →
   profile → Pad → mass)"; for Angle that was false at every stage. This is how
   a Critical reached review with a PASS beside it.
2. **The mechanism I gave for my own Major #1 was fabricated.** I wrote that the
   Angle residual "reduced to `atan2(0, 0)`". There was no `atan2` in the
   implementation — the only occurrence was the comment I had read and
   paraphrased. Describing what the code was meant to do rather than what it
   does is the exact failure a previous review round already flagged on this
   project, and it is what let C1 pass unnoticed under my own eyes.
3. **"Release tests 448/448"** — not measured for 333 of them (M9).
4. **"`M5_UI_012` — no group, no failure, no misleading status"** — that test
   asserts three things, none of them about a group or a status, and **both**
   extra claims were false in the code (M3).
5. **"`GATE_E` compares the sketch byte for byte"** — it compares four
   coordinates of one line.
6. **"Each launches the real window, lays out, and paints"** — nothing asserts a
   paint. Also: `M5_SER_005` checks 3 of 6 status names, not "every status
   name"; and "unknown sub-element rejected at load" covers unknown *names*
   only — a line's `CenterPoint` loads fine and fails later at solve time.

### What the reviewers verified as TRUE

Reported for balance, and because each was re-executed rather than taken on
trust:

- Both mutation-testing claims reproduce exactly (deleting `addDependency` fails
  `M5_RECT_003`, `M5_CIRCLE_001` **and** `GATE_B`; deleting `removeDependency`
  fails `M5_FACADE_001`).
- `M5_DOF_004`/`005` genuinely fail under the forbidden `variables − constraints`
  formula.
- `M5_ADV_008` really enumerates all 16 entity-kind × sub-element pairs;
  `M5_ADV_001` really runs all 23 permutations.
- Eigen is in **exactly one** translation unit — verified with
  `dumpbin /archivemembers` and symbol counts (Solver 12269; Core, ViewerCore,
  KernelOcct all **0**), not asserted.
- The binary boundary holds: `ParametricCADCoreTests.exe` depends only on
  KERNEL32, MSVCP140, VCRUNTIME140(_1) and the UCRT.
- Nothing positional reaches the file; a hand-written integer sub-element
  ordinal is **rejected**.
- Constraint and entity identity survives interleaved add/remove, cascades and
  round trips.
- The transactional commit is unreachable-by-construction on the failure path,
  and retention-vs-currency holds end to end — confirmed in the *running viewer*.

---

## Status after fixes

- **464 / 464** tests pass in Debug **and** in Release, the latter now genuinely
  running Release binaries.
- Every Critical and every accepted Major has a regression test, and each was
  mutation-verified: the fix was removed, the test observed to fail, the fix
  restored and the tree re-verified byte-identical.

  **[CORRECTED]** — when first written this sentence was false. The
  `setGeometryKernel` half of M8 had **no** test, which a re-reviewer proved by
  reverting the fix and watching the suite stay green. `M5_REV2_013` closes it.
  A claim that "everything is guarded" is exactly the kind that has to be
  measured rather than asserted, and I asserted it.
- Seven Minor findings remain open and are listed above rather than closed.
- One Major was declined with reasons, and carries an open question to the owner.

**A re-review has not yet been run against the fixed tree.** Until it has, the
scores above stand as the last independent measurement.

---

# Round 2 — re-review of the fixed tree

The same three-way partition, run against the tree after the round-1 fixes. Each
re-reviewer was asked for a FIXED / PARTIALLY FIXED / NOT FIXED verdict on every
round-1 finding, with execution evidence, plus a fresh look at what the fixes
touched.

| Reviewer | Weight | Round 1 | Round 2 | Verdict |
|---|---|---|---|---|
| 1 — solver / constraints / DOF | 40 | 22 | **31.5** | REQUEST CHANGES (narrow) |
| 2 — architecture / identity / persistence | 33 | 22 | **26** | REQUEST CHANGES (narrow) |
| 3 — recompute / recovery / UI / evidence | 27 | 19 | **21** | APPROVE WITH FOLLOW-UP |
| | **100** | **63** | **78.5** | |

## Round-1 findings: verdicts

Every round-1 Critical and every accepted Major was confirmed **FIXED** by
execution and by mutation in both directions. Highlights of what the
re-reviewers did rather than read:

- **C3** — 12 deletion orderings the tests do not try, with a dangling-handle
  audit after each (16 audits, 0 problems). With the fix removed, the same
  program dies on the first scenario.
- **M6/M7** — a 13-state edge-set equality comparison across every reachable
  mutation path, before and after a save/load round trip. **Every state:
  identical.**
- **M1** — attacked with three coincident points and interacting nudges, both
  residual orders, targets from 1e-6 to 1000 mm, origins to 10 000 mm. Drift
  over 100 re-solves **0.0e+00**; worst order difference **0.000000 mm**.
- **C1** — a 24×24 grid of (target, start), measured with `atan2` on the solved
  coordinates. **563 of 576** exact; the 13 misses were all the half-turn (below).
- **M9** — independently reproduced: `ctest -C Release` now names
  `build/Release/...` in **464 of 464** command lines, zero `build/Debug/`.

## Round-2 findings — all fixed

| # | Severity | Finding | Fix |
|---|---|---|---|
| R2-1 | **Major** | **A round-1 fix introduced an out-of-bounds write.** The ADR-M5-014 degeneracy nudge was placed ABOVE the slot guard and indexed `x[]` raw, so an under-packed `Distance` — exactly what the guard refuses — was dereferenced first: assertion abort in Debug, silent OOB read **and write** in Release. The guard's own test uses `Angle`, which the nudge skips, so it could not see it. | Everything that indexes `x[]` by slot now sits below the validation loop — `M5_REV2_002` |
| R2-2 | **Major** | **ADR-M5-006 claimed behaviour the code did not have.** It said the half-turn was "a measure-zero starting configuration the solve converges through from either side". It stalled at **iteration 1**, reported "did not converge within the iteration limit" after taking no step, and one variant falsely reported `Conflicting`. Reachable from two axis-aligned lines and a 180° angle. | Rotate the starting guess off the antipode (the ADR-M5-014 pattern applied to `Angle`); ADR corrected to state the truth — `M5_REV2_001` |
| R2-3 | **Major** | **M5 was only half fixed.** "Bound to a non-Parameter" was rejected; "bound to **nothing**" was not — the same accepted finding. `target = 0.0` reached the solver, and the document **saved cleanly and could never be loaded back**. | `IsDimensional` capability check in the facade, the problem builder **and** the save validator — ADR-M5-017, `M5_REV2_010/011` |
| R2-4 | **Major** | **ADR-M5-013 said "every mutation path" and there was a fifth.** `addSketch` returns a mutable `Sketch&`, so `Sketch::addConstraint` is reachable with no facade — through it a dimension edit silently did nothing and the document behaved differently before and after a save/load. | `reconcileAllSketchParameterEdges()` at the start of every recompute pass, as a net — `M5_REV2_012` |
| R2-5 | Major | `setGeometryKernel` recovery had **no test at all** — proven by reverting the fix and watching the suite stay green. My write-up had claimed everything was guarded. | `M5_REV2_013` |
| R2-6 | Minor | The `registerObject` check closed C2's *instance*, not its class: `restoreParameter`/`restoreBody`/`restoreMaterial` still discarded the result, and a reviewer reproduced the identical silent symptom one type over. | ADR-M5-018 |
| R2-7 | Minor | `rewireMassPropertiesSource` re-added the MassPropertiesNode's graph node without re-registering it — a node the engine can schedule but not resolve. | ADR-M5-018 |
| R2-8 | Minor | An **empty** sketch reported "Under-constrained, DOF 0" — the exact self-contradiction ADR-M5-012 exists to remove. | `M5_REV2_014` |
| R2-9 | Minor | `SketchSolveResult::degreesOfFreedom` still defaulted to `0`, so every failed result handed out "fully constrained". | `M5_REV2_003` |
| R2-10 | Minor | A comment above `Evaluate` still described the **removed** `sin()` algorithm — the same hazard that produced C1, pointing the other way. | Replaced |
| R2-11 | Minor | **`M5_ANGLE_002` did not test what it is named for.** Against the `sin()` formulation it was written to reject, it **passed** — the fixture started close enough to the correct root. | Fixture now starts near the supplementary root; the sin() mutation fails 5 angle tests, up from 3 |
| R2-12 | Minor | The Diameter floor fix had no test. | `M5_REV2_015` |
| R2-13 | Minor | `--sample <typo>` fell back to the M4 sample and still printed `SELFTEST OK`, so a CI typo silently downgraded an M5 gate. | Unknown sample now fails; exit 1 |

**Open, recorded rather than closed:** the slot guard is arity-only, so a
mis-ORDERED residual passes it (the geometric tests catch it — an injected
`Distance` slot swap fails 49 tests — but a `SolveVariable::Component`
cross-check is the real answer); `editSketch` bypasses the facade's binding
validation, though the solver, panel and save validator all catch the result;
`OutlineState::Blocked` is still never assigned; `CommitSolvedGeometry` does not
re-run `IsValidSketchGeometry`; `UnitType::Unitless` satisfies both unit checks;
`result.iterations` is inaccurate on the step-rejection path.

## What round 2 says about round 1

Two of the round-2 Majors were **caused by round-1 fixes** (R2-1, and R2-2's
false ADR clause), and one was a round-1 fix that stopped halfway (R2-3). A
fourth was an ADR of mine that enumerated "every mutation path" from the paths I
had just edited rather than the ones that exist (R2-4).

That is the useful finding of this round: fixing a defect is where the next one
gets written, and an ADR asserting a property is a claim that has to be measured
like any other. Every round-2 fix now carries a mutation-verified test, and the
six batched mutations each killed exactly their intended test and nothing else.

---

# Round 3 — focused re-review of the round-2 fixes

Two reviewers, scoped to the round-2 deltas only, and told explicitly: *two of
the three round-2 Majors were defects introduced by round-1 fixes, so assume the
round-2 fixes are broken until you have evidence otherwise.*

**Both returned: not safe to ship.** They were right.

## What they found

| # | Severity | Finding |
|---|---|---|
| F1 | **Major** | **The round-2 half-turn nudge was a regression.** Its guard (1e-6 rad) and rotation (1e-4 rad) are absolute; the residual's usable angular resolution is `1e-7·|coordinate|/length`, which is not. Over a 760-case grid it improved 131 configurations and **broke 42** — turning correct `Solved` results into false "did not converge" failures for short lines far from the sketch origin. Proven with one binary and a run-time toggle. Zero regressions for lines ≥ 1 mm near the origin, which is exactly why 464 tests stayed green. |
| F2 | **Major** | **The ADR clause that replaced the false one was also false.** It said "only the exact point was unreachable". Measured: a failure *band*, before the fix and after it — still 1e-5 rad wide for an ordinary 10 mm line 1000 mm from the origin, against a 1e-6 guard. |
| F3 | **Major** | `while (d > pi) d -= 2*pi` is unbounded and any finite angle is accepted: `Angle = 1e300` rad made `recompute()` **never return**; `1e9` rad took 21.8 s. Pre-existing, but round 2 **copied the loop into a second site**. |
| F4 | Minor | `M5_ANGLE_E2E_001` — added in round 1 specifically so C1 could not ship again — **could not fail against C1's formulation**. R2-11 fixed that weakness in `M5_ANGLE_002` and left it next door. |
| F5 | Minor | A non-converged result still handed out a measured DOF, frequently 0 = "fully constrained". |
| D1 | **Major** | **Five round-2 fixes had no test at all.** All five reverted in one build: **473/473 green.** And the round-2 write-up's closing sentence — "Every round-2 fix now carries a mutation-verified test" — was false in exactly the way R2-5 was raised about. |
| D2 | **Major** | `restoreMaterial` assigned `material_` before the registration check, so the throw **destroyed the live Material while the registry kept its address**, and the next recompute read density out of freed memory. |
| D3 | **Major** | ADR-M5-018's "every restored type" covered four of six: `restoreBoxFeature`/`restorePadFeature` discarded the result, and a duplicate feature id produced a document that **saves and can never load** — C2's symptom on the types the fix skipped. |
| D4 | Minor | The per-pass reconciler silently revokes a `Parameter → Sketch` edge added through the public `addDependency`. Correct, but undocumented. |
| D5 | Minor | **The reconciler created phantom edges it could never remove** — it added for any bound id but removed only Parameter prerequisites. Verbatim M7's defect class, re-created by M7's own fix. |
| D6 | Minor | `restoreParameter` left the duplicate in `ParameterManager` before throwing → saves cleanly, never loads. |
| D7 | Minor | `--sample` with a **missing value** still printed `SELFTEST OK` — the CI downgrade R2-13 closed for a wrong *name*, one notch over. |
| D8 | Minor | After removing the mass-properties node, `massProperties().valid` stayed `true` forever: a stale volume reported as current through every later edit. |

## What they verified as genuinely clean

- Slot-guard ordering: no path indexes `x[]` above the guard; moving the nudges
  back reproduces the Eigen abort immediately.
- 72 already-satisfied angles: **0 disturbed**. Both lines Fix-pinned at the
  antipode: still `Conflicting`, nothing committed. 100 feedback re-solves:
  **0.0e+00 drift**.
- `IsDimensional` across a **90-case matrix** (9 kinds × 4 creation routes × 3
  bindings) plus 12 hand-written-JSON attacks: **0 save-but-not-load, 0
  load-but-not-save**.
- Spec 13 selectivity intact with the per-pass reconciler, on a fresh document
  and on one loaded from disk. Reconciler cost at 2000 sketches: 6.74 ms, of
  which 5.91 ms is the pre-existing topological sort.
- R2-7's registry/graph invariant across **18 scenarios**: 0 violations.
- `DOF = -1` is safe at every reader; the empty-sketch branch is right, and
  "entities but zero variables" is unreachable.

## Round-3 fixes

All fixed. The Angle fix is a redesign rather than a retune:

**The nudge is deleted.** Wrapping makes the residual's *value* jump at the
antipode; its *gradient* does not. `ComputeJacobian` now wraps the **difference**
of the two evaluations for angular residuals, which recovers the true derivative
at any scale and needs no tuning constant. Wrapping is `std::remainder`, not a
`while` loop.

New regression tests, each mutation-verified: `M5_REV3_001` (140-case sweep over
line length × distance from origin × offset from the antipode — the space that
exposed the nudge), `M5_REV3_002` (huge-angle termination), `M5_REV3_010..014`
(the five previously-unguarded round-2 fixes, plus D2/D3/D5/D8), and two ctest
cases pinning **both** shapes of bad `--sample`.

Two of those tests **could not fail when first written** — `M5_REV3_011` and
`M5_REV3_013` — and a mutation caught both. `013` used a fixture whose mass had
never been valid, so "expect invalid" held for the wrong reason. They were
rewritten and re-verified.

**482 / 482 in Debug and Release**, all Release command lines naming Release
binaries.

## What three rounds have shown

Round 2 found that round-1 fixes introduced defects. Round 3 found that round-2
fixes introduced defects — including a *scale-blind* fix for a scale-dependent
problem, and a reconciler that re-created the phantom-edge class it existed to
close. Each round the write-up claimed everything was guarded, and each round
that claim was false.

The pattern is not carelessness in any single fix; it is that **a fix is written
with the failing case in view and the rest of the space out of view**. The nudge
was tested at nominal scale; the reconciler was reasoned about for Parameters;
the registration check was applied to the types in front of me. What caught all
three was a reviewer sweeping the space rather than the case — 760 grid points,
90 matrix cells, 18 scenarios.

That is the argument for the sweeps now in the suite, and against ever again
writing "every path" or "every type" in an ADR without enumerating them.

---

# Round 4 — final review of the round-3 fixes

Two reviewers, scoped to the round-3 deltas, told plainly that rounds 2 and 3
had each found defects created by the previous round's fixes, and to assume the
same of these.

**Both returned: not safe to ship.** They were right again, and for the same
reason: **two of the five Majors were written by round-3 fixes, and a third was
a round-3 fix that stopped one line short.**

## Findings

| # | Severity | Finding | Origin |
|---|---|---|---|
| S1 | **Major** | **`CommitSolvedGeometry` validated EVERY entity, not just what it wrote.** `restoreEntity` deliberately does not validate — a hand-edited file must round-trip — so one degenerate entity anywhere in a loaded document made the whole sketch **permanently unsolvable**, with a diagnostic naming no constraint and a stale DOF of 0 reading as "fully constrained". | **round-3 fix** |
| S2 | **Major** | **`kMinSketchDimensionMm` and `kSketchToleranceMm` were the same number with inclusive bounds from opposite sides.** The smallest *accepted* dimension was the largest *rejected* geometry: a Length of exactly 1e-6 was legal as a dimension and degenerate as geometry. 41 of 144 swept configurations had a converged solve refused. | pre-existing, exposed by the round-3 commit validation |
| S3 | **Major** | **The wrapped-difference Jacobian is not scale-free.** The step is relative to the *coordinate* while angular sensitivity is `1/L`, so the wrap caps the derivative once `\|coordinate\|/length > ~1.6e7`: 132 of 336 Jacobians wrong, 75 false `NumericalFailure`s. My ADR called it "scale-free"; the tuning constant had moved from the deleted nudge into the finite-difference step. | **round-3 fix** (round-3's own F1 class, relocated) |
| S4 | **Major** | **A large Angle target produced a false "constraints are contradictory".** `WrapToPi(a − b − target)` subtracted the raw target, so at 1e9 rad its ULP swamped the angular signal. Round 3 converted a hang into a wrong answer instead of a right one — violating ADR-M5-014's own rule that a false accusation of contradiction is the damaging kind. | **round-3 fix** |
| D1 | **Major** | **`restoreMaterial`'s success path was still a read-after-free.** Round 3 moved the check above the mutation, which fixed the throw path and left the line below it untouched: replacing `material_` destroys the previous Material while the registry still resolves its id. In **Release** the freed memory still read the old density, so the document reported a plausible but **wrong mass as current**, with `Success` and no diagnostic. | **round-3 fix, one line short** |
| D2 | **Major** | **`--sample=value` was silently dropped** — it failed `strcmp`, so the M4 rectangle was built and `SELFTEST OK` printed. The third appearance of this CI downgrade, after an unknown name and a missing value, and the most commonly typed form of the three. | pre-existing |
| S5-S7, D3 | Minor | Four fixes had **no test at all**; each mutation left the suite green. | recurrence of D1/R2-5 |
| D4, D5 | Minor | Outline and engine disagree when failure passes through a Suppressed node; a Blocked *sketch* row loses its solve message and is overwritten back to Failed by the profile branch. | round-3 `Blocked` derivation |
| S8 | Minor | `iterations` under-reported by one on the early-stop exits. | round-3 fix |

## Fixes

All fixed and mutation-verified except one documented gap. The three that
mattered most were root-cause changes, not adjustments:

- **S1** — a commit is answerable for **what it writes**, not for what it found.
  Only entities the solve actually changed are validated.
- **S2** — `kMinSketchDimensionMm` is now `1e-5`, ten times the coincidence
  tolerance, and the gap is enforced by a **`static_assert`**. Reverting the
  constant now fails the **build**, which is a stronger guard than any test.
- **D1** — `detachCurrentMaterial()` unhooks the outgoing Material from the
  registry and the graph before the assignment destroys it, on **both** call
  sites.

**S3 is not closed by code.** The reviewer's own reachability note stands: it
needs a coordinate/length ratio above ~1.6e7 — a 1 µm line 10 m from the origin
— which is outside spec 30's "reasonable geometry". What was wrong was the
**claim**, and ADR-M5-006 is corrected for the third time to state the measured
envelope instead of asserting scale-freedom.

## Three defects found in my own tests

Recorded because each changed what the evidence could prove:

1. The dimension-floor test **depended on where the solve happened to land** —
   41 of 144 configurations fail, and its one configuration was among the other
   103. Replaced by the compile-time assertion.
2. The failure-message test **skipped its own assertion** when the fixture ran
   to the iteration limit. A test that opts out of its subject is not a test.
3. The `--sample=value` fix had **no ctest case**. Three were added, including
   one asserting the `=` form with a *valid* name still works — so the parser
   cannot "pass" by rejecting everything containing an `=`.

## One gap left open, not papered over

The failure message's choice between "iteration limit" and "could not improve on
its last step" is only reached on **full-rank non-convergence**. I could not
build a deterministic fixture for it: a full-rank residual system that reliably
stalls is precisely what a working solver does not produce on demand. A mutation
hard-coding the iteration-limit message therefore survives. `M5_REV4_003` was
renamed to what it actually verifies — that a rank-deficient failure is reported
as a contradiction and not as an iteration limit — rather than left carrying a
name it did not earn.

## What four rounds have shown

Every round found defects created by the previous round's fixes: round 2 in
round-1's, round 3 in round-2's, round 4 in round-3's. Four rounds, and the
write-up claimed "everything is guarded" in three of them, wrongly each time.

The mechanism is consistent and worth stating plainly, because it is not
carelessness in any individual change:

- **A fix is written with the failing case in view and the rest of the space out
  of view.** The nudge was tested at nominal scale. The registration check was
  applied to the types in front of me. The material reorder fixed the path the
  finding named and not the one beside it.
- **What caught all of them was a reviewer sweeping the space rather than the
  case** — 760 grid points, 336 Jacobians, 144 configurations, 90 matrix cells,
  18 orderings, 13 states, 12 deletion orderings.
- **Claims about coverage must be measured like any other claim.** "Every fix is
  mutation-verified" was false three rounds running, each time detected by
  someone actually removing the fix and watching the suite stay green.

The durable outputs are the sweeps now in the suite, the `static_assert` where a
constant relationship can be checked at compile time, and the rule that an ADR
must state a measured envelope rather than assert a property.
