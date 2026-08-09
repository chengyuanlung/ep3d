# M3 Independent Review

Baseline: `8245c89` (M2)
Reviewed Commit: `f4916af` (branch `m3-wip`) plus uncommitted working-tree changes.

**This document records three review passes.**

| | Rev 1 (initial) | Rev 2 (after Major fixes) | Rev 3 (this document) |
|---|---|---|---|
| Decision | `REQUEST CHANGES` | `REQUEST CHANGES` | **`APPROVE`** |
| Score | 92/100 | 91/100 | **97/100** |
| Critical | 0 | 1 (introduced by the Major-2 fix) | **0** |
| Major open | 5 | 0 (1 regressed into the Critical) | **0** |
| Tests | 161 | 174 | **178** |

Rev 1 raised five Majors. Rev 2 confirmed four fixes and found that the fifth had introduced a
Critical defect — an assertion abort in Debug and silent state corruption in Release for any
feature without a graph node. Rev 3 confirms that Critical is fixed, along with the residual
Major-1 gap and three Minors. **No Critical or Major finding remains open.**

Reviewer: independent agent. Every build, test, binary and numeric result below was produced by
the reviewer executing the command shown, in a build tree **deleted and recreated from empty for
each revision** at `C:\Users\cheng\AppData\Local\Temp\...\scratchpad\rbuild`, outside the
repository. The developer's account of each fix was treated as an unverified assertion and
checked by execution.

Decision:
**APPROVE**

Score:
**97/100**

Approval covers the code, tests and architecture. Two spec-28 gate items remain outstanding and
are documentation-only — see **Final Gate** below. `M3 = COMPLETE` cannot be declared until they
are done, but nothing in the implementation blocks it.

---

## Build Evidence

Windows 11 (10.0.26200), MSVC 14.44.35207, Visual Studio 17 2022 x64, OCCT 8.0.1 via vcpkg.
Build directory `rm -rf`'d before configuring — a genuine from-empty build.

```
rm -rf <scratch>/rbuild
cmake -S . -B <scratch>/rbuild -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake -DPARAMCAD_BUILD_KERNEL_OCCT=ON
cmake --build <scratch>/rbuild --config Debug     && ctest --test-dir <scratch>/rbuild -C Debug   --output-on-failure
cmake --build <scratch>/rbuild --config Release   && ctest --test-dir <scratch>/rbuild -C Release --output-on-failure
```

| Step | Rev 1 | Rev 2 | Rev 3 |
|---|---|---|---|
| Configure from empty | exit 0 | exit 0 | exit 0, `OCCT 8.0.1 found` |
| Debug build | exit 0 | exit 0 | exit 0, **0 errors** |
| Debug tests | 161/161 | 174/174 | **178/178 passed, 0 failed** (5.11 s) |
| Release build | exit 0 | exit 0 | exit 0, **0 errors** |
| Release tests | 161/161 | 174/174 | **178/178 passed, 0 failed** (4.37 s) |

Test-count reconciliation, re-derived independently:
103 (M2 baseline, from `git show 8245c89:tests/...`) + 37 (M3 Core, phases 1-4) + 21
(OCCT-linked) + 17 (`tests/M3ReviewFindingTests.cpp`) = **178**. Matches ctest exactly.

Boundary re-verified after the Core changes:

```
grep -rnE "TopoDS_|BRep|gp_|AIS_|V3d_|OpenCASCADE|QObject|QWidget|QString" src/Core/  -> 0 matches
```

`dumpbin /dependents`, Release binaries, re-run: `ParametricCADCoreTests.exe` 14 DLLs / **0**
`TK*`; `ParametricCADKernelOcctTests.exe` 18 / 4; `ParametricCADApp.exe` 14 / 4. Unchanged
across all three revisions.

### Adversarial re-probe

The reviewer's standalone harnesses were rebuilt against the current libraries and run in
**both** Debug (assertions live) and Release, since the Rev-2 Critical was precisely a
Debug/Release disagreement. Full Rev-1 adversarial suite re-run as a regression check.

---

## Fix Audit

