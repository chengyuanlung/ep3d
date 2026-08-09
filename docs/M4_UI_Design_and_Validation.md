# EP3D M4 — Qt UI Design & Validation Specification

**Purpose:** Define a consistent CAD UI style and an objective validation process for EP3D M4.  
**Applies to:** Qt 6 application shell, Model Tree, Property Panel, 3D Viewer, toolbars, status feedback and M4 workflow UI.  
**Architecture baseline:** Core must remain independent of Qt and OCCT visualization APIs.

---

# 1. UI Mission

M4 UI is not a visual-polish milestone. Its job is to establish a professional, stable CAD interaction shell that can grow into Sketch constraints, DXF workflows, Parts and Assembly without redesigning the entire application.

Primary layout:

```text
┌──────────────────────────────────────────────────────────────┐
│ Menu                                                         │
├──────────────────────────────────────────────────────────────┤
│ Main Toolbar                                                 │
├─────────────────┬──────────────────────────┬─────────────────┤
│ Model Tree      │                          │ Properties      │
│                 │                          │                 │
│ Part            │                          │ Object Name     │
│ ├─ Sketch001    │        3D Viewer         │ Placement       │
│ ├─ Pad001       │                          │ Length          │
│ └─ ...          │                          │ Material        │
│                 │                          │                 │
├─────────────────┴──────────────────────────┴─────────────────┤
│ Status / Selection / Coordinates / Recompute State           │
└──────────────────────────────────────────────────────────────┘
```

The 3D Viewer is the dominant work area.

---

# 2. Design Principles

The UI must prioritize:

1. CAD information density without clutter.
2. Immediate visibility of the active object and current selection.
3. Fast property editing.
4. Consistent interaction and state feedback.
5. Minimal modal dialogs.
6. Keyboard-friendly numeric editing.
7. High-DPI correctness.
8. Clear distinction between semantic document state and transient viewer state.
9. Future compatibility with Sketch, Assembly and robot/tool coordinate workflows.
10. Visual consistency over decorative effects.

Avoid oversized consumer-app controls, excessive rounded cards, decorative gradients, animation-heavy transitions and large unused spacing.

---

# 3. Design Tokens

Create centralized UI constants/theme definitions rather than scattered hard-coded values.

Recommended baseline:

```text
Spacing:
4 px   micro
8 px   standard
12 px  grouped
16 px  section
24 px  major separation

Toolbar icon:
20–24 px logical size

Property row:
28–32 px logical height

Tree row:
24–28 px logical height

Minimum left Model Tree:
220 px

Recommended right Property Panel:
260–340 px

Status bar:
22–26 px

Control corner radius:
small/subtle only

Borders:
1 logical px where separation is needed
```

Qt logical pixels should scale correctly under device pixel ratio.

Do not hard-code physical pixel assumptions for 100% DPI only.

---

# 4. Typography

Use the platform/system UI font unless the project intentionally adopts a redistributable UI font.

Rules:

```text
Normal UI text        regular
Panel section heading medium/semibold
Object name           regular/medium
Numeric property      tabular digits if available
Error                  not represented by font/color alone
```

Avoid using more than approximately three font sizes in the normal application shell.

Required validation:

- no clipped text at 100%, 125%, 150%, 200% scaling,
- long object names elide or resize predictably,
- property values remain readable,
- units do not collide with values.

---

# 5. Main Window Layout

Recommended Qt structure:

```text
QMainWindow
├─ MenuBar
├─ Main Toolbar
├─ Left Dock: Model Tree
├─ Central: Viewer
├─ Right Dock: Property Editor
└─ StatusBar
```

Dock panels must be resizable.

The application must remain usable at:

```text
1366 × 768
1920 × 1080
2560 × 1440
4K / high DPI
```

Do not allow side panels to consume most of the Viewer by default.

---

# 6. Model Tree

The Model Tree represents semantic document structure.

Example:

```text
Part001
├─ Sketch001
├─ Pad001
└─ MassProperties
```

Future:

```text
Assembly001
├─ Part001
├─ Part002
└─ Joints
```

Required states must be visually distinguishable:

```text
Normal
Selected
Active
Hidden
Suppressed
Dirty
Failed
```

Do not communicate these states only with color.

Use a combination of icon/state marker/text treatment/tooltips.

Tree selection must correspond to the object shown in the Property Panel.

A transient 3D face highlight must not silently change persistent semantic selection.

---

# 7. Property Panel

The Property Panel is the primary numeric-editing surface.

Example:

```text
Pad001

General
  Name            Pad001

Geometry
  Length          20.000 mm

Placement
  X                0.000 mm
  Y                0.000 mm
  Z                0.000 mm

Material
  Material         Aluminum
  Density          2700 kg/m³
```

Rules:

