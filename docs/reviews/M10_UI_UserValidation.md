# M10 UI User-Assisted Validation — Reference Frames & Connectors

**This is OWNER MANUAL VALIDATION.** It is not an independent agent review and
must never be described as one (ADR-M4-016). An agent may prepare this document,
run the mechanical checks, and state expected values — an agent may **not** fill
a Result cell.

**Status: NOT EXECUTED.** Every Result cell below is blank. Nothing here may be
reported PASS until you have actually run it (M10 spec §11).

**M10 has had no independent review, and neither has M9.** This checklist does
not substitute for one.

---

## What I have already checked, so you do not need to

Automated: **856/856 in Debug AND Release**, plus single-process and
shuffled-seed runs of all five binaries in both configurations. M10's release
gates A–J, M/N/O and P/P2 pass on hand-computed oracles, behind a 19-mutation
battery plus one kernel mutation.

Two findings are worth knowing before you start, because both change what you
should distrust:

1. **A mass-properties defect older than M10 was found by GATE_P**
   (ADR-M10-005): `BRepGProp::VolumeProperties` on a compound of disjoint solids
   returned an exactly right VOLUME and a 2 % wrong CENTRE OF MASS. It survived
   for six milestones because *every analytic oracle in this project is a
   volume*. It is fixed. **Its lesson for you: in this sample the volume is
   deliberately useless as a check** — a frame moves a solid without changing
   how much material it contains. Read the centre of mass, not the volume.
2. **A test failed about one run in a hundred** under shuffled ordering and was
   invisible to `ctest` (ADR-M10-006). Fixed, and the suite has since run 150
   shuffled iterations per configuration clean.

**What automation cannot see, and why you are needed.** The precedent is M6.14:
`propertiesOf()` returned ten fully populated rows while the running application
showed ten labels and an empty column. Correct data, invisible to the person it
was for.

M10's own exposure is sharper than usual, and it is specifically about
*believability*:

- A frame hierarchy has **two numbers for the same thing** — Local and World.
  They agree at the root and diverge everywhere else. If the panel does not make
  clear which is which, a user will read the wrong one and be confident.
- **A frame is invisible in the 3D view.** M10 puts frames in the tree and the
  panel only. Whether a part that has silently moved 30 mm is *comprehensible*
  when nothing on screen draws the thing that moved it is exactly the judgement
  no test can make.
- A connector **has no geometry at all**. It is a name, a role and a frame
  reference.

---

## What M10 does NOT put in the UI — do not record these as failures

- **Frames have no creation command.** M10.5 delivered *inspection*, not
  authoring: frames reach the tree and the panel and come from the sample, and
  nothing in the menus makes one. This is the milestone's largest UI gap and it
  is recorded as a known limitation, not a defect.
- **Connectors are not drawn in the 3D view**, and have no visibility toggle of
  their own. Visibility is per-context presentation state (roadmap §18.3) and
  A02 keeps presentation out of Core.
- **Mirror and Pattern have no menu command** either, although M10.6 delivered
  both in Core with full gate coverage.
- **No attachment to a planar face**, and **no realign-to-axis-entity** — both
  need selection architecture that does not exist yet (roadmap §18.2, and the
  same reason ADR-M8-006 deferred per-edge fillets).
- **Pattern is LINEAR only.** No circular, no 2D.
- Nothing **previews** a frame move before you commit it.

---

## How to run

From `D:\Program2\EP3D\ParametricCAD_Starter`:

```
build\Debug\ParametricCADViewer.exe --sample m10-frame
```

Add `--dark` for the dark-palette pass.

**What the sample is.** A 100 × 50 rectangle padded 20 mm — the same plate every
other sample uses — but its sketch is placed on a **frame**, and that frame has
a parent:

```
Origin
Root                       local (0, 0, 30)          — lifts everything 30 mm in Z
  PlateFrame               local rotation 90° about X — turns the plate on edge
    MountPoint             a connector, role Mount
```

The sketch's support is **PlateFrame**. The pad is built on the sketch. Nothing
else in the document knows the frames exist.

**Hand-computed, and this is the whole point of the sample:**

- Volume is **100000 mm³** and mass **0.2700 kg** — *unchanged by the frames*.
  A frame moves material; it does not create or destroy it.
- The plate's local centroid is (50, 25, 10). PlateFrame's 90° X-rotation sends
  it to (50, −10, 25). Root's lift then adds 30 in Z:
  **centre of mass = (50, −10, 55)**.

That last number is the release proof, and it is reachable **only** through the
composed frame chain. Getting the local transform right and the composition
wrong gives (50, 25, 40); getting the composition right and the rotation wrong
gives (50, 25, 55). Neither of those is what you should see.

---

## Test A — The frame tree  `--sample m10-frame`

