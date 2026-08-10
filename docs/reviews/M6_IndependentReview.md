# M6 Independent Review — findings and responses

Three rounds. Round 3 is recorded at the end of this file; rounds 1 and 2
follow immediately below, in the order they happened.

Two independent reviewer agents reviewed M6 in parallel, partitioned by the
spec 23 areas. Neither wrote the code. Both were told that M5 needed **four**
review rounds, that each round found defects created by the previous round's
fixes, and to assume M6 held an equivalent defect until they had evidence
otherwise.

| Reviewer | Areas (spec 23) | Verdict |
|---|---|---|
| 1 | DXF semantics and units, ARC semantics, tests that duplicate production formulas | **Not safe to ship** — 1 Critical, 6 Major |
| 2 | Identity/persistence/fresh-load, rollback and malformed input, architecture and dependency boundary, downstream Pad | **Safe except one Major** — the same ARC defect, found independently |

Every finding below was reproduced by the reviewer running code.

---

## Critical

### C1 — Block-definition geometry was imported as model geometry

libdxfrw dispatches a BLOCK's contents through the same `addLine`/`addCircle`/
`addArc` callbacks it uses for model space, so the collector could not tell them
apart. A file whose ENTITIES section held only an `INSERT` imported the block's
geometry **at definition coordinates** — losing placement, scale, rotation and
multiplicity — while reporting the `INSERT` itself as "skipped". A block
inserted five times appeared once, in the wrong place.

**The realistic case is worse.** Every DXF carrying a DIMENSION also carries an
anonymous `*D1…` block holding its dimension and extension lines. A drawing with
one real line and one dimension imported **four lines**. Spec 4 states that
dimensions and annotations must not silently become model content.

**Fixed** — ADR-M6-009, `M6_REV_001`.

**And the self-validation report's claim about this was false in both halves.**
It said "Blocks (INSERT) are skipped, not expanded. A drawing built from blocks
imports as nothing plus a diagnostic." Neither clause was true.

---

## Major — all fixed

| # | Finding | Fix |
|---|---|---|
| M1 | **Units could be applied zero times while the result reported they were applied.** libdxfrw dispatches sections in file order, so a file with ENTITIES before HEADER was scaled by the 1.0 default and only then learned the unit — 25.4× wrong, reported as `unit = inches, unitWasDefaulted = false`. The one signal ADR-M6-002 relies on to make that visible was asserting the opposite. A two-HEADER file split one sketch across two scales. | Collect raw, scale once after the read — ADR-M6-010, `M6_REV_002` |
| M2 | **`$INSUNITS` 3 and 7–20 were all unmapped**, taking the millimetre default with a message saying the file had not stated a usable unit — which it had. Mils made PCB geometry 39.4× too large; microns 1000×; kilometres 10⁶× too small. | Map every real length unit — ADR-M6-011, `M6_REV_007` |
| M3 | **The ARC unit conversion was entirely unguarded.** Removing `* scale()` from all three arc values left **526/526 green**, because no fixture had an arc in a non-millimetre file. | `M6_REV_003` |
| M4 | **The arc sweep direction rested on one boolean assertion** that restated the ADR rather than measuring a curve. Flipping `counterClockwise` turns a 170° arc into 190° and a 350°→40° arc into 310° — wrong by up to 260° — and exactly one assertion failed. No M6 test drove an imported ARC through the profile/Pad path at all. | Measured through geometry: a 90° sector's padded volume differs 3× from the 270° one — `M6_REV_004` |
| M5 | **A 0°→360° or zero-sweep ARC aborted the entire file import.** Legal DXF that several exporters emit instead of a CIRCLE; a drawing with 500 good lines and one such arc imported as nothing. Every other degenerate entity is skipped and reported — the sweep was the one hole in that rule. *Found independently by both reviewers.* | Reader applies the model's own sweep test — ADR-M6-012, `M6_REV_005` |
| M6 | **A non-default extrusion silently mirrored geometry.** ADR-M6-008 called it "wrong orientation". A reviewer measured it: for `210 = (0,0,-1)` the correct result is a **mirror**, and a mirrored part has identical area, volume, mass and centre of mass — **so no analytical oracle of the kind every gate in this project uses could ever detect it.** Worse, LINE is stored in world coordinates while CIRCLE/ARC are stored in the entity's own, so ignoring 210 mixed two frames in one file: a hole imported at (−25, 30) instead of (25, 30) with `IMPORT OK`. | Refused and reported — ADR-M6-013, `M6_REV_006` |