- property names aligned consistently,
- numeric value and unit clearly separated,
- editable vs read-only state obvious,
- invalid entry receives immediate feedback,
- Enter commits,
- Esc cancels current edit where practical,
- normal edits should not require modal confirmation,
- changing one property must not unexpectedly change selection,
- recompute result must be visible.

Preferred workflow:

```text
Select Pad001
→ click Length
→ type 30
→ Enter
→ recompute
→ Viewer refresh
```

Avoid:

```text
Select → Open dialog → Advanced dialog → Enter → Apply → OK → Close
```

---

# 8. Units

Never present ambiguous naked engineering values when a unit is relevant.

Examples:

```text
20.000 mm
90.000 deg
0.270 kg
2700 kg/m³
```

The UI may later support unit systems, but M4 must use a single documented convention consistently.

A wrong displayed unit or value/unit mismatch is a Critical UI defect.

---

# 9. Toolbar

M4 toolbar should remain small.

Suggested commands:

```text
New/Open/Save (if already available)
Fit All
View orientation controls if implemented
Create Sketch
Pad
Show/Hide
```

Do not expose every future command.

Requirements:

- consistent icon visual language,
- tooltip for every non-obvious icon,
- disabled commands visibly disabled,
- toolbar order follows workflow,
- destructive actions visually distinct,
- icon-only actions must be discoverable through tooltip/menu.

---

# 10. 3D Viewer Interaction

M4 minimum:

```text
Rotate
Pan
Zoom
Fit All
Whole-object selection
Selection highlight
Recompute refresh
```

Mouse mapping must be documented and remain stable.

Selection requirements:

- hover/preselection and committed selection must be distinguishable if both exist,
- selected object in Viewer maps to the correct `ObjectId`,
- Model Tree and Property Panel synchronize,
- Viewer selection does not transfer semantic ownership,
- deleting or recomputing an object must not leave stale UI selection referencing destroyed runtime geometry.

---

# 11. UI State Vocabulary

Define one consistent state vocabulary.

At minimum:

```text
Normal
Hover / Preselected
Selected
Active
Dirty / Needs recompute
Computing
Failed
Hidden
Suppressed
Disabled
```

`Failed` must not look the same as `Selected`.

`Dirty` must not look the same as `Failed`.

`Hidden` and `Suppressed` are semantically different and should not be represented identically.

Never rely on red/green alone.

---

# 12. Error Feedback

Errors should be actionable.

Bad:

```text
Operation failed.
```

Better:

```text
Pad001 failed: Sketch001 profile is open near E17.End.
```

UI should surface:

```text
affected object
failure category
useful diagnostic
```

A failed feature should be identifiable in the Model Tree without opening logs.

Status bar may show the latest concise message; detailed diagnostic can appear in tooltip/property/diagnostic area.

---

# 13. Recompute Feedback

M4 recompute is normally fast, so avoid intrusive progress dialogs.

After editing:

```text
value commit
→ dirty state
→ recompute
→ success/failure state
→ Viewer refresh
```

The UI must not show stale geometry as if it were current after a failed recompute.

If M3/M4 policy retains last-valid geometry, its stale/failed status must be visually clear.

---

# 14. Dark / Light Themes

M4 should avoid theme assumptions even if only one theme is officially shipped initially.

Validate at least the primary theme plus a basic alternate-theme smoke test if practical.

Requirements:

- readable text,
- selected object visible,
- disabled text readable,
- error/warning states readable,
- Viewer background does not destroy edge visibility,
- icons remain understandable.

Do not encode semantic state solely by a specific hard-coded RGB value.

---

# 15. High-DPI Validation

Mandatory Windows scaling checks:

```text
100%
125%
150%
200%
```

Inspect:

- toolbar icons,
- menu height,
- dock titles,
- Model Tree rows,
- Property rows,
- numeric editors,
- units,
- status bar,
- Viewer overlays,
- dialogs used by M4.

Failures include clipping, overlapping, tiny icons, oversized icons, truncated units and misaligned controls.

---

# 16. Golden Screenshot Set

Create a stable screenshot checklist.

Recommended names:

```text
UI-001 EmptyDocument
UI-002 RectangleSketch
UI-003 PadSelected
UI-004 PropertyEditing
UI-005 FailedProfile
UI-006 SelectedSolid
UI-007 1366x768
UI-008 1920x1080
UI-009 DPI150
UI-010 AlternateTheme
```

Screenshots are review evidence, not substitutes for semantic tests.

If automated screenshot comparison is introduced, allow controlled tolerance for platform/font/rendering differences. Do not make fragile pixel-perfect comparison the only gate.

---

# 17. Task-Based Usability Validation

Reviewer must complete these tasks without source-code knowledge.

### TASK-01 — Change Pad Length

```text
Find Pad001
Change Length 20 → 30 mm
Confirm updated solid
```

Measure:

```text
completion
number of actions
discoverability
feedback
ambiguity
```

