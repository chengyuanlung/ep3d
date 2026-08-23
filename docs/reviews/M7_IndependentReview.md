# M7 Independent Review — Round 1

Three reviewers, partitioned per spec 33, each in a private `git archive` export
with its own build directory (AGENTS.md rule 2). Reviewed commit **`264eba3`**.

| Reviewer | Partition | Decision | Score |
|---|---|---|---|
| 1 | reconstruction semantics | `REQUEST CHANGES` | 71 / 100 |
| 2 | identity / persistence / transactions | `REQUEST CHANGES` | 71 / 100 |
| 3 | recompute / UI evidence / test quality | `REQUEST CHANGES` | 71 / 100 |

**M7 is `REQUEST CHANGES`.** Three reviewers converged on the same score
independently, and two found the same Critical from opposite directions.

---

## The pattern held, again

AGENTS.md predicts it in as many words: *"only a reviewer removing a line the
author did not think to remove has ever found an unguarded fix here."*

| | guards removed | caught by no test |
|---|---|---|
| Reviewer 1 | 13 | **5** |
| Reviewer 2 | 5 | **3** |
| Reviewer 3 | 12 (9 audit + 3 UI) | **3** |

**Of 30 guards deliberately removed, 11 were unguarded.** My own nine mutations
touched none of those eleven. Reviewer 3 re-ran all nine and **confirmed 9/9** —
the mutation table was honest, it was just aimed where I was already looking.

---

## Critical findings

### C1 — A rotated linear dimension applies its *projection* as a `Length`, silently producing wrong geometry
*Reviewer 1. Independently reproduced by me.*

`MeasuredValueMm` correctly returns the projection onto `directionRad` for a
`Linear` dimension. `AnalyzeForReconstruction` then hands that number to a
`LengthConstraint`, which controls the line's **Euclidean length**. Nothing
cross-checks the two.

My reproduction — a 13 mm line `(0,0)–(5,12)` with an ordinary horizontal
DIMLINEAR across its endpoints:

```
PROBE skipped=0 parameters=1
PROBE param Height = 5.000000
PROBE Length targets line=5   (the 13 mm line is 5)
```

Reconstruction reports **success, zero skips, no diagnostic**, and the solver
then shortens a 13 mm line to 5 mm. Spec 34's first Critical: *"reconstructed
dimension silently targets wrong geometry."* Secondary defect visible in the same
output: a **horizontal** dimension was named `Height`.

`MeasuredValueMm`'s own comment says collapsing the two kinds *"would silently
turn every such dimension into a wrong one"*. The kinds are kept apart at the
value layer and collapsed one level up.

No gate sees it because the release fixture's skew is 5e-4 rad, making projection
and length differ by 1.2e-5 mm — under every tolerance in the suite.

### C2 — `savePartDocument` can write documents its own loader refuses, and M7's CMake ordering made it live
*Reviewers 2 and 3, independently. Reproduced and half-fixed by me.*

`CMakeLists.txt` carried an instruction in capitals: `tests/SerializationTests.cpp`
contains a test that advances the shared `ObjectIdGenerator` to ~2^63 and **must
stay last**; *"Add new test files ABOVE this comment, never below."* **All five
M7 test files were added below it.** Three M7 persistence tests then ran against
a poisoned generator:

```
[  FAILED  ] M7Provenance.ProvenanceIsSessionScopedAndSaysSoOutLoud
[  FAILED  ] M7Adversarial.ASketchLoadedFromDiskCanStillBeReconstructed
[  FAILED  ] M7Adversarial.AnAlreadyReconstructedSketchIsStillRefusedAfterSaveAndLoad
document: field 'id' value 9223372036854776535 exceeds the maximum ObjectId
```

**A direct run of the binary was 665/668 in both Debug and Release.** `ctest`
reported 668/668 because `gtest_discover_tests` gives every test its own process,
so no ctest-based check could ever have seen it.

