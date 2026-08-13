# M6 UI User-Assisted Validation — DXF Import Workflow

**This is OWNER MANUAL VALIDATION.** It is not an independent agent review and
must never be described as one (ADR-M4-016).

**Status: NOT EXECUTED.** Every row below is blank. Nothing here may be reported
PASS until you have actually run it (M6 spec 22).

---

## What I have already checked, so you do not need to

Automated, **561/561 in Debug and Release**: every supported entity kind imports
against hand-computed oracles, unit conversion (mm / inch / metre / mil),
stable ids through save/load, source independence from the `.dxf` file,
shuffled-file-order equivalence, an imported closed profile driving a 3D solid
to 48000 mm³, and malformed/unsupported input producing diagnostics rather than
silence. Gates A–I all pass.

**What automation cannot see, and why you are needed.** The one defect this
milestone's UI validation has already produced was invisible to all 561 tests:
`propertiesOf()` returned ten fully populated rows while the running
application showed ten labels and an empty column, because one long diagnostic
had pushed the value column out of the dock. Correct data, invisible to the
person it was for. That class of defect — *is the right answer actually on
screen and findable* — is what this document is for.

**One thing to know before you start.** The fix for that defect landed in
`M6.14` and is now guarded by the smoke tests. If you see values in the
Properties panel throughout, that guard is doing its job; if you ever see a
label with no value beside it, stop and record it — it means the guard has a
hole.

---

## How to run

From `D:\Program2\EP3D\ParametricCAD_Starter`:

```
build\Debug\ParametricCADViewer.exe
```