## Minor and Info — fixed

- **`addSketch` checked `registerObject` only in a Debug `assert`.** In Release a
  collision would leave a sketch in `sketches_` and the graph but invisible to
  the recompute engine, and `removeObject` would then return false and leave it
  behind. `restoreSketch` was hardened after M5; `addSketch` was the path left.
- **Gate E compared only the entity KIND across save/load**, not the geometry
  the gate is named for. It now compares a full geometry fingerprint, shared
  with the order-independence test so "same geometry" has one definition.

## Accepted, not fixed

- **[NO LONGER ACCEPTED — FIXED after round 3.]** *A truncated LINE is silently
  misinterpreted.* The original text read: "Detecting it needs a
  group-code-presence hook the library does not offer." That was true when
  written and stopped being true when ADR-M6-015 made the reader read the file's
  group codes for itself to find unterminated blocks. **Nothing re-examined the
  limitation when the tools changed** — it was still listed as permanent two
  rounds later. Fixed by `M6_R3_008` and `M6_R3_009`.

  The lesson is not about DXF: **"cannot be fixed" is a claim with a date on
  it.** This list is the place where such claims go to be forgotten, so every
  entry on it needs re-reading whenever the surrounding capability changes.
- **The profile-connectivity tolerance band** (gaps in [1e-7, 1e-6] mm are
  accepted by `BuildProfile` and rejected by OCCT) is pre-existing M4 behaviour
  that M6 newly exposes to externally authored coordinates. It fails loudly and
  clears mass currency; only the diagnostic is the kernel's rather than the
  semantic one.

---

## What the reviewers verified as genuinely correct

Reported because each was measured, not read:

- **ARC angle semantics are right end to end.** Reviewer 1 built two closed
  profiles whose areas depend on the sweep and compared against hand-computed
  oracles: a rectangle capped by a 180° arc → expected 3813.716694, got
  **3813.716694**; a 350°→40° sector → expected 43.633231, got **43.633231**.
  Negative angles, angles beyond 360°, and 359.999° all import correctly.
- **libdxfrw really does deliver radians** — traced to `DRW_Arc::parseCode`
  dividing codes 50/51 by `ARAD`.
- **Fresh-process load works.** Reviewer 2 wrote the probe the self-validation
  admitted was missing: three separate processes, imported ids deliberately the
  largest in the file. Both sketches resolve as `Sketch*`, every entity id
  resolves, the mass node does not collide, the Pad rebuilds to 48000 mm³, and
  geometry compares bit-exactly across processes. **Mutation-verified.**
- **Rollback is clean.** Nine failure positions × two document states, comparing
  registry size, every graph node with its state and edges, sketches, bodies,
  feature states, parameters, mass properties **and the serialized bytes**.
  Every case byte-identical.
- **The architecture boundary holds.** `ParametricCADCore.lib`,
  `ParametricCADViewerCore.lib`, `ParametricCADSolver.lib` and
  `ParametricCADKernelOcct.lib` all carry **0** libdxfrw symbols;
  `ParametricCADImportDxf.lib` carries a non-zero count (filter-dependent;
  reported as 205/109 and independently measured as 206-209 / 104-107 -- the
  number was never the point, the zeros are). The licence
  claims in ADR-M6-001 and `CMakeLists.txt` match the installed vcpkg port.
- **Downstream Pad handling is sound**: an unclosed imported profile gives
  `OpenLoop` and a loud Pad failure; duplicate and reversed-duplicate sides give
  `DuplicateEntity`; a beyond-tolerance gap is not healed; breaking a valid
  imported profile under a live Pad correctly drops mass currency.
- The mutation table in the self-validation report reproduces exactly as stated.

---

## What this round says about the self-validation report

Three claims were false, and the pattern is the same one M5 produced four times:

1. **"Unguarded mutations: none."** Six mutations I chose proved six things
   about code I had chosen to test. Two fixes had no coverage at all.
