# M9 UI User-Assisted Validation — History, Suppression, Rollback, Creation

**This is OWNER MANUAL VALIDATION.** It is not an independent agent review and
must never be described as one (ADR-M4-016). An agent may prepare this document,
run the mechanical checks, and state expected values — an agent may **not** fill
a Result cell.

**Status: NOT EXECUTED.** Every Result cell below is blank. Nothing here may be
reported PASS until you have actually run it.

M9's automated gates A–J pass and its 23-mutation battery is recorded in
`docs/reviews/M9_SelfValidationReport.md`. **M9 has had no independent review of
any kind**, and this checklist does not substitute for one.

---

## What I have already checked, so you do not need to

Automated: **856/856 in Debug AND Release**, plus single-process and
shuffled-seed runs of all five test binaries in both configurations. M9's own
release gates A–J pass with hand-computed oracles, and 23 mutations were
injected and killed.

Four defects in M9 were found by its own gates while it was being written, and
one class recurred twice more in M10: **something during construction or load
being recorded as a user edit**. Every instance was caught by one rule — *a
loaded document starts with an empty history* (ADR-M9-001).

**What automation cannot see, and why you are needed.** The precedent is M6.14:
`propertiesOf()` returned ten fully populated rows while the running application
showed ten labels and an empty column. Correct data, invisible to the person it
was for.

M9's exposure to that class is larger than M8's, because M9 is the first
milestone whose features are almost entirely **verbs the user performs** rather
than values the user reads. Undo, Suppress and Roll Back each have a correct
model answer that says nothing about whether the person driving the application
can tell what just happened. Three specific risks:

1. **Undo that silently does nothing** looks identical to undo that worked, if
   you were not watching the number.
2. **A suppressed feature and a rolled-back feature are the same predicate
   inside** (`isFeatureActive`) and are deliberately meant to look *different*
   to you. If you cannot tell them apart on screen, that is a finding.
3. **A feature created by a menu command lands somewhere in the tree**, and
   whether that somewhere is findable is not something a test can judge.

---

## What M9 does NOT put in the UI — do not record these as failures

- **There are no creation dialogs.** ADR-M9-005 made creation a set of
  COMMANDS with sensible defaults, each one transaction: `Insert ▸ Pad` creates
  a `PadLength` of 20 mm and then expects you to edit it in the panel. If you
  find yourself looking for a dialog with a length field, that absence is the
  decision, not a defect.
- **There is no rollback BAR.** Rollback is a menu command against the selected
  feature (`Model ▸ Roll Back to Selected`), not a draggable marker in the tree.
- **Mirror and Pattern are absent from M9** (ADR-M9-006, delivered in M10) and
  M10 gave them no menu command either.
- **Undo has no history panel** — one step at a time, with the menu item naming
  the step it will undo.
- **Preview / edit sessions are Core-only** (ADR-M9-003). `FeatureEditSession`
  has no UI in M9; nothing on screen previews an edit before you commit it.

---

## How to run

From `D:\Program2\EP3D\ParametricCAD_Starter`:

```
build\Debug\ParametricCADViewer.exe --sample m8-chain
build\Debug\ParametricCADViewer.exe --sample m5-rectangle
```

Add `--dark` to either for the dark-palette pass.

**Editing a value:** select the object in the model tree, then double-click the
value cell in the Properties panel, type, and press Enter.

**Reading the numbers:** the status bar shows `Volume ... mm^3` to **one**
decimal and `Mass ... kg` to **four**. Every expected value below was computed
by hand from the sample's dimensions — not by running the code under test.
Density is aluminium, 2700 kg/m³, throughout.

---

## Test A — Undo and Redo  `--sample m8-chain`

100 × 50 rectangle padded 20 mm, with a 20 × 30 pocket 10 mm deep:
100000 − 6000 = **94000 mm³**, **0.2538 kg**.

