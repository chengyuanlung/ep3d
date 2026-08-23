# M8 UI User-Assisted Validation — The Feature Chain

**This is OWNER MANUAL VALIDATION.** It is not an independent agent review and
must never be described as one (ADR-M4-016). An agent may prepare this document,
run the mechanical checks, and state expected values — an agent may **not** fill
a Result cell.

**Status: NOT EXECUTED.** Every Result cell below is blank. Nothing here may be
reported PASS until you have actually run it (M8 spec §11).

---

## What I have already checked, so you do not need to

Automated, **808 registered / 804 executing, 808/808 passing in Debug AND
Release** (the four non-executing entries are registered-Skipped children that
their parent tests spawn in their own processes). Single-process Core: 444
registered / 441 passing / 3 child-Skipped, identical in both configurations.
Release gates A–I all pass with hand-computed oracles, selectivity proven by
counters rather than by equal values, and a 17-mutation battery plus the
V/W/X/Z fix-verification batteries behind them.

Four independent review rounds have run, each finding defects the previous
round's fixes introduced. Round 4's own fixes have not been reviewed.

**What automation cannot see, and why you are needed.** The precedent is M6.14:
`propertiesOf()` returned ten fully populated rows while the running application
showed ten labels and an empty column, because a long diagnostic had pushed the
value column out of the dock. Correct data, invisible to the person it was for.
Every test asked the model; none asked the widget. That class of defect — *is
the right answer actually on screen, findable, and believable* — is what this
document is for.

M8 adds three new panel row groups (`Chain / Base feature`, `Geometry / Angle`,
`Geometry / Radius`) and a rule that is entirely about what you see: **the
viewer shows the chain's TAIL, never the tail plus its intermediates**
(ADR-M8-003). A chain that displays its base *and* its result looks like two
overlapping solids, and no unit test can tell you whether that looks wrong.

---

## What M8 does NOT put in the UI — do not record these as failures

Feature **creation dialogs** are deferred to M9 with the edit-transaction work
(ADR-M8-007). There is no "Insert → Pocket" command. What M8 ships is the chain
**reachable, displayed as its tail, and driven by panel edits** — which is what
these samples give you. If you find yourself looking for a way to create a
fillet from the menu, that absence is the deferral, not a defect.

Also deferred, each with an ADR: Hole, Mirror, Pattern, Shell (ADR-M8-007);
per-edge fillet/chamfer selection (ADR-M8-006 — the samples dress **all** edges,
which is the whole feature today); rollback bar and suppression (M9).

---

## How to run

From `D:\Program2\EP3D\ParametricCAD_Starter`:

```
build\Debug\ParametricCADViewer.exe --sample m8-chain
build\Debug\ParametricCADViewer.exe --sample m8-revolve
build\Debug\ParametricCADViewer.exe --sample m8-dress
```

Add `--dark` to any of them for the dark-palette pass.

**Editing a value:** select the object in the model tree, then double-click the
value cell in the Properties panel, type, and press Enter. The edit commits
through the document facade and recomputes immediately — you should not need to
press Ctrl+R. (Ctrl+R is there if you want to prove a number is not stale.)

**Reading the numbers:** the status bar shows `Volume ... mm^3` to **one**
decimal and `Mass ... kg` to **four**. Every expected value below is quoted in
that format and was computed by hand from the sample's dimensions — not by
running the code under test. Density is aluminium, 2700 kg/m³, throughout.

---

## Test A — The pocket chain  `--sample m8-chain`

A 100 × 50 constrained rectangle padded 20 mm, with a 20 × 30 pocket cut 10 mm
into it. This is the milestone's headline: `Sketch001 → Pad001 → Pocket001`.

Hand-computed: 100 × 50 × 20 = 100000 mm³; the pocket removes 20 × 30 × 10 =
6000 mm³; 100000 − 6000 = **94000 mm³**; × 2700 kg/m³ = **0.2538 kg**.

