# M7 Completion Report — DXF Dimension and Constraint Reconstruction

> **STATUS: REQUEST CHANGES — round 1 complete, round 2 required.**
>
> Independent review round 1 ran three partitioned reviewers per spec 33. All
> three returned `REQUEST CHANGES` at **71/100**, and two reached the same
> Critical from opposite directions.
>
> Of **30 guards** the reviewers deliberately removed, **11 were caught by no
> test.** My own nine mutations touched none of those eleven — Reviewer 3
> re-ran all nine and confirmed 9/9, so the table was honest, it was simply
> aimed where I was already looking.
>
> **Two claims in the self-validation report were false**: "668/668 in Debug and
> Release" was true under `ctest` only (665/668 in a single-process run), and
> mutation 8 was credited with guarding Gate J against a skipping child, which
> it does not. Both corrected in place.
>
> **All four Criticals and all fourteen Majors are now fixed and
> mutation-verified**, in five commits. What remains before M7 can close:
>
> - **Round 2 review: NOT EXECUTED.** This project has never had a review round
>   that did not find defects introduced by the previous round's fixes. Round 1
>   changed a great deal.
> - **Owner manual UI validation: NOT EXECUTED.** `M7_UI_UserValidation.md`,
>   every row blank. Its Test B is now actually runnable — see below.
> - Two items inherited from M6: `M6.11`–`M6.14` were never reviewed, and M6's
>   own owner UI validation was never run.
>
> M7 is **not declared complete here.**

**Mission:** an imported 2D drawing stops being stable geometry and becomes a
native parametric model.

**Baseline:** `144867b` — the accepted M6 master state.
**Branch:** `m7-wip`, not pushed, not merged.

---

## Implementation summary

| Slice | Commit | What it delivered |
|---|---|---|
| M7.1 | `ac750b7` | reconstruction boundary, `ReconstructionPlan`, explicit linear dimension → Parameter + `Length`, deterministic naming, the release chain |
| M7.2 | `0a59246` | general Horizontal / Vertical / Coincident over arbitrary geometry |
| M7.3 | `7e01cbf` | Radius and Diameter |
| M7.4 | `3da2f8b` | provenance report and ambiguity diagnostics |
| M7.5/6 | `0ad9957` | adversarial matrix, transaction cases, Gate H |
| M7.7 | `e7cbc6a` | persistence, source independence, a real fresh process |
| M7.8 | `ad078e9` | the workflow in the running application |
| M7.9 | `d4a5bd3` | Gate K completion, self-validation report |
| M7.10 | `264eba3` | owner checklist, completion report |
| review | `e6821ab` | three reviewers, `REQUEST CHANGES`, two false claims corrected |
| fix 1 | `1f9448c` | C1, C3, C4, M1, M7, M12 + five previously untested rules |
| fix 2 | `d20050a` | C2 save-cap, and five WholeSuite ctest entries |
| fix 3 | `05d64e5` | UI evidence now discriminates (M2, M3, M4) |
| fix 4 | `531f608` | Gate J proof-of-work, counter-based Gate K, Gate H stale state |
| fix 5 | *this* | the viewer extrudes the imported sketch; doc corrections |

**The architecture in one line:** DXF → M6 parser → format-neutral geometry
*and annotation* → native Sketch → M7 analysis → `ReconstructionPlan` →
PartDocument facade → Parameter + SketchConstraint → solver → Pad.

Reconstruction never writes geometry and never touches the solver. It creates
ordinary document objects through documented APIs, and after saving, the result
does not depend on the DXF file, the parser, or the reconstruction code having
ever run.

---

## The release proof

```
dimensioned_rectangle.dxf   (drawn 99.5 x 49.7, skewed, corners open)
  → M6 import
  → M7 reconstruct Width=100, Height=50
  → Solved, DOF 0, 100000 mm³
  → save
  → DELETE the DXF
  → load in a SECOND PROCESS
  → Width 100 → 120
  → 120000 mm³
```

Every number hand-computed. The fixture is drawn wrong on purpose: at 100 × 50
an implementation that reconstructed nothing would pass every assertion.

---

## Test totals

| | M6 baseline | M7 |
|---|---|---|
| Total | 561 | **668** |
| `ParametricCADCoreTests` | 302 | 380 |
| `ParametricCADIntegrationTests` | 87 | 106 |
| `ParametricCADImportTests` | 59 | 68 |
| `ParametricCADSolverTests` | 53 | 53 |
| `ParametricCADKernelOcctTests` | 47 | 47 |
| Viewer smoke (ctest) | 13 | 13 |

**690/690 in Debug and Release** after round 1's fixes, plus **395/395 in a
single-process run** of the Core binary — the number the first report got wrong.
Five new `WholeSuite_*` ctest entries run each binary once, unfiltered, so
single-process state leakage can never again be invisible.

---

## Gates A–L

All **PASS**. Evidence per gate is tabulated in `M7_SelfValidationReport.md`;
the load-bearing ones:

- **A/B/C/D** — the rectangle, measured on native geometry rather than metadata,
  100000 → 120000 → 192000 mm³.