2. **"Blocks are skipped, not expanded."** Asserted, never exercised.
3. **The ARC-near-360° NOT EXECUTED row's justification.** It claimed
   `arc_crossing_zero.dxf` covered the risk. A Major lived exactly there — and
   the row was the one place the document told the reader where not to look.

Reviewer 2's verdict on the report's honesty was "yes, with one exception", that
exception being item 3. Everything measurable it checked — symbol counts,
dependents, test totals, the Release-binary proof, the rollback mutation killing
five tests — was accurate.

The durable lesson is narrow and worth stating: **a mutation suite I write
against my own code measures my imagination, not my coverage.** Both genuinely
unguarded fixes were found by someone removing a line I had not thought to
remove.

---

## Status

- 1 Critical and **6** Major fixed, each with a regression test.
- **[CORRECTED] "six mutations run against those fixes, none unguarded" was
  false.** A re-review found two unguarded halves — `endBlock()` teardown and
  the sweep guard's upper clause — each of which left all 45 import tests green
  while restoring a defect. Covered now by `M6_RR_002` and `M6_RR_003`.
- **[CORRECTED] The completion report and the M6.9 commit message both said
  "7 Major".** This document enumerates six (M1–M6). Six is right; the
  over-count appeared in three places from one miscount.
- 2 Minor/Info fixed; 2 accepted and recorded as limitations.
- **The fixes in this round have not themselves been reviewed.** On this
  project's record that is a real gap, not a formality.
- **Owner manual UI validation of the import workflow: NOT EXECUTED**, and the
  owner's to perform (ADR-M4-016).

---

# Round 3 — reviewing the second round's fixes

Two reviewers again, partitioned. Both were told the project's record and asked
to assume `029c2f4` held a defect until they had evidence otherwise. Both found
one, and they converged.

## The finding that matters most

> **Every unguarded line was the third leg of a LINE / CIRCLE / ARC triple where
> only one or two legs got a fixture.**

Six guards could be deleted with all fifty import tests still passing. The
author's round-2 mutation run had mutated the LINE leg and the shared code and
concluded "no unguarded fix" — for the fourth time on this project, falsely.

| Deleted line | Tests still green? | Now caught by |
|---|---|---|
| ARC non-default-extrusion guard | yes, 50/50 | `M6_R3_001` |
| CIRCLE post-scale finiteness re-check | yes, 50/50 | `M6_R3_002` |
| ARC post-scale finiteness re-check | yes, 50/50 | `M6_R3_002` |
| ARC angle terms in `AllFinite` | yes, 50/50 | `M6_R3_003` |
| ARC block guard | yes, 50/50 | `M6_R3_004` |
| block attribution in `noteUnsupported` | yes, 50/50 | `M6_R3_005` |
| LINE raw finiteness | yes, 56/56 | `M6_R3_007` |
| CIRCLE raw finiteness | yes, 56/56 | `M6_R3_007` |

Two of these are worth naming individually because the ADRs say no oracle can
catch what they prevent:

- **ARC extrusion.** ADR-M6-013 states that for `210 = (0,0,-1)` the correct
  result is a mirror, and a mirrored part has identical area, volume, mass and
  centre of mass — so the guard is the entire defence. `ocs_extrusion.dxf`
  carried a LINE and a CIRCLE and no ARC. A reviewer measured the mutant
  importing a fillet arc at (25,30) whose true position is (−25,30), with
  `IMPORT OK`.
- **ARC inside a block.** ADR-M6-009 was written because every DIMENSION carries
  an anonymous `*D1` block. An **angular** dimension's annotation geometry is an
  arc — the entity kind most likely to appear in the block that motivated the
  ADR was the one kind whose guard nothing tested.

## Live defect — an unterminated BLOCKS section discards the drawing

A `BLOCK` with no `ENDBLK` makes libdxfrw read the entire rest of the file as
block content. Two real model lines vanished, `read()` returned **true** with no
parse error, and the user was told *"every entity in the file was skipped;
nothing to import"* — aiming them at their geometry when the fault is one
missing group code in a section they never open.

