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