### TASK-02 — Find Failure

Break a Sketch profile and identify which object failed and why.

### TASK-03 — Show/Hide

Hide and show the Pad/solid.

### TASK-04 — Fit All

Recover a useful camera view.

### TASK-05 — Inspect Physical Properties

Select the relevant object and find Volume/Mass/COM where exposed.

### TASK-06 — Selection Synchronization

Select object in Viewer and verify Tree/Property Panel refer to the same semantic object.

No task should require knowledge of internal ObjectIds or developer terminology.

---

# 18. Workflow Efficiency Targets

These are guidance, not arbitrary hard performance laws.

For common operations:

```text
Select object → edit common property:
prefer 1 selection + 1 property edit

Fit All:
one command

Show/Hide selected:
one command/menu action

Identify failed feature:
visible from tree without opening developer logs
```

Reviewer should flag unnecessary modal steps or repeated navigation.

---

# 19. Accessibility / Robustness

M4 minimum:

- semantic state not conveyed by color alone,
- keyboard focus visible,
- tab navigation reasonable in Property Panel,
- controls have meaningful tooltips/accessibility labels where practical,
- contrast is sufficient for normal CAD work,
- disabled and read-only states are distinguishable.

Full formal accessibility certification is not an M4 goal.

---

# 20. UI Architecture Boundary Tests

Verify:

```text
Core does not include Qt
Core does not depend on QWidget/QObject
Viewer does not own/delete semantic document objects
Property Panel edits model through controlled application/document API
Tree items map to stable ObjectId
Viewer selection maps to stable ObjectId
runtime OCCT presentation objects are not serialized
```

A Qt signal may exist in App/UI, but Core must not require Qt signals to function.

---

# 21. UI Self-Validation Protocol

Codex must create:

```text
docs/reviews/M4_UI_SelfValidationReport.md
```

Run:

```text
1. Design-system token audit
2. Core/Qt boundary scan
3. Main-window layout inspection
4. Model Tree state inspection
5. Property editing workflow
6. Viewer interaction smoke test
7. Selection synchronization
8. Failure-state test
9. Recompute refresh test
10. 1366x768 layout
11. 1920x1080 layout
12. DPI 100/125/150/200 where environment permits
13. primary theme
14. alternate-theme smoke test where practical
15. task-based usability tests
16. Golden Screenshot capture/review
17. keyboard/focus smoke test
18. self-score
```

If the execution environment cannot perform a specific visual/DPI/manual action, report `NOT EXECUTED` with reason. Never fabricate a PASS.

---

# 22. UI Self-Validation Report Format

```text
# M4 UI Self-Validation Report

Commit:
OS:
Qt:
Resolution:
DPI:
Theme:

## Design System
Spacing:
Typography:
Icons:
Rows:
Result:

## Architecture
Core Qt scan:
Viewer ownership:
ObjectId mapping:
Result:

## Layout
1366x768:
1920x1080:
High DPI:
Result:

## Model Tree
Selection:
Active:
Dirty:
Failed:
Hidden:
Suppressed:
Result:

## Property Panel
Numeric edit:
Units:
Validation:
Enter commit:
Result:

## Viewer
Display:
Rotate:
Pan:
Zoom:
Fit:
Selection:
Refresh:
Result:

## Error / Recompute State
Failure visibility:
Diagnostic:
Stale geometry indication:
Recovery:
Result:

## Task Tests
TASK-01:
TASK-02:
TASK-03:
TASK-04:
TASK-05:
TASK-06:

## Golden Screenshots
UI-001:
...
UI-010:

## Accessibility Smoke
Focus:
Keyboard:
Color independence:
Result:

## Findings
Critical:
Major:
Minor:

## Self Score
XX / 100

## Ready for UI Review
YES / NO
```

---

# 23. Independent UI Reviewer Scorecard

```text
Visual hierarchy                 12
Spacing / alignment              12
Typography / readability          8
Icon / command consistency        8
CAD information density          10
Model Tree clarity               10
Property editing                 12
Viewer interaction/selection     10
State/error/recompute feedback   10
High-DPI/theme/accessibility      8
                                ---
                                100
```

Decision:

```text
90–100 APPROVE
80–89  APPROVE WITH MINOR FOLLOW-UP
<80    REQUEST CHANGES
```

Automatic REQUEST CHANGES:

- wrong unit/value presentation,
- selection indicates a different semantic object than the operation target,
- UI can mutate/delete the wrong object because selection state is inconsistent,
- failed recompute is displayed as current success,
- Core gains a Qt dependency,
- normal M4 workflow becomes unusable at 1366x768,
- severe DPI clipping prevents editing,
- required viewer/property workflow cannot be completed.

---

# 24. Severity Examples

## Critical