| # | Step / expectation | Result |
|---|---|---|
| A1 | The model tree has a **Frames** group containing **Origin** and **Root** at the top level, with **PlateFrame nested under Root** | |
| A2 | **MountPoint** appears **under PlateFrame** — a connector is listed on the frame it is on, not in a flat list of its own | |
| A3 | The nesting reflects **parentage**, not creation order. Say whether the hierarchy was readable at a glance or whether you had to work it out | |
| A4 | There are exactly **three frames**. Origin is there once — not twice | |
| A5 | Every frame and connector row shows **a value beside every label** in the panel (the M6.14 defect, on M10's new rows) | |

---

## Test B — Local versus World  `--sample m10-frame`

| # | Step / expectation | Result |
|---|---|---|
| B1 | Select **Root**. `General / Type` reads **Frame** | |
| B2 | Root's `Local / X, Y, Z` read **0.000, 0.000, 30.000 mm** | |
| B3 | Root's `World / X, Y, Z` read **the same** — 0, 0, 30. Root has no parent, so local and world coincide | |
| B4 | Select **PlateFrame**. Its `Local / X, Y, Z` read **0.000, 0.000, 0.000** — its local transform is a pure rotation, no offset | |
| B5 | PlateFrame's `World / X, Y, Z` read **0.000, 0.000, 30.000** — it inherits Root's lift. **This row is the one that proves composition happens**; if World equalled Local here, the hierarchy would be decorative | |
| B6 | PlateFrame's panel has a `Hierarchy / Parent` row reading **Root**. Root and Origin have no such row at all — a root frame does not get an empty Parent cell | |
| B7 | **Judgement:** with Local reading 0,0,0 and World reading 0,0,30 on the same object at the same time, was it clear which number meant what? This is the row most likely to produce a real finding | |
| B8 | Select **Origin**. Local and World both read **0, 0, 0** | |

---

## Test C — The connector  `--sample m10-frame`

| # | Step / expectation | Result |
|---|---|---|
| C1 | Select **MountPoint**. `General / Type` reads **Connector** | |
| C2 | `General / Role` reads **Mount** — not a number, not `1`, not blank | |
| C3 | `Frame / On frame` names **PlateFrame** | |
| C4 | Its `World / X, Y, Z` read **0.000, 0.000, 30.000** — the connector sits where its frame sits, because it *is* its frame's transform | |
| C5 | The connector is **not drawn in the 3D view**, and nothing suggests it should be. Confirm that its absence reads as "not shown yet" rather than "broken" | |
| C6 | **Judgement:** a connector is an object with a name, a role and a location and no appearance. Was it obvious what it was for, or did it read as an empty row? | |

---

## Test D — The solid is where the frames put it  `--sample m10-frame`

| # | Step / expectation | Result |
|---|---|---|
| D1 | Status bar reads **`Volume 100000.0 mm^3`** and **`Mass 0.2700 kg`** — the plate is exactly the plate, unchanged by two frames | |
| D2 | **The plate is standing on edge, and up in the air.** Not lying flat at the origin. Use Fit All (Ctrl+Shift+F) if you need to find it | |
| D3 | Rotate the view. It is **one solid**, correctly formed — the frame chain moved it, it did not distort it | |
| D4 | Select **Sketch001**. Its panel has a `Placement / Support` row naming **PlateFrame** — not `(its own plane)` | |
| D5 | **This is the release proof.** The plate's position is reachable only by composing PlateFrame's rotation with Root's lift. If the plate were lying flat at z = 30, composition happened but the rotation did not apply; if it were standing on edge at z = 0, the rotation applied but the parent did not | |
| D6 | Select **Pad001**. It shows no frame row of its own — a pad is placed by its sketch, and its sketch is placed by a frame. Confirm that chain was followable in the UI | |
| D7 | **Judgement — the milestone's real UX question.** The part is 30 mm up and turned on its side, and **nothing on screen draws the frames that put it there.** Was the part's position comprehensible, or did it look like the model was simply wrong? Record this even if everything above passed | |

---

## Test E — Cross-cutting judgement

| # | Step / expectation | Result |
|---|---|---|
| E1 | Selection **round-trips**: click a frame in the tree, click away, click back — you land on the same object | |
| E2 | Selecting a frame or a connector does **not** disturb the 3D view or clear the status bar's Volume/Mass | |
| E3 | Run again with **`--dark`**. Frame and connector rows, and the Local/World grouping, are legible in the dark palette | |
| E4 | Narrow the window. The Local/World rows keep their values visible — six numeric rows on one object is the most crowded panel in the application | |
| E5 | Open **`--sample m8-chain`** as a control. Its sketch's `Placement / Support` reads **`(its own plane)`**, and nothing about it changed in M10. A pre-M10 part must be untouched by the new machinery | |
| E6 | **Overall judgement:** M10 claims a part can be positioned by a hierarchy of named frames. Having driven it, is that claim usable — or is it true in the model and unusable on screen because frames cannot be created and cannot be seen? | |

---

## Recording the result

Fill the Result cells with PASS / FAIL / a note. For any FAIL, say what you saw
rather than what you expected — the expected value is already written down.

Rows B7, C6, D7 and E6 ask for a judgement, not a pass. Those are the rows this
document exists for; an answer of "fine" on all four is a weaker result than one
specific complaint.

**M10 cannot close** even with every cell filled: it has had no independent
review, it sits behind M9 which has had none either, and both sit behind M8,
whose round-4 fixes are unreviewed.