| # | Step / expectation | Result |
|---|---|---|
| A1 | The model tree shows the feature history in order: **Pad001 then Pocket001**, both under Body001 | |
| A2 | Status bar reads **`Volume 94000.0 mm^3`** and **`Mass 0.2538 kg`** | |
| A3 | The view shows **ONE solid** — a box with a rectangular notch cut into its top. Not two overlapping boxes, not a box with no notch | |
| A4 | Rotate the view. The pocket is a real cavity from every angle — no z-fighting, no surface that flickers between two solids | |
| A5 | Select **Pocket001**. The panel shows `Geometry / Depth` = **10.000 mm**, and it is **editable** (the cell accepts a cursor) | |
| A6 | The same panel shows `Chain / Base feature` naming Pad001's id, and that row is **read-only** — visibly different from the editable Depth | |
| A7 | Select **Pad001**. Its panel shows `Geometry / Length` = 20.000 mm and **no** `Base feature` row — a pad consumes nothing | |
| A8 | Every panel you opened showed **a value beside every label** — the M6.14 defect's own test, now on M8's new rows | |

### A′ — Editing the chain (the parametric proof)

| # | Step / expectation | Result |
|---|---|---|
**Do these in order.** The expected numbers assume the state each step leaves
behind — a Width edit made while Depth is still 20 gives 108000, not 114000.

| # | Step / expectation | Result |
|---|---|---|
| A9 | With Pocket001 selected, set **Depth = 20**. Volume becomes **`88000.0 mm^3`**, mass **`0.2376 kg`** | |
| A10 | The cut now goes **all the way through** — you can see through the hole. This is the "through cut" case and it must be legal, not an error | |
| A11 | Set **Depth back to 10**. Volume returns to **`94000.0 mm^3`** | |
| A12 | Now in the tree select the **`Width`** parameter and set its Value to **120**. Volume becomes **`114000.0 mm^3`** (120 × 50 × 20 − 20 × 30 × 10), mass **`0.3078 kg`** | |
| A13 | The solid **visibly got longer** and the pocket stayed the same size — the pocket sketch is not scaled by Width | |
| A14 | This is the milestone's release proof: **one edit rebuilt the sketch, the pad AND the pocket**, in order, with no manual step in between | |
| A15 | Set Width back to **100**. Volume returns to **`94000.0 mm^3`** exactly | |
| A16 | Through all of that, did the tree selection **stay where you put it** — no jumping to another node after each edit? | |

### A″ — Failure isolation and recovery

| # | Step / expectation | Result |
|---|---|---|
| A17 | Set Pocket001's **Depth = -5**. The pocket goes to a **failed state** and says so somewhere you can see | |
| A18 | Status bar reads **`Mass properties: not current`** — it must NOT show the last good numbers as if they were still true | |
| A19 | **Pad001 is still valid** — the failure did not corrupt the base it consumes | |
| A20 | **The solid disappears from the view entirely.** This is deliberate (GATE_H): a stale result is shown as *absence*, never as a healthy-looking wrong solid. Confirm nothing half-built or garbage is left behind | |
| A21 | Set Depth back to **10**. Volume returns to **`94000.0 mm^3`** — full recovery, no restart needed | |
| A22 | **Judgement:** a part that vanishes on a bad edit is honest, but is it *alarming*? Would you know the geometry is still there and recoverable? | |
| A23 | Type **`abc`** into the Depth cell. The status bar says it is not a number and the **old value comes back**; nothing is corrupted | |

---

## Test B — Revolve  `--sample m8-revolve`

A 20 × 50 rectangular profile at x ∈ [10, 30] revolved a **full turn** about a
sketch line at x = 0. The axis line is an ordinary sketch entity treated as
construction geometry (ADR-M8-005) — it is in the sketch but is not part of the
profile.

Hand-computed annulus: π(30² − 10²) × 50 = 40000π = **125663.7 mm³**;
× 2700 kg/m³ = **0.3393 kg**.

| # | Step / expectation | Result |
|---|---|---|
| B1 | Status bar reads **`Volume 125663.7 mm^3`** and **`Mass 0.3393 kg`** | |
| B2 | The view shows a **tube / hollow cylinder** — outer radius 30, a 10-radius hole down the middle, 50 tall. Not a solid cylinder | |
| B3 | Select **Revolve001**. The panel says `General / Type` = **Revolve** | |
| B4 | The panel shows `Geometry / Angle` = **6.283 rad** and it is editable | |
| B5 | The panel shows **no** `Chain / Base feature` row — a revolve builds from a sketch, it consumes no solid | |
| B6 | Select the sketch. It reports **5 entities** — the four profile lines **plus** the axis line | |
| B7 | The axis line is **not extruded into the solid** — there is no thin wall or spoke at x = 0 | |
| B8 | Set Angle to **3.141593**. Volume becomes **`62831.9 mm^3`**, mass **`0.1696 kg`** — exactly half | |
| B9 | The view shows a **half tube** — an open C-shape you can see the inside of | |
| B10 | Set Angle back to **6.283185**. Volume returns to **`125663.7 mm^3`** | |
| B11 | **Judgement:** the Angle is shown and typed in **radians**. Is that acceptable to you as a user, or does it need a degrees display before M9? Roadmap §7 says internal radians / UI degrees is the eventual target; M8 did not do it | |