The underlying product defect is worse than the ordering: `AdvancePast` clamps to
`kMaxObjectId` and then adds 1, so `Next()` can issue ids the serializer rejects
on load. `ASSERT_TRUE(savePartDocument(...))` **succeeds** and the very next
`loadPartDocument` refuses the file just written — a save that silently produces
an unopenable document.

**Ordering fixed** (`tests/SerializationTests.cpp` moved back to last; single
process is now 380/380). **The save/load defect is NOT fixed** and is inherited,
not M7's.

### C3 — A `ReconstructionPlan` is not bound to the document it was built from
*Reviewer 2, demonstrated by execution.*

`ObjectId` is a process-local counter starting at 1, so two documents carry
overlapping ids by construction. Applying document A's plan to document B was
accepted — `ok=1`, no diagnostic — leaving B's 250 mm edge driven by A's
`Width=100`. Latent (the only production caller uses the one-shot path), but
`ApplyReconstruction` is public API documented as safe for exactly this, and
`ReconstructionPlan.h` claims the case is covered. It is not.

### C4 — `PlanParameterSlot` containment rests on one untested line; removing it is memory-unsafe
*Reviewer 2, demonstrated by execution.*

`slotToParameter[static_cast<std::size_t>(planned.parameter)]` has no bounds
check. The only protection is one branch in `ValidatePlan`. Deleting that branch:
**no test failed**, and a plan carrying slot 5 with one parameter produced
`SEH exception 0xc0000005`.

---

## Major findings

| # | Finding | Source |
|---|---|---|
| **M1** | **Re-running reconstruction on an undimensioned sketch duplicates every geometric constraint** and reports success — 9 → 18 → 27. The idempotence guard tests only for *dimensional* constraints, and a DXF with no DIMENSION entities creates none. Damage is masked until dimensions bring the sketch to DOF 0, where it reports `OverConstrained` and points at the dimension the user just added. | R1 + R2, independently |
| **M2** | **The M7 UI evidence does not discriminate.** Swapping the "From source" and "Inferred" counts, deleting every skip-diagnostic row, and hard-coding `propertyPanelFitsItsPanel()` to `true` each left **13/13 viewer smoke tests green**. The selftest asserts only that the strings are non-empty. | R3 |
| **M3** | **`propertyPanelFitsItsPanel()` cannot fail for the defect class it exists to guard.** Column 1 is `Stretch`, so `header()->length()` is *always* exactly `viewport()->width()`. My own "296 == 296, fits with nothing to spare" was a misreading — it is a tautology. M6.14 is alive under a different mechanism: a 133-character skip diagnostic is delivered as **nine characters**, three skip rows become indistinguishable, and the guard passes. | R3 |
| **M4** | **The only running-application M7 fixture has no dimensions.** `ViewerSmokeTest_ImportsDxf` uses `mixed.dxf` (0 DIMENSION entities), so in CI "From source" is always "0" and the skip-row loop never executes. `dimensioned_rectangle.dxf` is imported by no registered test. Spec 27 steps 5–10 have no automated coverage at any layer. | R3 |
| **M5** | **Gate K's headline claim is not proved by its evidence.** Inserting a real second `ReconstructSketch` into the test still passes, because the idempotence guard makes it a no-op — so the test cannot distinguish "not a graph node" from "re-ran and was refused". Its unrelated-Parameter half uses equal final values, which spec 21 explicitly rejects as evidence. | R3 |
| **M6** | **Gate H never checks downstream stale state.** It asserts status, offending ids and geometry-by-measurement (all real), but never that mass properties are invalid while the sketch is `Conflicting` — spec 23 Gate H's own requirement and spec 34's "stale geometry displayed as current". | R3 |
| **M7** | **Radius/Diameter naming depends on file order.** The linear path sorts candidates geometrically before naming; the curve path names inline in the file-order loop with no sort. Two circles, list order swapped → `Radius`/`Radius_2` change places. ADR-M7-002 forbids exactly this. | R1 |
| **M8** | **The geometric sort ADR-M7-002 calls "the whole point" has no test.** Deleting it fails nothing, yet it is load-bearing from the second dimension of a given orientation onward. | R1 |
| **M9** | **The circle-centre Fix min-search has no test**, contrary to ADR-M7-016's explicit claim that it does. Deleting it lets entity vector position decide a persistent placement. | R1 |
| **M10** | **`AClosedCurveIsNotMadeCoincidentWithItself` runs on an empty sketch.** Its arc (`2π − 1e-9`) is refused by the Sketch model, so the test asserts `Coincident == 0` on a sketch containing nothing — it would pass with the entire engine deleted. | R1 |
| **M11** | **Gate J passes when its child does nothing.** The anti-vacuity guard (`EXPECT_TRUE(exists(path))`) is tautological: the parent wrote that file itself two statements earlier. I verified by mutation — child forced to `GTEST_SKIP`, gate still **OK**. My mutation 8 tested the failing-child path only. | R2, verified by me |
| **M12** | **Rollback does not clear `report.entries`.** After a failed apply the report still says "reconstructed: 10 constraint(s)", naming ids rollback destroyed. Diagnostics only — the document is correctly restored — but the failure path's whole justification is that it tells the truth when things go wrong. | R2 |
| **M13** | **Spec 24's vertical tolerance-boundary pair does not exist.** Only the horizontal pair is present; the self-validation report claims both. | R1 |
| **M14** | **Spec 12 `Distance` is neither implemented nor declared missing.** No code path constructs a `DistanceConstraint`; the reconstruction matrix has no Distance row and Known Limitations does not mention it. | R1 |

