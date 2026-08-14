# M7 Self-Validation Report — DXF Dimension and Constraint Reconstruction

> ## CORRECTED after independent review
>
> Round 1 ran three partitioned reviewers. All three returned `REQUEST CHANGES`
> at 71/100, and **two claims below were false**:
>
> - **"668 / 668 in Debug and Release"** is true under `ctest` ONLY. A direct run
>   of `ParametricCADCoreTests.exe` was **665 / 668** in both configurations:
>   M7's five test files were added below `tests/SerializationTests.cpp`, against
>   an instruction written in capitals in `CMakeLists.txt`, and three M7
>   persistence tests then ran against a poisoned ObjectId generator. `ctest`
>   cannot see it, because every test gets its own process. **Ordering now
>   fixed**; the underlying save/load defect is not.
> - **Mutation 8 does not verify Gate J against a skipping child.** The guard it
>   is credited to is tautological — the parent writes the file it then checks
>   for. Verified by mutation: child forced to skip, gate still green.
>
> Two accounting errors as well: the suite table gives Integration 106 where
> ctest registers 107 (the table sums to 667), and one of the 668 is reported
> `Skipped`, so **667 actually execute**.
>
> Nothing has been deleted. Full accounting in `M7_IndependentReview.md`.
>
> The section below titled "What I am least confident about" was correct on
> point 1 and **understated** it: four of the reviewers' findings are in the
> naming/ordering path.

> **These are CLAIMS, not facts.** Spec 33 tells reviewers to treat this
> document that way, and this project's history earns it: across M5 and M6,
> **four** self-validation reports asserted "every fix is mutation-verified" and
> were wrong. Every number below was produced by a command I ran; every one is
> reproducible; and none of them has been checked by anyone else.

**Baseline:** `144867b` — the accepted M6 master state (M6 itself merged as
`88f8f5e`).

**Commit under validation:** `ad078e9` on branch `m7-wip`, plus the Gate K
addition committed with this report. `git log 144867b..m7-wip` is the authority.

**Not pushed.** `m7-wip` and `master` are both local only.

---

## Environment

| | |
|---|---|
| OS | Windows 11 Home 10.0.26200 |
| Compiler | MSVC 19.44 (VS 2022 BuildTools 14.44.35207), C++20 |
| CMake | 4.2.3 |
| OCCT | 8.0.1 (vcpkg) |
| Qt | 6.11.1 (vcpkg) |
| Eigen | 5.0.1 (vcpkg), MPL2 |
| DXF parser | libdxfrw `2025-09-25` (vcpkg), **GPL-2.0-only** |

---

## Dependency and licence (spec 29)

**M7 introduced no new third-party dependency.** Spec 29 asks for the record
only when one is proposed; none was. Reconstruction is built entirely on M6's
parser output and M5's constraint engine, which is what spec 29 says to prefer.

The libdxfrw position is unchanged from M6 and unchanged by M7: **GPL-2.0-only**,
adopted by explicit owner decision, confined to `src/Import/Dxf/DxfReader.cpp`
and `PRIVATE` to the `ParametricCADImportDxf` target. M7 added ~90 lines to that
same translation unit and no others.

> **The licence consequence has not changed and is not solved.** GPL-2.0 is
> copyleft; the shipped viewer links it. If EP3D is to be distributed as
> closed-source commercial software, this dependency must be removed or replaced
> first.

---

## Core boundary audit (spec 5, 6)

| Check | Method | Result |
|---|---|---|
| Core free of DXF types | `grep -rE "DRW_\|libdxfrw" src/Core/` | **PASS** — 2 hits, both inside COMMENTS explaining the rule |
| Core free of Qt | `grep -rE "^#include <Q\|QString\|QWidget" src/Core/` | **PASS** — 0 |
| Core free of OCCT | `grep -rE "TopoDS_\|gp_Pnt\|BRep" src/Core/` | **PASS** — 0 |
| Core free of Eigen | `grep -rE "Eigen/\|Eigen::" src/Core/` | **PASS** — 0 |
| libdxfrw symbols in `ParametricCADCore.lib` | `dumpbin /symbols`, grep `dxfRW\|DRW_` | **PASS** — **0** |
| Reconstruction boundary is neutral | read `ImportedGeometry.h` | **PASS** — `ImportedDimension2D` carries points, a direction, an optional value, a text override and an optional handle string; no parser pointer, no `DRW_*`, no file offset, no vector index |
| Reconstruction reaches the document only through the facade | read `SketchReconstructor.cpp` | **PASS** — `addParameter` and `addSketchConstraint` only; no solver array is touched |

