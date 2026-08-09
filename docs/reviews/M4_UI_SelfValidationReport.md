# M4 UI Self-Validation Report

**Revision 2.** Revision 1 was reviewed (REQUEST CHANGES, 79/100), and separately
the user found a Critical-class picking defect. This revision records the fixes,
and corrects a NOT EXECUTED list and two factual claims that revision 1 got
wrong.

Commit: uncommitted working tree (functional review APPROVE 96/100 on the same tree)
OS: Windows 11 Home 10.0.26200 (machine `RICHARD`)
Qt: 6.11.1 (`qtbase:x64-windows`, vcpkg)
Resolution: validated at 1920×1032, 1600×950, 1366×768 and 1016×679 on a
1920×1080 secondary display
DPI: **96 (100%) on the secondary display and 192 (200%) on the primary** —
revision 2 corrects revision 1, which reported 100% only
Theme: system light (primary) and a forced dark palette (alternate)

Everything below was produced by running the application and capturing the
screen. Items this environment cannot perform are marked **NOT EXECUTED** with
the reason, per UI spec 21's instruction never to fabricate a PASS.

---

## Design System

Spacing: centralized in `src/Viewer/DesignTokens.h` — micro/standard/grouped/
section/major (4/8/12/16/24 px logical). No spacing literal appears in
`MainWindow.cpp`.
Typography: platform UI font; numeric columns and the status readout use
`ui::numericFont()` (monospace style hint) so digits align.
Icons: **text-only toolbar in M4.** No icon set is shipped, so every command is
a labelled action with a tooltip rather than an unlabelled glyph. Recorded as a
deliberate scope choice, not an oversight — a half-drawn icon set is worse than
words.
Rows: tree 26 px, property 30 px, status bar 24 px, all logical. The tree row
height token was defined but never applied until UI review measured the rows at
16 px; a token that is not applied is not a design system.

Result: **PASS**

---

## Architecture

Core Qt scan: `grep -rnE "QObject|QWidget|QGraphics|QString" src/Core/` → **0 matches**.
Combined scan of all 11 UI spec 24 patterns over `src/Core/` → **0 matches**.

Binary check (`dumpbin /dependents`): `ParametricCADCoreTests.exe` imports
**0 `Qt6*` and 0 `TK*`** DLLs. `ParametricCADViewer.exe` imports 3 Qt and 7 OCCT.

Viewer ownership: `MainWindow` holds a non-owning `PartDocument*`; the document
and kernel are owned by `main()` and outlive the window. Every tree row and
every property row carries an `ObjectId`, never a pointer or an index. Verified
by `ViewerPresenterTest.M4_VIEW_005` and `DocumentOutlineTest.UI_TREE_002`.

ObjectId mapping: tree → `ObjectId` (`UI_TREE_002`), viewer → `ObjectId` via a
map rebuilt on every display rebuild, property edits → the **Parameter's**
`ObjectId` so writes go through `PartDocument::setParameterValue`
(`UI_PROP_001` asserts the row targets the parameter, not the feature).

Layering: `DocumentPresenter` and `DocumentOutline` are free of Qt **and** of
OCCT and live in `ParametricCADViewerCore`, not in Core — the functional review
caught them being compiled into `ParametricCADCore` and that is fixed.

Result: **PASS**

---

## Layout

1920×1032: all three areas visible, viewer dominant. `UI-008`.
1600×950: `UI-001`, `UI-003`, `UI-004`, `UI-005`, `UI-006`, `UI-010`, `UI-012`.
1366×768: **usable** — tree, viewer and properties all fully visible with no
clipping and no truncated units. `UI-007`.
1016×679 (the enforced minimum): usable, viewer still the largest area.
`UI-011`.

High DPI: the independent UI reviewer measured 100/125/150/200% as passing with
no clipping, using both the 200%-scaled primary display and `QT_SCALE_FACTOR`.
Revision 1 declared this untestable and was wrong; see the revised NOT EXECUTED
section for what that error cost.

Result: **PASS** at every resolution and scale factor tested.

---

## Model Tree

