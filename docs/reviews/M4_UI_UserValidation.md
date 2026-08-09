# M4 UI User-Assisted Validation

**This is owner/user manual validation, NOT an independent agent review.**
Recorded per `docs/M5_UI_User_Assisted_Validation_Guide.md`, whose workflow is
applied here to M4 because M4's outstanding gap is exactly the visual and
interactive validation a developer cannot perform on their own work
(guide §17, §18).

Build/Commit: `fcfd0b4` on `m4-wip`, plus the uncommitted sample generator
OS: Windows 11 Home 10.0.26200
Resolution: _recorded per group; the machine has a 2560×1600 primary at 200% and a 1920×1080 secondary at 100%_
Display Scale: _see DPI section_
Qt: 6.11.1

Samples are generated rather than committed as files
(`examples/README.md`), and each is additionally checked against its own
analytical value by `--selftest --sample <name>`.

### Deviation from the guide's sample set, and why

The guide's samples B and C assume under-constrained and conflicting sketch
states — M5 concepts, and M4 has no constraint solver. Its Test A edits sketch
Width, which M4's UI does not expose; only the Pad Length Parameter is editable.
Substitutes exercise the same property using something M4 can do:

| Guide | Here | Property under test |
|---|---|---|
| A (edit Width) | `m4-rectangle`, edit Pad Length | does a numeric edit reach geometry, mass and the 3D view? |
| B / C (under-constrained, conflict) | `m4-failed-profile` | is a broken state visible with a useful reason, and is a stale result ever shown as current? |
| D (circle R10→R20) | `m4-circle-r10` / `m4-circle-r20` | does a dimensional change give the analytically correct volume ratio? |

DOF and constraint-status checks are **NOT APPLICABLE** to M4, not skipped.

---

## Sample A — Rectangle Pad, numeric edit

`--sample m4-rectangle`. Initial 100 × 50 mm, Pad 20 mm, aluminium 2700 kg/m³.
Edit Pad Length 20 → 30 mm.

| Check | Result |
|---|---|
| Initial Volume 100000 mm³ / Mass 0.2700 kg / COM (50, 25, 10) mm | **PASS** |
| Length field discoverable in the Property Panel | **PASS** |
| Unit `mm` shown, separate from the value | **PASS** |
| No modal dialog during the edit | **PASS** |
| 3D updates on Enter | **PASS** |
| Solid grows in thickness, not width or length | **PASS** |
| Volume 150000 mm³ / Mass 0.4050 kg after the edit | **PASS** |
| Selection preserved across the edit | **PASS** |
| Tree / Properties / Viewer refer to the same object | **PASS** |

**Sample A: PASS.** Reported by the project owner.

Notes: the numeric result matches the analytical oracle exactly
(100 × 50 × 30 mm³ = 150000 mm³; 2700 kg/m³ × 1.5e-4 m³ = 0.405 kg), and the
same values are asserted automatically by `--selftest --sample m4-rectangle`.

---

## Sample B — Failed profile

`--sample m4-failed-profile`. The same rectangle with the top edge missing, so
the profile is open and the Pad cannot build.

| Check | Result |
|---|---|
| Tree identifies the failing object at a glance | **PASS** |
| Failure not conveyed by colour alone (`!` marker + "Failed" text) | **PASS** |
| Dirty (`*`) visually distinct from Failed (`!`) | **PASS** |
| Status bar names the object AND the reason, no log needed | **PASS** |
| Tooltip carries full name, type, state and diagnostic | **PASS** |
| Property Panel exposes the profile diagnostic | **PASS** |
| 3D viewport empty — the retained last-valid solid is NOT drawn | **PASS** |
| Fit All / Recompute / rotate / zoom cannot make the stale result look current | **PASS** |
| No crash | **PASS** |

**Sample B: PASS.** Reported by the project owner.