| # | Step / expectation | Result |
|---|---|---|
| A1 | Open the **Edit** menu. It has **Undo** (Ctrl+Z) and **Redo** (Ctrl+Y) | |
| A2 | Select **Pad001** and set `Geometry / Length` to **40**. Volume becomes **`194000.0 mm^3`** (100 × 50 × 40 − 6000), mass **`0.5238 kg`** | |
| A3 | Open the Edit menu again. **Undo names the step**: it reads **`Undo Change PadLength`**, not a bare `Undo`. The label is not cosmetic — it is how you know which of several edits is about to be reversed | |
| A4 | Press **Ctrl+Z**. Volume returns to **`94000.0 mm^3`**, mass **`0.2538 kg`**, and the status bar says **`Undone`** | |
| A5 | The **panel's Length cell also went back to 20.000 mm.** Undo that fixes the model and leaves a stale number in the panel is the M6.14 defect wearing a new hat | |
| A6 | Press **Ctrl+Y**. Volume returns to **`194000.0 mm^3`** and the status bar says **`Redone`** | |
| A7 | Press **Ctrl+Z** once more to get back to 94000.0 | |
| A8 | Now make a NEW edit (set Length to **25** → **`119000.0 mm^3`**). **Redo is now disabled** — the branch you had redone into is gone. Confirm the menu item is greyed, not enabled-but-inert | |
| A9 | Set Length back to **20** (**`94000.0 mm^3`**) | |
| A10 | Press Ctrl+Z repeatedly until Undo greys out. **It greys out** — it does not stay enabled and do nothing, and the application does not become confused or empty | |
| A11 | **Judgement:** at the moment Undo greys out, is the document in a state you recognise? Say what you see; the expected answer is written below and it may surprise you | |

**About A10/A11.** The viewer builds its sample through the same document facade
a user drives, and **nothing clears the history afterwards**, so the startup
history contains the sample's own construction. Undoing far enough therefore
dismantles the sample. That is consistent with ADR-M9-001 (history is session
state; a *loaded* document starts empty — this document was not loaded, it was
built), and it is **not** currently considered a defect. Whether it is
acceptable in the shipping application is a call only you can make, so record
your judgement rather than a pass or fail.

---

## Test B — Suppression  `--sample m8-chain`

| # | Step / expectation | Result |
|---|---|---|
| B1 | Select **Pocket001**. Press **Ctrl+U** (or `Model ▸ Suppress/Unsuppress Selected`) | |
| B2 | The status bar says **`Feature suppressed`** | |
| B3 | Volume becomes **`100000.0 mm^3`**, mass **`0.2700 kg`** — the pad alone. The pocket is not merely hidden; it is not computed | |
| B4 | **The view shows the pad with no notch** — one solid, not a notched solid with a ghost overlay | |
| B5 | In the tree, Pocket001 is **visibly marked as suppressed** (its state column reads `Suppressed`). Say whether you could tell at a glance without reading the column | |
| B6 | **Pocket001 is still in the tree, in its original position.** Suppressing must not reorder or hide the feature — you have to be able to find it again to unsuppress it | |
| B7 | Select Pocket001 and press **Ctrl+U** again. Status says **`Feature unsuppressed`**, volume returns to **`94000.0 mm^3`** | |
| B8 | Suppress **Pad001** instead — the feature the pocket CONSUMES. Volume goes to **`0.0 mm^3`** or mass reads **not current**, and the view empties. Whichever happens, the application must not show a healthy-looking wrong solid | |
| B9 | Unsuppress Pad001. Volume returns to **`94000.0 mm^3`** — full recovery, no restart | |
| B10 | Press **Ctrl+Z** right after a suppress. **Suppression is undoable** and the volume follows | |

---

## Test C — Rollback  `--sample m8-chain`

| # | Step / expectation | Result |
|---|---|---|
| C1 | Select **Pad001** and choose `Model ▸ Roll Back to Selected` | |
| C2 | The status bar says **`Rolled back to step 1`** | |
| C3 | Volume becomes **`100000.0 mm^3`**, mass **`0.2700 kg`** — the same number suppression gave in B3, by a different route | |
| C4 | **This is the row that matters.** Pocket001 is now inactive *for a different reason* than in Test B. Look at the tree: **can you tell rolled-back apart from suppressed?** If both render identically, that is a real finding — the model distinguishes them deliberately (ADR-M9-002) and only the display can tell you which one you are in | |
| C5 | Choose `Model ▸ Roll Forward to End`. Status says **`Rolled forward to the end`** and volume returns to **`94000.0 mm^3`** | |
| C6 | Roll back to Pad001 again, then **edit the Length to 30** while rolled back. Volume becomes **`150000.0 mm^3`** — editing while rolled back is legal and the rolled-back state survives it | |
| C7 | Roll forward. Volume is **`144000.0 mm^3`** (150000 − 6000): the pocket was recomputed against the NEW pad, not against a cached old one | |
| C8 | Set Length back to **20** (**`94000.0 mm^3`**) | |

