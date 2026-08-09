# M4 UI Independent Review

Commit: uncommitted working tree (same tree as the functional review APPROVE 96/100)
Environment: Windows 11 Home 10.0.26200, MSVC 2022 BuildTools 14.44.35207, Qt 6.11.1 (vcpkg),
build `cmake --build build --config Debug`, `ctest` **317/317 passed** including `ViewerSmokeTest`
Resolution: 1600x950, 1366x768, 1016x679 (enforced minimum), 2200x1400 on a 2560x1600 panel
DPI: **96 (100%) and 192 (200%, real hardware)**, plus `QT_SCALE_FACTOR` 1.25 / 1.5 / 2.0
Theme: system light (primary) and `--dark` (alternate)

Reviewer note: all six task tests were performed against the running application before any
source file was opened. Source was consulted afterwards only to locate defects already observed.
Every screenshot in this report was captured by the reviewer, not taken from
`docs/reviews/screenshots/`.

---

Decision:
**REQUEST CHANGES**

Score:
**79 / 100**

The decision does not rest on the score being one point under the 80 threshold. UI spec 23's
**automatic REQUEST CHANGES** list includes *"required viewer/property workflow cannot be
completed"*, and TASK-03 (Show/Hide) cannot be performed at all — the application's entire
command surface is three commands and none of them affects visibility. That trigger fires
independently of the numeric score.

This is nonetheless a strong shell. The error/recompute layer is the best part of the milestone
and survived a deliberate attack; the Core/Qt boundary holds at source *and* binary level; and
the state vocabulary is genuinely colour-independent. The required changes are bounded.

---

## Critical Findings

**None.**

Four candidate Critical conditions from UI spec 24 were attacked specifically and all held:

| Attack | Result |
|---|---|
| Displayed mm value actually metres / naked values | **Not reproducible.** Every dimensional row carries a separate `Unit` column; `Volume 100000.0 mm^3`, `Mass 0.2700 kg`, `COM (50.00, 25.00, 10.00) mm`, `2700.0 kg/m^3`. Values verified analytically at three lengths (10/20/30/137.5 mm) — e.g. Length 30 -> Volume 150000.0 mm^3, Mass 0.4050 kg, COM z=15.00 mm, all exactly correct. |
| Viewer selects A while Property Panel edits B | **Not reproducible.** Viewer click, tree click and arrow-key navigation always drove status bar, tree row and property panel to the same object. |
| Failed Pad appears valid/current | **Not reproducible** — see the attack log under *State Feedback*. |
| UI action mutates the wrong object | **Not reproducible.** In the one desynchronised state I did find (Major 2) the property panel is *empty*, so no edit can be misdirected. |

---

## Major Findings

### MAJOR-1 — Selecting an object in the Model Tree produces no highlight in the 3D viewer
`src/Viewer/MainWindow.cpp:289-309` (`MainWindow::selectObject`)

`selectObject()` rebuilds the property panel and mirrors the selection into the tree, but never
informs the viewer. There is no viewer-side counterpart to `OcctViewWidget::selectionChanged`,
so synchronisation is one-way only: viewer -> tree, never tree -> viewer.

Measured, not inferred. Sampling the viewport (50 190 sampled pixels):

```
deselected                 highlightPixels =   5
after clicking Pad001 in the Model Tree    =   5      <-- no change at all
after clicking the solid in the viewer     = 168      <-- highlight appears
```

UI spec 10 requires Model Tree and Viewer to synchronise, and spec 28 lists `select/highlight`.
A user who selects a feature in the tree gets no confirmation in the dominant work area.

### MAJOR-2 — Clicking empty space in the viewer desynchronises the tree, and the row cannot be re-selected
`src/Viewer/MainWindow.cpp:300` — the guard `if (rowId == id && id != kInvalidObjectId)`

When the viewer reports a deselection, `selectObject(kInvalidObjectId)` runs, but the loop that
mirrors selection into the tree is guarded on `id != kInvalidObjectId`, so **the tree's current
item is never cleared**. The tree keeps showing the old row highlighted while the status bar says
`No selection` and the property panel is empty. Because Qt emits no `itemSelectionChanged` when
the already-current row is clicked, clicking that highlighted row does nothing — the user is
stuck until they click a *different* row.

Reproduced verbatim:

```
1. clicked Pad001 in tree      tree=[Sld] Pad001   status=Selected: Pad001 (Pad)   panel=populated
2. clicked EMPTY viewer bg     tree=[Sld] Pad001   status=No selection             panel=<EMPTY>
3. clicked the SAME row again  tree=[Sld] Pad001   status=No selection             panel=<EMPTY>   <-- dead end
4. clicked Sketch001           tree=[Skt] Sketch001 status=Selected: Sketch001     panel=populated
5. clicked Pad001              tree=[Sld] Pad001   status=Selected: Pad001 (Pad)   panel=populated
```

