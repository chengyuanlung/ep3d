# M6 Self-Validation Report — DXF Import to Stable Sketch Entities

**Baseline:** `a6e7078` — the accepted M5 master state.

**Commit under validation:** branch `m6-wip`, head recorded in
`M6_CompletionReport.md` when the milestone closes.

> ## CORRECTED after independent review
>
> Two reviewers verified this report's claims by execution. **Three of them were
> false**, and the falsest was one this document leaned on hardest:
>
> - "Unguarded mutations: **none**" — the ARC unit conversion could be removed
>   entirely with all 526 tests still green.
> - "Blocks (INSERT) are skipped, not expanded. A drawing built from blocks
>   imports as nothing plus a diagnostic" — **false in both halves**. Block
>   contents were imported as model geometry, including the annotation lines of
>   every DIMENSION.
> - The ARC-near-360° NOT EXECUTED row's justification ("`arc_crossing_zero.dxf`
>   covers the wrap") — it does not, and a Major lived exactly there.
>
> Each is corrected in place and marked **[CORRECTED]**; nothing was deleted.
> Full accounting in `M6_IndependentReview.md`.

Spec 22 governs this document: nothing here is reported PASS unless it was
actually executed, and anything not executed says **NOT EXECUTED** and why.

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
| **DXF parser** | **libdxfrw `2025-09-25` (vcpkg), GPL-2.0-only** |

---

## Dependency and licence audit (spec 20)

| | |
|---|---|
| Library | libdxfrw |
| Version | `2025-09-25`, vcpkg port, x64-windows |
| Licence | **GPL-2.0-only** |
| Linkage | dynamic (vcpkg x64-windows triplet default) |
| Target | `ParametricCADImportDxf` **only**, `PRIVATE` |
| Translation units naming it | **one** — `src/Import/Dxf/DxfReader.cpp` |
| Crosses into Core? | **No** — measured below |
| Owner decision | **Yes.** The owner was asked and chose libdxfrw over `dime` (BSD-3-Clause) and over an own minimal reader. Recorded in ADR-M6-001. |

**The licence consequence, stated rather than buried.** GPL-2.0-only is
copyleft. Linking it — statically or dynamically — generally makes the combined
work a derivative that must be distributed under GPL-2.0-compatible terms,
including corresponding source. `ParametricCADViewer` links it, so the shipped
application inherits that. If EP3D is to be distributed as closed-source
commercial software, this dependency must be removed or replaced first. Nothing
in the build warns about it, which is why it is written in three places: the
ADR, `CMakeLists.txt` beside the `find_package`, and here.

---

## Core boundary audit (spec 5, ADR-M6-003)

| Check | Method | Result |
|---|---|---|
| Core free of DXF types | `grep -rnE "DRW_\|libdxfrw" src/Core/` excluding comments | **PASS** — zero |
| Core free of Qt / OCCT / Eigen | same scan | **PASS** — zero |
| libdxfrw symbols in `ParametricCADCore.lib` | `dumpbin /symbols` | **PASS** — **0** |
| libdxfrw symbols in `ParametricCADViewerCore.lib` | `dumpbin /symbols` | **PASS** — **0** |
| libdxfrw symbols in `ParametricCADImportDxf.lib` | `dumpbin /symbols` | **[CORRECTED] a non-zero count, filter-dependent.** I wrote "205"; a reviewer measured 206–209 (Debug) and 104–107 (Release) depending on the grep pattern. The number was never the point and should not have been stated as if it were exact — **the load-bearing half is the zeros above**, which reproduce exactly. |
| Core test binary dependents | `dumpbin /dependents` on the Release exe | **PASS** — KERNEL32, MSVCP140, VCRUNTIME140(_1), UCRT only |
| Import representation is neutral | read `ImportedGeometry.h` | **PASS** — coordinates, radii, angles, unit metadata and skip records; no parser pointer, no handle, no file offset |

Core compiles, links and tests with libdxfrw absent: the import target is behind
`if(libdxfrw_FOUND)` and Core links neither it nor the library.

---

## Builds and regression

| | Result |
|---|---|
| Debug build | **PASS** — 0 errors |
| Debug tests | **551 / 551** |
| Release build | **PASS** — 0 errors |
| Release tests | **551 / 551** |
| Release actually ran Release binaries | **PASS** — every ctest command line names `build/Release/...`, none names `build/Debug/`. Independently reproduced by a reviewer with `ctest -C Release -N -V`. |
| M0–M5 regression | **PASS** — every pre-M6 test still passes |

