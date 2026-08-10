# M6 Completion Report — DXF Import to Stable Sketch Entities

> **STATUS: FUNCTIONALLY COMPLETE — two verification items open.**
>
> All required DXF entities import and Gates A–I pass. **Three** independent
> review rounds have run, and every one found defects created by the previous
> round's fixes:
>
> | Round | Found |
> |---|---|
> | 1 | 1 Critical, 6 Major |
> | 2 | 1 Critical, 4 more — including an unbounded loop that froze the application on a legal DXF file, the identical bug this project had already fixed and documented in M5 |
> | 3 | 1 Major live defect, **six unguarded guards**, and three still-false documentation claims |
>
> Round 3's finding is the one worth carrying forward, because it is mechanical:
> **every unguarded line was the third leg of a LINE / CIRCLE / ARC triple where
> only one or two legs got a fixture.** Two reviewers found it independently by
> deleting lines the author had not thought to delete.
>
> All findings are fixed. Every fix is mutation-verified **except one**, named
> below, which no single-mutation test can reach.
>
> **Round 3's fixes have not themselves been reviewed**, and **owner manual UI
> validation of the import workflow has not been performed.** M6 is not declared
> complete here.

**Baseline:** `a6e7078` — the accepted M5 master state.

**Branch:** `m6-wip`. This file is rewritten by each round, so naming a head
commit here has been wrong more often than right; `git log m6-wip` is the
authority.

---

## Mission

DXF becomes an **input format**, not part of the document model:

```
DXF file → libdxfrw → ImportedSketchGeometry → importer
         → PartDocument/Sketch API → ordinary Sketch entities
```

After import there is nothing about an entity that says it came from a file. It
carries an ordinary `SketchEntityId`, persists through the v5 schema, and can be
constrained, solved, extruded and deleted exactly like geometry the user drew —
which `M6_ADV_006` proves by constraining an imported line with a Parameter and
letting M5's solver move it.

---

## Dependency and licence — the owner's decision

| | |
|---|---|
| Library | **libdxfrw** `2025-09-25` (vcpkg) |
| Licence | **GPL-2.0-only** |
| Chosen by | **Explicit owner decision**, as spec 20 requires |
| Alternatives offered | `dime` (BSD-3-Clause); an own minimal ASCII reader |
| Confinement | one translation unit, one CMake target, `PRIVATE` |

**GPL-2.0-only is copyleft.** Linking it — statically or dynamically —
generally makes the combined work a derivative that must be distributed under
GPL-2.0-compatible terms, including corresponding source. `ParametricCADViewer`
links it, so the shipped application inherits that. **If EP3D is to ship
closed-source, this dependency must be replaced first**, and nothing in the
build will warn about it. That is why it is written in four places: ADR-M6-001,
`CMakeLists.txt` beside the `find_package`, the self-validation report, and here.

The confinement is what makes replacement cheap: swapping libdxfrw for `dime`
or an own reader means rewriting `src/Import/Dxf/DxfReader.cpp` and nothing else.

---

## Test totals

| | M5 baseline | M6 |
|---|---|---|
| Total | 498 | **560** |
| `ParametricCADCoreTests` | 301 | 301 |
| `ParametricCADSolverTests` | 53 | 53 |
| `ParametricCADKernelOcctTests` | 47 | 47 |
| `ParametricCADIntegrationTests` | 87 | 87 |
| **`ParametricCADImportTests`** | — | **59** |
| Viewer smoke tests (ctest) | 10 | 13 |

**560 / 560 in Debug and Release**, with the Release run verified to invoke
Release binaries — independently reproduced by a reviewer. `M6_RR_001..005` are
one per second-round finding; `M6_R3_001..009` one per third-round finding.

---

## Gates A–I

| Gate | Result |
|---|---|
| **A** LINE import against a hand-computed oracle | **PASS** |
| **B** CIRCLE centre and radius | **PASS** |
| **C** ARC measured geometrically — centre, radius, start/end direction, orientation | **PASS** |
| **D** Mixed file: count, kinds, unique ids, no dependence on array position | **PASS** |
| **E** Save/load identity — ids, geometry, kinds | **PASS** — *"references" removed: no M6 test round-trips a constraint reference on an imported sketch. `M6_ADV_006` constrains an imported line but never saves it. Native-sketch constraint round-trip is covered by M5's v5 tests, so this is an evidence gap, not a functional one.* |
| **F** Source independence — the saved document names no `.dxf` at all | **PASS** |
| **G** Imported closed profile drives a real solid: 48000 mm³, 0.1296 kg, hand-computed | **PASS** |
| **H** Invalid/unsupported input: documented result, useful diagnostics, no corrupt partial document | **PASS** |
| **I** Regression in Debug and Release, Release proven | **PASS** |

Every expected number is computed by hand from a fixture small enough to read.
No expectation was produced by running the importer.

---

## ADRs

Seven were required by spec 21; **fifteen** exist. Eight more had to be written
once review found what the first seven had got wrong — and ADR-M6-014 exists
because a reviewer grepped the log for "overflow", got nothing, and found a fix
that had shipped two rounds earlier with a test and no record of why.