| Finding | Rev 2 verdict | Rev 3 verdict |
|---|---|---|
| Critical-1 — sync aborts/corrupts for graph-less features | *(introduced)* | **RESOLVED** |
| Major-1 — stale `MassProperties` reports current | RESOLVED | RESOLVED |
| Major-1 residual — currency lagged until next recompute | *(raised as Minor-1)* | **RESOLVED** |
| Major-2 — `Feature::state()` falsely `Valid` | REGRESSED | **RESOLVED** |
| Major-3 — `removeObject` left feature in its `Body` | RESOLVED | RESOLVED |
| Major-4 — save/load asymmetry | RESOLVED | RESOLVED |
| Major-5 — wrong regression baseline | RESOLVED | RESOLVED |
| Minor-2 — `describe()` applied to one catch block only | open | **RESOLVED** |
| Minor-3 — stale invalid-dimension message | open | RESOLVED (see Minor-1 below) |
| Minor-4 — false "private/befriended" comment | open | **RESOLVED** |
| Minor-5 — `kMinBoxDimensionMm` had no ADR | open | **RESOLVED** (ADR-M3-009) |

### Critical-1 — RESOLVED

`src/Core/Document/PartDocument.cpp:231` now carries the guard
`if (!graph_.hasNode(feature->id())) continue; // not graph-scheduled`. All four Rev-2 reproductions were re-run in
Debug (assertions live) and Release, and now agree:

```
                            Rev 2 Debug   Rev 2 Release        Rev 3 Debug        Rev 3 Release
recompute, Valid placeholder   ABORT      demoted->Dirty    OK, stays Valid      OK, stays Valid
setParameterValue, ditto       ABORT      demoted->Dirty    OK, stays Valid      OK, stays Valid
load v2 file + recompute       ABORT      demoted->Dirty    OK, stays Valid      OK, stays Valid
load mixed Box+placeholder     ABORT      states rewritten  OK                   OK
```

The mixed-document load — the worst Rev-2 case, since it crashed inside `loadPartDocument`
itself — now produces exactly the right semantics in both configurations:

```
SURVIVED LOAD. ok=1
  feature 'Pad001' type=Pad  state=Valid    <- graph-less: persisted state preserved
  feature 'Box001' type=Box  state=Dirty    <- graph node: correctly demoted
```

Debug and Release no longer disagree about document semantics, which was the disqualifying
property. The three `CRITICAL1_*` tests
(`tests/M3ReviewFindingTests.cpp:224, 237, 249`) assert exactly the behaviour I measured as
broken in Rev 2, so they are genuinely discriminating; I confirmed they run and pass in both
configurations from the from-empty tree.