This directly violates UI spec 6 ("Tree selection must correspond to the object shown in the
Property Panel"). It is not Critical only because the panel empties rather than retargeting.

### MAJOR-3 — TASK-03 (Show/Hide) cannot be performed; no visibility command exists
`src/Viewer/MainWindow.cpp:63-93` (`buildMenus`, `buildToolbar`)

The complete command surface, enumerated from the running application:

```
File  -> Exit
View  -> Fit All        (Ctrl+Shift+F)
Model -> Recompute      (Ctrl+R)
Toolbar: [Fit All] [Recompute]
Context menus: none on the tree, none in the viewer
```

There is no Show/Hide anywhere, and no `Hidden` value exists in the `OutlineState` vocabulary
(`src/Viewer/DocumentOutline.h:19-26`). `Suppressed` *is* in the vocabulary and is rendered, but
no command can set it. This fails UI spec 17 TASK-03, spec 18 ("Show/Hide selected: one
command/menu action") and spec 28 (`Hidden state`, `Suppressed state`), and is the trigger for
the automatic REQUEST CHANGES.

### MAJOR-4 — The root Part row is permanently labelled "Not computed"
`src/Viewer/DocumentOutline.cpp:91` (`root.state = OutlineState::Normal`) rendered by
`src/Viewer/DesignTokens.h:82-83` (the `default:` arm -> label `"Not computed"`)

The top row of the Model Tree reads `[Part] ViewerDemo   Not computed` in every state of the
document — including when every child is `Up to date` and the status bar is showing current mass
properties. The `Parameters` group row shows the same. The first thing a user reads in the tree
is a recompute state that is always wrong.

`OutlineState::Normal` is documented as "exists, nothing computed yet and nothing wrong", but it
renders through the same `default:` branch that produces the "Not computed" wording, so a
container node is indistinguishable from a genuinely uncomputed one.

---

## Minor Findings

1. **Two tree rows cannot be selected, and clicking them clears the selection.**
   `MassProperties` (`src/Viewer/DocumentOutline.cpp:151-157`) and the `Parameters` group
   (`:94`) are built without an `id`, so they keep `kInvalidObjectId`. Clicking either produces
   `No selection` and an empty panel. This is the single biggest obstacle to TASK-05: the row
   literally named *MassProperties* is the natural place to look for Volume/Mass/COM, and it is
   inert.

2. **Selecting the Part shows an empty Property Panel.** `[Part] ViewerDemo` selects correctly
   (`Selected: ViewerDemo (Part)`) but `propertiesOf()` returns no rows, so the panel is blank.
   UI spec 7 expects at least a Name.

3. **7 of 12 design tokens are dead code.** `kMicro`, `kStandard`, `kGrouped`, `kSection`,
   `kMajor`, `kBorder` and `kTreeRowHeight` (`src/Viewer/DesignTokens.h:18-32`) have **zero**
   uses outside their own header. The self-report's "No spacing literal appears in
   `MainWindow.cpp`" is true but misleading: no spacing is applied at all, so the layout is Qt
   defaults throughout. UI spec 28's `centralized design tokens` is met only nominally.

4. **Model Tree rows are 16 px, not the 26 px the token declares nor the 24-28 px spec 3
   requires.** `kTreeRowHeight = 26` (`DesignTokens.h:28`) is never applied;
   `MainWindow.cpp:100` sets only `setUniformRowHeights(true)`. Measured row pitch via UI
   Automation: 16 px at 96 dpi (property rows correctly measure 29-30 px, status bar 24 px).
   The self-report's "Rows: tree 26 px" is not what ships.

5. **Tree tooltips carry type and state but never the object's name**
   (`src/Viewer/MainWindow.cpp:164-169`; observed tooltip on Pad001: `Pad - Up to date`).
   The self-report's Minor 3 states that elided names are recoverable from the tooltip — they are
   not. `[Phy] MassProper...` elides at the default dock width with no way to read the full name
   short of resizing the dock.

6. **Tree columns waste the dock width while names elide.** `Model` and `State` are 158 px each
   inside a 320 px dock; `State` is `ResizeToContents` but is allotted a fixed half, leaving a
   visible empty gutter to its right while column 0 truncates.

7. **Docks never shrink, so the viewer is squeezed at small sizes.** Model Tree stays 320 px and
   Properties 300 px at every window width. At the enforced minimum (1016x679) the viewer is
   372 px wide against 620 px of docks — the panels consume more than the viewer, contrary to
   spec 5's intent, though 1366x768 (viewer 722 px, 53%) is comfortably fine.

8. **`Blocked` state never surfaces.** `OutlineState::Blocked` with marker `-` is defined and
   rendered, but in `--scenario failed-profile` the dependent Pad001 shows `Failed`, not
   `Blocked`, so a downstream victim is indistinguishable from the root cause in the State column
   (the status bar does disambiguate).

9. **`Active` state is not modelled at all** (no active-object concept in M4).

10. **The status bar's left field is shared** between the selection readout and the failure
    message, so while a failure is displayed you cannot see what is selected.

11. **Only the first failure is reported.** `reportHealth()`
    (`src/Viewer/MainWindow.cpp:250-266`) returns after the first failed child.

12. **`[   ] Parameters` renders an empty type tag** because the group node has no `kind`
    (`DocumentOutline.cpp:94` -> `kindTag(Other)`), breaking the `[Xxx]` visual rhythm.

13. **Parameter property row is labelled `Value / Value`** — group and label are both "Value".

14. **No `Placement` rows on the Pad**, though spec 7's worked example shows them (the Sketch
    does expose `Placement / Origin X|Y|Z` with units).

15. **No disabled command states.** `Recompute` is always enabled, even when nothing is dirty;
    spec 9 asks for visibly disabled commands.

16. **No hover/preselection in the viewer** (measured: hovering the solid changes 0 pixels).
    Spec 10 requires preselection and selection to be distinguishable *if both exist*, so this is
    compliant — but there is no hover affordance at all.

17. **Tab navigation steps through read-only and empty cells.** Six Tab presses from the tree
    walk `General / Name` -> `Pad001` -> `<empty unit>` -> `General / Type` -> ... rather than
    stopping at editable values.

---

## Visual Hierarchy

The layout is a textbook CAD shell and matches spec 1: menu, thin toolbar, left Model Tree,
dominant central viewer, right Property Panel, status bar carrying selection on the left and
physical properties on the right. The viewer is the largest pane at every size tested
(952 px of 1600, 722 px of 1366, 372 px of 1016).

Weaknesses are Major-4 (the always-wrong root state is the first thing read) and Minor-7 (fixed
dock widths). The text-only toolbar is a defensible M4 choice, coherently applied, and every
button has a tooltip naming its shortcut.

**Score: 9 / 12**

## Layout / Spacing

Nothing overlaps, clips or truncates at any resolution or scale factor tested. Property rows,
headers and the status bar all honour their tokens. The deductions are Minor-3 (spacing tokens
entirely unapplied) and Minor-4 (16 px tree rows against a 24-28 px spec and a 26 px token) —
both concrete, measured deviations rather than taste.

- **1366x768**: usable. Viewer 722x658, tree 320, properties 300. All five Pad property rows
  visible with units, no clipping. **PASS**
- **1016x679 (enforced minimum)**: usable but cramped; viewer 372 px. **PASS with reservation**
- Resizing, minimise/restore and dock re-layout all repaint correctly (ADR-M4-012's `resizeEvent`
  regression did not recur).

**Score: 8 / 12**

## Typography / Readability

Platform UI font; numeric columns and the status readout use the monospace style hint so digits
align. No clipped text at 96 dpi, 192 dpi, or `QT_SCALE_FACTOR` 1.25/1.5/2.0. Units never collide
with values — they are in a separate column, which is also what makes the units check auditable.
A 7-character value (`137.500`) fits the 68 px Value column without eliding. Only deduction is
Minor-5 (elided names unrecoverable).

**Score: 7 / 8**

## Icon / Command Consistency

No icon set ships; every command is a text label. Applied consistently, so there is no visual
mismatch to find. Tooltips are present and useful — hovering `Fit All` gives
*"Fit the whole model in the view (Ctrl+Shift+F)"*. Menus carry mnemonics (`&File`, `&View`,
`&Model`) and accelerators are displayed in the menu.

Deductions: the command surface is only three commands (Major-3), there are no context menus
anywhere, no view-orientation controls, and no disabled states (Minor-15).

**Score: 7 / 8**

## CAD Information Density

Good: mass properties are permanently visible with units; the tree carries a `[Xxx]` kind tag,
a state marker and a state label on every row; the property panel groups rows as `Group / Label`.

Deductions: the `MassProperties` row is inert (Minor-1), the Part panel is empty (Minor-2), and
the Pad exposes no Placement (Minor-14). The flattened `Group / Label` presentation is a
reasonable simplification of spec 7's indented sections.

**Score: 8 / 10**

## Model Tree

The state presentation is the strongest thing in the tree and is genuinely well built:

```
[Part] ViewerDemo        Not computed        <-- always, even when healthy (MAJOR-4)
  [   ] Parameters       Not computed        <-- unselectable, empty kind tag
    [Par] PadLength      Up to date
[Skt] ! Sketch001        Failed              red + bold + "!" + "Failed"
[Sld] ! Pad001           Failed
[Mat] Aluminium          Up to date
[Phy] * MassProperties   Needs recompute     amber + "*" + "Needs recompute"
```

Selection maps to a stable `ObjectId` (`MainWindow.cpp:162`, `kIdRole`), never to an index or a
pointer. Arrow-key navigation moves the selection and the status bar follows.

Against it: Major-4, Minor-1, Minor-4, Minor-6, Minor-8, Minor-12, and the absence of
`Hidden`/`Active` with `Suppressed` unreachable.

**Score: 6 / 10**

## Property Panel

The best-executed panel in the shell.

- Edit path is exactly spec 7's preferred workflow: select -> double-click value -> type ->
  Enter. **Three actions**, no modal at any point.
- `Enter` commits, the inline editor pre-selects the current text, Esc cancels.
- Editable values render in normal text and are focusable; read-only values use the palette's
  *disabled* colour (`MainWindow.cpp:203-204`) so they stay legible in both themes.
- Units are a separate column and never collide with values.
- Edits write through `PartDocument::setParameterValue` using the **Parameter's** id
  (`MainWindow.cpp:197, 331, 347`), not the feature's — the panel never writes into a Feature.
- Selection is deliberately preserved across the recompute (`MainWindow.cpp:353-355`) and I
  confirmed it is.

Invalid input, exercised deliberately:

```
'abc'  -> value restored to 30.000, status "'abc' is not a number", no modal
'0'    -> Length 0.000, "Pad001 failed", "Mass properties: not current"
'-5'   -> Length -5.000, "Pad001 failed", "Mass properties: not current"
''     -> status "'' is not a number", previous value kept
'20'   -> full recovery, mass properties current again
```

Deductions are cosmetic only: Minor-13 (`Value / Value`) and Minor-14 (no Placement).

**Score: 11 / 12**

## Viewer

Everything spec 10 lists as the M4 minimum works, and I drove each one:

| Interaction | Method | Result |
|---|---|---|
| Rotate | left-drag | works |
| Pan | **middle-drag** | **works** — the self-report's NOT EXECUTED item |
| Zoom | wheel | works |
| Fit All | toolbar, `View` menu, `Ctrl+Shift+F` | works, one action, recovers from a wrecked view |
| Whole-object selection | left-click | works; resolves to the correct `ObjectId` |
| Selection highlight | left-click | white edge outline, 5 -> 168 highlight pixels |
| Recompute refresh | property edit | solid rebuilds and re-fits |

Shading is correct (top and side faces distinctly lit, silhouette reads as a solid) and the
triedron is present. `refreshFromDocument()` rebuilds the display wholesale, and the
`AIS_InteractiveObject -> ObjectId` map is rebuilt with it, so a stale presentation object cannot
resolve to a document object — consistent with ADR-M4-006.

Against it: **MAJOR-1** (no tree -> viewer highlight) and **MAJOR-2** (background-click desync).

**Score: 6 / 10**

## State Feedback

The strongest area of the milestone. I attacked the spec 24 Critical condition
*"failed recompute is displayed as current success"* deliberately and could not produce it.

Attack log on `--scenario failed-profile` (viewport sampled at 89 320 points; the solid is
detected as pixels where R-B > 40):

```
initial                    solidPixels = 3/89320   status: "Sketch001 failed: profile is not
                                                    closed: an endpoint near (0.000000,
                                                    0.000000) is used by only one curve"
                                                    right: "Mass properties: not current"
Fit All while failed       solidPixels = 3         unchanged
Recompute while failed     solidPixels = 3         unchanged
rotate while failed        solidPixels = 3         unchanged
pan while failed           solidPixels = 3         unchanged
zoom in while failed       solidPixels = 3         unchanged
resize 1600->1300->1600    solidPixels = 3         unchanged
minimise + restore         solidPixels = 3         unchanged
```

The retained last-valid geometry is never repainted and the retained mass numbers are never
shown; the status bar substitutes `Mass properties: not current` (`MainWindow.cpp:228-233`).
Breaking the model by property edit (Length 0 / -5) produced the same behaviour, and repairing it
restored the solid and current numbers.

Failure diagnostics satisfy spec 12 in full — affected object, category and a useful diagnostic,
in the status bar *and* in the row tooltip, with no log open:

```
status bar : Sketch001 failed: profile is not closed: an endpoint near (0.000000, 0.000000)
             is used by only one curve
row tooltip: Sketch - Failed
             profile is not closed: an endpoint near (0.000000, 0.000000) is used by only one curve
```

Deductions: Minor-10 (shared status field) and Minor-11 (only the first failure reported).
Note that a bare Length failure produces only `Pad001 failed` with no reason, because the
diagnostic string is empty for that path — less actionable than the sketch case.

**Score: 9 / 10**

## DPI / Theme

**DPI.** This machine has a 200%-scaled monitor attached (`DISPLAY1`, 2560x1600 physical,
192 dpi) alongside the 1920x1080 @ 100% secondary. I ran the application on both and additionally
forced `QT_SCALE_FACTOR`. Metrics scale proportionally with **no clipping, no truncation and no
overlap** anywhere:

| Scale | Source | Tree row | Property row | Header | Value col | Result |
|---|---|---|---|---|---|---|
| 100% | real, DISPLAY2 | 16 | 29 | 24 | 69 | PASS |
| 125% | `QT_SCALE_FACTOR=1.25` | 20 | 36 | 30 | 86 | PASS |
| 150% | `QT_SCALE_FACTOR=1.5` | 24 | 43 | 36 | 103 | PASS |
| 200% | `QT_SCALE_FACTOR=2` | 64 | 116 | 96 | 276 | PASS |
| **200%** | **real, DISPLAY1 @ 192 dpi** | 32 | 58 | 48 | 138 | **PASS** |

Editable values remain readable and editable at every one of these. Spec 23's
"severe DPI clipping prevents editing" does not fire.

**Theme.** `--dark` produces a readable dark shell: light text on dark base, the selected row is a
clearly visible blue, read-only values remain legible in the palette's disabled colour, failed
rows are readable red and dirty rows readable amber, and the status-bar diagnostic is legible.

The developer's claim that state colours are **palette-derived rather than hard-coded** is
**verified numerically** — I sampled the rendered pixels in both themes:

```
                 light theme          dark theme
Failed text      RGB(163, 29, 29)     RGB(232,125,125)
Dirty  text      RGB(138, 98,  0)     RGB(232,200,106)
```

The values differ per theme, matching the `dark ? ... : ...` selection at
`src/Viewer/DesignTokens.h:67-68`. No semantic state is a fixed RGB.

**Colour independence.** I converted the Model Tree to greyscale and every state remains
unambiguous, because the marker and the label carry the meaning:

```
[Part] ViewerDemo        Not computed
  [   ] Parameters       Not computed
    [Par] PadLength      Up to date
[Skt] ! Sketch001        Failed            (also bold)
[Sld] ! Pad001           Failed
[Mat] Aluminium          Up to date
[Phy] * MassPrope...     Needs recompute
```

`Failed` vs `Dirty` vs `Normal` are distinguishable with colour removed entirely. Spec 11/19
satisfied.

**Keyboard/focus.** Tab reaches the tree and the property table; arrow keys move tree selection
and the status bar follows; `Ctrl+R` and `Ctrl+Shift+F` work; menu mnemonics work. Only Minor-17
against it.

**Score: 8 / 8**

---

## Task Results

All six executed against the running application, without prior source knowledge.

**TASK-01 — Change Pad Length 20 -> 30: COMPLETED.**
Actions: **3** (click `[Sld] Pad001` in the tree; double-click the `Geometry / Length` value;
type `30` + Enter). Meets spec 18's "1 selection + 1 property edit" target.
Discoverability: good — the row is labelled `Geometry / Length` with a `mm` unit column, and the
value is visibly editable (normal text) while its neighbours are greyed.
Feedback: immediate and correct — the solid rebuilt and re-fitted, and the status bar went to
`Volume 150000.0 mm^3   Mass 0.4050 kg   COM (50.00, 25.00, 15.00) mm`, which is analytically
exact for 100x50x30 at 2700 kg/m^3.
Ambiguity: none. No modal at any point. Selection was preserved across the recompute.

**TASK-02 — Identify the failed object and why: COMPLETED, no logs or source needed.**
Actions: **0** — the information is on screen at launch.
The tree shows `[Skt] ! Sketch001  Failed` and `[Sld] ! Pad001  Failed` in bold red, and the
status bar names the object and the reason:
`Sketch001 failed: profile is not closed: an endpoint near (0.000000, 0.000000) is used by only
one curve`. The same text is in the row tooltip. The status bar correctly identifies *Sketch001*
as the root cause rather than the downstream Pad.
Ambiguity: minor — both rows read `Failed`, so the State column alone does not distinguish root
cause from victim (Minor-8); the status bar resolves it.

**TASK-03 — Hide and show the solid: CANNOT BE COMPLETED.**
No visibility command exists in any menu, on the toolbar, or in any context menu (there are no
context menus). No `Hidden` state exists in the vocabulary. This is MAJOR-3 and the trigger for
the automatic REQUEST CHANGES. Reported plainly as a failure, not a gap in the review.

**TASK-04 — Recover a useful camera view: COMPLETED.**
Actions: **1**. I deliberately wrecked the view first (hard zoom, middle-drag pan off-centre,
left-drag rotate). `Fit All` restored a correctly framed, fully visible solid. Available three
ways — toolbar button, `View` menu, `Ctrl+Shift+F` — all verified working.
Discoverability: good; labelled button, and the menu displays the shortcut.

**TASK-05 — Find Volume / Mass / COM: COMPLETED, but by the wrong route.**
Volume, Mass and COM are permanently in the status bar with correct units
(`Volume 100000.0 mm^3   Mass 0.2700 kg   COM (50.00, 25.00, 10.00) mm`), so the information is
never more than a glance away and needs **0 actions**.
However, the natural discovery path fails: clicking the tree row literally named
`[Phy] MassProperties` yields `No selection` and an **empty** property panel (Minor-1). A user
looking for mass properties clicks the object called MassProperties, gets nothing, and loses
their previous selection in the process. Per-object density is on the Material and Pad rows
(`2700.0 kg/m^3`).
Ambiguity: moderate, for the reason above.

**TASK-06 — Selection synchronisation: COMPLETED with a defect.**
Clicking the solid in the viewer drove all three surfaces to the same semantic object:

```
status bar    : Selected: Pad001 (Pad)
tree highlight: [Sld] Pad001
property panel: General / Name = Pad001 | Geometry / Length = 20.000 mm | ...
```

No `ObjectId` or developer terminology is exposed anywhere; objects are named. Arrow-key
navigation keeps all three in step.
The defect is the reverse direction and the empty case: tree -> viewer produces no highlight
(MAJOR-1), and clicking empty viewer space leaves the tree highlighted while the panel empties
(MAJOR-2).

---

## Assessment of the Self-Report's NOT EXECUTED Claims

The self-report asked that these be treated as the agenda. They were. **Four of the six were
executable in this environment**, and I executed them.

| # | Claim | Verdict |
|---|---|---|
| 1 | **DPI 125/150/200%** — "requires altering a system setting and signing out" | **OVERSTATED — executed.** Wrong on both counts. (a) A 200%-scaled monitor is *already attached* (`DISPLAY1`, 2560x1600 @ 192 dpi); I ran the app on it and measured exact 2x scaling with no clipping. (b) `QT_SCALE_FACTOR=1.25/1.5/2` works without touching any system setting. This was unattempted, not impossible. **Result: PASS at all four scales.** |
| 2 | **2560x1440** — "no display of that size is attached" | **PARTLY OVERSTATED.** Literally true for 2560x1440, but `DISPLAY1` is 2560x1600, so a 2560-wide canvas *was* available. I exercised the app at 2200x1400 on it. |
| 3 | **TASK-01 … TASK-06** — "requires a reviewer without source knowledge" | **LEGITIMATE.** Correctly withheld. All six executed here; results above. |
| 4 | **Pan (middle-drag)** — "implemented and reviewed, never driven" | **OVERSTATED — executed.** I drove a middle-button drag; the view pans correctly. **Result: PASS.** |
| 5 | **UI-002** (no sketch editor) / **UI-009** (DPI 150) | **UI-002 LEGITIMATE** — there is genuinely no sketch-editing state to photograph. **UI-009 OVERSTATED** — captured via `QT_SCALE_FACTOR=1.5`. |
| 6 | **Show/Hide (TASK-03)** — "the command does not exist in M4" | **LEGITIMATE as a statement of fact**, but it is recorded as a scope note when it is a **defect** against spec 17/18/28 and triggers spec 23's automatic REQUEST CHANGES. |

**Two further self-report claims are inaccurate:**

- *"Rows: tree 26 px"* — measured **16 px** at 96 dpi. `kTreeRowHeight` is never applied
  (Minor-4).
- Minor 3: *"Full names are in the tooltip"* — they are not. The tooltip is
  `typeLabel - stateLabel [+ diagnostic]` (`MainWindow.cpp:164-169`); the object's name never
  appears (Minor-5).

Claims I **confirmed** as accurate: the Core/Qt boundary at source and binary level;
palette-derived state colours; colour independence; stale geometry never presented as current;
units on every dimensional row; `1016x679` as the enforced minimum; and toolbar tooltips (which
I initially mis-tested and re-verified — they are present and good).

---

## Screenshot Review

Reviewer-captured. The developer's set in `docs/reviews/screenshots/` was not relied on.

| ID | Subject | Verdict |
|---|---|---|
| UI-001 | EmptyDocument (`--scenario empty`) | PASS — Pad absent, `MassProperties * Needs recompute`, `Mass properties: not current`. Correct. |
| UI-002 | RectangleSketch | **N/A** — no sketch editor in M4. Legitimately unphotographable. |
| UI-003 | PadSelected | PASS — all five property rows with units, tree row highlighted, status names the object. |
| UI-004 | SketchSelected | PASS — `Entities 4`, `Placement / Origin X|Y|Z` in mm, `Profile / Status: Closed loop`. |
| UI-005 | FailedProfile | PASS — best screenshot in the set; `!` markers, red+bold, empty viewer, full diagnostic. |
| UI-006 | SelectedSolid | PASS — white edge highlight distinguishes selected from unselected. |
| UI-007 | 1366x768 | PASS — no clipping, viewer dominant at 722 px. |
| UI-008 | 1920x1080-class | PASS — captured at 1600x950 and 2200x1400. |
| UI-009 | DPI150 | **PASS — captured** via `QT_SCALE_FACTOR=1.5`, plus a real-hardware 200% capture. Contradicts the self-report's NOT EXECUTED. |
| UI-010 | AlternateTheme (`--dark`) | PASS — readable, selection clearly visible, colours palette-derived. |
| UI-011 | MinimumSize 1016x679 | PASS with reservation — usable, viewer squeezed to 372 px. |
| UI-012 | DarkThemeFailed | PASS — failed red and dirty amber both readable on dark. |

---

## Architecture Boundary (UI spec 20)

Verified independently at both levels.

**Source.** Combined scan of `src/Core/` for `QObject|QWidget|QString|QApplication|QGraphics|
Q_OBJECT|#include <Q|QtCore|QtWidgets|QtGui` -> **0 matches**. Qt appears in exactly six files,
all under `src/Viewer/`.

**Binary** (`dumpbin -dependents`):

```
ParametricCADCoreTests.exe        Qt6: 0   TK*: 0     <-- Core is clean
ParametricCADApp.exe              Qt6: 0   TK*: 4
ParametricCADKernelOcctTests.exe  Qt6: 0   TK*: 4
ParametricCADViewer.exe           Qt6: 3   TK*: 7
```

Consistent with ADR-M4-006. The viewer holds a non-owning `PartDocument*`; tree rows and property
rows carry an `ObjectId` (`kIdRole`), never a pointer or an index; mutation flows one way through
`PartDocument::setParameterValue` + `recompute`. **No Core Qt dependency. This automatic
REQUEST CHANGES trigger does not fire.**

ADR-M4-012's smoke rule holds: `ViewerSmokeTest` is registered in CTest and passed in my run
(317/317).

---

## UI Spec 28 Final Checklist

| # | Item | Status |
|---|---|---|
| 1 | centralized design tokens | **PARTIAL** — file exists; 7 of 12 tokens unused (Minor-3) |
| 2 | CAD-density main layout | PASS |
| 3 | Viewer is dominant area | PASS at >=1366; marginal at the 1016 minimum (Minor-7) |
| 4 | resizable Model Tree | PASS |
| 5 | resizable Property Panel | PASS |
| 6 | Tree <-> ObjectId mapping | PASS |
| 7 | Viewer <-> ObjectId mapping | PASS |
| 8 | Property Panel edits correct object | PASS — writes the Parameter's id via the facade |
| 9 | numeric units explicit | PASS |
| 10 | Enter-to-commit common numeric edit | PASS |
| 11 | Normal state | **PARTIAL** — renders as "Not computed" on containers (MAJOR-4) |
| 12 | Selected state | PASS |
| 13 | Active state | **FAIL** — not modelled |
| 14 | Dirty state | PASS |
| 15 | Failed state | PASS |
| 16 | Hidden state | **FAIL** — not in the vocabulary |
| 17 | Suppressed state | **PARTIAL** — modelled and rendered, unreachable from the UI |
| 18 | states not color-only | PASS — verified in greyscale |
| 19 | useful failure diagnostic | PASS |
| 20 | stale failed geometry visually distinguished | PASS — attacked and held |
| 21 | rotate | PASS |
| 22 | pan | PASS — executed |
| 23 | zoom | PASS |
| 24 | fit all | PASS |
| 25 | select/highlight | **PARTIAL** — viewer click only; tree selection does not highlight (MAJOR-1) |
| 26 | recompute refresh | PASS |
| 27 | Core free of Qt | PASS — source and binary |
| 28 | Viewer does not own semantic objects | PASS |
| 29 | 1366x768 usable | PASS |
| 30 | 1920x1080 usable | PASS |
| 31 | DPI 100% checked | PASS |
| 32 | DPI 125% checked | PASS — `QT_SCALE_FACTOR` |
| 33 | DPI 150% checked | PASS — `QT_SCALE_FACTOR` |
| 34 | DPI 200% checked | PASS — **real 192 dpi hardware** |
| 35 | primary theme checked | PASS |
| 36 | alternate theme smoke | PASS |
| 37 | keyboard/focus smoke | PASS |
| 38 | TASK-01 through TASK-06 | **FAIL** — TASK-03 impossible; other five completed |
| 39 | Golden Screenshot set reviewed | PASS — reviewer-captured |
| 40 | M4_UI_SelfValidationReport | PASS — exists; two claims inaccurate |
| 41 | independent UI review | PASS — this document |
| 42 | no Critical UI finding | **PASS** |
| 43 | no unresolved Major UI finding | **FAIL** — four Majors |
| 44 | UI Reviewer score >= 80 | **FAIL** — 79 |

---

## Score Summary

| Category | Weight | Score |
|---|---|---|
| Visual hierarchy | 12 | 9 |
| Spacing / alignment | 12 | 8 |
| Typography / readability | 8 | 7 |
| Icon / command consistency | 8 | 7 |
| CAD information density | 10 | 8 |
| Model Tree clarity | 10 | 6 |
| Property editing | 12 | 11 |
| Viewer interaction/selection | 10 | 6 |
| State/error/recompute feedback | 10 | 9 |
| High-DPI/theme/accessibility | 8 | 8 |
| **Total** | **100** | **79** |

---

## Required Changes

Blocking (must be fixed before M4 UI can be READY):

1. **Implement Show/Hide** for the selected object, reachable in one action from the toolbar or a
   menu, and add a `Hidden` state to the outline vocabulary rendered with its own marker and
   label. Resolves MAJOR-3, checklist items 16 and 38, and the automatic REQUEST CHANGES trigger.
2. **Propagate tree selection to the viewer.** Give `MainWindow::selectObject`
   (`src/Viewer/MainWindow.cpp:289-309`) a viewer-side counterpart so selecting a row highlights
   the solid. Resolves MAJOR-1 and checklist item 25.
3. **Clear the tree's current item on deselection.** Drop the `id != kInvalidObjectId` guard at
   `src/Viewer/MainWindow.cpp:300` (or call `tree_->setCurrentItem(nullptr)` /
   `clearSelection()` when the id is invalid) so the tree cannot show a highlighted row while the
   panel is empty, and so re-clicking that row is not a dead end. Resolves MAJOR-2.
4. **Stop labelling container nodes "Not computed."** Either give `OutlineState::Normal` its own
   neutral label (e.g. an em dash) distinct from the uncomputed case, or derive the Part's state
   from its children. Resolves MAJOR-4.

Strongly recommended (small, high value):

5. Give `MassProperties` a real `ObjectId` and a `propertiesOf()` branch returning
   Volume / Mass / COM with units, so TASK-05's natural path works
   (`src/Viewer/DocumentOutline.cpp:151-157`). Also give the Part at least a Name row.
6. Make the `Parameters` group non-selectable rather than selection-clearing, and give it a kind
   tag.
7. Apply `kTreeRowHeight` (or delete it and the five unused spacing tokens). A 16 px tree row is
   below spec 3's 24-28 px range.
8. Include the object's name in the tree tooltip so elided names are recoverable.
9. Surface `OutlineState::Blocked` for features whose input failed, so the root cause is
   distinguishable from its victims in the State column.
10. Provide a diagnostic string for geometry failures (`Pad001 failed` currently has no reason),
    and report more than the first failure.

---

## M4 UI Readiness

**NOT READY**

Four Majors are open, checklist items 13, 16, 38 and 43 fail, and UI spec 23's automatic
REQUEST CHANGES trigger for an incompletable required workflow (TASK-03) has fired.

This should not be read as a weak milestone. There are **no Critical findings**: units are
correct and explicit everywhere, no path was found on which the UI edits or mutates the wrong
object, the failed-recompute-shown-as-current condition resisted a deliberate multi-vector
attack, the Core/Qt boundary holds at source and binary level, the layout is usable from
1016x679 to a real 192 dpi display, and state is conveyed independently of colour on two themes.
The property-editing workflow and the error/recompute feedback are both at or near
specification.

All four blocking changes are localised — three of them are a handful of lines in
`MainWindow.cpp` — and none requires redesigning the shell. A re-review after those four should
be quick.