---

## Test D — Creation commands  `--sample m5-rectangle`

A constrained 100 × 50 rectangle padded 20 mm: **100000 mm³**, **0.2700 kg**.

**Do these in order** — each step's expected number assumes the state the
previous step left behind.

| # | Step / expectation | Result |
|---|---|---|
| D1 | Open the **Insert** menu. It lists **Pad from Selected Sketch**, **Pocket from Selected Sketch**, **Fillet on Current Solid**, **Chamfer on Current Solid** | |
| D2 | With **nothing** selected, `Insert ▸ Pad` is **greyed out**. The command states are driven from the model — an always-enabled item that silently does nothing is what M9 set out to avoid | |
| D3 | Select **Sketch001**. `Insert ▸ Pad` and `Insert ▸ Pocket` both become enabled | |
| D4 | Choose **`Insert ▸ Pocket from Selected Sketch`**. The status bar says **`Pocket created; edit its Depth in the panel`** | |
| D5 | Volume becomes **`50000.0 mm^3`**, mass **`0.1350 kg`** — the new pocket uses the same 100 × 50 sketch at the default depth of 10 mm, so it takes the top 10 mm off the 20 mm pad | |
| D6 | **The new feature is selected**, and the panel shows `Geometry / Depth` = **10.000 mm**, editable | |
| D7 | **You can find the new feature in the tree without hunting.** Say where it appeared and whether that was where you expected | |
| D8 | A new **`PocketDepth`** parameter appeared in the tree — creation makes the parameter as well as the feature | |
| D9 | Press **Ctrl+Z**. Volume returns to **`100000.0 mm^3`** AND the `PocketDepth` parameter is **gone**. This is ADR-M9-005's release proof: creation is ONE transaction, so undo cannot leave an orphan parameter behind | |
| D10 | Choose **`Insert ▸ Fillet on Current Solid`**. Status says **`Fillet created; edit its Radius in the panel`**, and volume becomes **`99427.0 mm^3`**, mass **`0.2685 kg`** (the Minkowski oracle for a 100 × 50 × 20 box with every edge rounded at the default 2 mm) | |
| D11 | The box's edges are **visibly rounded** | |
| D12 | Press **Ctrl+Z** (volume back to **`100000.0 mm^3`**), then choose **`Insert ▸ Chamfer on Current Solid`**. Status says **`Chamfer created; edit its Distance in the panel`** | |
| D13 | The edges are **flat bevels, not rounds** — and the volume is **LOWER than 99427.0**, because at the same 2 mm a flat cut removes more material than a round does. *(No exact analytic oracle is offered for the chamfered box; the inequality is the check.)* | |
| D14 | Press **Ctrl+Z** to get back to **`100000.0 mm^3`** | |
| D15 | **Judgement:** commands with silent defaults (20 mm, 10 mm, 2 mm) create a feature and then tell you to go edit it. Did that feel like it worked, or like something happened that you did not ask for? | |

---

## Test E — Cross-cutting judgement

| # | Step / expectation | Result |
|---|---|---|
| E1 | Through everything above, did the **tree selection stay where you put it** after each command? | |
| E2 | Every panel you opened showed **a value beside every label** — no empty value column at any window width (the M6.14 defect) | |
| E3 | Run any one test again with **`--dark`**. Suppressed and rolled-back markers are still legible in the dark palette | |
| E4 | Resize the window narrow. The status bar's Volume/Mass text is still readable — it is where every number in this document is read from | |
| E5 | Did any command leave the application in a state you could not get out of without restarting it? | |
| E6 | **Overall judgement:** M9 claims that a user can now edit, undo, suppress, roll back, and create features without touching a file. Having driven it, is that claim true? | |

---

## Recording the result

Fill the Result cells with PASS / FAIL / a note. For any FAIL, say what you saw
rather than what you expected — the expected value is already written down.

If everything passes, M9's UI validation is complete. **M9 still cannot close**:
it has had no independent review, and it sits behind M8, whose round-4 fixes are
also unreviewed.