```text
Displayed mm value is actually interpreted as meters
Viewer selects PartA but Property Panel edits PartB
Failed Pad appears valid/current
UI action can delete/change wrong semantic object
```

## Major

```text
150%/200% DPI clips editable values
active Sketch state is unclear
common Pad Length edit requires unnecessary modal workflow
Model Tree and Viewer frequently lose synchronization
error only exists in developer console
Dark/alternate theme makes selection unreadable
```

## Minor

```text
small alignment inconsistency
non-critical icon style mismatch
slightly excessive spacing
tooltip wording inconsistency
```

---

# 25. Independent UI Review Output

```text
# M4 UI Independent Review

Commit:
Environment:
Resolution:
DPI:
Theme:

Decision:
APPROVE | APPROVE WITH MINOR FOLLOW-UP | REQUEST CHANGES

Score:
XX / 100

## Critical Findings
...

## Major Findings
...

## Minor Findings
...

## Visual Hierarchy
...

## Layout / Spacing
...

## Model Tree
...

## Property Panel
...

## Viewer
...

## State Feedback
...

## DPI / Theme
...

## Task Results
TASK-01:
...
TASK-06:

## Screenshot Review
UI-001:
...
UI-010:

## Required Changes
...

## M4 UI Readiness
READY | NOT READY
```

---

# 26. Integration With M4 Completion Gate

M4's functional Reviewer and UI Reviewer are separate concerns.

Recommended completion logic:

```text
M4 Core/Geometry Review
        PASS
          +
M4 UI Review
        PASS
          ↓
M4 COMPLETE
```

A beautiful UI cannot compensate for broken geometry architecture, and correct geometry cannot compensate for a UI that edits the wrong object.

For M4, UI review is required for the viewer/application-shell portion. If the environment cannot execute visual review, M4 Completion Report must explicitly list that limitation rather than claiming full UI validation.

---

# 27. Codex UI Prompt

```text
Implement and validate the EP3D M4 Qt UI according to:
docs/M4_UI_Design_and_Validation.md

Read AGENTS.md and the main M4 implementation specification first.

Goals:
- professional CAD-style QMainWindow shell,
- dominant 3D Viewer,
- left Model Tree,
- right Property Panel,
- concise toolbar/status bar,
- stable ObjectId synchronization between Tree, Viewer and Properties,
- fast numeric property editing,
- clear Normal/Selected/Active/Dirty/Failed/Hidden/Suppressed states,
- useful failure diagnostics,
- recompute/view refresh correctness,
- rotate/pan/zoom/fit/select,
- high-DPI-safe layout,
- Core remains completely Qt-free.

Do not spend M4 on decorative polish or a full Sketch editor.

Create centralized design tokens/theme constants rather than scattering spacing/icon/row-size values.

Run the UI Self-Validation Protocol and create:
docs/reviews/M4_UI_SelfValidationReport.md

Capture/review the Golden Screenshot set when the environment supports screenshots.

Run an independent UI Reviewer if supported. The reviewer must use the scorecard and task-based tests in this specification.

Never claim a visual/DPI/manual test passed if it could not actually be executed. Mark it NOT EXECUTED with the reason.

Any selection mismatch, wrong engineering unit, stale failed result shown as current, or Core Qt dependency blocks approval.

Report the final UI score, Critical/Major/Minor findings and M4 UI readiness.
```

---

# 28. Final UI Checklist

```text
[ ] centralized design tokens
[ ] CAD-density main layout
[ ] Viewer is dominant area
[ ] resizable Model Tree
[ ] resizable Property Panel
[ ] Tree ↔ ObjectId mapping
[ ] Viewer ↔ ObjectId mapping
[ ] Property Panel edits correct object
[ ] numeric units explicit
[ ] Enter-to-commit common numeric edit
[ ] Normal state
[ ] Selected state
[ ] Active state
[ ] Dirty state
[ ] Failed state
[ ] Hidden state
[ ] Suppressed state
[ ] states not color-only
[ ] useful failure diagnostic
[ ] stale failed geometry visually distinguished
[ ] rotate
[ ] pan
[ ] zoom
[ ] fit all
[ ] select/highlight
[ ] recompute refresh
[ ] Core free of Qt
[ ] Viewer does not own semantic objects
[ ] 1366x768 usable
[ ] 1920x1080 usable
[ ] DPI 100% checked
[ ] DPI 125% checked where possible
[ ] DPI 150% checked where possible
[ ] DPI 200% checked where possible
[ ] primary theme checked
[ ] alternate theme smoke where possible
[ ] keyboard/focus smoke
[ ] TASK-01 through TASK-06
[ ] Golden Screenshot set reviewed
[ ] M4_UI_SelfValidationReport
[ ] independent UI review where supported
[ ] no Critical UI finding
[ ] no unresolved Major UI finding
[ ] UI Reviewer score >= 80

Only then:
M4 UI = READY
```