Notes: the last two checks are the ones UI spec §24 classes as Critical
("failed recompute displayed as current success"). The owner exercised four
separate routes — fit, recompute, rotate, zoom — and the viewport stayed empty
with `Mass properties: not current` throughout. This is the display-layer form
of ADR-M3-006, and it holds.

---

## Sample D — Circle, dimensional ratio

`--sample m4-circle-r10` then `--sample m4-circle-r20`, both Pad 30 mm. M4's UI
cannot edit a circle's radius (sketch geometry is not parameterised until M5),
so the dimensional change is compared across two samples rather than edited
live.

| Check | Result |
|---|---|
| r = 10 → Volume 9424.8 mm³ (π·100·30 = 9424.77796) | **PASS** |
| r = 20 → Volume 37699.1 mm³ (π·400·30 = 37699.11184) | **PASS** |
| Ratio is 4× — area scales with r² | **PASS** |
| Units mm³ / kg shown in both | **PASS** |
| Geometry reads as a cylinder, not a polygon | **PASS** |
| r20 cylinder visibly thicker than r10 | **PASS** |
| Pad Length stays 30.000 mm in both — the radius change did not disturb it | **PASS** |
| COM z stays 15.00 mm in both — height unaffected | **PASS** |

**Sample D: PASS.** Reported by the project owner.

Notes: this is the only group exercising curved geometry, where OCCT computes
the volume rather than a polyhedral formula giving it exactly. The measured
ratio matching 4× to the displayed precision confirms the curved-surface path
agrees with the analytical result. The last two checks target the failure mode
guide §5 names specifically — a dimensional edit silently changing a different
dimension.

---

## Selection synchronization

`--sample m4-rectangle`. Six steps: select Sketch in tree, select Pad in tree,
click the solid in the viewer, click empty viewport background, re-select the
same tree row, then recompute with a selection held.

| Check | Result |
|---|---|
| Sketch selected — tree, properties, status agree | **PASS** |
| Pad selected — all three agree, and the 3D solid highlights | **PASS** |
| Viewer click → tree and properties follow | **PASS** |
| Empty-space click → all four surfaces clear together, no residual highlight | **PASS** |
| Re-selecting the same row after clearing works | **PASS** |
| Selection survives a Recompute | **PASS** |
| Never showed "tree says A while properties edit B" | **PASS** |

**Selection: PASS.** Reported by the project owner.

Notes: the last check is UI spec §23's automatic REQUEST CHANGES condition, and
the fifth was a Major in the first UI review — clicking empty space left the
tree highlighting the old object, and because re-clicking an already-current row
emits no signal, the user could not get back to it without clicking a different
row. Both are confirmed fixed by observation.

---

## Viewer — Show/Hide, Rotate, Pan, Zoom, Fit All

`--sample m4-rectangle`, Pad001 selected.

| Check | Result |
|---|---|
| Show/Hide removes and restores the solid | **PASS** |
| Hiding does NOT clear the mass numbers — visibility is view state only | **PASS** |
| Hidden state visible in the tree (`h` marker, "Hidden") | **PASS** |
| Rotate (left drag) | **PASS** |
| Pan (middle drag) | **PASS** |
| Zoom (wheel) | **PASS** |
| Fit All recovers a deliberately wrecked view | **PASS** |

**Viewer: PASS.** Reported by the project owner.

Notes: the second check is the one that distinguishes Hidden from Suppressed
(ADR-M4-013) — a hidden solid is still computed, still contributes its mass and
still serializes. Pan had never been driven by anyone before this round; the
first UI self-validation report marked it NOT EXECUTED.

---

## Failure / recovery through the UI

**NOT EXECUTED.** The owner chose not to run this group. No PASS is inferred for
it, and nothing elsewhere in this report substitutes for it.

What it would have covered (guide §11): typing `-5` into the Pad Length field
(expect Failed, solid gone, `not current`), typing `abc` (expect rejection with
a message and the committed value restored), then typing `20` (expect full
recovery to 100000 mm³).

What *is* covered by other evidence, and what is not:

