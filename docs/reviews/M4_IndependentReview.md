# M4 Independent Review

Baseline: `f583f29` on `master` (M3 COMPLETE). `HEAD` at review time is `cc48eaa`
(a docs-only commit on top of `f583f29`).

Reviewed Commit: **uncommitted working tree**, reviewed in full at every revision (never as
a diff against an intermediate state).

| Revision | Scope | Decision | Score |
|---|---|---|---|
| Rev 1 | initial submission (39 files, 284 tests) | REQUEST CHANGES | 74 / 100 |
| Rev 2 | after the Critical + five Major fixes (46 files, 297 tests) | REQUEST CHANGES | 89 / 100 |
| **Rev 3 — current** | after the two remaining Majors + four viewer runtime defects (48 files, 302 tests) | **APPROVE** | **96 / 100** |

**Scope of this approval.** It covers the functional milestone specified by
`docs/M4_Implementation_SelfValidation_and_Evaluation.md` only. A separate UI
specification (`docs/M4_UI_Design_and_Validation.md`) arrived during Rev 2 and describes a
menu, main toolbar, model tree and property panel that the current viewer does not
implement. This review makes no finding about it, and this APPROVE is **not** UI sign-off.

Nothing in `docs/reviews/M4_SelfValidationReport.md` or in any developer summary was
accepted as evidence at any revision. Every build, test run, and behavioural claim below was
produced by the reviewer, in build directories outside the repository, using purpose-written
adversarial probes, a deliberate compile-failure translation unit, and — new in Rev 3 — a
live GUI session driven with synthetic input and captured to PNG. No repository file other
than this review was modified at any revision.

---

## Build Evidence

### Rev 3 (current)

Rev 2's build trees were deleted first; fresh configure into a new directory outside the
repo.

```
cmake -S . -B <scratch>/build-rv3 -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build <scratch>/build-rv3 --config Debug      -> 0 errors
ctest  --test-dir <scratch>/build-rv3 -C Debug --output-on-failure
        -> 100% tests passed, 0 failed out of 302   (8.65 s)
cmake --build <scratch>/build-rv3 --config Release    -> 0 errors
ctest  --test-dir <scratch>/build-rv3 -C Release --output-on-failure
        -> 100% tests passed, 0 failed out of 302   (8.52 s)

cmake -S . -B <scratch>/build-nv3 … -DPARAMCAD_BUILD_VIEWER=OFF   -> viewer skipped
cmake --build … --config Debug   -> 0 errors
ctest  … -C Debug                -> 100% tests passed, 0 failed out of 302   (9.08 s)
```

297 → 302: +5, all in `tests/M4ReviewFindingTests.cpp`
(`NEWMAJOR1_RemovingMaterialClearsPadReference`, `…ClearsBoxReference`,
`…DocumentStillRoundTripsAfterMaterialRemoval`, `…SaveRejectsAStaleMaterialReference`,
`MAJOR1_SketchesAccessorIsGenuinelyReadOnly`). I read all five; each asserts what its name
claims, and the round-trip one is precisely the test whose absence let the Rev-2 regression
through.

Boundary re-verified after the CMake changes:

```
grep -rnE "TopoDS_|BRep|Geom_|gp_|AIS_|V3d_|OpenCASCADE|QObject|QWidget|
           QGraphics|QString|Standard_"  src/Core/          -> 0 matches
dumpbin /archivemembers ParametricCADCore.lib               -> no Presenter member
dumpbin /dependents     ParametricCADCoreTests.exe          -> 0 TK*, 0 Qt6*
dumpbin /dependents     ParametricCADViewer.exe             -> 7 TK*, 3 Qt6*
```

### Rev 1 / Rev 2 (for the record)

Rev 1: fresh configure, Debug and Release 284/284, viewer-off 284/284.
Rev 2: fresh configure, Debug and Release 297/297, viewer-off 297/297.

---

## Fix Verdicts (Rev 3)