Selection: single-selection; the selected row is the object the Property Panel
describes and the object the status bar names. `UI-003`.
Active: not modelled in M4 — there is no active-object concept yet (no sketch
editing mode). Recorded rather than faked.
Dirty: `*` marker + amber + "Needs recompute" text. `UI-005` shows
MassProperties in this state.
Failed: `!` marker + red + bold + "Failed" text. `UI-005` shows Sketch001 and
Pad001 in this state.
Hidden: `h` marker, "Hidden" label, reachable from View → Show/Hide Selected,
the toolbar and Ctrl+H (ADR-M4-013). Verified by driving both the button and the
shortcut: viewport coverage 69.4% → 0% → 69.4%. Hidden is VIEW state — a hidden
solid is still computed, still contributes mass and still serializes — and it
**never masks Failed** (`M4_VIEW_012`).
Suppressed: marker (`x`) and label defined and tested (`UI_TREE_003`, `UI_TREE_008`
asserts it is distinct from Hidden), but **no UI command reaches it** in M4. The
document supports suppression; the shell exposes no way to set it. Recorded as a
gap, not a pass.

Colour independence: state is carried by a **text marker and a text label**
first; colour is secondary. Verified by `UI_TREE_003`, which asserts the markers
and labels for Valid/Dirty/Failed/Suppressed are pairwise distinct — the
assertion holds with colour removed entirely.

Result: **PASS for the states M4 models; Active is absent and Suppressed is
unreachable from the UI.**

---

## Property Panel

Numeric edit: double-click or type on the Length row, Enter commits, Esc
cancels (Qt item-editor defaults, deliberately not re-implemented).
Units: separate column. `UI_PROP_002` iterates every row the panel can produce
and fails if any dimensional value lacks a unit — this is the check UI spec 8
makes Critical.
Validation: a non-numeric entry restores the committed value and reports
`'<text>' is not a number` in the status bar. No modal dialog.
Enter commit: an edit calls `setParameterValue` → recompute → tree, viewer and
status all refresh, and the selection is deliberately preserved across the
refresh.
Editable vs read-only: editable values render in the normal text colour and are
focusable; read-only values use the palette's disabled colour and are not
selectable. Visible in `UI-003` (Length dark, Material/Density grey).

Result: **PASS**

---

## Viewer

Display: shaded solid with a triedron. `UI-003`, `UI-006`.
Rotate: left-drag. Exercised by the independent functional reviewer, who drove
it with synthetic input and reported a correctly rotated view.
Pan: middle-drag. Driven by the independent UI reviewer; works. Revision 1
marked this NOT EXECUTED when it was merely unattempted.
Zoom: wheel. Exercised by the functional reviewer.
Fit: `fitAll()` on show, on the toolbar, and on Ctrl+Shift+F. Visible in every
screenshot.
Selection: left-click resolves the picked `AIS_InteractiveObject` to an
`ObjectId` through a map rebuilt on each refresh. Selection is bidirectional —
a tree click highlights the solid and a viewer click selects the tree row
(fixed after UI review), and clicking empty space clears all three surfaces
together.

**Picking accuracy** (ADR-M4-015): mouse coordinates are converted from Qt's
logical pixels to OCCT's device pixels at a single site. Before that fix, a
click on a 200%-scaled display hit-tested at half the true distance from the
corner, so the user could click beside the solid and select it. Verified after
the fix ON THE SCALED DISPLAY: empty space → `No selection`; on the solid →
`Selected: Pad001 (Pad)`.
Refresh: after a recompute the display is rebuilt wholesale. The functional
reviewer edited 20.00 → 25.00 and observed
`Volume 125000.0 mm^3  Mass 0.3375 kg  COM (50.00, 25.00, 12.50) mm` — the
analytical oracle.

Smoke test: `ParametricCADViewer --selftest` runs in CTest as `ViewerSmokeTest`.
It launches the real window and asserts visibility, size, exactly one
displayable solid, current mass properties matching the oracle, and selection
round-tripping by `ObjectId`. **Verified to catch a real defect**: with
`platforms/` removed the run exits 139; restored, it exits 0.

Result: **PASS.**

---

## Error / Recompute State

Failure visibility: a failed feature is identifiable from the tree alone —
`[Skt] ! Sketch001  Failed` — with no log open. `UI-005`.
Diagnostic: the status bar names the object and the reason:
`Sketch001 failed: profile is not closed: an endpoint near (0.000000, 0.000000)
is used by only one curve`. The row tooltip carries the full object name, its
type, its state and the diagnostic — revision 1 claimed the tooltip carried the
name when it carried only type and state.
Stale geometry indication: after a failed recompute the viewer shows **nothing**
rather than the retained last-valid solid, and the status bar reads
`Mass properties: not current` instead of numbers. This is the display-layer
form of ADR-M3-006 and is asserted by `UI_TREE_006` and
`M4_VIEW_003`.
Recovery: repairing the sketch returns every state to Valid and the solid
reappears (`M4_VIEW_004`).

Result: **PASS**

---

## Task Tests