---

## Two false claims in my self-validation report

Recorded because spec 33 tells reviewers to treat that report as claims, and
these two failed:

1. **"668/668 in Debug and Release"** — true under `ctest` only. A direct run of
   `ParametricCADCoreTests.exe` was **665/668** in both configurations (C2).
2. **Mutation 8 credited Gate J's anti-vacuity guard.** That guard is vacuous;
   mutation 8 exercised a different path (M11).

Two further accounting errors, from Reviewer 3: the suite table gives
Integration 106 where ctest registers 107 (the table sums to 667, not 668), and
"668" counts one test that ctest reports as `Skipped`, so **667 actually
execute**.

---

## Found outside the review, by running the application

Automated GUI verification (screenshot → click → type), recorded here because it
is the same round of evidence. **This is not owner manual validation** and does
not substitute for it (ADR-M4-016).

- **The viewer never extrudes the imported sketch.** `main.cpp:101` pads the
  demo sketch; the import happens at line 297. Editing a reconstructed `Width`
  re-solves the imported sketch correctly (Solved, DOF 0) while the 3D view and
  the volume readout continue to describe an unrelated demo box. Spec 27 steps 9
  and 10 do not happen in the shipped application. This is the same gap
  Reviewer 3 reached from the test side as M4.
- **The import status message states the skip count twice** — `SketchImporter`
  already appends `"; skipped N"` and `MainWindow` appends `[N skipped]`.
  Pre-existing M6 behaviour, made conspicuous by M7's third bracketed clause.
- **`M7_UI_UserValidation.md` Test B4/B5 are unrunnable as written**, for the
  first reason above. I shipped a checklist asking the owner to verify something
  the program does not do.

---

## Fixed so far

| Item | Status |
|---|---|
| C2, build half — `SerializationTests.cpp` restored to last | **FIXED**, single process now 380/380 |
| Leftover `PANELFIT` `fprintf(stderr, …)` shipped in production code | **FIXED** |

The `fprintf` deserves its own note. I added it as instrumentation, ran a script
that printed "instrumentation removed" *unconditionally*, and read the `grep -c`
output of `1` on the next line as build output. It was committed. This is the
"a check that cannot fail loudly proves nothing" rule, broken by me in the same
session I quoted it — and no reviewer needed to find it, because my own
verification printed the evidence and I misread it.

