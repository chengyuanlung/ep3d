# M5 UI User-Assisted Validation

**This is OWNER MANUAL VALIDATION.** It is not an independent agent review and
must never be described as one (ADR-M4-016).

**Status: COMPLETE.** Tests A-E reported PASS by the owner. Test F (display scaling) skipped at the owner's direction.

---

## What I have already checked, so you do not need to

Automated, 488/488 in Debug and Release: solve status, DOF, the constraint list,
dimension values and units, offending-constraint ids, and that editing a
dimension changes the **3D volume** — all against hand-computed oracles.

**What automation cannot see, and why you are needed:** whether the numbers are
*findable*, whether the layout is usable, whether anything is unreadable or
overlapping, whether it feels responsive, and anything about display scaling.

---

## How to run

From `D:\Program2\EP3D\ParametricCAD_Starter`:

```
build\Debug\ParametricCADViewer.exe --sample m5-rectangle
build\Debug\ParametricCADViewer.exe --sample m5-underconstrained
build\Debug\ParametricCADViewer.exe --sample m5-conflict
build\Debug\ParametricCADViewer.exe --sample m5-circle
```

Add `--dark` to any of them for the dark-palette pass.

Each sample is built deterministically in code, so you can recreate any of them
exactly. **The geometry starts deliberately off-size and skewed** (112 × 58, not
100 × 50) — so if you see 100 × 50 on screen, the solver did that, which is the
whole point.

---

## Test A — Fully constrained rectangle  `--sample m5-rectangle`

| # | Step / expectation | Result |
|---|---|---|
| A1 | Window opens; nothing is cut off or overlapping | **PASS** |
| A2 | Model tree shows `Sketch001` with **11 constraint rows underneath it** | **PASS** |
| A3 | Select `Sketch001`. Panel shows **Solve status = Solved**, **Degrees of freedom = 0**, **Count = 11** | **PASS** |
| A4 | Constraint rows read like `Length = Width (100.000 mm)` — the **parameter name and value**, not just "Length" | **PASS** |
| A5 | Select the `Length = Width` row. Panel shows Type, ID, Sketch, **Value 100.000 with unit `mm`**, Parameter `Width` | **PASS** |
| A6 | The 3D solid is a **shaded** box, roughly 2:1 | **PASS** |
| A7 | Status bar shows current mass properties (**volume 100000 mm³, mass 0.27 kg**) | **PASS** |

### A-edit — the release-critical proof

| # | Step / expectation | Result |
|---|---|---|
| A8 | Change **Width 100 → 120** in the Value row, press Enter | **PASS** |
| A9 | Was the edit field easy to find? | **PASS** |
| A10 | No modal dialog appeared | **PASS** |
| A11 | The 3D solid **visibly widens**, promptly | **PASS** |
| A12 | Volume becomes **120000 mm³**, mass **0.324 kg** | **PASS** |
| A13 | Status still **Solved**, DOF still **0** | **PASS** |
| A14 | The viewer never shows the old geometry while claiming it is current | **PASS** |
| A15 | Change **Height 50 → 80**. Volume becomes **192000 mm³**, mass **0.5184 kg** | **PASS** |

---

## Test B — Under-constrained  `--sample m5-underconstrained`

| # | Step / expectation | Result |
|---|---|---|
| B1 | Panel shows **Solve status = Under-constrained** | **PASS** |
| B2 | **Degrees of freedom is greater than 0** — *not* `0`, and not blank | **PASS — owner reported DOF = 2**, which is the analytically correct value: 16 variables (4 lines x 4 scalars) minus 14 independent residuals (4 Coincident x 2, 2 Horizontal, 2 Vertical, 1 Fix x 2) = 2, and those 2 are exactly Width and Height. The rank measurement agrees with the hand count. |
| B3 | A solid is still drawn — under-constrained is a legal state, not a failure | **PASS** |
| B4 | Nothing in the UI presents this as finished or as broken | **PASS** |

---

## Test C — Conflict and recovery  `--sample m5-conflict`

| # | Step / expectation | Result |
|---|---|---|
| C1 | Sketch row is marked failed, **with a `!` marker and not only a colour** | **PASS** |
| C2 | Panel shows a conflict status and a **Solver diagnostic** | **PASS** |
| C3 | Panel shows **Offending constraint IDs** — actual numbers | **PASS** |
| C4 | The individual offending constraint rows are marked, not just the sketch | **PASS** |
| C5 | The Pad row says **Blocked** (`-` marker) and its tooltip names the sketch — it must **not** look identical to a Pad that failed on its own | **PASS** |
| C6 | Status bar reports mass properties as **not current** | **PASS** |
| C7 | Nothing renders a stale solid as if it were current | **PASS** |

---

## Test D — Constrained circle  `--sample m5-circle`

| # | Step / expectation | Result |
|---|---|---|
| D1 | Panel shows **Solved**, **DOF = 0** | **PASS** |
| D2 | Radius row shows **20.000 mm** (drawn at 7 — the solver moved it) | **PASS** |
| D3 | Change **Radius 20 → 40**. The cylinder visibly grows and the volume goes **×4** | **PASS** |

---

## Test E — Selection, view, theme

| # | Step / expectation | Result |
|---|---|---|
| E1 | Clicking a tree row selects the matching object; the panel follows | **PASS** |
| E2 | **Clicking a solid in the 3D view selects the object actually under the cursor** — this is the defect you found in M4, at 200% scaling | **PASS** |
| E3 | Show / Hide works, and "hidden" never hides a failure | **PASS** |
| E4 | Fit All frames the model | **PASS** |
| E5 | Re-run any sample with `--dark`: text stays readable, state markers still distinguishable | **PASS** |

---

## Test F — Display scaling *(you asked to skip; run only if you want to)*

| # | Step / expectation | Result |
|---|---|---|
| F1 | At **200%** scaling, is the whole shell on screen and usable? | SKIPPED BY OWNER |

I know this one currently fails: the shell's minimum size is 1000 × 640 logical
pixels, and a 200%-scaled 1280 × 800 desktop is only 640 × 376 logical, so it
cannot fit. Fixing it is a layout change, and you asked me to leave DPI alone.

---

## Your findings

Report in whatever form suits you. The minimum I need per problem: **which
sample, which step number, what you saw, what you expected.**

| # | Sample | Step | What happened | Severity |
|---|---|---|---|---|
| | | | | |

---

## Result

- [x] **PASS** — M5 UI is acceptable
- [ ] **PASS WITH FINDINGS** — acceptable, findings listed above
- [ ] **FAIL** — findings must be fixed first

**Validated by:** the project owner, manually, in the running application.
**Reported:** Tests A-E all PASS, no findings raised. DOF = 2 was reported
explicitly for B2 and matches the hand-computed value.

**Scope of this result — stated because it limits what it proves:**

- **Test F (display scaling) was NOT executed**, at the owner's direction. The
  shell's 1000x640 logical minimum cannot fit a 200%-scaled 1280x800 desktop,
  which is known and unfixed. The one Critical this project's owner has ever
  found was a display-scaling defect, so this is the gap that matters most.
- **The display configuration used was not recorded.** By ADR-M4-015, a UI
  verified on one display configuration has been verified on one display
  configuration -- and which one is not known here.
- This is **owner manual validation**. It is not an independent agent review and
  must never be described as one (ADR-M4-016).