- The underlying failure and recovery **semantics** are verified automatically
  and by Sample B: `M4_PAD_022` drives four invalid Pad lengths
  (0 / negative / NaN / infinity) and asserts the feature fails and the mass
  result stops being current; `M4_PAD_021` and Gate C verify deterministic
  recovery; Sample B confirmed by observation that a failed state cannot be made
  to look current through four different viewer actions.
- What remains unobserved is specifically the **UI input path**: whether typing
  an invalid value into the Property Panel is rejected sensibly, whether the
  message is understandable, and whether a recovering edit restores the display.
  The commit path is asserted by `UI_PROP_001` and the non-numeric branch exists
  in `MainWindow::onPropertyCommitted`, but no one has typed into that field and
  looked at the result.

This is a gap in observation, not a known defect.

---

## DPI / layout

The owner ran the validation on **both displays** — the 2560×1600 primary at
**200% scaling** and the 1920×1080 secondary at **100%** — and reported the
layout and interaction as good on both.

| Check | Result |
|---|---|
| 100% (1920×1080 secondary) | **PASS** |
| 200% (2560×1600 primary) | **PASS** |
| Text not truncated to the point of being unusable | **PASS** |
| Property values editable, units readable | **PASS** |
| 3D picking accurate at both scales | **PASS** |
| 150% | **NOT EXECUTED** — not attempted; would require changing the Windows display scale |

**DPI: PASS at 100% and 200%; 150% NOT EXECUTED.**

Notes: the 200% result is the one that matters most here. The picking defect the
owner found in ordinary use (ADR-M4-015) exists *only* at non-unity scaling —
Qt reports mouse positions in logical pixels while OCCT's view is in device
pixels — and every screenshot and every agent-driven interaction before that
report had run at 100%. Confirming picking on the 200% display is therefore the
observation that closes that finding, and it could not have been made by any
amount of testing on the secondary screen.

---

## User Findings

**Critical: none.** Every condition UI spec §24 classes as Critical was tested
and none reproduced: no wrong or naked unit, no case where the viewer showed one
object while the panel edited another, no route by which a failed recompute
could be made to look current, and no action that changed the wrong semantic
object.

**Major: none.**

**Minor: none reported.**

Not a finding, but recorded because it shaped this session: one Critical-class
defect *was* found by the owner earlier, outside this structured pass — clicking
beside the solid selected it, because Qt's logical mouse coordinates were passed
to OCCT unconverted (ADR-M4-015). It is fixed, and its confirmation is the
200%-display picking check above.

---

## Summary

| Group | Result |
|---|---|
| Sample A — rectangle Pad, numeric edit | **PASS** |
| Sample B — failed profile visibility | **PASS** |
| Sample D — circle dimensional ratio | **PASS** |
| Selection synchronization | **PASS** |
| Viewer — Show/Hide, rotate, pan, zoom, fit | **PASS** |
| DPI / layout — 100% and 200% | **PASS** |
| DPI — 150% | **NOT EXECUTED** |
| Failure / recovery through the UI | **NOT EXECUTED** |

Six groups passed by direct observation. Two were not executed and are recorded
as such rather than inferred.

---

## User Validation Result

**ACCEPTED WITH ONE UNOBSERVED GROUP.**

Everything the owner exercised passed, including all four Critical conditions
and the 200%-scaling picking behaviour that no amount of agent-driven testing on
the 100% display could have reached. The failure/recovery input path was not
exercised; its underlying semantics are covered by automated tests, but the UI
path itself is unobserved and is not claimed as passing.

Validated by: **project owner, manual validation.**

**This is user-assisted / owner manual validation. It is NOT an independent
agent review** (guide §4, §18). The independent UI reviewer's last recorded
verdict remains REQUEST CHANGES 79/100 against an earlier build; its four Major
findings have been fixed and are covered by regression tests, and the owner has
now confirmed by observation that the behaviours those findings concerned are
correct — but no independent reviewer has re-verified them, and this report does
not stand in for that.
