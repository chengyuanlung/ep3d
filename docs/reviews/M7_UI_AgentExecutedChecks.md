# M7 UI — Agent-Executed Checks (NOT owner validation)

> **This document is NOT owner manual validation and must never be cited as
> one (ADR-M4-016).** It records an agent driving the running application
> through the mechanical steps of `M7_UI_UserValidation.md` with a GUI
> automation tool, reading pixels and widget text back.
>
> **What this can establish:** that a step is executable, that a widget shows
> the value the model holds, that an edit propagates to the 3D view, that a
> control exists and responds. It has found real defects before — the viewer
> never extruding the imported sketch was found exactly this way, and no test
> in the suite could see it.
>
> **What this can NOT establish**, and why the owner rows stay blank: every
> judgement row. A6/A7 ("are those two numbers understandable?"), C3 ("would
> you be misled?"), E3 ("would you have NOTICED?"), F4 ("could you tell which
> constraint to remove?"), G4 (readability), H1/H2 (display scaling on the
> owner's actual display). An agent asserting "yes, this is understandable"
> is asserting its own expectation back at itself. M6.14 — ten labels, zero
> values, invisible to 561 passing tests — is what that failure mode looks
> like.
>
> `M7_UI_UserValidation.md` stays blank until a person runs it.

**Reviewed build:** `9e0c399`, `build\Debug\ParametricCADViewer.exe`.
**Status:** NOT YET EXECUTED — this file is the runbook; results are filled in
by the run, and rows that are not executed stay empty rather than assumed.

---

## Environment preconditions (each must be recorded, not assumed)

| # | Precondition | Result |
|---|---|---|
| P1 | Keyboard layout forced to en-US before any typing (the IME sends keystrokes to a composition buffer, so a typed Width silently does nothing) | |
| P2 | Monitor selected explicitly; screenshot region confirmed to contain the app window | |
| P3 | The build under test is `9e0c399` (viewer reports its sample/selftest normally) | |
| P4 | Display configuration recorded verbatim (resolution + scaling) — ADR-M4-015: a UI verified on one configuration has been verified on ONE configuration | |

## Mechanically checkable rows (agent executes)

Test A: A1 (status bar text), A2 (tree row exists), A3 (row count = 11),
A4 (**every panel label has a non-empty value beside it** — M6.14's own test,
read through the widget), A5 (Solved / DOF 0), A6 (From source 2 / Inferred 9
as displayed text), A8 (Skipped 0), A9 (Width 100 / Height 50 visible),
A10 (volume 100000).

Test B: B1 (type 120, Enter), B3 (no modal appeared), B4 (rendered solid
changes — pixel diff of the 3D viewport), B5 (volume 120000), B6 (Solved /
DOF 0), B8 (Height 80 → 192000). **Plus the discriminator the checklist
itself records:** COM x moves 50 → 60, which is what distinguishes the
imported box from the demo box (both are 100×50×20 before the edit).

Test C: C1, C2 (Radius 10 / Diameter 30 present), C4 (radius edit changes the
right circle), C5 (**Diameter 60 → radius 30, not radius 60** — the checklist
calls a wrong result here a Critical).

Test D: D1, D2 (From source 0, Inferred > 0), D4 (not reported failed).

Test E: fixture must be constructed first (duplicate the first LINE block);
E1 (import succeeds), E2 (status bar says something was not reconstructed),
E4 (Skipped 1 + a Skipped item row with a reason), E6 (**no Width parameter
invented**).

Test F: F1 (`--sample m5-conflict`), F2 (`!` marker present, not colour
alone), F3 (offending constraint IDs named), F5 (Pad reads Blocked).

Test G: G1 (tree click → panel follows), G2 (3D click selects), G3 (Fit All),
G5 (`--dark`: Reconstruction rows still show values — readable-to-a-human is
G4 and stays with the owner).

## Rows this agent will NOT fill (owner judgement)

A7, B2, B7 (partly — "never shows the old shape while claiming it is current"
is checkable only for the transitions the agent thinks to try), C3, D3, D5,
E3, E5, F4, F6, G4, H1, H2.

---

## Results

*(filled in by the run; empty = not executed)*

| Test | Row | Observed | Verdict |
|---|---|---|---|
| | | | |

## Defects found by this run

| # | File | Step | What happened | What was expected | Severity |
|---|---|---|---|---|---|
| | | | | | |

---

**Executed by:** an agent, mechanically. **Not** owner validation
(ADR-M4-016). The owner checklist at `M7_UI_UserValidation.md` remains
unexecuted regardless of anything recorded here.