Baseline was 498 at the end of M5; **53 tests added**, twelve of them
regressions for review findings — seven from the first round (`M6_REV_*`) and
five from the second (`M6_RR_*`).

---

## Release gates (spec 16)

| Gate | Result | Evidence |
|---|---|---|
| **A** — LINE import against a hand-computed oracle | **PASS** | `M6_GATE_A` — (0,0)→(100,50) read from a fixture small enough to check by eye |
| **B** — CIRCLE centre and radius | **PASS** | `M6_GATE_B` — centre (25,30), radius 10; centre deliberately off the origin so dropping it would show |
| **C** — ARC centre, radius, start/end direction, orientation, measured geometrically | **PASS** | `M6_GATE_C` computes the arc's start and end POINTS **from the reader's output** and compares them with hand-computed coordinates — **[CORRECTED]**, this row previously said "from the model"; `M6_ARC_004` is the document-level check and `M6_REV_004` measures the sweep through a real Pad volume |
| **D** — mixed file: count, kinds, unique ids, no dependence on array position | **PASS** | `M6_GATE_D_MixedFileImportsEveryKind`, `..._FileOrderIsNotIdentity` (same entities, shuffled file, geometry compared as sets), `..._RepeatedImportsOfOneFileAgree` |
| **E** — save/load identity | **PASS** | `M6_GATE_E_MixedImportSurvivesSaveLoadWithEveryId` — every id resolves after reload and keeps its kind **and its full geometry** (**[CORRECTED]**: the row was written before the geometry comparison was added and never updated, so it understated its own test) |
| **F** — source independence | **PASS** | `M6_GATE_EF` asserts the saved document contains no `.dxf` reference at all, then reloads and checks the geometry |
| **G** — imported closed profile drives 3D | **PASS** | `M6_GATE_G` — 60×40×20 = 48000 mm³ and 0.1296 kg at 2700 kg/m³, both hand-computed; still parametric afterwards; survives save/load onto a **fresh** kernel |
| **H** — invalid/unsupported input | **PASS** | `M6_GATE_H` ×3 — malformed file diagnostics, a failed import leaving sketch count, registry size and graph node count unchanged, and NaN/∞ rejected at the importer |
| **I** — regression, Debug and Release, Release proven | **PASS** | see Builds above |

Every expected number in Gates A–C and G is computed by hand from the fixture.
No expectation was produced by running the importer.

---

## Fixtures (spec 15)

All hand-written and small enough to read. Several are built to make a passing
test mean something:

| Fixture | Deliberate design |
|---|---|
| `line.dxf` / `line_inches.dxf` | **Identical coordinates, different units.** If the unit conversion never ran, both tests would pass with the same expectations. |
| `circle.dxf` | Centre **off the origin** — a reader that dropped the centre would still get the radius right. |
| `circle_metres.dxf` | Centre and radius are separate group codes; scaling one and not the other is an easy mistake, so both are asserted. |
| `arc.dxf` | 30°/200°: not 0/90/180, sweep 170° so neither a quarter nor a half turn, centre off-origin with **negative y**. An unconverted 30 would mean 30 **radians** = 107°, a different point. |
| `arc_crossing_zero.dxf` | 350°→40°, a 50° sweep **through zero**. An importer assuming `end > start` gets it backwards; an arc that does not cross zero cannot reveal that. |
| `unsupported.dxf` | The unsupported entity sits **between** two supported ones — a trailing one could not distinguish "skipped it" from "stopped there". |
| `mixed.dxf` / `mixed_shuffled.dxf` | Same entities, different order, geometry compared as **sets**. |
| `closed_rectangle.dxf` | Sides listed **out of traversal order with two reversed**, because a real DXF has no obligation to list a loop in order. |
| `*_degenerate.dxf` | Bad entities **between** valid ones: abandoning the file returns too few, silently repairing returns too many. |

---

## Mutation verification (spec 18)

Every mutation was applied, the suite rebuilt, the failures recorded, then the
source restored **and rebuilt again** before re-verifying.

**[CORRECTED]** The table below was accurate for the mutations it lists, and
the conclusion drawn from it — "unguarded mutations: none" — was **false**. A
reviewer removed the ARC unit conversion and all 526 tests stayed green, because
no fixture contained an arc in a non-millimetre file. Six mutations chosen by me
proved six things about the code I had chosen to test.