The obvious fix does not work, and that is worth recording: `endBlock()` is
called at EOF, so "are we still inside a block when the read finishes?" is
**always false**. I wrote that check first and measured it doing nothing.
Detecting it requires reading the file's section structure outside the parser —
ADR-M6-015, `M6_R3_006`.

## What the reviewers verified as genuinely correct

- **The round-2 Critical fix is sound across its whole domain.** Negative
  angles, angles beyond 2π, ±1e308°, denormals, exactly-2π, and the
  `fmod`-returns-tiny-negative-then-rounds-to-2π case were each constructed and
  measured. The reader's rejection band matches the model's **bit for bit**:
  `2.0 * kPi` in the reader and `kTwoPi` in `SketchTypes` are the same double,
  `0x401921fb54442d18`. ADR-M6-012's claim that "the reader applies the same
  sweep test the model uses" is, this time, true.
- **ARC semantics measured through solid volume** against four hand oracles,
  including a 240° sector declared in inches — which confirms centre and radius
  are scaled and angles are not, without asserting on a struct field.
- **Units:** all of `$INSUNITS` 0–25, −1, 999, `INT_MAX`, no HEADER, HEADER
  without `$INSUNITS`, `$INSUNITS` as a real, ENTITIES before HEADER, and three
  two-HEADER variants. `unitWasDefaulted` truthful in all 36 cases.
- **Rollback:** nine failure positions against a populated document, compared
  down to the serialized bytes. Every case byte-identical.
- **Architecture boundary at binary level:** Core and ViewerCore carry
  `dxfrw=0 occt=0 qt=0 eigen=0`; `DxfReader.cpp` is the only translation unit
  including any libdxfrw header.
- **Licence:** the installed vcpkg port is `2025-09-25`, `GPL-2.0-only` — as
  ADR-M6-001 and `CMakeLists.txt` state.

## Documentation still false after a commit that claimed to audit it

- **`$INSUNITS`.** "Values other than 0/1/2/4/5/6 map to `Unrecognized`" —
  falsified by running every value, and contradicted eleven lines below in the
  same list. Found by both reviewers.
- **"All are fixed and mutation-verified."** False; see the table above.
- **"1 Critical and 7 Major"** had a **fourth** site, in the self-validation
  report, after the M6.10 commit said the over-count "appeared in three places".
- **`M6_RR_004` had no ADR and no DecisionLog entry at all.** A reviewer found
  it by grepping the log for "overflow" and getting nothing. Now ADR-M6-014.

## Process failure — mine, not the reviewers'

I launched both reviewers into the **same working tree and build directory**.
Reviewer 2 saw Reviewer 1's mutation of `DxfReader.cpp` appear mid-run and hit
PDB lock failures (`error C1041`) that silently broke two rebuilds — precisely
the stale-binary hazard I had warned them about, created by me. Reviewer 2
recovered by exporting an isolated tree with `git archive` and re-running
everything there. Reviewer 1's results came from the shared tree, so they are
corroborated rather than assumed: both reviewers independently reached the same
verdict on the four guards they both tested. `AGENTS.md` now carries the rule.

## Status after round 3

- 1 Major live defect fixed (ADR-M6-015); 8 unguarded guards now covered by
  `M6_R3_001..007`; 4 documentation claims corrected.
- **One accepted limitation retired.** The scan built for the unterminated-block
  defect made the truncated-LINE defect detectable, which two rounds of notes
  had recorded as impossible with the library's interface. `M6_R3_008/009`.
  Mutating that new code myself found two defects in it before a reviewer did —
  the first time on this project that step caught anything first, and both were
  the same shape as everything else here: a case my own fixture could not reach.
- **560 / 560 in Debug and Release.**
- Twelve mutations re-run under a protocol that fails loudly: **11 CAUGHT, 0
  INCONCLUSIVE, 1 UNGUARDED** — the NaN-safe rewrite of the sweep guard, which
  no single-mutation test can reach because `AllFinite` rejects the only input
  that produces a NaN sweep. It is listed as uncovered rather than counted.
- **Round 3's fixes have not themselves been reviewed.**
- **Owner manual UI validation of the import workflow: still NOT EXECUTED**, and
  the owner's to perform (ADR-M4-016).