| ADR | Subject |
|---|---|
| M6-001 | DXF parser selection, and what its licence costs |
| M6-002 | DXF unit policy — **superseded in timing by M6-010, corrected in coverage by M6-011** |
| M6-003 | Import architecture and the Core boundary |
| M6-004 | Imported entity identity |
| M6-005 | Unsupported entity policy |
| M6-006 | Transactional import |
| M6-007 | ARC orientation and angle convention |
| M6-008 | What M6 does not read — **assessment replaced by M6-013** |
| M6-009 | Block definitions are not model geometry |
| M6-010 | Units are applied once, after the whole file is read |
| M6-011 | Every `$INSUNITS` value the format defines |
| M6-012 | A degenerate sweep is skipped, not fatal |
| M6-013 | A non-default extrusion is refused, not guessed |
| M6-014 | Finiteness is checked on both sides of the unit multiply — *written three rounds late; the fix it describes shipped with a test and no ADR* |
| M6-015 | Two structural facts are read outside the parser: an unterminated BLOCKS section, and a LINE with no end point |

---

## Independent review

Three rounds, two reviewers each, recorded in full in `M6_IndependentReview.md`.

**1 Critical, 6 Major, 2 Minor/Info** — all fixed with regression tests, except
two accepted and recorded as limitations. (An earlier draft of this line, and
the M6.9 commit message, said "7 Major"; the review enumerates six.)

A **second** review round then verified those fixes and found the round had
recreated the pattern: **1 more Critical and 4 more findings, all introduced by
the fixes**, including an unbounded loop that froze the application on a legal
DXF file — the identical bug this project fixed and documented in M5.

Three claims in my own self-validation report were **falsified by execution**,
including the one it leaned on hardest ("unguarded mutations: none"). All are
corrected in place and marked, nothing deleted.

A **third** round then reviewed the second round's fixes. The Critical fix was
found genuinely correct across its whole domain — measured, not read — but the
round's other fixes had landed on one or two of the three entity kinds each, and
**six lines could be deleted with all fifty tests still passing.** Both
reviewers converged on the same mechanical rule, which is worth more than any
individual finding:

> **Every unguarded line was the third leg of a LINE / CIRCLE / ARC triple where
> only one or two legs got a fixture.**

Round 3 also found one live defect: a `BLOCK` with no `ENDBLK` made libdxfrw
read the entire rest of the file as block content, so the whole drawing vanished
while the reader reported success and the message blamed the user's geometry
(ADR-M6-015).

The durable finding: **a mutation suite written against my own code measures my
imagination, not my coverage.** Every genuinely unguarded fix across three
rounds was found by someone removing a line I had not thought to remove. Round
2's mutation run mutated the LINE leg and the shared code and concluded "no
unguarded fix"; round 3 deleted the ARC legs and found six.

### The one fix that is not mutation-verified

The sweep guard was rewritten from `sweep < lo || sweep > hi` into positive
form, because both clauses of the negative form are **false for NaN** and
`fmod(inf, 2*pi)` is NaN. Restoring the negative form breaks no test, and
**no single-mutation test can break it**: the only route to a NaN sweep is a
non-finite angle, which `AllFinite` rejects first (a reviewer traced libdxfrw's
degree-to-radian conversion and confirmed `|end - start|` cannot overflow). It
is defence in depth behind a guard that is itself tested, and it is listed here
rather than counted as covered.

**No score is claimed.** Spec 23 defines no scorecard for M6, and a number I
awarded myself after my own report was shown to contain three false claims would
be worth nothing.

---

## Known limitations

- **Supported entities: LINE, CIRCLE, ARC.** Everything else is reported by kind
  and never reinterpreted.
- **`INSERT` is not expanded**, so a block-based drawing imports as nothing plus
  a diagnostic.
- **A non-default extrusion (code 210) is refused for CIRCLE and ARC**, not
  applied. LINE is unaffected because DXF stores it in world coordinates.
  Refusing is recoverable; importing something mirrored and reporting success
  is not.
- **Z is ignored** — entities are read as their X and Y.
- **Binary DXF is untested.** libdxfrw reads it; no fixture exercises it.
- **`$INSUNITS` 11, 12, 17–20** are deliberately unmapped (ADR-M6-011).
- **Fresh-process load is proven but not in CI.** A reviewer's three-process
  probe passed and was mutation-verified; it lives in scratch, so nothing runs
  it automatically.

---

## M6 status and M7 readiness

**M6 is functionally complete and not yet certifiable.** Two items stand between
that and complete, and neither is closed by more of my own testing:

1. **The second round's fixes are unreviewed.** M5 needed four rounds and M6 has
   now needed two, every one of them finding defects the previous round's fixes
   introduced. M5 needed four rounds, each finding
   defects the previous round's fixes had introduced. Assuming this round broke
   that pattern is the assumption that failed four times.
2. **Owner manual UI validation of the import workflow** has not been performed.
   It is the owner's to do (ADR-M4-016) and must never be described as an
   independent agent review.

**M7 readiness** (dimension/constraint reconstruction from DXF): the dependency
M7 needs — DXF geometry arriving as stable, semantic, constrainable Sketch
entities — is in place and exercised end to end. But M7 reconstructs constraints
from DXF **dimensions**, and M6 currently refuses the block contents that carry
them (ADR-M6-009). Reading a DIMENSION deliberately, rather than importing its
annotation lines by accident, is the first thing M7 has to design.