| Mutation | Tests killed |
|---|---|
| Omit the unit conversion | `M6_UNITS_001`, `M6_UNITS_003` |
| Swap ARC start/end | `M6_GATE_C`, `M6_ARC_001`, `M6_ARC_003`, `M6_ARC_004` |
| Skip one imported entity kind | 7 tests incl. `M6_GATE_D`, `M6_CIRCLE_003` |
| Break transaction rollback | 5 tests incl. both `M6_TRANSACTION_*` and `M6_GATE_H` |
| Duplicate an imported entity | 13 tests incl. `M6_GATE_A`, `M6_GATE_D`, both `M6_GATE_G` |
| Stop reporting skipped entities | `M6_SKIP_001` |

**Originally claimed: "Unguarded mutations: none." That was false.** Two fixes
were entirely unguarded and a reviewer proved it by execution:

| Unguarded fix | How it was found | Now covered by |
|---|---|---|
| ARC unit conversion | removed `* scale()` from all three arc values; 526/526 still passed | `M6_REV_003` (an arc in an inches file) |
| ARC sweep direction | flipped `counterClockwise`, turning a 170° arc into 190°; exactly ONE assertion failed, and it restated the ADR rather than measuring a curve | `M6_REV_004` (a 90° sector's padded volume, which differs 3× from the 270° one) |

Six further mutations were run against the review fixes — block guard, scale
timing, arc scaling, sweep guard, extrusion guard, mil mapping — and each killed
its own test.

**[CORRECTED AGAIN] "Unguarded among those: none" was false, for the second
time in this document.** A re-review ran mutations I had not thought of and
found two unguarded halves:

| Unguarded half | Consequence when removed | Now covered by |
|---|---|---|
| `endBlock()` teardown | a file with a BLOCKS section followed by real geometry imported **nothing**; 45/45 still green | `M6_RR_002` |
| the sweep guard's **upper** clause | an arc of 359.99999° restored the whole-file abort; 45/45 still green | `M6_RR_003` |

The first paragraph of this section already said that a mutation suite I write
measures my imagination rather than my coverage. I then wrote a second one and
drew the same conclusion from it. **The only mutation set that has ever found an
unguarded fix on this project is one written by someone else.**

---

## Adversarial coverage (spec 17)

| Case | Status |
|---|---|
| Zero-length LINE | **PASS** — `M6_ADV_001` |
| Zero / negative CIRCLE radius | **PASS** — `M6_CIRCLE_002` |
| Invalid ARC radius | **PASS** — `M6_ARC_005` |
| ARC crossing 0° | **PASS** — `M6_ARC_003` |
| Shuffled DXF entity order | **PASS** — `M6_GATE_D_FileOrderIsNotIdentity` |
| Very small / very large coordinates | **PASS** — `M6_ADV_002` (0.001 mm and 10 m in one file) |
| NaN / Infinity | **PASS** — `M6_GATE_H_NonFiniteValuesNeverReachTheDocument` |
| Unsupported entity between supported ones | **PASS** — `M6_SKIP_001` |
| Import failure after valid entities were parsed | **PASS** — `M6_TRANSACTION_002` |
| Save/load after import | **PASS** — `M6_GATE_E`, `M6_GATE_EF` |
| Repeated import into one document | **PASS** — `M6_MIXED_001` |
| Deleting the imported sketch, then recompute | **PASS** — `M6_MIXED_002`, `M6_ADV_005` |
| Malformed DXF | **PASS** — `M6_GATE_H_AMalformedFileFailsWithAUsefulCause` |
| **Duplicate / missing DXF handles** | **NOT EXECUTED** — nothing in M6 reads a DXF handle, by design (ADR-M6-004): identity comes from the shared id generator, so a duplicate or missing handle cannot affect correctness. There is no code path to exercise. |
| **ARC near 360°** | **[CORRECTED] PASS, and the original justification was wrong.** This row said `arc_crossing_zero.dxf` covered the wrap. It does not, and a Major lived precisely there: a 0°→360° arc aborted the entire file import. Now `M6_REV_005` (`arc_full_turn.dxf`) and ADR-M6-012. |
| **Fresh-process load** | **PASS — executed by a reviewer, not by me.** A reviewer wrote the probe this row admitted was missing: three genuinely separate processes, with the imported sketch/entity ids deliberately the largest in the file. Both sketches resolve as `Sketch*` (not aliased), every entity id resolves, the mass node does not collide, the Pad rebuilds to 48000 mm³, and geometry compares bit-exactly across processes. Mutation-verified. **Still NOT in the suite** — the probe lives in the reviewer's scratch, so nothing in CI runs it. |

---

## UI (spec 19)

| Requirement | Status |
|---|---|
| Invoke DXF import | **PASS** — File ▸ Import DXF… (Ctrl+I) |
| Select a file | **PASS** — `QFileDialog`; the only part a test cannot drive, and the only part left in the menu slot |
| Import into a Sketch | **PASS** — `ViewerSmokeTest_ImportsDxf` drives the whole path through `MainWindow` in the running application |
| Appears in the model tree | **PASS** — the selftest counts sketch rows in the outline, so an import the tree does not list fails |
| Visible through the normal presentation path | **PASS (functional)** — the imported sketch is an ordinary document object and goes through the same refresh; **pixels NOT EXECUTED** |
| Useful failure diagnostic | **PASS** — the reader's cause is carried through; `ViewerSmokeTest_ReportsFailedImport` requires a bad path to FAIL the selftest |
| **Owner manual UI validation** | **NOT EXECUTED** — the owner's to perform (ADR-M4-016), and it must never be described as an independent agent review |

---

## Known limitations

- **Supported DXF entities: LINE, CIRCLE, ARC only.** Everything else is
  reported as skipped and never reinterpreted. POINT, RAY, XLINE, ELLIPSE,
  LWPOLYLINE, POLYLINE, SPLINE, INSERT, TRACE, 3DFACE, SOLID, TEXT, MTEXT,
  DIMENSION, LEADER, HATCH, IMAGE and VIEWPORT are recognised by kind so the
  diagnostic can name them.
- **Binary DXF is untested.** libdxfrw reads it; no fixture exercises it.
- **Blocks (`INSERT`) are skipped, not expanded.** A drawing built from blocks
  imports as nothing plus a diagnostic. **[CORRECTED]** — when first written
  this was false in both halves: block CONTENTS were imported as model geometry
  at definition coordinates, and a drawing with one DIMENSION imported its three
  annotation lines as real entities. Fixed in ADR-M6-009; the sentence is true
  now.
- **3D DXF is flattened by ignoring Z.** Entities are read as their X and Y.
- **A non-default extrusion (code 210) is refused and reported for CIRCLE and
  ARC.** **[CORRECTED]** this bullet used to say so without qualification, and
  a reviewer pointed out it is false for LINE: a LINE carrying `210 = (0,0,-1)`
  imports with no skip record. That is *correct* behaviour — DXF stores LINE in
  world coordinates, so 210 does not affect its geometry, and `M6_REV_006`
  asserts the line still imports — but the sentence claimed more than the code
  does. **[CORRECTED]** — this bullet used to say the entity imported "at
  the wrong orientation, not detected, not reported". A reviewer measured it and
  found worse: for the common `(0,0,-1)` the correct result is a **mirror**, and
  a mirrored part has identical area, volume and mass, so no oracle this project
  uses could detect it. And because LINE is stored in world coordinates while
  CIRCLE/ARC are stored in the entity's own, ignoring 210 mixed two frames in
  one file — a hole imported at (−25, 30) instead of (25, 30) with `IMPORT OK`.
  See ADR-M6-013.
- **DXF `$INSUNITS` values other than 0/1/2/4/5/6** map to `Unrecognized` and
  take the millimetre default with a diagnostic.
- **A truncated LINE is silently misinterpreted.** libdxfrw default-initialises
  a missing second point to (0,0,0) and exposes no presence flag, so a LINE with
  codes 10/20 and no 11/21 imports as a line to the sketch origin, with no skip
  record. Found by a reviewer. Detecting it needs a group-code-presence hook the
  library does not offer. In practice it opens or branches the loop and the Pad
  fails loudly, so it produces a phantom entity and a misleading success message
  rather than a wrong solid — but it is a real hole in "never silently
  misinterpreted".
- **`INSERT` is not expanded**, so block-based drawings import as nothing.
- **`$INSUNITS` 11, 12, 17–20** (angstrom, nanometre, gigametre, astronomical
  unit, light year, parsec) are deliberately unmapped — see ADR-M6-011.
- **The owner UI validation has not been run.**

---

## Self score

**Not claimed.** M5 produced a self-validation reporting "Critical: 0" over a
broken mandatory constraint, and four review rounds then found defects the
previous round's fixes had introduced. A number I award myself before an
independent review is worth nothing.

## Ready for review

**[CORRECTED] Originally "YES", with "mutation verification shows no unguarded
fix" as part of the reason. That reason was false.** Two independent reviews
then found 1 Critical and 7 Major, all now fixed with mutation-verified tests.

**Ready for RE-review: YES.** Not ready to be called complete: the round of
fixes above has not itself been reviewed, and on this project's record — M5
needed four rounds, each finding defects the previous round's fixes introduced —
that is not a formality. Owner manual UI validation of the import workflow is
also outstanding and is the owner's to perform (ADR-M4-016).