The fix was also framed correctly rather than patched. ADR-M3-007 now carries an explicit
**SCOPE** rule ("a feature outside the graph has no graph state that could be authoritative
over it ... that is a semantic rule, not a defensive check"), a "How this was learned" section,
and the observation that this was the second defect from the same
"every `Feature` is a `BoxFeature`" assumption, with a forward rule for M4: *any code iterating
`Body::features()` must state which kinds of feature it applies to before it touches one.* That
is the right level to fix a recurring class of defect at.

### Major-1 residual — RESOLVED

`syncFeatureStatesFromGraph()` (`PartDocument.cpp:241-244`) now also clears
`massProperties_.valid` when the graph says the mass node is not current, so both staleness
signals flip at the same instant. Measured, real OCCT kernel, identical in Debug and Release:

```
                                 Rev 2                        Rev 3
after recompute                  mp.valid=1 V=100000          mp.valid=1 V=100000
after edit (no recompute yet)    mp.valid=1 V=100000  <-lag   mp.valid=0 V=100000
after recompute                  mp.valid=1 V=120000          mp.valid=1 V=120000
```

Retention is preserved throughout — the numbers survive, only the currency claim is withdrawn,
which remains the correct reading of ADR-M3-004/006.

### Rev-1 regression sweep

The full Rev-1 adversarial suite was re-run against the current tree. Every defect it originally
exposed is gone, and nothing else moved:

| Rev 1 observation | Rev 3 |
|---|---|
| `after failure: valid=1 V=100000 m=0.27` | `valid=0`, numbers retained |
| `density negative/NaN/inf -> valid=1 mass=0.27` | `valid=0`, numbers retained |
| `removeObject(Box)` → `body still owns 1 feature(s)` | `owns 0`, later recompute succeeds |
| removed box still serialized (`contains "Box001": 1`) | `contains "Box001": 0` |
| `removeObject(Width)` → `save ok=1` then unloadable file | `save ok=0`, refused up front |
| `restored feature state=Valid shape.isValid=0` | `state=Dirty shape.isValid=0` |
| 1e-7 mm → `GeometryConstructionFailed`, empty message | `InvalidDimension` with a diagnostic |
| inertia frame, volume, COM, mass, large dimensions | unchanged and still correct |

---

## Critical Findings

**None.**

## Major Findings

**None open.** All five Rev-1 Majors and the Rev-2 Critical are resolved and independently
verified.

## Minor Findings

1. **The corrected diagnostic prints the C++ identifier instead of its value.** Both kernels now
   emit, verbatim:
   ```
   invalid box definition: every dimension must be finite and at least kMinBoxDimensionMm
   ```
   (`src/Kernel/Occt/OcctGeometryKernel.cpp:41-42`, `tests/Fakes/FakeGeometryKernel.h:37-38`).
   Adjacent string-literal concatenation was used where interpolation was clearly intended, so a
   user-facing message names an internal symbol rather than saying "at least 1e-06 mm". The
   substance of Minor-3 is fixed — the message no longer claims "finite and positive", which was
   the false part — but the replacement should carry the number. One-line fix.

2. **Carried, unaddressed:** the minimum-dimension tests
   (`tests/M3ReviewFindingTests.cpp:281-298`) still use `FakeGeometryKernel`, so they exercise
   `IsValidBoxDefinition` but never verify that 1e-6 mm is actually above OCCT's threshold —
   which is the entire justification for the constant. I re-verified it myself and **the value
   is correct**: 1e-6 accepted (V = 1e-18 exact), 2e-6 and 1e-5 accepted, 9.9e-7 and 1e-7
   rejected, mixed 1e-6 × 100 × 50 builds. Moving one case to the OCCT-linked suite would stop
   the floor silently drifting below what OCCT accepts. ADR-M3-009 records the measurements, so
   the knowledge is not lost — only the automated guard is missing.

3. **Carried, unaddressed:** gate step C
   (`tests/Kernel/OcctRecomputeIntegrationTests.cpp:211-220`) still asserts nothing about
   downstream currency, even though that property is now implemented and is a one-line
   `EXPECT_FALSE(...massProperties().valid)`. The property is covered by
   `MAJOR1_GeometryFailureClearsMassPropertiesCurrency`, so this is completeness of the
   release-gate test rather than a coverage hole.

4. **Carried, unaddressed:** `tests/Kernel/OcctGeometryKernelTests.cpp:159-160` still derives its
   expected mass from the kernel's own volume rather than `w*h*d`. The Core-side twin
   (`tests/MassPropertiesNodeTests.cpp:106-107`) does it correctly. Volume is independently
   checked in `M3_KERNEL_002`, so the chain is sound.

5. **Carried:** `src/Core/Geometry/MathTypes.h:20-24` leaves `Matrix3` defaulting to identity, so
   an uncomputed inertia tensor reads `Ixx = 1` (observed on a freshly loaded document, guarded
   by `valid=0`).

6. **Trivial:** `tests/M3ReviewFindingTests.cpp:275-276` asserts
   `recompute().success == false || recompute().success == true`, which is tautologically true
   and calls `recompute()` twice. Its real purpose — "must not abort" — is served by the
   statement executing at all, but a plain call with a comment would say so more honestly.

7. **`PartDocument::massProperties()` non-const overload** — pushback accepted in Rev 2 and not
   revisited. The header note at `PartDocument.h:90-96` records the trade-off honestly and defers
   to M4. Not an action item.

None of these blocks merge.

---

## Architecture

**Core free of OCCT:** **PASS.** Re-scanned after all three rounds of Core changes — 0 matches on
all 9 spec-20 patterns; the std-header set is unchanged (23 distinct, all standard library).
Binary check re-run: `ParametricCADCoreTests.exe` imports 0 `TK*` DLLs, which is linker-proof.

**Public headers:** **PASS.** No OCCT type crosses the boundary; `<typeinfo>` is confined to
`src/Kernel/Occt/`.

**Kernel isolation:** **PASS.** OCCT includes and libraries remain `PRIVATE` on
`ParametricCADKernelOcct`; discovery is portable with the package-registry guard intact.

**Injection:** **PASS.** `BoxFeature` still only ever sees `context.kernel`; the only concrete
kernel constructions are `src/App/main.cpp` and the OCCT-linked tests.

**Shape ownership:** **PASS.** Transactional build unchanged and still correct; the restored-
`Valid`-without-shape defect is fixed for graph-scheduled features, and graph-less features now
correctly keep their persisted state.

**Architecture invariants:** 1-6 and 8 **PASS**; 7, 9, 10 **NOT APPLICABLE**. Invariant 3 (model
vs computed geometry) is materially better served than at Rev 1: ADR-M3-006's retention/currency
separation and ADR-M3-007's scope rule are general decisions with stated forward rules, not
patches.

---

## Geometry

**Validity / Volume / COM:** **PASS**, unchanged and re-verified. Volume exact against `w*h*d`
from 1e-6 mm (V = 1e-18 mm³) through 1e9 mm (relative error ≤ 3.2e-16). COM at (50, 25, 10) mm
for the mandatory box and (w/2, h/2, d/2) for the asymmetric one, to 1e-6 mm.

---

## Physics

**Units:** **PASS.** The single conversion site (`MassPropertiesNode.cpp`) is untouched by all
three rounds of fixes; `kMm3ToM3`/`kMm5ToM5` remain the only place a density meets a mm-valued
quantity, verified by grep over `src/`.

**Mass:** **PASS.** Gate values 0.27 / 0.324 / 0.942 / 0.628 kg all still reproduce.

**Inertia:** **PASS.** The Rev-1 conclusion stands and was re-measured:
`GProp_GProps::MatrixOfInertia()` is COM-relative — proved from OCCT source
(`GProp_GProps.cxx:110-115` subtracts the `GProp::HOperator` parallel-axis term) and numerically
(`actual Ixx = 6.525e-05` matches the about-COM oracle to 1e-15 relative, while about-corner is
2.61e-04, 4× off; `actual Ixy = 1.6e-19` against a corner-frame −3.375e-04). **No Huyghens
correction is needed and adding one would be a defect.**

**Density-only:** **PASS.** Geometry not rebuilt, verified by kernel call counters.

---

## Recompute

**Dimensions / Unrelated / Density-only:** **PASS**, unchanged.

**Failure:** **PASS** at engine *and* data level. Geometry failure, invalid density, missing
parameter and missing kernel all report failure, block downstream, retain last-valid numbers, and
clear the currency flag.

**Recovery:** **PASS**, and restores currency.

**Stability:** **PASS.** The Rev-2 abort is gone; `recompute()`, `recomputeFrom()`,
`setParameterValue()`, `setMaterialDensity()`, `markDirty()` and `loadPartDocument()` are all
safe against heterogeneous feature sets in both configurations.

---

## Persistence

Payload semantics unchanged and correct: only decimal `ObjectId` strings — no pointer, index,
tessellation, or OCCT runtime state. Save/load symmetry is enforced in both directions; removed
features no longer resurrect on load; a `Valid` `PlaceholderFeature` round-trips losslessly; a
restored `BoxFeature` correctly comes back `Dirty` and recomputes to bit-identical volume, mass,
COM and inertia with all ObjectIds preserved. **PASS.**

---

## Self-Validation Audit

| Claim | Verdict |
|---|---|
| Critical-1 fixed with the suggested guard | **CONFIRMED** (4 reproductions, Debug + Release) |
| Three `CRITICAL1_*` tests, verified in both configurations | **CONFIRMED** (present, discriminating, passing) |
| Residual Major-1 fixed; both signals flip together | **CONFIRMED** (measured) |
| `describe()` now used at both call sites | **CONFIRMED** (`OcctGeometryKernel.cpp:73, 96`) |
| Diagnostics no longer say "finite and positive" | **CONFIRMED** — but see Minor-1 |
| `MassPropertiesNode.h` comment corrected | **CONFIRMED** (the false friendship claim is gone) |
| ADR-M3-007 carries the scope rule; ADR-M3-009 added | **CONFIRMED** (both read in full) |
| 178 tests, Debug and Release both 178/178 | **CONFIRMED** (reproduced from empty) |

The self-validation report and DecisionLog are now accurate on every point I can check. The
developer's stated diagnosis of the abort matches what I measured exactly, including the file and
line. Rev-1's two evidence defects (wrong baseline, over-claimed downstream property) are both
corrected, and the correction explains the error rather than quietly deleting it.

One process observation worth keeping. Across three passes the same failure mode recurred: a
change was verified against the inputs its author had in mind, and the suite agreed, but the
defect lived in an input nobody had thought to construct — a non-`BoxFeature` feature, a
succeed-then-fail sequence, a save after a delete. Each was found by building the adversarial
input by hand rather than by running the suite. That is a coverage-design lesson, not a
criticism of the fixes, and ADR-M3-007's forward rule is the right response to it.

---

## Final Gate (spec 28)

Explicit item-by-item read, as requested. Everything marked **verified** was checked by the
reviewer's own execution, not accepted from a report.

| # | Item | Status |
|---|---|---|
| 1 | OCCT integrated and version documented | **PASS** — 8.0.1 via vcpkg, reported at configure |
| 2 | Core does not link OCCT | **PASS** — `dumpbin`: 0 `TK*` imports |
| 3 | Core headers contain no OCCT types | **PASS** — 0 grep matches |
| 4 | Shape ownership ADR | **PASS** — ADR-M3-001 |
| 5 | Units ADR | **PASS** — ADR-M3-002 |
| 6 | Kernel injection ADR | **PASS** — ADR-M3-003 |
| 7 | Failure policy ADR | **PASS** — ADR-M3-004, extended by 006 |
| 8 | Valid Box B-Rep | **PASS** |
| 9 | Analytical Volume | **PASS** — independently recomputed |
| 10 | Analytical COM | **PASS** |
| 11 | Analytical Mass | **PASS** |
| 12 | Analytical Ixx/Iyy/Izz | **PASS** — plus OCCT-source proof of the frame |
| 13 | Off-diagonal inertia checked | **PASS** — 1.6e-19 against a 1e-9 tolerance |
| 14 | W/H/D incremental recompute | **PASS** — verified by call counters |
| 15 | Density-only does not rebuild geometry | **PASS** |
| 16 | Unrelated branch untouched | **PASS** |
| 17 | Invalid dimensions handled | **PASS** — zero, negative, NaN, Inf, degenerate |
| 18 | Downstream failure semantics correct | **PASS** — the Rev-1 blocker, now verified |
| 19 | Recovery works | **PASS** |
| 20 | Semantic persistence works | **PASS** |
| 21 | Runtime OCCT state not persisted | **PASS** — payload inspected |
| 22 | Load/recompute equivalent | **PASS** — bit-identical |
| 23 | Stable ObjectIds survive | **PASS** |
| 24 | All previous tests pass | **PASS** — all 103 M2 tests present, unmodified, passing |
| 25 | All M3 tests pass | **PASS** — 178/178 |
| 26 | Debug clean build | **PASS** — from empty, exit 0 |
| 27 | Release clean build | **PASS** — exit 0 |
| 28 | Static Core boundary scan | **PASS** |
| 29 | Mandatory release gate | **PASS** — `M3_GATE_ReleaseScenario`, Debug and Release |
| 30 | SelfValidationReport | **PASS** — exists, corrected, accurate |
| 31 | Independent review where supported | **PASS** — this document, three passes |
| 32 | No Critical findings | **PASS** |
| 33 | No unresolved Major findings | **PASS** |
| 34 | Reviewer score ≥ 80 | **PASS** — 97 |
| 35 | README/Roadmap/AGENTS updated | **OUTSTANDING** |
| 36 | CompletionReport created | **OUTSTANDING** |

**32 of 34 substantive items pass. Two remain, both documentation-only:**

- `README.md:18` still says M3 is `IN PROGRESS`; `AGENTS.md:28` still says
  `Current target: M3`. (`docs/Roadmap.md`'s M3 section is written and accurate but carries no
  completion marker.)
- `docs/reviews/M3_CompletionReport.md` does not exist. Spec 25 lists its required contents.

Additionally, spec 26 step 20 requires committing and reporting the final hash: the entire M3
implementation is still an uncommitted working tree (12 modified files plus 3 untracked). This is
not a review finding, but `M3 = COMPLETE` cannot honestly cite a final commit until it is done.

**Read: the completion report may say `M3 COMPLETE` once items 35 and 36 are done and the work is
committed — and not before.** No code, test, or architecture work is required.

---

## Required Changes

**None blocking.** Optional, in rough order of value:

1. Minor-1 — interpolate the actual value into the invalid-dimension message.
2. Minor-2 — move one minimum-dimension test to the OCCT-linked suite.
3. Minor-3 — add the one-line currency assertion to release-gate step C.
4. Minor-4/5/6 — oracle independence, `Matrix3` zero default, tautological assertion.

To reach `M3 = COMPLETE`: update README / Roadmap / AGENTS, create
`docs/reviews/M3_CompletionReport.md`, commit, and report the final hash.

---

## Score Breakdown (spec 22)

| Category | Item | Max | Rev 1 | Rev 2 | Rev 3 |
|---|---|---:|---:|---:|---:|
| **Architecture Boundary** | Core has no OCCT dependency | 7 | 7 | 7 | 7 |
| | Core public API has no OCCT type | 5 | 5 | 5 | 5 |
| | Adapter isolated | 4 | 4 | 4 | 4 |
| | Feature does not instantiate concrete kernel | 4 | 4 | 4 | 4 |
| | *subtotal* | *20* | *20* | *20* | **20** |
| **Shape Ownership** | Safe explicit ownership | 4 | 4 | 4 | 4 |
| | No persistent pointer identity | 3 | 3 | 3 | 3 |
| | Failed build cannot expose dangling/partial shape | 3 | 2 | 3 | 3 |
| | *subtotal* | *10* | *9* | *10* | **10** |
| **Geometry Correctness** | Valid solid / volume / COM | 15 | 15 | 15 | **15** |
| **Physical Correctness** | Units, mass, COM, inertia, documentation | 20 | 20 | 20 | **20** |
| **Parametric / Recompute** | W/H/D behaviour | 5 | 5 | 5 | 5 |
| | Density-only | 4 | 4 | 4 | 4 |
| | Unrelated branch | 3 | 3 | 3 | 3 |
| | Failure / recovery | 3 | 1 | 0 | 3 |
| | *subtotal* | *15* | *13* | *12* | **15** |
| **Persistence** | Semantics persist | 2 | 2 | 2 | 2 |
| | Runtime shape does not | 1 | 1 | 1 | 1 |
| | Reload / recompute equivalent | 2 | 1 | 0 | 2 |
| | *subtotal* | *5* | *4* | *3* | **5** |
| **Tests / Self-validation** | Matrix coverage | 4 | 3 | 2 | 3 |
| | Independent analytical oracle | 2 | 2 | 2 | 2 |
| | Release gate | 2 | 2 | 2 | 2 |
| | Actual evidence | 2 | 1 | 2 | 2 |
| | *subtotal* | *10* | *8* | *8* | **9** |
| **Documentation** | ADRs | 3 | 3 | 3 | 3 |
| | README / Roadmap / AGENTS updated | 2 | 0 | 0 | 0 |
| | *subtotal* | *5* | *3* | *3* | **3** |
| **TOTAL** | | **100** | *92* | *91* | **97** |

Changes from Rev 2: Failure/recovery 0 → 3 (currency correct at both failure and edit time, no
abort); Reload equivalence 0 → 2 (lossless for graph-less features, correct demotion for
scheduled ones); Matrix coverage 2 → 3 (heterogeneous features, the currency window, removal and
save/load symmetry are all covered now; the three carried test-quality Minors keep it off 4).

The two documentation points remain unearned only because the work is deliberately sequenced
after review; they are the last step, not an omission.

---

## M4 Readiness

**READY**, subject to the two outstanding documentation items above.

The architecture earns this rather than merely surviving review. The Core/Kernel boundary holds
at source and binary level and will hold when a second backend or the Qt viewport arrives. The
`IGeometryKernel` seam is small, injectable, and already carries two independent implementations,
which is what lets Core-only tests run without OCCT — that property will matter more in M4, not
less. The density-independent second-moment split is the right factoring for Assembly and
Physics: mass properties compose across instances without re-entering the kernel. Persistence is
semantic-only with stable `ObjectId`s and re-derived edges, so schema growth is cheap.

Three decisions taken under review pressure are genuinely load-bearing for M4 rather than local
patches:

- **ADR-M3-006** separates *retention* from *currency* and states a general rule for every future
  derived value — M4 will add many.
- **ADR-M3-007** establishes that features are heterogeneous and that any code iterating
  `Body::features()` must declare which kinds it applies to. M4 adds feature types; this rule is
  what stops the "every `Feature` is a `BoxFeature`" assumption recurring a third time.
- **ADR-M3-008** makes save enforce every invariant load enforces. Deletion becomes routine once
  component instances reference features, so removal completeness and save/load symmetry both
  pay off immediately.

Known limitations remain declared rather than hidden: `MassPropertiesNode` is hard-wired to a
single `BoxFeature` (ADR-M3-005, flagged for M4), and `Feature::recompute()` survives as vestigial
alongside `IRecomputable` (ADR-M3-004, with the collapse named as the M4 cleanup). Both are
accepted debt under AGENTS hard rule 8, and both are the right scope call for "first parametric
solid".