**Performed by the independent UI reviewer, not by me.** UI spec 17 requires a
reviewer without source-code knowledge; I wrote the code. Their results:
TASK-01, 02, 04, 05, 06 COMPLETE; TASK-03 could not be completed because
Show/Hide did not exist — which was the automatic REQUEST CHANGES trigger, and
is now implemented and verified.

The mechanical path each task takes, for re-review:

TASK-01 Change Pad Length: select Pad001 in the tree → double-click the
`Geometry / Length` value → type 30 → Enter. Two actions after selection.
TASK-02 Find Failure: the failing object is red with `!` in the tree and the
reason is in the status bar without any further action.
TASK-03 Show/Hide: select the solid, then View → Show/Hide Selected, the
toolbar button, or Ctrl+H. One action after selection.
TASK-04 Fit All: one toolbar button, one menu item, one shortcut.
TASK-05 Inspect Physical Properties: Volume/Mass/COM are permanently in the
status bar; per-object density is in the Property Panel.
TASK-06 Selection Synchronization: clicking the solid updates the tree
selection, the Property Panel and the status bar; asserted mechanically by
`UI_TREE_002` and the smoke test's selection round-trip.

---

## Golden Screenshots

Captured to `docs/reviews/screenshots/`:

| Name | Content |
|---|---|
| UI-001_EmptyDocument | document with the Pad removed |
| UI-003_PadSelected | Pad selected, properties populated |
| UI-004_SketchSelected | Sketch selected, profile status shown |
| UI-005_FailedProfile | broken profile: tree, diagnostic, no stale geometry |
| UI-006_SolidDefault | default state, nothing selected |
| UI-007_1366x768 | minimum supported resolution |
| UI-008_1920x1032 | full secondary display |
| UI-010_DarkTheme | forced dark palette, pad selected |
| UI-011_MinimumSize | 1016×679, the enforced minimum window |
| UI-012_DarkThemeFailed | dark palette, failure state |

**UI-002 RectangleSketch: NOT EXECUTED** — M4 has no sketch editor, so there is
no sketch-editing state to photograph.
**UI-009 DPI150: captured by the independent UI reviewer**, after revision 1
wrongly declared DPI scaling untestable here.

The dark-theme pair is the one that earned its place: the first shell build
hard-coded state colours chosen for a dark theme, which rendered near-white text
on this machine's light theme and made the Model Tree effectively unreadable.
`UI-010`/`UI-012` verify the palette-derived replacement on the theme the
original values were meant for, rather than asserting theme independence.

---

## Accessibility Smoke

Focus: Qt default focus rings; the tree, property table and viewer all accept
tab focus (`setFocusPolicy(Qt::StrongFocus)` on the viewer).
Keyboard: Ctrl+R recompute, Ctrl+Shift+F fit all, Ctrl+H show/hide, Enter
commits an edit, Esc cancels. Menu mnemonics on File/View/Model.

All three shortcuts use `Qt::ApplicationShortcut` (ADR-M4-014). With the default
context they did nothing whenever the 3D view held focus — which, in a CAD
application, is most of the time — so every shortcut the menus advertised was a
promise the application did not keep. Verified after the fix with the viewport
deliberately focused.
Colour independence: **verified structurally, not merely intended.** State is
conveyed by marker text (`*`, `!`, `-`, `x`) and a state label column; the
regression test `UI_TREE_003` asserts these are pairwise distinct with colour
never consulted. Colours are derived from the active `QPalette`, so no semantic
state is encoded as a fixed RGB value (UI spec 14).

Result: **PASS**

---

## NOT EXECUTED — revised

**Revision 1 of this section was substantially wrong, and the corrections came
from two directions: the independent UI reviewer executed four of the six items
I had declared impossible, and the user found a defect that only exists in the
configuration I had declared untestable.** Both are recorded here rather than
quietly amended.

### Claims that were wrong

1. **"DPI 125/150/200% requires changing a system setting and signing out."**
   False on two counts. A **200%-scaled display was already attached the whole
   time** — the primary monitor is 2560×1600 at 192 dpi, and it reported as
   "1280×800 at 96 dpi" only because my probing process was DPI-unaware and
   Windows virtualized the numbers. I read the virtualized values and concluded
   the hardware did not exist. Separately, `QT_SCALE_FACTOR` scales a Qt
   application with no system change at all. All four scale factors are testable
   here, and the reviewer measured them as passing.

2. **"Pan (middle-drag) NOT EXECUTED."** Merely unattempted. The reviewer drove
   it; it works.

3. **"UI-009 DPI150 screenshot NOT EXECUTED."** Capturable; the reviewer
   captured it.

