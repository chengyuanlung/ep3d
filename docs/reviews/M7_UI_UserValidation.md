# M7 UI User-Assisted Validation — Reconstruction Workflow

**This is OWNER MANUAL VALIDATION.** It is not an independent agent review and
must never be described as one (ADR-M4-016).

**Status: NOT EXECUTED.** Every row is blank. Nothing here may be reported PASS
until it has actually been run (spec 31).

---

## What is already checked, so you do not need to

Automated, **668/668 in Debug and Release**: every supported dimension kind
reconstructs against hand-computed oracles, Gates A–L pass, nine mutations were
verified, and a real child process edits a reconstructed document with the DXF
deleted.

**What automation cannot see.** Whether the reconstructed dimensions are
*findable*. Whether "From source 2 / Inferred 9" means anything to a person.
Whether a skipped dimension is noticeable rather than merely present. Whether
editing Width feels direct. And anything about display scaling.

**The precedent worth knowing before you start.** M6's UI validation found, on
its first attempt, a property panel showing ten labels and no values — invisible
to all 561 tests then passing, because every one of them asked the model and
none asked the widget. M7 added three rows to that same panel and immediately
re-triggered the guard that defect produced. Assume this document can still find
things.

---

## How to run

From `D:\Program2\EP3D\ParametricCAD_Starter`:

```
build\Debug\ParametricCADViewer.exe
```

Then **File → Import DXF...** (`Ctrl+I`). Reconstruction runs automatically as
part of the import — there is no separate command to find.

