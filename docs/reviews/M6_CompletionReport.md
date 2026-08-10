# M6 Completion Report — DXF Import to Stable Sketch Entities

> **STATUS: FUNCTIONALLY COMPLETE — two verification items open.**
>
> All required DXF entities import, Gates A–I pass, and one independent review
> round found 1 Critical and 7 Major which are fixed and mutation-verified.
> **Those fixes have not themselves been reviewed**, and **owner manual UI
> validation of the import workflow has not been performed.** M6 is not
> declared complete here.

**Baseline:** `a6e7078` — the accepted M5 master state.

**Branch:** `m6-wip`. Head at the time of writing: `881f0bc` (M6.9 review
fixes), plus the documentation commit that records this file.

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
| Total | 498 | **546** |
| `ParametricCADCoreTests` | 301 | 301 |
| `ParametricCADSolverTests` | 53 | 53 |
| `ParametricCADKernelOcctTests` | 47 | 47 |
| `ParametricCADIntegrationTests` | 87 | 87 |
| **`ParametricCADImportTests`** | — | **45** |
| Viewer smoke tests (ctest) | 10 | 13 |

**546 / 546 in Debug and Release**, with the Release run verified to invoke
Release binaries — independently reproduced by a reviewer.

---

## Gates A–I

| Gate | Result |
|---|---|
| **A** LINE import against a hand-computed oracle | **PASS** |
| **B** CIRCLE centre and radius | **PASS** |
| **C** ARC measured geometrically — centre, radius, start/end direction, orientation | **PASS** |
| **D** Mixed file: count, kinds, unique ids, no dependence on array position | **PASS** |
| **E** Save/load identity — ids, references, geometry, kinds | **PASS** |
| **F** Source independence — the saved document names no `.dxf` at all | **PASS** |
| **G** Imported closed profile drives a real solid: 48000 mm³, 0.1296 kg, hand-computed | **PASS** |
| **H** Invalid/unsupported input: documented result, useful diagnostics, no corrupt partial document | **PASS** |
| **I** Regression in Debug and Release, Release proven | **PASS** |

Every expected number is computed by hand from a fixture small enough to read.
No expectation was produced by running the importer.

---

## ADRs

Seven were required by spec 21; **thirteen** exist. Six more had to be written
once review found what the first seven had got wrong.

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

---

## Independent review

One round, two reviewers, recorded in full in `M6_IndependentReview.md`.

**1 Critical, 7 Major, 2 Minor/Info** — all fixed with mutation-verified
regression tests, except two accepted and recorded as limitations.

Three claims in my own self-validation report were **falsified by execution**,
including the one it leaned on hardest ("unguarded mutations: none"). All are
corrected in place and marked, nothing deleted.

The durable finding: **a mutation suite written against my own code measures my
imagination, not my coverage.** Both genuinely unguarded fixes were found by
someone removing a line I had not thought to remove.

**No score is claimed.** Spec 23 defines no scorecard for M6, and a number I
awarded myself after my own report was shown to contain three false claims would
be worth nothing.

---

## Known limitations

- **Supported entities: LINE, CIRCLE, ARC.** Everything else is reported by kind
  and never reinterpreted.
- **`INSERT` is not expanded**, so a block-based drawing imports as nothing plus
  a diagnostic.
- **A non-default extrusion (code 210) is refused**, not applied. Refusing is
  recoverable; importing something mirrored and reporting success is not.
- **Z is ignored** — entities are read as their X and Y.
- **A truncated LINE is silently misinterpreted** as a line to the origin.
  libdxfrw exposes no group-code-presence flag, so detecting it needs a
  lower-level parse. It opens or branches the loop and the Pad then fails
  loudly, so the effect is a phantom entity plus a misleading success message
  rather than a wrong solid.
- **Binary DXF is untested.** libdxfrw reads it; no fixture exercises it.
- **`$INSUNITS` 11, 12, 17–20** are deliberately unmapped (ADR-M6-011).
- **Fresh-process load is proven but not in CI.** A reviewer's three-process
  probe passed and was mutation-verified; it lives in scratch, so nothing runs
  it automatically.

---

## M6 status and M7 readiness

**M6 is functionally complete and not yet certifiable.** Two items stand between
that and complete, and neither is closed by more of my own testing:

1. **The M6.9 review fixes are unreviewed.** M5 needed four rounds, each finding
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