Then use **File → Import DXF...** (`Ctrl+I`) and pick a file from
`tests\fixtures\dxf\`. You can also load one directly at startup:

```
build\Debug\ParametricCADViewer.exe --import tests\fixtures\dxf\mixed.dxf
```

Add `--dark` to any of them for the dark-palette pass.

The fixtures below are small enough to read by eye, and each one's expected
numbers are computed by hand from the file — not by running the importer.

---

## Test A — Mixed file  `tests\fixtures\dxf\mixed.dxf`

Four LINEs forming a closed 40 × 30 rectangle, one CIRCLE (centre 80,80,
radius 7) and one ARC (centre −20,−20, radius 5, 15°→95°), deliberately
interleaved so that an importer which buckets by kind cannot look right by
accident. Units are millimetres.

| # | Step / expectation | Result |
|---|---|---|
| A1 | Import succeeds; status bar reads `Imported mixed.dxf: ...` | |
| A2 | Status message is **readable in full** — not cut off at the edge of the bar | |
| A3 | Model tree gains a sketch row named **`mixed`** — the file's own base name, not "Imported" or "Sketch002" | |
| A4 | Select it. The Properties panel shows **a value beside every label** — this is the M6.14 defect's own test | |
| A5 | Panel reports **6 entities** | |
| A6 | Nothing about the sketch says "imported" as a special state — it looks like an ordinary sketch | |
| A7 | The imported geometry is **visible in the view** and roughly matches the description above (a 40 × 30 box, a small circle up and to the right, a short arc down and to the left) | |
| A8 | Was the imported sketch easy to **find** in the tree after import, without hunting? | |

---

## Test B — Closed profile drives a solid  `tests\fixtures\dxf\closed_rectangle.dxf`

A 60 × 40 rectangle built from four LINEs, deliberately listed out of order and
with two sides reversed. This is Gate G: the profile validator has to find the
loop.

| # | Step / expectation | Result |
|---|---|---|
| B1 | Import succeeds, 4 entities | |
| B2 | The sketch reports a **valid closed profile** — no failure marker | |
| B3 | Pad it (or use the sample that does). Volume reads **48000 mm³** at length 20 | |
| B4 | Mass reads **0.1296 kg** at 2700 kg/m³ | |
| B5 | The solid is **shaded** and visibly a 3:2 box | |
| B6 | Status bar reports mass properties as **current**, not stale | |

Both numbers are hand-computed: 60 × 40 × 20 = 48000 mm³; 48000 mm³ ×
2700 kg/m³ = 0.1296 kg.

---

## Test C — Unit conversion  `tests\fixtures\dxf\line_inches.dxf`

The same coordinates as `line.dxf` — (0,0) → (100,50) — but declared in
**inches**. An inch is 25.4 mm exactly.

| # | Step / expectation | Result |
|---|---|---|
| C1 | Import succeeds, 1 entity | |
| C2 | The line's end point reads **(2540, 1270) mm**, not (100, 50) | |
| C3 | The unit is **visible** somewhere the user can see it — a number without a unit is not an answer | |
| C4 | Import `line.dxf` (millimetres) into the same document. It is **visibly 25.4× smaller**, side by side | |

C4 is the one that matters: if both files draw the same size, the conversion
never ran.

---

## Test D — Unsupported entity is reported, not hidden  `tests\fixtures\dxf\unsupported.dxf`

A LINE, then a TEXT, then another LINE. The unsupported entity sits *between*
two supported ones on purpose.

| # | Step / expectation | Result |
|---|---|---|
| D1 | Import succeeds with **2 entities** — the second line is not lost | |
| D2 | Status bar says something was **skipped**, with a count (`[1 skipped]`) | |
| D3 | Was that message **noticeable**? Would you have seen it if you were not looking for it? | |
| D4 | Nothing presents the import as complete-and-clean when one entity was dropped | |

D3 is a judgement call and it is the point of this test. A skip a user does not
notice is a skip they will assume did not happen.

---

## Test E — Failure diagnostics

| # | File / step | Expectation | Result |
|---|---|---|---|
| E1 | `tests\fixtures\dxf\malformed.dxf` | Status says **malformed DXF**, and names the cause — *not* "file not found", *not* a bare "import failed" | |
| E2 | A path that does not exist | Status says **file not found** — a different message from E1 | |
| E3 | `tests\fixtures\dxf\empty.dxf` | Handled cleanly; no crash, no phantom empty sketch presented as success | |
| E4 | After any failed import | The document is **unchanged** — no half-imported sketch in the tree | |
| E5 | After any failed import | Is the error **understandable to you** without reading the source? | |

---

## Test F — Persistence and source independence

| # | Step / expectation | Result |
|---|---|---|
| F1 | Import `mixed.dxf`, save the document | |
| F2 | **Rename or move the original `.dxf`**, then load the saved document | |
| F3 | The sketch is still there, still 6 entities, still the right geometry | |
| F4 | Nothing in the UI asks for the DXF file or reports it missing | |
| F5 | The imported geometry can still be selected, constrained and deleted like any other | |

---

## Test G — Selection, view, theme

| # | Step / expectation | Result |
|---|---|---|
| G1 | Clicking the imported sketch row selects it; the panel follows | |
| G2 | Clicking the imported solid in the 3D view selects the object **under the cursor** | |
| G3 | Show / Hide works on imported geometry, and "hidden" never hides a failure | |
| G4 | Fit All frames the imported model | |
| G5 | Re-run with `--dark`: text stays readable, state markers still distinguishable | |
| G6 | Property values stay **on screen** in the dark pass too — the M6.14 defect was a layout defect, and layout can differ by theme | |

---

## Test H — Display scaling *(skipped by owner in M5; run only if you want to)*

| # | Step / expectation | Result |
|---|---|---|
| H1 | At **200%** scaling, is the whole shell on screen and usable? | |
| H2 | At **200%** scaling, does the Properties panel still show values beside labels? | |

The known problem is unchanged from M5: the shell's minimum is 1000 × 640
logical pixels, so a 200%-scaled 1280 × 800 desktop (640 × 376 logical) cannot
fit it. H2 is new and worth a moment even if you skip H1 — the defect M6.14
fixed was a width defect, and scaling changes widths.

**The one Critical this project's owner has ever found was a display-scaling
defect.** That is the whole reason this section keeps being offered.

---

## Your findings

Report in whatever form suits you. The minimum I need per problem: **which
file, which step number, what you saw, what you expected.**

| # | File | Step | What happened | Severity |
|---|---|---|---|---|
| | | | | |

---

## Result

- [ ] **PASS** — M6 import UI is acceptable
- [ ] **PASS WITH FINDINGS** — acceptable, findings listed above
- [ ] **FAIL** — findings must be fixed first

**Validated by:** *(the project owner, manually, in the running application)*
**Date:** *(not yet run)*
**Display configuration used:** *(record it — by ADR-M4-015, a UI verified on
one display configuration has been verified on one display configuration, and
M5's result is weaker than it looks because this was left blank)*

**Scope of this result — stated in advance because it limits what it proves:**

- This is **owner manual validation**. It is not an independent agent review and
  must never be described as one (ADR-M4-016).
- It covers the **import workflow only**. The M5 constraint/parameter UI was
  validated separately in `M5_UI_UserValidation.md`.
