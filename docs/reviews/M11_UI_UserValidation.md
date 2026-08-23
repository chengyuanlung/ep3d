# M11 — Owner Manual UI Validation

**Status: NOT EXECUTED.** Every Result cell is blank and only the owner may fill
them (ADR-M4-016). Agent-executed checks exist — 19 `PropertyEditingTest` cases
and the `--selftest --sample m11-expression` assertions inside the running
window — and they are **not** this. They cannot fill a judgement row and must
never be cited as if they had.

What the agent checks CAN say: the Expression row exists, carries the right
text, refuses bad input with a column and a caret, keeps what you typed, and
drives the solid. What only you can say: whether any of that is **usable** —
discoverable without being told, readable at your DPI and theme, and worth the
keystrokes.

## How to start

```
build\Debug\ParametricCADViewer.exe --sample m11-expression
```

The document is a 100 × 50 rectangle padded by `PadLength`, and `PadLength` is
**driven by the expression `#PadBase / 2`** where `PadBase` is 40 mm. So the
solid should be 100 000 mm³, and the number 20 in the panel should be a
*computed* number, not one anybody typed.

Select **PadLength** in the model tree before starting.

## A — The panel says what is going on

| # | Do this | Expect | Result | Notes |
|---|---|---|---|---|
| A1 | Look at the Properties panel with PadLength selected | There is a row labelled `Value / Expression` showing `#PadBase / 2` | | |
| A2 | Look at the `Value / Value` row | It reads 20 (mm), and it looks read-only — visibly different from an editable row | | |
| A3 | Hover the `Value / Value` row | The tooltip says the value is driven, and NAMES `#PadBase / 2` | | |
| A4 | Hover the `Value / Expression` row | The tooltip explains what can be typed and shows examples | | |
| A5 | Select `PadBase` instead | Its Value row IS editable, and its Expression row is present and empty | | |
| A6 | **Without having read this document**, could you have found the Expression row and known it was editable? | Answer honestly — this is the discoverability question and nothing else in the project asks it | | |

## B — A good expression

| # | Do this | Expect | Result | Notes |
|---|---|---|---|---|
| B1 | Select PadLength, double-click the Expression cell, type `#PadBase / 4`, press Enter | The Value row becomes 10, the solid gets visibly thinner, the status bar says what changed | | |
| B2 | Check the status bar volume | 50 000 mm³ | | |
| B3 | Select PadBase, set its Value to 80, press Enter | PadLength follows to 20 and the solid thickens — **without touching PadLength** | | |
| B4 | Ctrl+Z | The PadBase edit is undone and PadLength follows back | | |
| B5 | Try `#PadBase / 2 + 3 mm` | Accepted; the value is the arithmetic you expect | | |
| B6 | Try `max(#PadBase / 4, 15 mm)` | Accepted | | |

## C — A bad expression, which is the half that matters

| # | Do this | Expect | Result | Notes |
|---|---|---|---|---|
| C1 | Type `#PadBase / #Nope` into the Expression cell and press Enter | REFUSED. The status bar names a **column number** and says no parameter is called Nope | | |
| C2 | Look at the Expression cell after C1 | **Your text is still there.** You do not have to retype it | | |
| C3 | Hover that cell | The tooltip shows three lines: what you typed, a `^^^^^` row under the offending characters, and the message | | |
| C4 | Is the caret pointing at the right characters? | It should sit under `#Nope`, not near it | | |
| C5 | Fix it to `#PadBase / 2` and press Enter | Accepted, and the panel goes back to normal | | |
| C6 | Try `3mm + 2deg` | Refused — a length and an angle cannot be added | | |
| C7 | Try `#PadBase * #PadBase` | Refused — that would be mm², which no field can hold | | |
| C8 | Try `#PadLength` (its own name) | Refused — an expression cannot read itself | | |
| C9 | Try typing a number into the locked `Value` row while an expression drives it | Refused, and the message tells you to clear the Expression row instead | | |

## D — Clearing, and getting back to a plain number

| # | Do this | Expect | Result | Notes |
|---|---|---|---|---|
| D1 | Clear the Expression cell (select all, Delete, Enter) | The expression is gone, the **value stays** at whatever it last computed | | |
| D2 | Look at the `Value` row now | It is editable again | | |
| D3 | Type 35 into Value, Enter | Accepted; the solid rebuilds | | |
| D4 | Ctrl+Z | The expression comes back AND starts driving the value again | | |

## E — Cycles

| # | Do this | Expect | Result | Notes |
|---|---|---|---|---|
| E1 | Set PadLength's expression to `#PadBase / 2` (if not already) | Accepted | | |
| E2 | Select PadBase and set ITS expression to `#PadLength * 2` | Refused, and the message shows the PATH of the cycle by name, not just the word "cycle" | | |
| E3 | Check PadLength still works after E2 | Change PadBase's value; PadLength must still follow | | |

## F — The things a test cannot judge

| # | Question | Result | Notes |
|---|---|---|---|
| F1 | Is the Expression cell wide enough to read a real expression, or does it elide too early? | | |
| F2 | At your display scaling, is the caret row in the tooltip aligned with the text above it? (It is monospace-dependent) | | |
| F3 | Run with `--dark`. Is the read-only Value row still distinguishable from an editable one? | | |
| F4 | Is "col 9" a useful way to point at a character, or would you rather have the cell itself highlight? | | |
| F5 | Two rows (Value + Expression) versus one field that switches mode — does the two-row form read clearly to you? This was a deliberate choice (see the comment in `DocumentOutline.cpp`) and is worth overturning if it reads badly | | |

## Known gaps, stated before you find them

- **No syntax highlighting, no autocomplete.** Typing `#` does not offer the
  parameter names. The reference model does this; EP3D does not yet.
- **No inline error decoration.** The message is in the status bar and the
  tooltip; the cell itself is not marked.
- **The Expression row only appears for length, angle and unitless
  parameters.** Mass, time and density parameters take a literal value only
  (ADR-M11-001), so no row is offered rather than one that always refuses.
- **Only Parameters have expressions.** Sketch dimensions reach them through
  the Parameter they bind, which is the M5 design; there is no expression field
  on a constraint itself.

## Sign-off

| | |
|---|---|
| Validated by | |
| Date | |
| Overall | PASS / FAIL / PASS WITH NOTES |