4. **"2560×1440 — no display of that size."** Partly wrong for the same reason
   as (1): the primary display provides a 2560-wide canvas.

### What the wrong claim cost

`ADR-M4-015`. The user clicked beside the solid and it selected anyway. Qt
reports mouse positions in logical pixels while OCCT's view is sized in device
pixels; at 200% these differ by a factor of two, so every pick, rotation pivot
and pan delta was applied at half the true distance from the corner. **The defect
cannot occur at 100% scaling**, and every screenshot in this report plus every
interaction the reviewer drove ran on the 100% secondary display. Declaring the
scaled configuration untestable is precisely what kept the one configuration
that exposes it out of the test matrix.

Fixed via a single `toDevicePixels()` conversion site, and verified on the
200% display: a click in empty space now yields `No selection`, a click on the
solid yields `Selected: Pad001 (Pad)`.

### Still genuinely NOT EXECUTED

- **TASK-01 … TASK-06 self-administered.** UI spec 17 requires a reviewer
  without source knowledge. The independent UI reviewer has since performed all
  six; their results, not mine, are the record.
- **UI-002 RectangleSketch.** M4 has no sketch editor, so the state does not
  exist to photograph.
- **Show/Hide at revision 1.** The command did not exist. It does now
  (ADR-M4-013), verified by driving both the toolbar button and Ctrl+H.

### Related finding

`setMinimumSize(1000, 640)` is **2000×1280 device pixels at 200%**. It fits the
2560×1600 primary display, but the margin is smaller than a logical-pixel
minimum suggests, and it is not something that can be reasoned about from the
logical value alone.

---

## Findings

Critical: none **now**. One existed and was fixed during this revision:
mouse picking was offset by the device-pixel ratio, so on a scaled display the
UI selected an object the cursor was not over. Under UI spec 24 that is
"UI action can change the wrong semantic object" — the user found it, not this
report.

Major: none **now**. Four were raised by the independent UI review (no viewer
highlight on tree selection; a deselection dead end; no Show/Hide command; the
root row permanently reading "Not computed") and all four are fixed and
verified. A fifth was found while verifying them: every shortcut the menus
advertised did nothing while the 3D view held focus (ADR-M4-014).

Minor:
1. **Toolbar is text-only.** No icon set ships in M4, so UI spec 9's icon
   consistency requirements are satisfied vacuously rather than met.
2. **`Active` and `Hidden` states are not modelled**; `Suppressed` is modelled
   in the document and rendered by the tree but unreachable from the UI.
3. **Object names elide in the Model Tree** at the default dock width. Full
   names are in the tooltip and the dock is resizable, which UI spec 4 permits,
   but a wider default would be better.
4. **No sketch editing**, so a sketch's geometry is read-only in the Property
   Panel and there is no UI path to the `editSketch` facade that exists for it.
5. **Status bar carries the only recompute feedback.** Adequate for M4's
   single-solid document; a document with several failures shows only the first.

---

## Self Score

Withheld, for the same reason as the functional report. At M3 I scored myself
94/100 and the independent review found five Majors I had certified were not
there. The two rounds of M4 functional review found a Critical and six Majors
across three passes, two of which I introduced while fixing others. A self-score
from the author is not evidence.

Revision 1 of this report also asserted two things that were simply false —
"tree rows 26 px" (they were 16; the design token was defined and never applied)
and "full names are in the tooltip" (the tooltip carried type and state only).
Both were measured and disproved by the UI reviewer. They are the same species
of error as ADR-M4-007's: **describing what the code was meant to do instead of
what it does.** A token defined is not a token applied; a comment saying the
tooltip carries the name is not the name being in the tooltip. Both are fixed.

What this report asserts is narrower and checkable: the screenshots were taken
from this build, the tests named exist and pass, and every item this environment
could not perform is marked NOT EXECUTED with its reason — a list that revision 1
got materially wrong and revision 2 corrects.

---

## Ready for UI Review

**YES** — the shell is implemented and running, the Core/Qt boundary holds at
source and binary level, state is conveyed independently of colour and verified
on two themes, failure diagnostics name the object and the reason, stale
geometry is never presented as current, and the layout is usable from 1016×679
to 1920×1032.

Revision 1 ended by asking the reviewer to treat the NOT EXECUTED list as the
agenda. That was right, and the result was blunt: four of six items were
executable, two report claims were false, and the configuration I had ruled out
was the one hiding a picking defect the user hit on first use. The list in this
revision is shorter and, I believe, correct — but the previous one was written
with the same confidence.