| Finding | Raised | Verdict |
|---|---|---|
| **C1** self-intersection orientation-dependent | Rev 1 | FIXED at the cause (Rev 2), re-verified Rev 3 |
| **M1** sketch mutable through a const document | Rev 1 | **FIXED** — now enforced by the type system |
| **M2** `removeObject` owner gap | Rev 1 | FIXED (Rev 2), re-verified Rev 3 |
| **M3** `validateSaveable` missed Pad | Rev 1 | FIXED (Rev 2), re-verified Rev 3 |
| **M4** presenter compiled into Core | Rev 1 | FIXED (Rev 2), re-verified Rev 3 |
| **M5** docs claimed but not done | Rev 1 | FIXED (Rev 2) |
| **NM1** material removal → unloadable file | Rev 2 (regression) | **FIXED**, both halves, verified independently |
| viewer: 4 runtime defects | Rev 3 disclosure | **FIXED**, verified by running the binary |

### M1 — now enforced, not described

`PartDocument::sketches()` returns `std::vector<const Sketch*>` by value
(`PartDocument.h:139`). Verified two ways.

*Type level*: `std::is_same_v<element, const Sketch*>` → 1, pointee const → 1,
`findSketch` → `const Sketch*`.

*Compile level* — a deliberate translation unit containing the exact construction I
demonstrated in Rev 1 and Rev 2, compiled against the current headers:

```
negative.cpp(10): error C2440: cannot convert from 'const paramcad::Sketch'
                               to 'paramcad::Sketch &'
```

The hole is closed by the compiler rather than by a comment. Read access is unaffected, and
all consumers (`PartDocumentSerializer.cpp:194,380` plus the test sites) still work — as
predicted, they were all read-only.

`MAJOR1_SketchesAccessorIsGenuinelyReadOnly` static_asserts the element type, so a future
change back to the owning vector fails the build rather than silently reopening the hole.
That is the right form of regression test for this defect.

**On the `addSketch` residual**: recorded as a Minor rather than argued away, which is the
correct outcome. With `sketches()` closed, the remaining mutable handle is one the caller
explicitly received at creation — a narrow, auditable surface, and materially different from
"any const reference to the document."

### NM1 — fixed in both halves, and each half verified to do work

New `IMaterialReferencing` capability (`src/Core/Feature/IMaterialReferencing.h`:
`materialId()` + `clearMaterialReference()`), implemented by `BoxFeature` and `PadFeature`.
`removeObject`'s `Material*` branch clears every referrer
(`PartDocument.cpp:417-432`); `validateSaveable` rejects any feature whose `materialId` does
not match the document's, in one capability-driven loop rather than a type list
(`PartDocumentSerializer.cpp:385-400`).

Reproduced for **both** feature types:

```
--- Pad ---                                --- Box ---
removed=1  materialId now=0                removed=1  materialId now=0
recompute: success=1 mass=0.0000           recompute: success=1 mass=0.0000
           volume=100000.0 valid=1                    volume=100000.0 valid=1
save   : ok=1   file material record=0     save   : ok=1   file material record=0
                "materialId": "0"                          "materialId": "0"
load   : ok=1                              load   : ok=1
reload+recompute: V=100000.0 mass=0.0000   reload+recompute: V=100000.0 mass=0.0000
```