Files to use, all under `tests\fixtures\dxf\`:

| File | What it is |
|---|---|
| `dimensioned_rectangle.dxf` | 100 × 50 with two linear dimensions. **Drawn 99.5 × 49.7 and skewed** — if you see 100 × 50, the solver did that. |
| `dimensioned_circle.dxf` | two circles, one radial and one diametric dimension |
| `mixed.dxf` | geometry with **no** dimensions at all |
| `closed_rectangle.dxf` | a clean rectangle, no dimensions |

Add `--dark` for the dark-palette pass.

> **Keyboard note.** This machine's IME defaults to Chinese, and while it is in
> that mode keystrokes go to the composition buffer instead of the application —
> so typing a new Width silently does nothing. Press **Win+Space** (or
> Alt+Shift) to get to English before Test B.

---

## Test A — the reconstructed rectangle  `dimensioned_rectangle.dxf`

| # | Step / expectation | Result |
|---|---|---|
| A1 | Import succeeds; the status bar mentions dimensions being reconstructed | |
| A2 | The model tree shows a sketch named **`dimensioned_rectangle`** | |
| A3 | **11 constraint rows** appear under it | |
| A4 | Select the sketch. Every label has a **value beside it** — this is M6.14's own test, re-run because M7 added rows | |
| A5 | Panel shows **Solve status = Solved** and **Degrees of freedom = 0** | |
| A6 | Panel shows **Reconstruction / From source = 2** and **Inferred = 9** | |
| A7 | Are those two numbers *understandable*? Does it read as "the drawing gave me 2 and I worked out 9"? | |
| A8 | Panel shows **Reconstruction / Skipped = 0** | |
| A9 | Parameters **Width = 100** and **Height = 50** exist and are visible | |
| A10 | The 3D solid is a shaded 2:1 box; volume reads **100000 mm³** | |

A9 is the one that matters most: the file draws 99.5 × 49.7. Seeing 100 and 50
means reconstruction read the dimensions and the solver applied them.

---

## Test B — editing a reconstructed dimension *(the central M7 proof)*

| # | Step / expectation | Result |
|---|---|---|
| B1 | Change **Width 100 → 120**, press Enter | |
| B2 | Was the field easy to find? Did it feel like editing a normal parameter, not an imported artefact? | |
| B3 | No modal dialog appeared | |
| B4 | The solid **visibly widens**, promptly | |
| B5 | Volume becomes **120000 mm³** | |

> **B4/B5 were unrunnable when this checklist was written**, and that was a
> defect in the checklist. The viewer never extruded the imported sketch, so
> editing a reconstructed Width re-solved the sketch correctly while the 3D view
> and the volume readout went on describing an unrelated demo box. Fixed after
> independent review; verified in the running application, where the edit now
> gives **Volume 120000.0 mm³, Mass 0.3240 kg, COM (60.00, 25.00, 10.00)** — the
> centre of mass moving from x=50 to x=60 is what proves it is the imported box
> and not the demo one.
| B6 | Status stays **Solved**, DOF stays **0** | |
| B7 | The viewer never shows the old shape while claiming it is current | |
| B8 | Change **Height 50 → 80**. Volume becomes **192000 mm³** | |

---

## Test C — a circle, and the two ways to dimension one  `dimensioned_circle.dxf`

| # | Step / expectation | Result |
|---|---|---|
| C1 | Import succeeds; two circles appear | |
| C2 | Parameters **Radius = 10** and **Diameter = 30** both exist | |
| C3 | Is it clear that one is a radius and the other a diameter? Would you be misled into thinking they are the same kind of number? | |
| C4 | Change **Radius 10 → 20**. The circle visibly doubles in size | |
| C5 | Change **Diameter 30 → 60**. The *other* circle doubles — and it is the one you expected | |

C5 is the check that Diameter halves into the radius correctly. If editing
Diameter to 60 produced a radius-60 circle, that is a Critical.

---

## Test D — a drawing with no dimensions  `mixed.dxf`

| # | Step / expectation | Result |
|---|---|---|
| D1 | Import succeeds | |
| D2 | Panel shows **From source = 0** and **Inferred** greater than 0 | |
| D3 | Nothing suggests dimensions were found when none existed | |
| D4 | The sketch is **not** reported as failed — an under-constrained import is a legal state | |
| D5 | Does the panel make clear that EP3D inferred *everything* here? | |

---

## Test E — skipped and ambiguous reconstruction

There is no committed fixture for this yet (see the completion report's deferred
work), so make one: copy `dimensioned_rectangle.dxf`, and in the copy duplicate
the first `LINE` block so two identical lines sit on top of each other. The
width dimension then matches both.

| # | Step / expectation | Result |
|---|---|---|
| E1 | Import the edited copy. It succeeds — one bad dimension does not fail the import | |
| E2 | The status bar says something was **not reconstructed** | |
| E3 | Would you have NOTICED that message if you were not looking for it? | |
| E4 | Panel shows **Skipped = 1** and a **Skipped item** row naming the reason | |
| E5 | Is the reason understandable without reading the source? | |
| E6 | **No Width parameter was invented anyway** — nothing guessed | |

E3 and E6 are the point. A skip the user does not notice is a skip they will
assume did not happen; a guessed constraint is worse than no constraint.

---

## Test F — conflict and recovery

| # | Step / expectation | Result |
|---|---|---|
| F1 | Run `build\Debug\ParametricCADViewer.exe --sample m5-conflict` | |
| F2 | The sketch row is marked failed with a `!`, not by colour alone | |
| F3 | The panel names **offending constraint IDs** | |
| F4 | Could you tell *which* constraint to remove from what is shown? | |
| F5 | The Pad reads **Blocked**, distinguishable from a Pad that failed on its own | |
| F6 | Nothing renders a stale solid as current | |

---

## Test G — selection, view, theme

| # | Step / expectation | Result |
|---|---|---|
| G1 | Clicking a reconstructed constraint in the tree selects it; the panel follows | |
| G2 | Clicking the solid in the 3D view selects the object under the cursor | |
| G3 | Fit All frames the imported model | |
| G4 | Re-run with `--dark`: text readable, markers distinguishable | |
| G5 | In dark mode the Reconstruction rows still show **values**, not just labels | |

---

## Test H — display scaling *(skipped in M4–M6; run only if you want to)*

| # | Step / expectation | Result |
|---|---|---|
| H1 | At **200%** scaling, is the whole shell usable? | |
| H2 | At **200%**, does the property panel still show values beside labels? | |

H2 is the one worth a moment even if you skip H1. The M6.14 defect was a WIDTH
defect, M7 added three rows to the same panel, and the automated guard measured
it at exactly 296 px against a 296 px viewport — it fits with nothing to spare.
Scaling changes widths.

**The one Critical this project's owner has ever found was a display-scaling
defect.**

---

## Your findings

Minimum per problem: **which file, which step number, what you saw, what you
expected.**

| # | File | Step | What happened | Severity |
|---|---|---|---|---|
| | | | | |

---

## Result

- [ ] **PASS** — M7 reconstruction UI is acceptable
- [ ] **PASS WITH FINDINGS** — acceptable, findings listed above
- [ ] **FAIL** — findings must be fixed first

**Validated by:** *(the project owner, manually, in the running application)*
**Date:** *(not yet run)*
**Display configuration used:** *(record it — by ADR-M4-015, a UI verified on one
display configuration has been verified on one display configuration, and both
M5's and M6's results are weaker than they look because this was left blank)*

This is **owner manual validation**. It is not an independent agent review and
must never be described as one (ADR-M4-016).