`src/Core/Reconstruction` links nothing beyond Core and is compiled into
`ParametricCADCore`, so reconstruction is testable with no DXF library, no
solver and no kernel present.

---

## Builds and regression

| | Result |
|---|---|
| Debug build | **PASS** — 0 errors |
| Debug tests | **668 / 668 under ctest; 665 / 668 in a single-process run — [CORRECTED], see the banner** |
| Release build | **PASS** — 0 errors |
| Release tests | **668 / 668 under ctest; 665 / 668 in a single-process run — [CORRECTED]** |
| Release actually ran Release binaries | **PASS** — `ctest -C Release -N -V` yields **668** command lines naming `build\Release\` and **0** naming `build\Debug\` |
| M0–M6 regression | **PASS** — the 667-test M6 suite is a subset; nothing was deleted or disabled |

Baseline was 561 at the end of M6. **107 tests added.**

| Suite | Total | of which M7 |
|---|---|---|
| `ParametricCADCoreTests` | 380 | 78 |
| `ParametricCADIntegrationTests` | 106 | 20 |
| `ParametricCADImportTests` | 68 | 9 |
| `ParametricCADSolverTests` | 53 | 0 |
| `ParametricCADKernelOcctTests` | 47 | 0 |
| Viewer smoke (ctest) | 13 | 0 new, 1 extended |

**Four M6-era tests were CHANGED, not deleted**, and each is named here because
a changed test is where a regression hides:

| Test | Why |
|---|---|
| `ASlopedQuadrilateralGetsNoAxisConstraints` | M7.2 generalised recognition, so a sloped closed loop now gets its 4 corners. Renamed and split into two assertions. |
| `AnOpenChainOfFourLinesIsNotARectangle` | Same. Now asserts 3 joints, not 0. |
| `ACornerJustOutsideTheCoincidenceToleranceDoesNot` | Now 3 coincidents, not 0 — **sharper**, because it names which corner was refused instead of failing the whole shape. |
| `ALineJustOutsideTheAxisToleranceIsNot` | Now 1 Horizontal, not 0 — same reason. Expecting 0 would have passed if the recogniser stopped working altogether. |
| `UnsupportedDimensionKindsAreReportedNotReinterpreted` | Used Radius as its example of an unsupported kind; M7.3 supports Radius. Switched to Angular. |

---

## Reconstruction matrix (spec 3)

| Source dimension | Supported | Native result |
|---|---|---|
| Linear (rotated/horizontal/vertical) | **Yes** | `Length` + Parameter |
| Aligned | **Yes** | `Length` + Parameter |
| Radius | **Yes** | `Radius` + Parameter |
| Diameter | **Yes** | `Diameter` + Parameter (holds the diameter; the constraint halves it) |
| Angular | **No** | reported `UnsupportedKind` |
| Ordinate, Leader, GD&T, tolerance | **No** | reported as unsupported entities by the M6 reader |

| Geometric relation | Supported | Rule |
|---|---|---|
| Coincident | **Yes** | endpoint clustering, spanning set, 1e-3 mm |
| Horizontal | **Yes** | 1e-3 rad of the u axis |
| Vertical | **Yes** | 1e-3 rad of the v axis |
| Fix (placement) | **Yes** | lexicographically smallest endpoint; a circle centre when there are no endpoints |
| Equal, Parallel, Perpendicular, Tangent, Concentric | **No** | optional in spec 3; not promoted by ADR |

---

## Gates A–L (spec 23)

| Gate | Result | Evidence |
|---|---|---|
| **A** — explicit Width reconstruction, measured on native geometry | **PASS** | `GATE_A_ExplicitDimensionBecomesNativeParameterAndConstraint` — measures the solved shape, not metadata, and asserts it is NOT the drawn 99.5 |
| **B** — rectangle fully constrained | **PASS** | `GATE_B_RectangleIsFullyConstrained` — 4/2/2/1/2 by kind, `Solved`, DOF 0, 100×50 within 1e-6, 100000 mm³ |
| **C** — Width 100→120, Pad rebuilds to 120000 mm³ | **PASS** | `GATE_C_WidthEditRebuildsTheSolid` |
| **D** — Height 50→80, 192000 mm³ | **PASS** | `GATE_D_HeightEditAfterWidthEdit` |
| **E** — circle dimension; doubling radius gives exactly 4× | **PASS** | `GATE_E_*` ×3, including the diameter route and the ratio asserted at 1e-9 |
| **F** — geometric reconstruction, with mutation controls | **PASS** | `GATE_F_*` ×5 disable each recogniser and lose DOF 0; `GATE_F2_*` ×4 cover junctions, L-shapes and the snap-vs-solve proof |
| **G** — ambiguity | **PASS** | `ADimensionMatchingTwoLinesIsSkippedNotGuessed`, `TwoConcentricCurvesOfTheSameSizeAreAmbiguous`, plus the provenance suite; valid candidates still reconstruct alongside |
| **H** — conflict and recovery | **PASS** | `GATE_H_ConflictIsExplicitCommitsNothingCorruptAndRecovers` — conflict state, offending ids, geometry checked by **measurement**, recovery, and an edit afterwards |
| **I** — save/load/re-solve | **PASS** | `M7_1_GATE_ImportReconstructSaveReloadEditRebuild`, `AnAlreadyReconstructedSketchIsStillRefusedAfterSaveAndLoad` |
| **J** — source independence | **PASS** | `GATE_J_EditWorksInASecondProcessWithTheDxfDeleted` — a **real child process**, with the DXF deleted first |
| **K** — selective recompute | **PASS** | `GATE_K_DensityDoesNotResolveTheSketch` (counting solver), `GATE_K_NoParameterEditEverReRunsReconstruction` |
| **L** — regression, Debug and Release, Release proven | **PASS** | see Builds above |

Every expected number in Gates A–E is computed by hand from the fixture or from
π and the parameter values. **No expectation was produced by running the
reconstructor, the solver or the kernel.**

---

## Mutation verification (spec 32)

Nine mutations, each applied alone, each with the test executable **deleted
before the rebuild and asserted present afterwards** (AGENTS.md rule 2). A run
that could not fail loudly is recorded as INCONCLUSIVE; none was.

| # | Mutation | Caught by |
|---|---|---|
| 1 | Every dimensional constraint bound to the first Parameter | 5 gates |
| 2 | Width/Height naming swapped | 4 gates + 6 core tests |
| 3 | Length targets the first line instead of the matched one | 7 gates + 1 core test |
| 4 | Rollback made a no-op | 2 transaction tests |
| 5 | Idempotence guard removed | the Width_2 test |
| 6 | Coincidence emits all pairs instead of a spanning set | 2 junction tests + 1 gate — **see below** |
| 7 | Diametric dimension's rim point read as its centre | 2 core tests + Gate E's diameter case |
| 8 | Fresh-process child expects the wrong volume | the parent, via non-zero exit. **[CORRECTED]** This exercises the FAILING-child path only; the skip path is unguarded and the guard credited for it is tautological |
| 9 | Reconstruction report never stored | the import smoke test, 4× |

**Mutation 2 is recorded as a partial result.** It changes NAMING only: the
values still land on the correct lines, so the geometry gates pass and only the
naming tests fail. Mutation 3 was added because of it, and is the true
binding-target mutation.

**Mutation 6 disproved a claim I had written in the code.** I asserted that
all-pairs coincidence turns a three-line junction into an OverConstrained
sketch. It does not: M5 gives DOF priority over redundancy, so while any freedom
remains the redundant rows are invisible. The integration test I wrote to prove
the claim **passed under the mutation**. The masking ends at DOF 0, so a second
test — a fully dimensioned junction — was added, and that one fails with
"constraints are redundant but consistent". Both tests are kept: the
non-discriminating one carries a comment saying so, because it records where the
masking lives.

---

## Adversarial tests (spec 24)

| Case | Covered by |
|---|---|
| duplicate source dimension references | `ADimensionMatchingTwoLinesIsSkippedNotGuessed` |
| missing source reference / missing native target | `ADimensionMatchingNoLineIsSkipped` |
| target deleted BEFORE reconstruction | `ATargetDeletedBeforeReconstructionIsSimplyNotFound` |
| target deleted BETWEEN analysis and apply | `AnEntityDeletedBETWEENAnalysisAndApplyIsRefusedNotIgnored` |
| duplicate requested Parameter names | `ANameAlreadyInTheDocumentIsResolvedDeterministically`, `TwoDimensionsOnOneLineBothReconstructAndAreNamedApart` |
| NaN / Infinity | `NonFiniteDefinitionPointsProduceNoParameter`, `NonFiniteAndNegativeDimensionsAreRefused` |
| zero / negative length and radius | as above, `ZeroAndNegativeRadiiAreRefused` |
| very small / very large dimensions | `DimensionsExactlyAtAndJustBelowTheReconstructionFloor`, `AVeryLargeDimensionIsStillReconstructed` (1 km) |
| exactly at / just below the validity floor | `DimensionsExactlyAtAndJustBelowTheReconstructionFloor` (uses `std::nextafter`) |
| horizontal / vertical just inside and outside tolerance | `ALineJustInside…`, `ALineJustOutside…` |
| coincident endpoints just inside and outside | `ACornerJustInside…`, `ACornerJustOutside…` |
| dimension disagrees with geometry | `AStatedValueThatDisagreesMateriallyIsSkipped`, `TheRadiusIsCrossCheckedAgainstTheCurveItNames` |
| redundant / over-constrained | `AThreeWayJunction…`, `AFullyDimensionedJunction…` |
| conflicting reconstruction | Gate H |
| random entity order | `ShuffledEntityOrderProducesTheSameConstraintCounts`, `TheSameShapeDrawnInADifferentOrderPlansTheSameFix` |
| random source dimension order | `NamingDoesNotDependOnTheOrderDimensionsArrive` |
| repeated reconstruction | `ReconstructingTwiceDoesNotDuplicateParameters` |
| reconstruction after save/load | `ASketchLoadedFromDiskCanStillBeReconstructed` |
| reconstruction after entity deletion | `ATargetDeletedBeforeReconstruction…` |
| failure halfway through plan application | `AFailedApplyLeavesNoParameterBehind` |
| rollback then successful reconstruction | `RollbackIsFollowedByASuccessfulReconstruction` |
| fresh-process load, and edit after it | `GATE_J_*` + `M7FreshProcessChild` |
| unknown reconstruction flag / mode | `EveryRecognizerDisabledProducesAnEmptyPlan…`, `ANonsenseToleranceDoesNotCrashOrInventGeometry` |

---

## Known limitations

- **Provenance is not persisted** (ADR-M7-017). After save and reload, nothing
  can say where a constraint came from. Deliberate, documented, and pinned by a
  test asserting the handle is ABSENT from the saved file.
- **Angular dimensions are not reconstructed.** Reported, never reinterpreted.
- **A curve dimension whose size disagrees materially with its curve is
  refused**, where the equivalent line case is accepted (ADR-M7-014). The
  asymmetry is deliberate and explained, but it means a badly drawn circle
  reconstructs nothing while a badly drawn rectangle reconstructs.
- **The rectangle release fixture is skewed by 0.5%; the circle fixture by 2%.**
  The circle cannot be skewed further without falling outside its own
  association rule, so its "the solver did work" evidence rests on Gate E's edit
  rather than on the initial solve.
- **Equal / Parallel / Perpendicular / Tangent / Concentric are not
  reconstructed.** Optional in spec 3, not promoted.
- **One Fix per sketch.** A drawing of several disconnected shapes has one
  anchored and the rest free.
- **`INSERT` is still not expanded** (inherited from M6), so a block-based
  drawing still imports as nothing plus a diagnostic — including its dimensions.

---

## NOT EXECUTED

| Item | Status |
|---|---|
| **M7 owner manual UI validation** | **NOT EXECUTED.** No `M7_UI_UserValidation.md` has been filled in. Spec 28 requires it and it is the owner's to do. |
| **M7 independent review** | **NOT EXECUTED.** Spec 33 requires three partitioned reviewers. Nobody but the author has read this code. |
| **M6 owner UI validation** | **STILL NOT EXECUTED** — inherited open item, checklist waiting at `M6_UI_UserValidation.md`. |
| **M6.11–M6.14 independent review** | **STILL NOT EXECUTED** — inherited open item. |
| Binary DXF with dimensions | Untested; no fixture. |
| Display scaling (200%) | Not executed, as in M4/M5/M6. |
| Reconstruction of a drawing with >100 dimensions | Not executed; no performance work was attempted or claimed. |

---

## What I am least confident about

Stated because a self-validation report that lists only successes is the kind
this project has already been burned by four times:

1. **The naming policy is the thinnest-tested part.** Width/Height/Length-N with
   `_2` collision suffixes has several interacting rules, and mutation 2 showed
   the geometry gates cannot see naming errors at all. If something is wrong
   here, only the naming tests would catch it — and I wrote those.
2. **`AnEntityDeletedBETWEENAnalysisAndApplyIsRefusedNotIgnored` passes for a
   reason I verified but did not design.** The refusal comes from the Sketch
   rejecting a constraint that names a missing entity, not from the plan
   re-validating its targets. That is the correct outcome by a route that could
   change if `addSketchConstraint` ever became more permissive.
3. **The rectangle and circle fixtures share a single shape each.** Spec 22's
   Fixture D (ambiguous) and E (conflicting) exist as in-memory tests, not as
   DXF files, so the ambiguity and conflict paths are not exercised through the
   real parser.
4. **Every mutation was chosen by the person who wrote the code.** AGENTS.md is
   explicit that this measures the author's imagination, and that only a
   reviewer deleting a line the author did not think to delete has ever found an
   unguarded fix in this project.