The document now round-trips into the state ADR-M3-004 declares valid ("no material
assigned" = density 0), rather than into a file its own loader rejects.

The save-side half is **not** dead code — I checked it independently by restoring a feature
carrying a materialId that was never this document's:

```
save with a foreign materialId: ok=0
  "feature 987654 (Ghost): materialId 777777 does not match this document's material"
```

So a stale reference arriving by any path other than removal is still caught. Both halves
earn their place, which is what I asked for.

Choosing a capability over a type list is the right call and directly addresses the reason
this class of defect has now recurred three times: `validateSaveable` and `removeObject` no
longer name `BoxFeature` or `PadFeature` at all for material purposes, so M5's next
referencing feature is covered by construction.

### Viewer — four runtime defects, verified by running the binary

These were outside the tree I reviewed in Rev 1 and Rev 2, and the disclosure is correct
that my Rev-2 viewer score was given against a tree where the executable could not start. I
verified the fixes directly: launched `ParametricCADViewer.exe` from my own build, drove it
with synthetic mouse and keyboard input, and captured the screen.

1. **Qt platform plugin deployment** (`CMakeLists.txt`, POST_BUILD copy of
   `Qt6::QWindowsIntegrationPlugin` into `platforms/`). Verified: `platforms/qwindowsd.dll`
   is present next to the Debug binary, and the process starts, creates a main window
   titled `EP3D - M4 Viewer`, and reports `Responding: True`.
2. **`WA_NativeWindow`** (`OcctViewWidget.cpp:29`). Verified visually: the Qt controls are
   intact and are not painted over — the "Pad length (mm)" label and spin box, the "Fit all"
   button, and the status bar all render normally alongside the 3D viewport.
3. **Shaded display** (`OcctViewWidget.cpp:106`, `Display(…, AIS_Shaded, 0, …)`). Verified
   visually: the solid renders as a shaded body, not wireframe.
4. **`resizeEvent` redraw** (`OcctViewWidget.cpp:142-151`). Verified by maximizing the
   window from 1000×700 to 2560×1600 under synthetic input: the viewport repaints in full,
   with no stale image in a corner and no undrawn region.

Beyond the four, I exercised the spec §17 scope itself:

* **fit-all** — the solid is correctly framed after maximize (the `showEvent` +
  `fittedOnce_` change makes the first fit happen at a genuinely-sized moment, which is the
  right place for it);
* **rotate** — a synthetic left-drag across the viewport produced a correctly rotated view
  showing the top and two side faces of the 100 × 50 × 20 slab;
* **whole-object selection** — the same press resolved the picked presentation back through
  the `AIS_InteractiveObject* → ObjectId` map, and the status bar read
  `Selected object id 12`;
* **parameter edit → recompute → refresh**, the milestone's whole point — clicking the spin
  box and sending five Up keys took Pad length 20.00 → 25.00, and the status bar read:

```
Volume 125000.0 mm^3   Mass 0.3375 kg   COM (50.00, 25.00, 12.50) mm
```

which matches the analytical oracle exactly (100 × 50 × 25 = 125000 mm³;
125000e-9 m³ × 2700 kg/m³ = 0.3375 kg; COM z = 12.5 mm), and the display refreshed and
re-fitted to the new solid.

Not exercised: pan (middle-drag). Its implementation is three lines mirroring rotate and I
have no reason to doubt it, but I did not run it.

Screenshots retained outside the repository at
`…/scratchpad/viewer_max.png`, `v_rotated.png`, `v_pad25.png`.

---

## Critical Findings

**None.** Rev 1's Critical was fixed at the cause in Rev 2 and re-verified in Rev 3.

## Major Findings

**None open.** All six from Rev 1 and the one regression introduced in Rev 2 are closed and
independently verified.

---

## Minor Findings (Rev 3)

Still open. None blocks; listed roughly by value.

1. **No way to assign a material to an existing feature.** Verified: remove the material
   (mass → 0, correct), then `addMaterial("Steel", 7800)` — mass stays **0.0000**, because
   `addBoxFeature`/`addPadFeature` capture `material_->id()` at creation and nothing rewires
   an existing feature or `massPropertiesNode_`. The document is internally consistent and
   saves and loads cleanly, so this is a missing capability rather than a wrong answer — but
   a user who removes and re-adds a material will see a document that has a material and a
   part that weighs nothing. Pre-existing since M3 (a Box created before any Material has
   always behaved this way); M4's removal support makes it easy to reach. The UI milestone
   will need `assignMaterial(featureId, materialId)` regardless.
2. **Profile validation is O(n³)** (`Profile.cpp:129-141` nests two loops around a linear
   `Sketch::findEntity`; `JunctionIndexFor` is O(n²)). Deliberately deferred — **I agree.**
   Invisible at M4 scale; belongs in the M6/DXF plan.
3. **`M4_PROFILE_031` still uses `tol * 100`, not "just outside".** Re-measured this round:
   `0.999×` and `1.0000×` accepted, `1.0001×` and `2×` rejected. Behaviour is right; the
   test does not exercise the boundary spec §10 asks for.
4. **ADR-M4-009 and ADR-M4-010 were not amended for the NM1 fix.**
   `IMaterialReferencing.h:15` and `PartDocument.cpp:424` both cite "ADR-M4-009, extended
   after review", but ADR-M4-009's text still states the rule only as *"every alternative of
   `ObjectRegistry::ObjectRef` needs a corresponding owner step"* — which is exactly the rule
   that proved insufficient, because NM1 was about **referrers**, not owners. ADR-M4-010
   still says a reference-enumeration capability is something *"M5+ should"* build, when this
   change already built it. Nothing false is asserted, but the log no longer records the
   decision the code points at — the mirror image of the Rev-1 M5 finding, in a project that
   has now been bitten by this twice.
5. **No ADR for the viewer runtime decisions.** `WA_NativeWindow`, platform-plugin
   deployment, shaded display mode, redraw-on-resize and fit-on-first-show are real
   decisions with a lesson worth keeping ("a build-and-link check cannot see it"). That
   lesson currently lives only in code comments.
6. **No automated smoke check for the viewer.** Four user-facing defects survived two full
   review rounds because nothing ever started the executable, and nothing prevents that
   recurring. Even a CTest entry that launches the binary with a short timeout and asserts a
   main window appears would have caught defect 1.
7. **`MassPropertiesNode` still names its source `boxFeatureId_`** (`MassPropertiesNode.h:29,
   30,49`). Deliberately deferred — **I agree**, it is a rename with no behavioural content.
8. **`PartDocumentSerializer.h`'s contract block is stale** — still "schema v3", "save always
   writes v3", "Not serialized: … sketches".
9. **Wiring failures are silently discarded** (`PartDocument.cpp` `wirePadFeature`/
   `wireBoxFeature` ignore every `GraphResult`). Now caught downstream at save, so the
   consequence is bounded.
10. **`MassPropertiesNode` remains a document singleton** bound to the most recently wired
    solid feature (ADR-M3-005, declared).
11. **Viewer interaction nits**: left-press both selects and starts a rotation, so every
    rotate drag changes the selection (confirmed on screen — the status bar switched to
    `Selected object id 12` on a drag); `SetZoom(1.1 * steps)` scales with wheel-step count
    rather than compounding.
12. **`addSketch`/`restoreSketch` still return `Sketch&`** — accepted residual, now recorded
    as a Minor.
13. **Sketch entity ids are unique only within their sketch** in the file format. Correct per
    ADR-M4-001; state the document-scope resolution rule before M5's `SketchElementRef`
    needs it.
14. **`README.md` / `AGENTS.md` declare M4 COMPLETE and cite `M4_CompletionReport.md`,**
    which does not exist. Acknowledged by the developer and scheduled to land with the
    report; noted here so the completion step does not lose it.

---

## Architecture

Core is free of Qt and OCCT at source and binary level; `src/Kernel/Occt` and `src/Viewer`
are the only places either appears; the presenter has its own Qt-free, OCCT-free target; and
`-DPARAMCAD_BUILD_VIEWER=OFF` configures, builds and passes 302/302. The dependency
direction is correct in every target.

`ISolidFeature` and `IMaterialReferencing` are now a matched pair, and together they are the
most valuable structural outcome of the milestone: both `MassPropertiesNode`,
`DocumentPresenter`, `removeObject` and `validateSaveable` depend on *capabilities* rather
than on `BoxFeature`/`PadFeature`. That is the direct discharge of ADR-M3-007's lesson, and
it is what stops the recurrence that ran through M3 Majors 3-4, Rev-1 M2/M3, and Rev-2 NM1.

ADR-M4-011's closing rule — *"a geometric predicate whose result depends on the orientation
of its input is wrong even where it happens to give the right answer … the suite tested the
predicate, not its invariance"* — is the sharpest thing in the DecisionLog and generalizes
beyond this defect.

Invariant verdicts:

| # | Invariant | Rev 1 | Rev 2 | **Rev 3** |
|---|---|---|---|---|
| 1 | Core independence | PASS* | PASS | **PASS** |
| 2 | Stable identity | PASS | PASS | **PASS** |
| 3 | Model vs computed geometry | PASS | PASS | **PASS** |
| 4 | Dependency-driven recompute | FAIL | FAIL | **PASS** |
| 5 | Command/transaction boundary | PASS | PASS | **PASS** |
| 6 | Units explicit | PASS | PASS | **PASS** |
| 7 | Coordinate frames first-class | PASS | PASS | **PASS** |
| 8 | Physics metadata separate | PASS | PASS | **PASS** |
| 9-10 | Assembly / Collision | N/A | N/A | **N/A** |

\* Rev 1 passed at source and binary level with a target-level layering defect noted.

---

## Identity

Held under three rounds of deliberate attack. Ids survive insertion, removal of the first
stored entity, re-add at the storage end, file reordering, hand-renumbering and round-trip;
duplicate entity ids within a sketch are rejected; hand-injected degenerate geometry loads
losslessly and is caught by the profile validator; `1e400` is rejected as malformed JSON;
nothing index-, pointer- or topology-derived is identity anywhere; and a removed Sketch or
Material no longer keeps its identity, its file record, or its ability to return on load.

---

## Sketch / Frame

Frame math verified independently against the analytical box oracle at every revision,
including the **full inertia tensor**. Rev 3 re-measurement:

```
world XY          V=100000.0  COM=(50.00,25.00,10.00)  Idiag=(6.5250e-05, 2.3400e-04, 2.8125e-04)
analytic                                                     (6.5250e-05, 2.3400e-04, 2.8125e-04)
rot +90 X         V=100000.0  COM=(50.00,-10.00,25.00) Idiag=(6.5250e-05, 2.8125e-04, 2.3400e-04)
rot +90 Z         V=100000.0  COM=(-25.00,50.00,10.00) Idiag=(2.3400e-04, 6.5250e-05, 2.8125e-04)
rot 30 (1,1,1)+t  V=100000.0  COM=(47.77,29.99, 8.24)  Idiag=(9.9298e-05, 2.1806e-04, 2.6314e-04)
                                                       trace 5.8050e-04 == world-XY trace
```

Exact diagonal match, correct permutations under the axis-aligned rotations, and rotational
invariance preserved under an arbitrary axis with a translated origin. No world-XY
hardcoding. Full-turn arcs (2π, 4π, clockwise) and zero-sweep arcs are rejected; half and
quarter arcs and the one-entity circle loop are accepted.

---

## Profile

Rev 3 re-verification of the C1 fix and the surrounding rules:

```
C1 counterexample rejected at 0/45/63/90/180/-60/1.1 rad : yes, all 7
rectangle + deep U-shape accepted at all 7 angles        : yes, no false positives
perpendicular tolerance independent of segment length    : offset 1e-5 accepted and
                                                           1e-9 rejected at L=10/100/1000
tolerance boundary 0.999x / 1.0x accept, 1.0001x / 2x reject
arc + chord accepted; duplicate line rejected
```

Arc-involved self-intersection remains a **declared, acceptable** M4 limitation for the
reasons in ADR-M4-005 — the check is not claimed to exist, the alternative would produce
false rejections, and OCCT still refuses a face it cannot build.

---

## Pad / Physics

Unchanged and correct across all three revisions. Rectangle 100×50×20 → 100000 mm³ /
0.27 kg / COM (50,25,10); PadLength 20→30 → 150000 / COM.z 15; sketch width 100→120 through
`editSketch` → 180000 / COM.x 60; circle r10×30 → π·100·30. Adversarial Pad lengths (0,
negative, NaN, Inf, 1e-7) all fail cleanly with `valid` cleared. And, new in Rev 3, the same
numbers were read off the running application's status bar.

---

## Recompute

Re-measured with a `CountingKernel` decorating the real `OcctGeometryKernel`:

```
initial       extrudes=1 mass=1   V=100000.0  mass=0.2700  COM=(50.00,25.00,10.00)
PadLength     +extrudes=1 +mass=1  V=150000.0  COM.z=15.00
density       +extrudes=0 +mass=1  mass=0.1500          <- geometry genuinely skipped
unrelated     +extrudes=0 +mass=0
editSketch    +extrudes=1 +mass=1  V=180000.0  COM.x=60.00
```

With `sketches()` closed and `editSketch` the enforced mutation path, "edited" and "dirtied"
can no longer be separated through any public API. Invariant 4 passes for the first time.

---

## Persistence

Complete. Pad references (Length, Sketch) and material references on both feature types are
enforced symmetrically on save and load; a removed Sketch or Material no longer produces a
file that resurrects it or that cannot be loaded; round-trip preserves sketch id, entity ids,
geometry, frame, Pad id and all references; second save is byte-identical; no OCCT topology,
index or address is ever written; and the loader still rejects duplicate entity ids, missing
sketches, dangling parameters and foreign material ids.

---

## Viewer

Now verified on screen, not only by construction. Ownership rules of ADR-M4-006 hold in
`OcctViewWidget.cpp` (non-owning `PartDocument*` and `DocumentPresenter*`, AIS handles owned
solely by the widget, presentation→ObjectId map rebuilt wholesale on every refresh, mutation
only through the document facade, correct destruction order in `main.cpp`), the presenter is
Qt-free and OCCT-free with 7 automated tests including the stale-shape case, and the
application demonstrably displays, fits, rotates, selects by whole object, resizes cleanly,
and completes the parameter-edit → recompute → refresh loop with analytically correct
values. Pan is the one scope item I did not exercise.

---

## Self-Validation Audit

Across three revisions no test was ever claimed without execution, and every build/test
figure I checked was reproducible. The developer's accuracy improved each round and the
Rev-3 disclosure — volunteering that the viewer had never been run, and that my Rev-2 score
was therefore given against a tree that could not launch — is exactly the behaviour that
makes an independent review useful rather than adversarial theatre.

Two process observations worth carrying into M5, neither of them a finding against the code:

* **Each round's escape was in the gap between "the fix" and "the thing the fix was
  demonstrated on."** Rev 2 fixed `findSketch` when the demonstration used `sketches()`;
  Rev 2's removal test asserted `material() == nullptr` and never saved, which is what let
  NM1 through. Rev 3 closed both by testing the demonstrated construction itself — a
  compile-failure static_assert and an actual round-trip. That is the right habit.
* **Four defects were invisible to 297 passing tests because nothing ran the binary.** The
  suite is strong on semantics and silent on integration. Minor 6 proposes the cheapest
  possible remedy.

---

## Mandatory Gates A-E

Re-run in Rev 3 (`tests/Kernel/M4ReleaseGateTests.cpp`, 9 tests, green in Debug **and**
Release) and independently reproduced in my own probes.

* **A — Rectangle: PASS.** Now also confirmed interactively (20 → 25 mm through the spin box
  gave 125000 mm³ / 0.3375 kg / COM (50,25,12.5) on screen).
* **B — Circle: PASS.**
* **C — Failure/recovery: PASS.**
* **D — Frame: PASS.** Volume exact, COM and full inertia tensor correct under translation
  and three rotations.
* **E — Save/load: PASS**, including the two cases Rev 1 and Rev 2 found missing — save/load
  after removing a Sketch, and after removing the Material.

---

## Score Breakdown (spec §27 weights)

| Category | Weight | Rev 1 | Rev 2 | **Rev 3** | Rev 3 reason |
|---|---|---|---|---|---|
| Architecture boundaries | 15 | 12 | 15 | **15** | Source + binary clean, presenter in its own target, viewer-off passes 302/302 |
| Identity/reference model | 15 | 13 | 15 | **15** | Survived three rounds of attack incl. hand-edited files; removal reaches owners and referrers |
| Sketch/frame correctness | 10 | 9 | 10 | **10** | Full inertia tensor correct under arbitrary rotation; arc sweep bounded correctly |
| Profile validation | 15 | 6 | 14 | **14** | Orientation-independent by construction, no false positives; −1 for the still-weak tolerance-boundary test (Minor 3) |
| Kernel Pad/Extrude | 15 | 14 | 15 | **15** | Kernel-neutral, structured failures, oracles exact, dangling-reference documents stopped at save |
| Parametric recompute | 10 | 7 | 7 | **10** | `editSketch` is now the only mutation path and the compiler enforces it; economy measured |
| Persistence | 8 | 5 | 4 | **8** | All reference classes symmetric on save/load; both removal regressions closed and round-trip tested |
| Viewer | 5 | 4 | 4 | **5** | Display, fit, rotate, selection, resize and the full edit→recompute→refresh loop verified on screen with correct values |
| Testing/self-validation/docs | 7 | 4 | 5 | **4** | 302/302 both configs reproduced and well-targeted regression tests; −3 for ADR-M4-009/010 not recording the decision the code cites them for, no ADR for the viewer decisions, no smoke check for a defect class that escaped two full rounds, and the premature COMPLETE status |
| **Total** | **100** | **74** | **89** | **96** | |

---

## Required Changes

None blocking. Recommended before the completion commit, in order of value:

1. Amend **ADR-M4-009** to state the rule as *owners and referrers*, and **ADR-M4-010** to
   record that reference enumeration by capability was built in M4, not deferred to M5
   (Minor 4). The code already cites these ADRs for decisions they do not contain.
2. Add a viewer smoke check to CTest — launch, assert a main window, exit (Minor 6) — and an
   ADR for the viewer runtime decisions (Minor 5).
3. Add `assignMaterial(featureId, materialId)` or equivalent, or record Minor 1 explicitly as
   deferred, before the UI milestone exposes material editing.
4. Fold in the one-line Minors: 3 (`tol * 1.0001`), 8 (serializer header contract), 9 (reject
   unknown references at the facade).
5. Land `docs/reviews/M4_CompletionReport.md` together with the README/AGENTS status lines
   and the exact final commit hash (Minor 14).

---

## M5 Readiness

**READY**, contingent only on the completion-process items below — nothing in the code or
architecture blocks M5.

### Spec §34 final gate — explicit read

| Item | Status |
|---|---|
| stable `SketchEntityId`; no vector-index/pointer/topology persistence | **PASS** |
| Point / Line / Circle / Arc; viable future sub-element refs | **PASS** |
| Sketch frame; XY / translated / rotated tests | **PASS** |
| semantic Profile; deterministic ordering/orientation; tolerance policy | **PASS** |
| rectangle / Circle / Line+Arc accepted | **PASS** |
| open / disconnected / branch / duplicate / degenerate rejected | **PASS** |
| self-intersection policy tested | **PASS** (orientation-independent; arc-involved case a declared limitation) |
| Core free of Qt/OCCT; neutral profile kernel API; OCCT Face + Extrude | **PASS** |
| rectangle/circle analytical volume; transformed-frame extrusion | **PASS** |
| `PadFeature` + Length Parameter | **PASS** |
| correct Sketch/Pad/Density/Unrelated recompute | **PASS** |
| failure + recovery; M3 MassProperties reused | **PASS** |
| semantic persistence; IDs survive load; no OCCT topology serialized; load/recompute equivalent | **PASS** |
| viewer display / rotate / pan / zoom / fit / select / refresh | **PASS** — all verified on screen except pan |
| viewer does not own semantic objects | **PASS** |
| all M0-M3 regression; all M4 tests; Debug and Release clean builds | **PASS** — 302/302 in both, reproduced |
| Gates A-E | **PASS** |
| ADR-M4-001..011 | **PASS** (Minor 4 asks for two amendments) |
| `M4_SelfValidationReport` | **PASS** |
| independent review | this document |
| no Critical | **PASS** |
| no unresolved Major | **PASS** |
| Reviewer ≥ 80 | **PASS** — 96 |
| README / Roadmap / AGENTS updated | **PASS** on content; status lines land with the completion report |
| `M4_CompletionReport` | **OUTSTANDING** |
| exact final commit recorded | **OUTSTANDING** — the tree is still uncommitted |

Every functional gate line passes. The two outstanding items are completion-process steps
that by definition follow this review; once `M4_CompletionReport.md` exists and the tree is
committed with its hash recorded, **M4 = COMPLETE** and **M5 = READY**.

M5 (Sketch Constraints & Dimensional Parameterization) builds directly on the parts that
proved sound here: `SketchElementRef` is reserved and unused exactly as intended, entity
identity is stable under every mutation I could construct, the frame is a single conversion
site, `editSketch` gives the constraint solver one enforced mutation path, and the two
capability interfaces mean a constrained-sketch feature type joins the document without
editing `removeObject`, `validateSaveable`, `MassPropertiesNode` or the viewer. The two
things to watch going in are the O(n³) profile validator (Minor 2), which M6's DXF import
will make real, and the document-scope entity-id question (Minor 13), which M5's constraint
references will force.