---

## What the reviewers found sound

Reported so the negatives are legible:

- **Core independence holds.** No Qt, OCCT or DXF type reaches
  `src/Core/Reconstruction`; it builds and tests with none of them present.
- **The mutation table is honest** — 9/9 confirmed by Reviewer 3, including the
  author's own disclosure that mutation 6 does not discriminate on the
  under-constrained junction.
- **Debug vs Release evidence is TRUE** — independently reproduced: 668 Release
  command lines naming `build\Release\`, 0 naming Debug, and the Gate J child
  path is per-config.
- **The reconstruction core is load-bearing**, not decoration: removing
  `AnalyzeForReconstruction` entirely fails 17 of 20 gates.
- **Association is geometric, never by index**; tolerance is a recognition rule
  and the solver — not the reader — moves coordinates (ADR-M7-011 holds).
- **Rollback ordering is correct and tested**; `PlanParameterSlot` never escapes
  the plan, is never serialized, and never becomes native identity.
- ADR-M7-014's "centre AND size" rule has both halves independently pinned.

---

## Fix status — all round-1 findings closed

| Finding | Fix | Mutation-verified |
|---|---|---|
| **C1** projection applied as a Length | refused, with the axis tolerance | `M7_REV_C1` (+ Aligned and along-the-line counterparts) |
| **C2** save writes unopenable files | refused at save; five `WholeSuite_*` ctest entries | `GeneratorLimitTest`, and the ordering mutation now fails WholeSuite while per-test passes 10/10 |
| **C3** plan applied to another document | `ValidatePlanAgainstDocument` checks geometry, not identity | `M7_REV_C3` (+ the still-describes counterpart) |
| **C4** unbounded slot index | `ValidatePlan` branch pinned | `M7_REV_C4` |
| **M1** undimensioned sketch reconstructed twice | idempotence widened to the Fix | `M7_REV_M1` |
| **M2/M3/M4** UI evidence did not discriminate | exact values, value-column measure, dimensioned fixture, negative control | all three review mutations now fail 2, 8 and 8 smoke tests |
| **M5** Gate K proved nothing | counter-based gate | solver/kernel counters |
| **M6** Gate H ignored stale state | mass properties asserted invalid then current | — |
| **M7** curve naming by file order | geometric sort, parallel to the linear path | `M7_REV_M7` |
| **M8** linear sort untested | — | `M7_REV_M8` |
| **M9** circle Fix min-search untested | — | `M7_REV_M9` |
| **M10** test ran on an empty sketch | sweep the model accepts, plus an entity-count assertion | `M7_REV_M10` |
| **M11** Gate J tautology | child deletes the document; parent asserts it is gone | forced skip now fails |
| **M12** rollback left a lying report | `report.entries` cleared | `M7_REV_M12` |
| **M13** vertical boundary pair missing | added | `M7_REV_M13` ×2 |
| **M14** Distance silently absent | declared in the matrix and Known Limitations | — |
| minors | `PANELFIT` fprintf removed, duplicate skip count removed, ADR-M7-013 and ADR-M7-016 corrected | — |

Plus the finding from running the application: **the viewer now extrudes the
imported sketch**, so spec 27 steps 9–10 happen for the first time. Verified in
the running app — Width 100 → 120 gives 120000 mm³ and COM x 50 → 60.

**690/690 in Debug and Release, 395/395 single-process.**

---

## Required before round 2

Ordered by severity, not by effort.

1. **C1** — refuse a `Linear` dimension whose direction is not parallel to the
   line it names; add the passing counterpart so the pair straddles.
2. **C2** — make id exhaustion explicit: refuse at allocation or at save, so a
   save can never emit a file the loader rejects. Add a ctest entry that runs
   each gtest binary **once, unfiltered**, so single-process state leakage can
   never again be invisible.
3. **C3** — add `documentId` to `ReconstructionPlan` and check it on apply.
4. **C4** — pin `ValidatePlan`'s slot-containment branch with a test.
5. **M1** — widen the idempotence predicate to cover the dimensionless case.
6. **M2/M3/M4** — assert exact panel *values*, not non-emptiness; measure the
   **value** column rather than the tautological header sum; register a viewer
   smoke test that imports `dimensioned_rectangle.dxf`.
7. **M5/M6** — counter-based Gate K; assert invalid mass properties in Gate H.
8. **M7–M10, M13** — the untested determinism rules and the empty-sketch fixture.
9. **Documentation** — ADR-M7-013 and ADR-M7-016 each contain a factually wrong
   sentence; correct both, and correct the two false claims in the
   self-validation report.

Reviewers 1 and 2 both reported that their harness working directory did not
match their assigned export. Both confirmed they worked exclusively in their
assigned directory via absolute paths and never touched another, so the results
stand — but the mapping should be fixed before round 2.

---

# Round 2

**Reviewed commit:** `9e0c399` (M7's Core is byte-identical to `m7-wip`; the
viewer carries M8's additions, so round 2 reviewed the state that would ship).
Exports at `D:/Program2/EP3D/m7review2/{r1,r2,r3}`.

| Reviewer | Partition | Decision | Score |
|---|---|---|---|
| R1 | reconstruction semantics / geometry | **REQUEST CHANGES** | 72/100 |
| R2 | identity / persistence / transactions | **REQUEST CHANGES** | 73/100 |
| R3 | recompute / UI evidence / test quality | **REQUEST CHANGES** | 73/100 |

**Round verdict: REQUEST CHANGES.** Round 1's own two Critical fixes did not
close their findings, and one of them silently voided another round-1 fix.
The headline is not a new feature defect — it is that **a fix can close the
test written for it while leaving the defect open**, twice over.

## Findings register

### Critical

| ID | Finding | Status |
|---|---|---|
| R2-C1 | **C3 is not closed.** `documentId` is a process-local counter starting at 1, so two parts saved in two SESSIONS are identity-indistinguishable; the only remaining separator was the 5% agreement band, which exists to ask "does this dimension describe this line". Demonstrated: a 100x50 plate's plan applied to a 103x80 bracket, `ok=1`, no diagnostic, the bracket's edge driven by the plate's `Width=100`. Round 1's `M7_REV_C3` diverges by 150% and passes on the band alone — deleting the identity branch failed nothing. | FIXED — `ReconstructionPlan::fingerprint`, an FNV-1a hash of every entity id and coordinate, stamped at analysis and checked first; pinned by `M7_REV2_C1.APlanFromADifferentDocumentWithTheSameIdsIsRefused`, whose id collision is forged the way two sessions produce it (one file, loaded twice, one copy edited 3% — inside the band) |
| R1-C1 | **A length-preserving edit is accepted and the solver then rewrites the user's geometry.** Validation re-checked only DIMENSIONAL magnitudes; every inferred rule was checked for entity existence only. Rotate a dimensioned edge 90 degrees about its start point — same id, same 100 mm — and the plan validates, `recompute.success=1`, and the user's deliberately vertical line comes back horizontal, silently. | FIXED — every inferred constraint is re-asserted against current geometry with the predicate that produced it, at the caller's own tolerances; pinned by `ALengthPreservingEditIsRefused` (fingerprint path) and `AHandBuiltPlanWithNoFingerprintIsStillCheckedAgainstGeometry` (the re-assertion itself — mutation Y2c shows it is the only test that dies when the re-check is neutered) |

### Major

| ID | Finding | Status |
|---|---|---|
| **All three reviewers, independently** | **The transactional rollback is unreachable dead code and its four tests are vacuous.** Round 1's C3 fix moved the entity-existence check into validation, which now refuses these plans before any object is created. Deleting the entire unwind — including round 1's own M12 fix `report.entries.clear()` — leaves all 633 tests green. The fix table's "M12 mutation-verified" was **false**. | FIXED BY TELLING THE TRUTH — the four tests are renamed to what they test (pre-apply refusal), the unwind is documented as defense in depth with no reachable failure through the public API and explicitly NOT mutation-guarded (ADR-M8-004's honesty pattern), and the self-validation claim table now reads NOT COVERED where it claimed coverage |
| R3-M1 | **`--expect-from-source` / `--expect-skipped` were never parsed.** Declared, never assigned, guarded by `>= 0` — so `--expect-from-source 999` returned SELFTEST OK and both ctest registrations handed numbers to a loop that discarded them. Fourth appearance of a class this file warns about three times in capitals. | FIXED — both parsed; plus the CLASS: any unrecognised `--flag` now fails the selftest. Three WILL_FAIL ctest entries (wrong from-source, wrong skipped, unknown flag) are the negative controls that make the positive ones mean something |
| R3-M5 | **A failed reconstruction is invisible in the running application** — no report stored, no panel rows, a status bar word-for-word identical to a drawing carrying no dimensions, while the sketch is still extruded and the volume still updates. | FIXED — the failure message is appended to the status bar |
| R1-M6 | A Parameter name taken between analyze and apply produced **two Parameters of one name** driving different geometry. | FIXED — re-checked in validation; pinned by `M7_REV2_M6` |
| R1-M4 | Analyze used `options.valueAgreementFraction`; validation hard-coded the global — so a widened band made reconstruction reject the plan it had just produced, blaming an edit that never happened. | FIXED — validation takes the caller's options; pinned by `M7_REV2_M4` |
| R1-M5 | The C1 refusal tolerance is correct but **pinned by nothing**: replacing it with 0.6 rad (600x wider, accepting a 30-degree line as a Length) failed no test, because all three C1 tests sit at 67 degrees. | FIXED — `M7_REV2_M5` pins the constant from both sides (0.9x accepted, 1.1x refused) |
| R1-M2 | Parameter naming still depends on file order when two dimensions resolve to one target (round 1's M7/M8 closed only for distinct targets). | FIXED — both sort keys are now TOTAL, with tiebreaks taken from the dimension itself (value, then kind, then source handle as a last resort only); pinned for lines and curves by `M7_REV2_M2` in both file orders |
| R1-M3 | With `placeFix=false`, repeated reconstruction accumulates again (8→16→24); round 1's M1 keyed idempotence on `FixConstraint`, and a public option removes the proxy. | FIXED — idempotence now asks whether the sketch carries ANY constraint, because every proxy for "M7 has run here" can be switched off by a supported option; pinned by `M7_REV2_M3` across three option combinations. Cost stated at the code: a hand-constrained sketch can no longer be reconstructed from a drawing, which is M7.1's refuse-not-merge contract rather than a regression |
| R2-M1 / R2-M2 | The save-side cap check's breadth is unpinned (only the document-id branch fires in its test), and `validateSaveable`'s id-uniqueness net stops short of sketch entities and constraints. | FIXED — `GeneratorLimitChild.EveryIdClassIsRefusedAtSaveOnceItPassesTheCap` reaches all seven branches (each in a document created before the poisoning, and each child test in its OWN process so the poisoning does not cascade); entity and constraint ids get per-sketch uniqueness checks, scoped exactly as the loader scopes them. **Two corrections from round 4 of the M8 review** (R2R4-M3/R3R4-M1 and R2R4-m2/R3R4-m1): "all seven branches" was false — there are EIGHT `capCheck` sites and the test reached five, with document, body and material reached by nothing (since fixed, all eight now driven); and the per-sketch entity/constraint checks are **unreachable through the public API** — `restoreEntity`/`restoreConstraint` refuse a duplicate id first — so they are defense in depth, **masked by design**, and deleting both fails nothing. Recorded that way rather than counted as coverage |
| R2-M3 | `reconstructionReports_` has no erase path; bounded today only because the shell has no File-Open and no delete-sketch command. | FIXED — `forgetProvenanceFor`/`forgetAllProvenance` are the explicit erase paths, and `pruneProvenance` runs after every recompute as the backstop, so no future removal command can leave the map wrong by forgetting to say so |
| R3-M2 | Gate J's failure is reported by ctest as **Skipped, not Failed** — the child's `[ SKIPPED ]` line reaches the parent's stdout and `gtest_discover_tests` stamps a SKIP regex on it. The assertion is sound; the registration hides it. | FIXED — the child's output is redirected; the parent asserts on the exit status and the deleted document, which is all the information it ever needed |
| R3-M4 | No registered viewer test renders a skip-diagnostic row (both fixtures produce zero skips); deleting the loop leaves 15/15 green. | FIXED — new fixture `dimensioned_rectangle_one_skipped.dxf` (width states 250 against 99.5 mm drawn, a 150% disagreement) registered as `ViewerSmokeTest_ImportsWithASkippedDimension`; mutation Z1 neutering the skip-row loop now fails it |
| R3-M6 | `WholeSuite_*` pins one link order; `--gtest_shuffle` still fails 71/11/16 tests at seeds 1/7/42 because `GeneratorLimitTest` permanently poisons the process counter. | FIXED — the poisoning moved into child processes, so nothing mutates the shared counter in an ordinary run: shuffled seeds 1/7/42 went from 71/11/16 failures to **0**, and `Shuffled_<suite>_seed{1,7,42}` ctest entries make order-independence a checked property rather than a convention |

## Fix-verification battery (Y)

Binaries deleted before each rebuild and asserted present; restores plain-copy
plus `touch`, `cmp`-verified.

| # | Mutation | Verdict |
|---|---|---|
| Y1 | fingerprint check deleted | **guarded** — `M7_REV2_C1.APlanFromADifferentDocument…` |
| Y2c | all three inferred re-assertions neutered | **guarded** — `AHandBuiltPlanWithNoFingerprint…` |
| Y3 | name-taken check deleted | **guarded** — `M7_REV2_M6` |
| Y4 | agreement band reverted to the global constant | **guarded** — `M7_REV2_M4` |

*(Y2 as first written was a no-op — `if (false) {} else if (…)` left the branch
live, producing a false UNGUARDED verdict. Recorded because a mutation that
does not mutate is a harness defect, and this project has been burned by that
class before.)*

## What round 2 confirmed sound (honest negatives)

Round 1's C2 fix (registration-order poisoning) is **closed and
discriminating** — reintroducing the violation with an M8-added file turns
`WholeSuite_ParametricCADCoreTests` red while per-test ctest stays 6/6 green.
C4 is closed. ADR-M7-013's and ADR-M7-016's post-round-1 corrections are both
**true**, re-derived algebraically and confirmed by mutation. Core independence
holds at the binary level — a reviewer linked a Core-only executable that drove
import-shaped reconstruction, save and load with no Qt, OCCT or libdxfrw
present. Every hand-edited JSON attack was refused with a correctly classified
error. 11 of 14 removed guards were killed by existing tests. Gate K is
genuinely counter-based; Gate H asserts invalid mass properties; the panel's
exact-value assertions and the `panelFitGuardCanFail` negative control both
discriminate.

## Standing blocks

**Round-2 follow-up (all six remaining Majors now closed).** The register above
carries per-item status; the fixes are pinned by `M7_REV2_M2`/`M7_REV2_M3`,
`GeneratorLimitChild.EveryIdClassIsRefusedAtSaveOnceItPassesTheCap`, the
`Shuffled_*` ctest entries, `ViewerSmokeTest_ImportsWithASkippedDimension`, and
mutation Z1. **These follow-up fixes are themselves unreviewed.**

M7 still does not close: **M7 owner UI validation has still never been run** — `M7_UI_UserValidation.md`
is blank, and no agent may fill it (ADR-M4-016). Agent-executed mechanical
checks are recorded separately in `M7_UI_AgentExecutedChecks.md`.