---

## Test C — Fillet, the dress chain  `--sample m8-dress`

The same 100 × 50 × 20 pad, with **every** edge rounded at radius 2 mm:
`Sketch001 → Pad001 → Fillet001`.

Hand-computed (the Minkowski decomposition of a rounded box — inner box, face
slabs, quarter-cylinders along the twelve edges, one sphere at the corners):

```
96 × 46 × 16                              = 70656
2·2 · (96·46 + 46·16 + 96·16)             = 26752
π · 2² · (96 + 46 + 16)                   =   632π
(4/3) · π · 2³                            =  (32/3)π
                                    total ≈ 99427.0 mm³
```

× 2700 kg/m³ = **0.2685 kg**.

| # | Step / expectation | Result |
|---|---|---|
| C1 | Status bar reads **`Volume 99427.0 mm^3`** and **`Mass 0.2685 kg`** | |
| C2 | The view shows **ONE solid** — a box with **visibly rounded** edges and corners. Not a sharp box, and not a box with a second ghost box inside it | |
| C3 | Zoom in on a corner. All three edges meeting there are rounded and they **blend into one another** — no facet, no crack | |
| C4 | Select **Fillet001**. `General / Type` = **Fillet**, `Geometry / Radius` = **2.000 mm** (editable), `Chain / Base feature` names Pad001's id (read-only) | |
| C5 | Select **Pad001** — the base. The view does **not** change to show the sharp-edged pad. Selecting an intermediate feature must not resurrect it as a displayed solid (ADR-M8-003) | |
| C6 | Set Radius to **1**. Volume becomes **`99855.4 mm^3`**, mass **`0.2696 kg`** — the volume goes **UP**, because less material is removed | |
| C7 | The rounding is **visibly tighter** than before | |
| C8 | Set Radius to **15** — impossible, wider than half the 20 mm slab, so the rounds collide. The fillet **fails**, the pad stays valid, mass reads **not current**, and the solid disappears from the view (same rule as A18) | |
| C9 | The diagnostic is **understandable** — it tells you the radius is the problem. (If it reads like raw OCCT text, record that: it is a known deferral, ADR-M8-002, and your judgement is what decides whether it can wait for M9) | |
| C10 | Set Radius back to **2**. Volume returns to **`99427.0 mm^3`** | |

---

## Test D — Cross-cutting judgement

These are the questions no assertion can ask. Answer them as a user, not as a
tester.

| # | Question | Result |
|---|---|---|
| D1 | Reading the model tree alone, is it **obvious which feature is the current result**? Or do all three rows look equally like "the model"? | |
| D2 | When a feature failed (A15, C8), could you tell **which** one had failed without hunting through panels? | |
| D3 | Is `Chain / Base feature` showing a raw **numeric id** acceptable, or does it need the base's **name** to be usable? | |
| D4 | After an edit, is it clear the 3D view is showing the **new** result and not a stale render? | |
| D5 | Did the Properties panel stay readable at every step — no label with an empty value, no diagnostic cut off mid-sentence? | |
| D6 | Run any sample with **`--dark`**. Is everything still legible — read-only rows still distinguishable from editable ones, failure states still visible? | |
| D7 | Resize the window narrow, then wide. Does the panel's value column survive it? (This is M6.14's exact shape) | |
| D8 | Anything that surprised you, in either direction. Free text | |

---

## Result

| | |
|---|---|
| Executed by | |
| Date | |
| Build | `build\Debug\ParametricCADViewer.exe`, commit |
| Overall | PASS / FAIL |

**Defects found (if any):**

---

## Note for whoever reads this later

The `m8-revolve` and `m8-dress` samples did not exist when M8's three review
rounds ran. Until they were added, `m8-chain` was the only M8 sample in the
shell, so Revolve, Fillet and Chamfer — three of the four features M8 spec §4
*requires* — were unreachable from the running application, and this document
could only ever have validated the pocket. The samples and their assertions
carry their own mutation record (four mutations, all guarded, each killing only
its own sample) in `M8_CompletionReport.md`, but they have **not** been through
an independent review round. Read that as the limitation it is.