- **E** — doubling a radius gives **exactly 4×**, asserted as a ratio at 1e-9.
- **F** — every recogniser individually disabled, each losing DOF 0; plus the
  proof that geometry is straightened by the SOLVER, not snapped by the reader.
- **H** — conflict verified by **measurement**, not by status: a status check
  alone would pass even if corrupt geometry had been committed.
- **J** — a genuine child process, with the DXF deleted first.

---

## Architecture and dependency audit

| | |
|---|---|
| New third-party dependencies | **none** — M7 is built on M6's parser output and M5's constraint engine, which spec 29 says to prefer |
| libdxfrw symbols in `ParametricCADCore.lib` | **0** (`dumpbin /symbols`) |
| DXF / Qt / OCCT / Eigen types in `src/Core` | **0** (the only `DRW_` hits are comments explaining the rule) |
| Reconstruction's route into the document | `addParameter` + `addSketchConstraint` only |

> **The licence position is unchanged and unsolved.** libdxfrw is
> **GPL-2.0-only** and the shipped viewer links it. If EP3D is to ship
> closed-source, this dependency must be replaced first. M7 did not make this
> better or worse; it added ~90 lines to the same confined translation unit.

---

## ADRs

Nine were required by spec 30; **seventeen** M7 ADRs exist
(`ADR-M7-001`…`ADR-M7-017`). The extra eight were forced by implementation, and
three are worth naming here:

- **ADR-M7-010** resolves a conflict *inside spec 37 itself*: it says M7.1 is
  dimensions-only with H/V/Coincident deferred, but its own required gate
  demands DOF 0, which two Length constraints cannot reach. Superseded by
  ADR-M7-013 once M7.2 generalised recognition.
- **ADR-M7-009 / ADR-M7-014** record a deliberate asymmetry: a LINE drawn 30%
  off its dimension reconstructs; a CIRCLE drawn 30% off is refused. A line's
  association evidence is its definition points, independent of length; a
  curve's is its centre, and size is all that separates it from its neighbours.
- **ADR-M7-011** answers spec 11's direct question: geometry is never snapped.

---

## Review rounds

**Zero.** This is the entire gap between "functionally complete" and "complete".

For calibration, from this project's own history: M5 needed **four** independent
review rounds and M6 needed **three**, and *every single round found defects the
previous round's fixes had introduced*. In four of them the implementer's claim
that "every fix is mutation-verified" was false.

M7 has had none. The self-validation report is written as claims rather than
facts for that reason, and carries a "what I am least confident about" section
naming the naming policy as the thinnest-tested part.

---

## Owner UI validation

**NOT EXECUTED.** `M7_UI_UserValidation.md` is written and waiting: 8 tests,
every row blank, with the fixtures and hand-computed expectations filled in.

Its Test C5 is the one I would not skip — editing **Diameter** to 60 must give a
radius-**30** circle. If it gives radius 60, that is a Critical.

---

## Known limitations

- Provenance is not persisted (ADR-M7-017) — deliberate, documented, pinned by a
  test asserting its absence from the saved file.
- Angular dimensions are not reconstructed.
- **Point-to-point `Distance` is not reconstructed**, though spec 3 lists it as
  required. A dimension spanning two different entities is skipped with a
  diagnostic. Deferred; named here because independent review found the gap was
  invisible rather than merely open.
- Equal / Parallel / Perpendicular / Tangent / Concentric are not reconstructed
  (optional in spec 3, not promoted by ADR).
- One Fix per sketch: a drawing of several disconnected shapes has one anchored
  and the rest free.
- `INSERT` is still not expanded (inherited from M6), so a block-based drawing
  still imports as nothing — including its dimensions.
- A curve dimension materially disagreeing with its curve is refused where the
  line equivalent is accepted.

---

## Deferred work

- **Fixtures D and E of spec 22 exist only as in-memory tests**, not as DXF
  files. The ambiguity and conflict paths are therefore not exercised through
  the real parser. The owner checklist's Test E asks the owner to hand-build one.
- **Performance was neither measured nor claimed.** Endpoint clustering is
  O(n²) in endpoints, which is fine for the drawings tested and unmeasured
  beyond them.
- **Binary DXF with dimensions** is untested; no fixture.

---

## M8 readiness

M7 leaves the project with imported native 2D geometry carrying stable
identities, reconstructed dimensional Parameters and geometric constraints,
editable parametric sketches, a verified 2D-dimension → 3D-rebuild path, and
save/load independence from the source DXF — which is spec 40's list.

M7 deliberately pre-decides nothing about M8.

---

## What has to happen before M7 can close

1. **Independent review** — three partitioned reviewers per spec 33, treating
   the self-validation report as claims. Reviewers should deliberately hunt for
   defects introduced by M7's own later slices, since M7.2 changed four M7.1
   tests and M7.3 changed one more.
2. **Owner manual UI validation** — `M7_UI_UserValidation.md`.
3. The two inherited M6 items, which merging M6 did not close.
