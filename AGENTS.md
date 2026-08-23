# Codex / AI Agent Instructions

## Multi-Agent Orchestration

**Default behavior for non-trivial tasks:** if the execution environment supports sub-agents, the primary agent MUST act as an Orchestrator and automatically create the required role agents described in `docs/OrchestratorGuide.md` and `docs/AgentRoles.md`. At minimum create separate Architect, Developer, Tester, and independent Reviewer roles. Do not wait for the user to ask for each role individually.

If sub-agent spawning is unavailable, follow the fallback sequential-role procedure in `docs/OrchestratorGuide.md` and explicitly state that independent agents could not be spawned. Never claim that separate agents were used when they were not.

Before orchestrating, read:
- `docs/OrchestratorGuide.md`
- `docs/AgentRoles.md`

Before changing code, read:
- `docs/Architecture.md`
- `docs/CodingRules.md`
- `docs/Roadmap.md`

Hard rules:
1. Do not add Qt dependencies to `src/Core`.
2. Do not add OpenCASCADE types to `src/Core` public APIs. Since M4 this
   extends to Qt: `src/Viewer` is the only place Qt appears, and
   `src/Kernel/Occt` plus the viewer are the only places OCCT appears.
   Features are heterogeneous -- code iterating `Body::features()` must state
   which kinds it applies to (ADR-M3-007), and semantic identity is never an
   OCCT handle, an explorer order, a vector index or an address (ADR-M4-004).
3. Keep persistent identity separate from container position.
4. Do not bypass document/parameter state rules with ad-hoc globals.
5. Keep each task limited to the requested milestone.
6. Add or update tests for Core behavior.
7. Build and run tests before reporting completion.
8. If an architectural conflict is discovered, document it instead of silently redesigning the project.
9. Mutation testing: DELETE the test binaries before each rebuild and assert
   they exist afterwards -- a stale binary once produced five identical false
   verdicts. **Second flavor, found independently by two M8 reviewers**:
   timestamp-preserving restores (`Copy-Item`, `cp -p`) make MSBuild skip the
   recompile, so the SOURCE is restored while the object file still carries
   the mutation (or a probe survives in a linked test binary). Restore with a
   plain copy and `touch` the file; treat two mutations with byte-identical
   failure lists as a harness alarm, not a coincidence.

M3 — Geometry Kernel Adapter & First Parametric Solid — is COMPLETE
(independent review APPROVE 97/100; see `docs/reviews/M3_CompletionReport.md`).

M4 — Sketch/Profile Foundation, Pad/Extrude & Basic 3D Viewer — is COMPLETE.
Functional independent review APPROVE 96/100; the UI was validated by owner
manual validation rather than an independent UI review, per ADR-M4-016. See
`docs/reviews/M4_CompletionReport.md`.

M5 — Sketch Constraints & Dimensional Parameterization — is complete on
`master` (`7238548`). It needed **four** independent review rounds, and every
one of them found defects that the previous round's fixes had introduced. Its
owner UI validation passed; DPI scaling remains unverified at the owner's
direction. See `docs/reviews/M5_CompletionReport.md`.

M6 — DXF Import to Stable Sketch Entities — is **merged to `master` with two
items open**, at the owner's explicit direction on 2026-08-13 so that M7 could
start. 561/561 in Debug and Release, Gates A–I pass. What is *not* established:

1. `M6.11`–`M6.14` have never been reviewed by anyone but their author. Three
   rounds ran before them; each found defects the previous round's fixes had
   introduced.
2. Owner manual UI validation is **NOT EXECUTED**. The checklist is written and
   waiting at `docs/reviews/M6_UI_UserValidation.md`. The one attempt at it
   produced `M6.14` — a property panel showing ten labels and no values, which
   none of the 561 tests could see, because all of them asked the model and none
   asked the widget.

Do not report either item as done because M6 is on `master`. See
`docs/reviews/M6_CompletionReport.md`.

Current target: **M10 — Reference Frames & Connectors** (`docs/M10_SPEC.md`), on
branch `m8-wip`, guided by the adopted
`docs/EP3D_Onshape_Alignment_Roadmap.md`. M8 (`docs/M8_SPEC.md`) is functionally
complete and NOT closed; M9 (`docs/M9_SPEC.md`) is implemented through M9.7 and
has never been reviewed by anyone but its author. See the standing blocks above
and below — M10 cannot close before M9, which cannot close before M8.

**M11.1 — the expression evaluator — is implemented** (`src/Core/Expression/`),
at the owner's direction on 2026-08-20 to take the P0 item from `todo.md`
first. It is Core-only and touches NOTHING else: no Parameter change, no
serializer change, no recompute change, no schema bump. 939/939 in Debug and
Release; 17/17 mutations killed.

- `ParseExpression` -> `ParsedExpression` -> `EvaluateExpression` /
  `EvaluateExpressionForField`. Parsing is SEPARATE from evaluation because
  `referencedVariables()` is the dependency-edge set and must be answerable
  before any value exists (ADR-M11-001..004).
- **The dimension model is three states**, not an exponent vector, because every
  compound/inverse unit is refused AT the operation (ADR-M11-001).
- **Promotion happens at the FIELD boundary only** — `3mm + 2` is an error,
  `5` in a length field is 5 mm (ADR-M11-002). This is an intentional difference
  from the reference model and it costs the reference's own ternary example.
- **A guard bounds the path it sits on, not the property it is named after.**
  The review pass found both shapes of this: `kExpressionMaxDepth` did not bound
  the EVALUATOR (`1+1+1+...` folds in a loop -- constant parser depth, one tree
  level per term), and the finiteness guard never saw a Literal, so `1e999`
  reached the model as infinity. `kExpressionMaxNodes` and a lex-time finite
  check close them (ADR-M11-003).

**M11.2 is implemented too** (ADR-M11-005..010). `setParameterExpression` is now
a VALIDATING facade: it parses, resolves names, evaluates once, and turns
`#name` into dependency-graph edges; every refusal leaves the document
byte-for-byte unchanged and returns a POSITIONED error. Recompute evaluates
expressions (ADR-011's documented limitation is closed). Edges are re-derived on
load and both save and load refuse a document that cannot carry them.
978/978 in Debug and Release; 16/16 M11.2 mutations killed.

Four things from M11.2 that bind future work:

- **A typed number REPLACES a formula** (ADR-M11-006), because undo replays
  value-then-expression as one pair.
- **Deleting a parameter another expression reads is REFUSED** (ADR-M11-007),
  which is ADR-M5-009 applied unchanged to the second kind of reference.
- **ParameterState follows the graph ONLY for parameters that have an
  expression** (ADR-M11-009). The first version synced all of them and demoted
  every freshly added literal parameter to Dirty, because a new parameter's
  graph node starts Dirty. A literal parameter is not stale.
- **A value check cannot see a wrong edge** (ADR-M11-010). Evaluation resolves
  by NAME; the edge only governs order and dirtying. Three mutations survived
  the first battery on this. The probe that works is the DIRECTION OF LEGALITY:
  a leftover `B -> A` edge makes `A = #B ...` refuse as a cycle.

**An accepted contract changed**: the expression field is no longer opaque data.
Two accepted `SerializationTests` used it as such and were updated — see the end
of ADR-M11-005 for what changed and why the subject of each test is intact.

**M11.3 shipped the UI** (ADR-M11-011..013). A parameter shows `Value` and
`Expression` as two rows; the value row locks while an expression drives it and
its tooltip NAMES the driver; a refused expression keeps the text the user typed
and delivers a three-line caret rendering in the cell's tooltip. Reachable in the
running shell as `--sample m11-expression`. 998/998 in Debug and Release.

- **The decision layer is Qt-free on purpose**: `src/Viewer/PropertyEditing.*`
  decides, `MainWindow` renders. Of 12 M11.3 mutations, 3 are killed by NOTHING
  BUT the viewer selftest -- the rejected-text restore and two tooltips. That is
  M6.14's shape (model right, screen wrong) and it is now covered.
- **A mutation harness with a broken oracle is worse than no harness**
  (ADR-M11-013). This battery first reported those three as SURVIVED because
  `$LASTEXITCODE` did not survive a native command's merged stderr under
  PowerShell 5.1 -- and when the preference was tightened it threw mid-mutation
  and LEFT THE SOURCE MUTATED. Match on the output text, and verify the tree
  after any aborted run.

**Owner UI validation for M11 is NOT EXECUTED**
(`docs/reviews/M11_UI_UserValidation.md`, every Result cell blank). The agent
checks -- 19 `PropertyEditingTest` cases plus the assertions inside the running
window -- are NOT owner validation (ADR-M4-016) and must never be cited as if
they were. **This is now the FOURTH milestone with an open owner UI
validation** (M6, M7, M8, M11).

Still NOT done, and not claimed: no syntax highlighting or `#` autocomplete, no
inline error decoration on the cell itself, and §1.2's document-scope named
parameters remain separate work -- what exists is name resolution over the
parameters a Part Studio already has.

**No independent agent reviewed M11.1.** Sub-agent spawning was unavailable, so
the Architect/Developer/Test/Reviewer roles ran sequentially in one agent, per
the fallback in `docs/OrchestratorGuide.md`. The reviewer pass found the two
defects above; a genuinely independent one may find more.


**M10 is implemented through M10.7** (all slices except the independent review).
Gates A-J plus M/N/O and P/P2 pass. What M10 added:

- **`ReferenceFrame` is FIRST-CLASS** (ADR-M10-001, superseding ADR-009 D6):
  registered, resolvable by id, a dependency-graph node, persisted (**schema
  v10**), undoable, and mutable only through the facade. A parent chain that
  would CYCLE is refused at creation and at re-parenting, by a walk BOUNDED by
  the frame count.
- **`worldTransform` is COMPOSED, never stored** (ADR-M10-002), and the
  quaternion arithmetic moved to `src/Core/Geometry/Transform.h`. **ADR-M4-002
  asked for ONE conversion site, not for that site to be `SketchFrame.cpp`** —
  the count is still one, and `SketchFrame.cpp` now calls it.
- **Sketch-on-frame** (ADR-M10-003): an optional `supportFrameId` with the
  embedded `SketchFrame` as FALLBACK, so world XY stays a CASE of the general
  path and every pre-M10 document is unaffected. A missing support frame FAILS
  loudly (`sketchSupportFrameIsMissing`); it must never fall back to world XY,
  and save refuses the dangling reference.
- **`Connector`** = a frame plus a role and an owner (ADR-M10-004). One kind,
  not two — §18.1's "implicit" route differs in when it is created, never in
  whether it can be re-resolved (A03). Visibility stays OUT of Core (§18.3, A02).
- **Mirror and Pattern** (M10.6) — ADR-M9-006's deferral CLOSED, on kernel verbs
  `mirrorShape` / `translateShape` / `fuseShapes`.

**READ ADR-M10-005 BEFORE TRUSTING ANY MASS NUMBER.**
`BRepGProp::VolumeProperties` on a COMPOUND of disjoint solids returns the right
VOLUME and a ~2% wrong CENTRE OF MASS. `calculateMassProperties` now measures
each solid and combines with `GProp_GProps::Add`. **The defect predates M10** and
would have survived indefinitely, because *every analytic oracle in this project
is a volume* — the one measurement the old code always got right.

Three shapes that keep recurring, all confirmed again in M10:

1. **A box and an extruded prism are not interchangeable in a test.** Three times
   in this milestone the failing case was the prism and the box passed — most
   sharply in `M10_KERNEL_010`, whose first version used `createBox` and pinned
   NOTHING: the mutation restoring the defect survived it.
2. **An oracle sitting at a symmetric centre hides the error it should catch.**
   GATE_M and GATE_N fuse disjoint solids and passed throughout the
   mass-properties defect, because their expected centroids are at the midpoint
   where the error cancels. Only GATE_P, whose expected value deliberately is
   not, went red. That is M8 GATE_RB2's lesson for the third time.
3. **Something during construction or load treated as a user edit.** FOUR
   instances across M9 and M10 (`restoreParameter`, the constructor's Origin,
   the loader's rollback restore, the loader's Origin removal). Every one was
   caught by the same two-line rule — *a loaded document starts with an empty
   history* (ADR-M9-001). Add a `restore*` twin; never reuse the recording facade
   from a loader.

**NEVER write a numeric id literal in a test** (ADR-M10-006). `ObjectIdGenerator`
is process-global and monotonic and every restore path calls `AdvancePast`, so a
literal both names an id AND shoves the counter at every test that runs after it.
`M4_PROFILE_022` restored the constant `999010` and failed about one shuffled run
in a hundred, because its own `addArc`/`addLine` were eventually handed 999009
and 999010. **`ctest` never saw it** — 856/856 in both configurations, many
times. `--gtest_shuffle` did. Allocate with `NextSketchEntityId()` /
`ObjectIdGenerator::Next()`: a fresh id is the only one GUARANTEED absent; a
constant is merely absent today. This includes ids picked to be "obviously
invalid" — unknown-ness is a property of the counter's position, not of the
number.

**When you add a feature type there are FIVE registration sites**, not the four
ADR-M9-006 listed: `kSolidFeatureTypeNames` + `M8_REV_322`,
`kConsumingFeatureTypeNames` + `M8_SER_205`, `SnapshotFeature` /
`RestoreFeatureFromSnapshot` + a row in `M9_UNDO_401`, and — found by M10.6 —
the reserved-typename check in `validateSaveable`, which was a chain of
concrete-type casts that made a real Mirror look like a placeholder and blocked
saving. It now asks the one question it always meant (`PlaceholderFeature`),
per ADR-M3-007: ask the capability, do not enumerate.

Prior milestone, kept because its rules still bind:

**M9 is implemented through M9.7.** `FeatureSnapshot` is the SEMANTIC description
of a feature and the ONE per-type dispatch; the serializer's loader restores
through it, so adopting it took the enumeration count DOWN. `UndoRecord` /
`PartDocument::{beginTransaction, commitTransaction, abortTransaction, undo,
redo, undoDepth, redoDepth, nextUndoLabel}`. `Body::moveFeatureToIndex`, and mass
re-pointing to the chain tail on removal.

- **Undo does NOT recompute**, exactly as `setParameterValue` does not. The
  caller decides when to rebuild — that is what lets GATE_D prove selectivity
  by counters.
- **The history is session state**, never serialized (ADR-M9-001). A loaded
  document starts empty.
- **Unreplayable operations CLEAR the history** rather than keep one that would
  lie: removing a Body/Parameter/Sketch/Material, or a feature another feature
  consumes. Observable through `undoDepth()`; pinned by GATE_C3.
- **Suppression and rollback share ONE predicate**, `isFeatureActive`: an
  inactive feature is not evaluated, not displayed, not a tail, and is walked
  past by chain resolution (`activeChainBase`). Only the REASON differs, and
  only the user cares about the reason. An inactive consumer does not consume;
  a FAILED one still does -- that distinction is ADR-M9-002 and both directions
  are pinned, because collapsing them either resurrects M8's
  healthy-looking-wrong-solid defect or makes a rolled-back body draw nothing.
- **Preview is a semantic COPY** (save/load), never apply-compute-revert
  (ADR-M9-003). Cancel cannot leave a trace because the document was never
  touched.
- **Creation is COMMANDS, not dialogs** (ADR-M9-005), each one transaction
  covering its Parameter AND its Feature -- without that, undo left an orphan
  parameter every time.

Two of M9.1's eleven tests exist because a mutation survived the first version,
and both are the project's own recurring shapes: `moveFeatureToIndex` was
untestable while every gate removed the LAST feature in its body, and gate B's
stack-drain loop was unbounded, so a mutation that broke undo made the suite
**hang** instead of fail. Bound your loops; a hanging test reports a timeout
with no finding.

M8 is implemented through all its slices (M8.1 Pocket+chain, M8.2 Revolve,
M8.3 Fillet/Chamfer, M8.4 deferral ADRs with demonstrations, M8.5 m8-chain
sample, M8.6 self-validation). M8.7 has run THREE independent review rounds
(record: `docs/reviews/M8_IndependentReview.md` -- read it before trusting
any M8 claim):

- Round 1 on `880e6cc`: REQUEST CHANGES (73/70/88) -- 2 Criticals (silently
  accepted consumption diamond; six save/load-symmetry gaps) + 10 Majors,
  all fixed (`ab8513a`): ADR-M8-008 consumption rule, 24 regression tests,
  V-battery.
- Round 2 on `ab8513a`: REQUEST CHANGES (89/78/81), 0 Criticals -- every
  round-1 finding confirmed closed; 4 new Majors, ALL in the fixes'
  enforcement machinery/evidence: the `bodies()` accessor bypass (now
  compile-time closed: Body mutators private, friend PartDocument,
  placeholder facade), the solid-type-frontier drift (now one shared
  `kSolidFeatureTypeNames` table + M8_REV_322 + GATE_BB), a vacuous
  M8_REV_308 (amended), and GATE_E3 miscredited a second time (doc retreat:
  it pins the two-layer system; the barrier's only direct pins are unit
  tests). W-battery re-killed round 2's surviving mutations.
- Round 3 on `c555269`: REQUEST CHANGES (77/84/78), 0 Criticals -- all four
  round-2 Majors confirmed closed and the W-battery audit came back clean,
  but the round-N pattern held: round 2's OWN placeholder facade was the
  seventh restore path with no duplicate-id guard (all three reviewers
  independently; save-OK->load-refused, fifth recurrence), M8_REV_322 pinned
  only HALF the type table (dropping "Chamfer" survived everything), and the
  const-accessor class had three more open doors (parameters/material/
  frames). All fixed: guard with a registry-blindness-aware feature scan +
  M8_REV_341/342, save-side id-uniqueness net, 322 fixture consumes every
  table name as a base, Parameter mutators private + setParameterExpression
  facade, material() returns const Material*, consumer-frontier table
  kConsumingFeatureTypeNames. X-battery re-killed round 3's survivors.

- Round 4 on `7a60b6b`: REQUEST CHANGES (71/68/76), scope = round 3's fixes
  AND M7's round-2 fixes, the two change sets nobody but their author had
  read. **3 Criticals**: the round-3 duplicate-id guard was ONE-DIRECTIONAL
  (its own comment named the sibling hole and closed none of them -- restoring
  a Pad onto a placeholder's id gave two features one ObjectId, and
  `removeObject` then destroyed the WRONG one and left an unregistered
  graph-less orphan that saved and loaded cleanly); `savePartDocument` wrote a
  `dependencies` edge its own loader refuses (ADR-M3-008, SIXTH recurrence,
  a different route from round 3's); and the X2 "masked by design" record entry
  was refuted by execution. Plus 7 Majors, two of them found independently by
  two reviewers each: the const-accessor class still open at
  `bodies()->features()` and at `ObjectRegistry::find` (whose pointees were
  never const), and `kConsumingFeatureTypeNames` pinning only some of its
  members with a comment claiming all -- R3R3-M1 reproduced inside the commit
  that fixed it. All fixed; see the round-4 section of
  `docs/reviews/M8_IndependentReview.md`.

**M8 still CANNOT CLOSE**: round 4's fixes are themselves unreviewed (round
5 or accepted-risk is the owner's call -- the change set shrinks each round),
and owner UI validation is open for M6, M7, M8, M9 AND M10 — all five checklists exist (`docs/reviews/M*_UI_UserValidation.md`) and every Result cell in all five is blank. Writing the rows is the part an agent may do; filling one is not (ADR-M4-016). **Schema has since moved
v8 -> v9 (M9.4, rollback position) -> v10 (M10.4, frames and connectors); the
v8 pins in the tests moved with it.** The "unreserved type name" examples
in old tests have been renamed three times now (Radius->, Revolve->Loft,
Fillet->Sweep) -- when adding a feature type, grep tests for its name as a
placeholder first. When adding an ISolidFeature type, its name goes in
`kSolidFeatureTypeNames` (serializer) AND a consumed-as-base row goes in
M8_REV_322 -- the test only reminds you about names it consumes (round 3
proved a name absent from BOTH drifts silently). A consuming type also goes
in `kConsumingFeatureTypeNames`. **That list is now FIVE sites, not three --
see the M10 block above; M10.6 found the fifth.**

M7 state: round 2 HAS NOW RUN (three reviewers on `9e0c399`, all REQUEST
CHANGES, 72/73/73) and it proved the project's own maxim on itself -- **round
1's two Critical fixes did not close their findings, and one of them silently
voided another round-1 fix**:

- C3 was still open: `documentId` is a process-local counter, so two parts
  saved in two sessions are identity-indistinguishable and a 100x50 plate's
  plan applied cleanly to a 103x80 bracket. FIXED with a content fingerprint.
- A length-preserving edit (a dimensioned edge rotated 90 degrees about its
  own start point) validated, and the solver then silently rewrote the user's
  geometry. FIXED by re-asserting every inferred constraint.
- **All three reviewers independently**: the transactional rollback is
  unreachable dead code, its four tests are vacuous, and the fix table's
  "M12 mutation-verified" was FALSE. Fixed by telling the truth -- tests
  renamed to what they test, the claim table corrected to NOT COVERED.
- `--expect-from-source`/`--expect-skipped` were never parsed (declared only),
  so both ctest entries proved nothing. Fixed, plus the class: unknown flags
  now fail, with three WILL_FAIL negative controls.

**All six round-2 Majors are now CLOSED** by `7a60b6b` (naming
order-dependence with two dimensions on one target; `placeFix=false` re-opening
accumulation; cap-check breadth and the id-uniqueness net stopping short of
entities/constraints; no erase path for `reconstructionReports_`; Gate J
reported Skipped not Failed; no CI fixture rendering a skip row;
`--gtest_shuffle` failing on generator poisoning). Round 4 re-verified five of
the seven claimed pins as exact and found two that were not -- see below.
Read `docs/reviews/M7_IndependentReview.md` round 2 before trusting any M7
claim.

**Also still open, and M8 cannot close before they do:**

- M8 owner UI validation (`docs/reviews/M8_UI_UserValidation.md`, every row
  blank). The checklist covers all four required M8 features because the shell
  now has `--sample m8-revolve` and `--sample m8-dress` alongside `m8-chain`;
  before those, three of the four were unreachable from the running
  application and this validation could only have covered the pocket.
- M7 owner UI validation (`docs/reviews/M7_UI_UserValidation.md`, every row
  blank). Agent-executed mechanical checks live in
  `docs/reviews/M7_UI_AgentExecutedChecks.md` and are NOT owner validation
  (ADR-M4-016) -- they cannot fill a judgement row and must never be cited
  as if they had.
- The two inherited M6 items (M6.11-M6.14 unreviewed; M6 owner UI validation
  not run).

Read `docs/reviews/M7_IndependentReview.md` before trusting any M7 claim, and
`docs/reviews/M7_SelfValidationReport.md` only as corrected.


---

**M12 — the 2D sketch drawing UI — is implemented** (`docs/M12_SPEC.md`), at the
owner's direction on 2026-08-20. **1051/1051 in Debug and Release.** Before it,
`MainWindow.cpp` contained ZERO
calls to `addLine` / `addCircle` / `addConstraint`: Core had had a full sketch
model since M5 and no user could reach any of it with a mouse. `todo.md` §15
listed that as the item blocking ALL real user validation.

Owner decisions taken on 2026-08-20, and they bind what comes next:

- **A 2D canvas FIRST, an OCCT overlay on a plane in the 3D view SECOND, sharing
  one implementation.** This is why `src/Viewer/SketchCanvas.*` and
  `src/Viewer/SketchCommands.*` are Qt-free AND OCCT-free: the hit-testing,
  snapping, tool state machine, constraint applicability and dimension
  placement must not be rewritten for the second renderer. **Do not move any of
  that logic into `SketchCanvasWidget`.**
- **A complete usable round**, not a drawing-only preview.

What M12 added:

- **Drawing**: Point, Line (chained), Rectangle, Circle, Arc, with endpoint /
  centre / origin / on-curve / grid snapping and Shift to suppress inference.
- **Inference IS a constraint generator** (roadmap §4.2). A snap that produces a
  reference produces a real `CoincidentConstraint` with a real id, counted in
  DOF and deletable. If any auto-constraint is refused the WHOLE command aborts
  -- a half-applied inference makes the DOF readout lie.
- **A rectangle is 4 lines + 2 Horizontal + 2 Vertical + 4 Coincident** in ONE
  transaction (roadmap §4.1), not a rectangle topology.
- **Dimensions**: one `D` command that INFERS the type from the selection
  (roadmap §7.1), plus explicit Radius/Diameter overrides. Each creates a
  Parameter and a constraint in one undo step, seeded at what the geometry
  measures, so adding a dimension never moves anything. Expressions work
  (`#Width / 2`, M11). **Angles are stored in radians and typed/shown in
  DEGREES, converted in exactly one place** (`CommitDimensionValue`).
- **Constraint manager** (roadmap §6.3) with an `AT FAULT` TEXT column, a
  **Delete Constraint button on the dock** (ADR-M12-014 -- the menu entry alone
  was unfindable, and deleting a constraint is the only way out of an
  over-constrained sketch), and a status line whose badge and sentence are never
  displaced by a command message.
- **Reshaping geometry in place is `PartDocument::setSketchEntityGeometry`**
  (ADR-M17-010) -- the only way to trim, extend or chamfer without discarding
  the constraints on the entity. Delete-and-recreate issues a NEW id and
  cascades every constraint away (ADR-M5-009).
- **A feature-creation command reports the FEATURE's own diagnostic**
  (ADR-M17-022). "Pad created" was printed whether it computed or not, so a
  failed pad reported success and drew nothing. **Fillet and Chamfer were still
  doing it** long after Pad, Pocket and Revolve stopped -- fixed at M17.11, and
  the smoke test now squeezes the part until the default radius cannot fit and
  checks the command says so. Every creation command goes through
  `describeCreatedFeature`; a hardcoded success string is the defect.
- **The Model toolbar shares the Insert menu's ACTIONS** (ADR-M17-026), so
  enabling stays in one place and the two surfaces cannot disagree. Buttons show
  a short `iconText()`; the menu keeps the descriptive wording. **There is no
  Hole feature** -- a hole is a Pocket with a circular profile, or a loop inside
  the profile (ADR-M17-024), and Pocket's tooltip says so.
- **An editable property row MUST carry a writable `field`** (ADR-M17-027).
  `PropertyRow::field` defaults to `PropertyField::None`, and five rows were
  built with the seven-argument aggregate -- so Pad's Length, Pocket's Depth,
  Fillet/Chamfer's size, Revolve's Angle and every dimension constraint value
  were cells that accepted typing, kept it on screen, and changed nothing.
  Every existing test chose the field ITSELF; the panel reads it off the row,
  so no test ever crossed that seam. Build editable rows with
  `EditableValueRow(group, label, parameter)` -- **a factory cannot forget an
  argument it does not take** -- and drive rows in tests the way the panel
  does, through `row.field`.
- **View > Solid / Wireframe changes SOLIDS only** (ADR-M17-032). Sketches are
  always wireframe -- they have no faces, and switching the whole scene would
  make the one object that cannot be shaded look broken. The switch is applied
  to the presentations already in the scene, not by rebuilding it, because a
  rebuild drops the selection. Read the mode BACK from the viewer to tick the
  menu; a menu that ticks itself is how the toolbar and the canvas came to
  disagree once already.
- **Tangency at a KNOWN point is PERPENDICULARITY, not distance**
  (ADR-M17-044). `TangentLineCircle` removes no freedom at all once a
  coincidence has pinned the touch point onto the line: the perpendicular
  distance cannot exceed the radius there, so the residual sits at a maximum
  and its gradient vanishes. It is true and not load-bearing, which is why a
  slot read DOF 9 instead of 5 and a fillet's tangency was decoration.
  `TangentConstraint::at` names which end of the line touches, and it is
  STORED, never re-derived from whichever coincidences happen to exist.
- **"Fully constrained" is PER ENTITY, not per sketch** (ADR-M17-053).
  `Sketch::isEntityFullyConstrained`. The sketch-wide status only carries
  TROUBLE (conflicting/invalid), which is a property of the whole system. A
  sketch containing a spline never reaches DOF 0, so colouring from the sketch
  status stopped meaning anything the moment splines existed.
- **A variable is free iff the Jacobian's null space has a component along it**
  (ADR-M17-053) -- the same question the rank answers, asked per column. SVD
  for the null space, the same relative threshold as the rank, so "how many"
  and "which" cannot disagree.
- **A failed or unrun solve knows NOTHING about which entities are pinned**
  (ADR-M17-053): the set is cleared, and false reads as loose. Claiming pinned
  would be claiming a measurement nobody made.
- **The script socket is ON BY DEFAULT, loopback-only, and never invisible**
  (ADR-M17-052): the port is in the title bar for as long as it is open, and
  `--no-listen` turns it off. A busy port falls back to a free one -- it must
  never stop EP3D starting.
- **The script socket is LOOPBACK-ONLY and has no flag to widen it**
  (ADR-M17-052). It executes commands that create, modify and save files;
  bound anywhere reachable it is an unauthenticated command service.
  `QHostAddress::LocalHost` appears at exactly one call site. Do not add an
  option for it.
- **`SketchScriptSession` is the primitive; `RunSketchScript` is one line on
  top** (ADR-M17-052). A socket delivers one command per message, so the
  current sketch, tool and names have to survive between calls. One session per
  connection -- two clients sharing one would finish each other's splines.
- **TCP is a stream: run only COMPLETE lines** (ADR-M17-052). A command can
  arrive in two packets and two commands in one. The viewer's `--selftest`
  covers both cases over a real socket.
- **Line numbers are formatted by the TRANSPORT, not the interpreter**
  (ADR-M17-052). `ScriptOutcome::message` and `ScriptLogEntry::text` carry no
  prefix; the file runner adds `line N:` and the socket does not.
- **`ep3d` is a SEPARATE console binary, not the viewer with a flag**
  (ADR-M17-051). The viewer builds a QApplication before it reads argv, so
  "headless" through it is still a GUI process. ViewerCore is Qt-free, so the
  script interpreter links it directly.
- **A script drives the SAME path the mouse does** (ADR-M17-051): `click` goes
  through `SnapCursor` and `SketchCanvasModel`, `constrain`/`dimension` through
  `requestConstraint`/`requestDimension`. Never build constraint structs
  directly from the script -- that is a second way into the model, and the
  second way is the one that keeps working while the first breaks.
- **The script's tool/constraint/dimension tables ARE the help text and the
  error message** (ADR-M17-051). One list each. `M17_CLI_001` counts the tool
  table against the enum's range so a new tool cannot be forgotten.
- **`IsCentredRef` is what Concentric needs, not `IsCurveRef`** (ADR-M17-051).
  `IsCurveRef` deliberately excludes ellipses because its other callers all
  want a single radius; Concentric only needs a centre, and asking the wrong
  predicate made it refuse a constraint the solver had supported for a
  milestone.
- **The serializer's READER is generic about bound Parameters, like the writer**
  (ADR-M17-050). It used to assign `c.parameterId` inside each kind's own
  branch -- eight copies -- and the two ellipse dimensions never got one, so
  they saved correctly and reloaded bound to nothing. Adding a dimensional
  kind now needs no serializer change at all.
- **Regenerate `examples/spline-and-ellipse.ep3d` when the format changes**:
  `cmake --build build --config Debug --target MakeExampleEp3d`, then run it.
  It saves, reloads and re-solves, so it walks the whole path -- which is how
  it found the bug above that 1400 tests had not.
- **A spline's variable count is not a property of its type** (ADR-M17-049) --
  the first such entity. `EntitySlots` carries a vector for it, and its points
  are written back BY POSITION: its interior points all carry `Whole`, so the
  sub-element routing every other kind uses would collapse them onto one field.
- **Only a spline's two ENDS can be named by a constraint** (ADR-M17-049).
  `SketchElementRef` is entity + sub-element and there are four sub-elements;
  an interior point is a variable the solver moves and nothing can name. Said
  out loud rather than worked around.
- **Core samples a spline (Catmull-Rom); the kernel interpolates it (OCCT)**
  (ADR-M17-049). They agree exactly AT the points and to within a fraction of a
  chord between them. Core's sampler decides drawing, picking and containment;
  the kernel decides the solid. Use `SampleSpline` -- never a second sampler.
- **An ellipse's two params are PARAMETERS, not angles** (ADR-M17-048).
  `point(t) = centre + R(rot)*(a cos t, b sin t)`; `t` equals the geometric
  angle only on a circle, and they agree exactly at the four axis points --
  which is what lets the mistake survive testing. `PointOnEllipse` and
  `EllipseParamOf` are the only two conversions; never reach for atan2.
- **A `get_if` chain that ends in a bare `std::get<T>` is a landmine**
  (ADR-M17-048). Four of them became `bad_variant_access` the day
  `SketchGeometry` grew -- one on the commit path of every solve. End such
  chains with an exhaustive `std::visit` and a `static_assert`. A FIFTH was
  missed in that sweep and killed the test process outright the next milestone
  (ADR-M17-049): there were two copies of "are these the same geometry", and
  only one got fixed. There is now one, `SameSketchGeometryValue` in Core.
- **The solver's damping is a FRACTION of J'J, not an absolute number**
  (ADR-M17-048), and the iteration counter increments in ONE place. Both were
  wrong and both only showed up on a residual nonlinear enough to need them.
- **A new dimensional constraint needs THREE lists updated or it is silently
  refused** (ADR-M17-048): the variant, `UnitMatches` (if it is angle-valued)
  and `DimensionValueValid` (if zero or negative is legal for it). Missing
  either produces no error the user can see -- just one more degree of freedom.
- **A command that writes to a sketch must READ EVERYTHING FIRST**
  (ADR-M17-047). `const Sketch*` points into the document, and every
  add/set/remove is a write to it -- Transform's copy loop walked
  `sketch->constraints()` while appending to it, copied two of eight, and
  reported success.
- **Copying geometry gives each copied DIMENSION its own Parameter**
  (ADR-M17-047, the rule Offset already followed). Sharing the original's looks
  right and then refuses to let the copy be resized.
- **A constraint's referenced elements live in ONE list**,
  `VisitConstraintElements` (ADR-M17-046) -- a template over const and mutable
  data, so reading them and retargeting them cannot drift. `ReferencedElements`
  and `ReferencedEntities` are both derived from it.
- **Splitting an entity decides each constraint's fate explicitly**
  (`SurvivesSplit`, ADR-M17-046): EveryPiece for a property every piece
  inherits, OwningPiece for one that names a point, Refuse for one about the
  whole extent. A Refuse REFUSES THE SPLIT and names the constraint -- copying
  it and dropping it are both silent changes to the user's model.
- **This repo has a large amount of UNCOMMITTED work.** Never
  `git checkout -- <path>` to undo an edit: it restores from the index, which
  for most of these files is many milestones behind, and the working copy is
  the only copy. Back a file up by hand before experimenting on it.
- **`TangentConstraint::at` names an end of `a`, not "of the line"**
  (ADR-M17-045). Line-curve pairs must put the LINE first; curve-curve pairs an
  ARC (a circle has no ends). That is what lets the tangent arc ask for a pinned
  tangency in one line without knowing which kind of host it grew from.
- **Two curves touching at a known point is COLLINEAR RADII**
  (ADR-M17-045), `cross(P - C1, P - C2) / (r1 r2)`. The centre-distance pair has
  the same vanishing-gradient disease as `TangentLineCircle`, because two
  circles sharing a point already satisfy |r1-r2| <= |C1-C2| <= r1+r2. There is
  no internal/external branch in the pinned form and it does not need one.
- **"Parallel" is not "smooth"**: the complementary arc has the same centre,
  radius and tips and doubles back from the joint -- a cusp whose sine is a
  clean zero. Smoothness assertions come in PAIRS (sine ~ 0 AND cosine > 0); a
  sine-only check let a reversed-heading mutant live.
- **A residual kind that is not in `SlotsRequired`'s switch used to be
  UNCHECKED** (ADR-M17-044) -- 0 meant both "reads no variables" and "nobody
  declared this", so the guard skipped it. Seven kinds lived there. `SlotAccepts`
  is a PREDICATE (an arc tip's angle slot legitimately takes either angle, which
  a lookup could not say), an unlisted kind accepts nothing, and an undeclared
  arity is now a refusal rather than a skip.
- **The schema version has ONE literal, in
  `SerializationV13Test.M17_SER_001`** (ADR-M17-044). Everything else asks
  `CurrentSchemaVersion()` or uses `tests/Support/SchemaVersionText.h`. Five
  suites used to spell it out, so every bump turned four unrelated files red.
- **A dimension's formula lives ONCE, in `MeasureConstraint`** (ADR-M17-042).
  It existed twice -- as the solver's residual and as the "seed" that makes
  adding a dimension not move anything -- and a reference dimension would have
  needed a third. Three answers to "how far apart are these points" is a
  reference dimension reading a number the driving one does not.
- **A driven dimension is measured AFTER the commit** (ADR-M17-042), from the
  session's own list of which constraints were driven. Measuring before
  publishes the previous solve's number, which is plausible and wrong.
- **A dimension and its placement are ONE undo step** (ADR-M17-041): the
  position rides on `SketchEdit`, written inside ApplySketchEdit's transaction.
  Writing it afterwards made Ctrl+Z move the dimension back to its automatic
  spot and leave it there. `setSketchDimensionPlacement` only records a delta
  and opens no transaction, so calling it from inside one is safe -- unlike the
  ScopedTransaction facades below.
- **`ScopedTransaction` ABORTS what is not explicitly committed** -- that is
  what makes a facade bailing out halfway safe, and it bites twice: calling
  such a facade from inside an open transaction rolls the CALLER's work back
  too (ADR-M17-040, the polygon's lines), and writing one without
  `transaction.commit()` silently undoes itself (ADR-M17-042). Create
  construction geometry with `addSketchEntity(..., construction)` in one step.
- **`PendingConstraint` handles a fixed set of kinds** -- extend it when a tool
  needs another, and note that an unknown kind now reports THAT rather than
  "the sketch refused", which described a constraint the sketch was never
  offered (ADR-M17-040).
- **Renaming goes through `PartDocument::renameObject`** (ADR-M17-039): one
  undo step, duplicates REFUSED, surrounding space trimmed. `setName` is
  private on all five named types with `friend class PartDocument`, so there is
  no way around either rule. The Name property row carries the OBJECT's id in
  `parameterId` -- that field has always meant "what to write" -- and
  `ApplyPropertyEdit` answers the Name field FIRST, before the parameter
  lookup that would otherwise refuse every sketch and feature rename.
- **A refused edit KEEPS the typed text on screen** (M11.3), so a smoke check
  of a refusal must read the MODEL, not the cell.
- **Every created object gets a UNIQUE name** (ADR-M17-038) via
  `uniqueObjectName`. Two rows reading "Pocket" is not cosmetic: the middle
  link of a chain cannot be deleted reversibly and the tail can, and the owner
  lost their undo history picking the wrong one. For PARAMETERS it is a
  correctness bug -- expressions resolve by NAME and `findByName` answers with
  the first match, so two `PocketDepth`s means every `#PocketDepth` binds to
  whichever was created first.
- **Never look a parameter up by the name a command gave it** -- ask the
  FEATURE for its own id (ADR-M17-038). A smoke check did, and it asserted a
  naming convention while claiming to assert the revolve's angle.
- **Whether a delete can be undone is READ FROM THE MODEL** (ADR-M17-037), by
  comparing `undoDepth()` across `removeObject`, never guessed from the type.
  Core records a parameter, a frame, a connector and an UNCONSUMED feature; it
  CLEARS the history for a consumed feature, a sketch, a body or a material --
  and `removeObject`'s own comment says the clearing is observable "so a UI can
  tell the user the history ended". No UI did, for two milestones: deleting a
  consumed pad wiped every undo step and said "Deleted".
- **`FeatureSnapshot` has TWO producers** -- `SnapshotFeature` and the JSON
  loader -- and a field added to the struct must reach both (ADR-M17-037).
  `edgeSelection` reached only the loader, so undoing a fillet's deletion
  brought it back dressing every edge instead of the chosen face: the solid
  changed and nothing said so.
- **A tracked face is re-resolved FIRST in recompute** (ADR-M17-036), before the
  solve: the solve works in (u,v) and the plane is what turns (u,v) into a
  position, so solving first gives this pass's geometry on last pass's plane and
  nothing downstream can tell. Set it through `setSketchTrackedFace`, which adds
  the graph edge BEFORE storing the query -- `addDependency` is the only thing
  that refuses a cycle. A face that cannot be found FAILS; keeping the last known
  plane is geometry sitting where the model no longer says it belongs.
- **A refusal on LOAD must fail the load** (ADR-M17-036). Tracked faces were
  first applied while restoring sketches -- before features exist -- so the
  facade refused every time and the refusal was dropped silently: the file said
  the sketch followed a face and the loaded document said it did not.
- **`FaceQuery` is a CONJUNCTION, `EdgeSelection` a UNION** (ADR-M17-036), and
  that is what composition looks like here: `{createdBy: pocket, extremeTowards:
  +Z}` names the pocket floor, which neither half can name alone. Ambiguity is
  REFUSED with a count, never resolved by picking one.
- **`CreatedBy` is the only query that survives geometry MOVING** (ADR-M17-035),
  because it describes provenance rather than shape -- and the only way to name
  an INNER face, since "the outermost face towards +Z" is the top, not a pocket
  floor. Provenance is recorded by comparing result against base AFTER the
  build (`tagCreatedFaces`), not by threading a tag through five kernel
  signatures for a fact none of those operations needs.
- **Changing what a feature PRODUCES must dirty it** (ADR-M17-035).
  `setEdgeSelection` was public once, and three different selections gave three
  identical solids: the graph was never told, so recompute skipped the feature
  and handed back the previous shape. It is private with `friend class
  PartDocument` now; go through `setFeatureEdgeSelection`.
- **Serialise a variant with `std::visit` + `if constexpr`, never an if/else
  chain ending in a bare `else`** (ADR-M17-035). The chain was written for three
  alternatives; a fourth made the `else` reach for the wrong one and throw on
  SAVE. A visit fails to COMPILE instead.
- **An edge selection is a QUERY, re-answered every rebuild** (ADR-M17-034) --
  never a stored edge and never an index into one. No query may hold
  coordinates from when it was made: a plane at z=20 is an index in better
  clothes. A selection matching NO edge FAILS with the query spelled out; a
  pick that the query vocabulary cannot express is REFUSED, never stored as the
  nearest thing, because the nearest thing dresses different edges next rebuild
  and looks deliberate.
- **The model tree is a TIMELINE that absorbs** (ADR-M17-033). Rows are ordered
  by ObjectId -- every id comes from one generator, so id order IS creation
  order, and restore keeps it -- and a sketch is drawn INSIDE the feature that
  consumes it. A sketch with two consumers is absorbed by neither: nesting it
  under one would be false for the other. Ask `ISketchConsuming`, never a list
  of concrete feature types; that list was already found out of date once, after
  Pocket and Revolve had both shipped.
- **rollUp only fills rows with NO state of their own** (ADR-M17-033). A
  container summarises because it has nothing else to say; a feature's state is
  a fact about that feature. Absorption made this load-bearing: a Pad BLOCKED by
  its sketch was being overwritten to Failed by its own child, destroying the
  distinction M5_DEF_012 exists to protect, in the case it was written for.
- **An extrusion distance is SIGNED** (ADR-M17-031): the sign chooses the side
  of the sketch plane. Guard it with `IsValidSignedExtrusionDistance`, NOT by
  relaxing `IsValidExtrusionDistance` -- that one also guards a fillet radius and
  a chamfer distance, where a negative is meaningless. A negative extrusion must
  still come out with a POSITIVE volume; an inverted orientation gives OCCT a
  negative one and every mass readout repeats it.
- **Direction is the SIGN, never a second stored flag** (ADR-M17-031). The
  `Reversed` property row reads and writes the sign of the SAME parameter the
  size row writes. A separate `reversed` field would be two truths about one
  fact: type -20 into Length and the flag would still say forward.
- **A pocket that removes nothing is LEGAL** (ADR-M8-002 stands): M8's gate pins
  a tool landing inside the hole of an annulus. Volume alone cannot tell that
  apart from a pocket built on the wrong side of a face, so the shell's Pocket
  COMMAND says so in the status bar instead of Core failing the feature -- what
  is allowed did not change, only what the user gets to read.
- **Negative is no longer how a test forces a failure** (ADR-M17-031). Several
  release gates used a negative length or depth as their canonical invalid
  input; they now use 0.0, which still has no magnitude. The value was always
  the lever, never the subject.
- **Never hand-copy one struct into another that holds the same data**
  (ADR-M17-030). `readPickedFace` copied the kernel's `FacePlane` field by field
  into a viewer-side `PickedFace` and dropped `boundary` -- so sketching on a
  face projected nothing while every layer was correct and 1287 tests passed.
  Neither struct named an OCCT type, so the second one had no reason to exist:
  `FacePlane` lives in Core and `PickedFace` is an alias for it. Watch for tests
  that REPRODUCE the production copy -- the integration helper did, which is why
  it could not catch the bug.
- **Sketches are drawn in the part view** (ADR-M17-030), as a wireframe on their
  own plane, via `BuildSketchWireframe` in the kernel layer -- where `BoundsOf`
  lets a test ask WHERE the geometry ended up. A sketch on a tilted plane drawn
  flat at the world origin looks perfect on the 2D canvas, which never asks
  where the plane is. A sketch consumed by a pad is STILL drawn, unlike a
  consumed solid: the two are different things, and the outline on the face is
  how a user checks the pad did what they meant.
- **The sketch toolbar is TWO rows** (ADR-M17-030). Qt hides toolbar overflow
  behind a chevron, so 36 icon-only buttons in one row left the last third off
  screen entirely. The split happens after the bar is built, at the separator
  nearest the middle; every readback goes through `sketchToolbarButtons()`,
  which spans both rows -- a per-row readback would silently halve the
  distinct-icon check.
- **Projected reference geometry is NOT an entity** (ADR-M17-029). A
  `SketchReference` has no solver variable, contributes no profile edge, and no
  constraint may name it -- so it lives in its own container, `references_`.
  Modelling it as an entity with a third flag would put it inside every loop
  that walks `entities_` (solver, profile, serializer, canvas, undo), and one
  forgotten skip is a reference edge silently becoming part of the solid.
  Add them through `PartDocument::addSketchReferences`, never straight into a
  Sketch.
- **A projection that drops an edge must SAY SO** (ADR-M17-029). A circle whose
  plane is tilted relative to the sketch projects to an ellipse, and EP3D has no
  ellipse entity -- so it is skipped and counted, never approximated by a circle
  of the original radius or of the projected minor axis. Both wrong answers look
  right and measure wrong. `ProjectedBoundary::skipped` and `skippedReason`
  carry the count and the cause into the status line.
- **Use converts through an ORDINARY edit** (ADR-M17-029). `PlanConvertReference`
  returns an AddPoint/AddLine/AddCircle/AddArc `SketchEdit`, not a new edit kind,
  so the undo delta, the label and the aborting transaction are all inherited. It
  fixes only what can be fixed WITHOUT redundancy: a line's two endpoints, but
  only the CENTRE of a circle or an arc -- pinning an arc's centre and both tips
  is six residuals against five degrees of freedom, and the solver would blame a
  constraint the user never added.
- **A sketch on a face is placed on that face's PLANE, not bound to the face**
  (ADR-M17-028). There is no topological naming yet, so a face reference could
  not survive a rebuild; the sketch stays where it was put, and the success
  message says so out loud rather than leaving the limit in a document. The 3D
  view now selects `TopAbs_FACE` (superseding the whole-object half of
  ADR-M4-004) -- object selection is unaffected, because
  `SelectedInteractive()` still resolves the picked face back to its shape.
  Read the plane with `PlaneOfFace` in the kernel layer, where a test can hand
  it a real box; **the normal must honour `TopAbs_REVERSED`**, or half of every
  solid's faces pad back into the material.
- **`selectObject()` re-arms the commands** (ADR-M17-025). It is the one place
  every selection change goes through, and it used to update the tree, the
  viewer and the properties panel WITHOUT refreshing enabled state -- so a
  command's availability only caught up when some other command happened to
  refresh it. Finish Sketch does; clicking a row in the tree did not, which left
  "Edit Selected Sketch" greyed out forever in a document whose only content was
  a sketch. Ask the QACTION whether a command is offered; the model's opinion is
  the half that was already right.
- **Profiles carry HOLES** (ADR-M17-024). `ValidatedProfile` has `inners`, and
  `BuildProfile` walks EVERY loop then decides the outer one by CONTAINMENT --
  never by size or draw order. Refused, never guessed: side-by-side loops (two
  solids), a loop inside a hole (an island), and loops that cross. In OCCT each
  inner wire is added **reversed**; a wire wound like the outer one describes a
  second boundary, and the result is a face OCCT accepts and nobody wanted --
  which only a VOLUME check can see.
- **File > Open works by RE-SEATING, not replacing** (ADR-M17-023).
  `PartDocument` is non-copyable and non-movable on purpose, so opening changes
  WHICH document the window, presenter and canvas point at -- all three hold
  pointers, which is what makes it possible. The window owns only documents it
  LOADED; the constructor's belongs to its owner. Kernel and solver are the
  application's and are carried across to the loaded document.
- **An arc's TIPS are solver variables** (ADR-M17-018) -- two per tip, bound to
  the centre, radius and angle by `ArcTipU`/`ArcTipV`, so a tip adds no freedom
  and **every constraint that holds a point holds an arc's end**. This REPLACES
  ADR-M12-003's refusal. A free arc now reports 5 DOF, which it always had; the
  old 3 was under-counting. Angles are the state, tips are derived -- never
  write solved tip coordinates back. The commit routes by sub-element THEN by
  entity type, because a line's StartPoint and an arc's StartPoint mean
  different things and folding them together made a tip overwrite a centre.
- **Fillet is arc + 2 Coincident + 2 Tangent + two setbacks** (ADR-M17-019), one
  transaction. Without the tangencies the corner is smooth today and kinked
  after the next parameter change. **A test that moves nothing proves nothing**:
  the first version of its solve test only recomputed a freshly-placed fillet,
  which is already consistent, and survived a mutation that unbound the arc tips
  entirely.
- **Dragging is PIN-then-SEED, and the problem is built BEFORE anything moves**
  (ADR-M17-017). Pinning alone makes any constraint the cursor violates report
  Conflicting, so the geometry never moves; seeding alone lets the solver split
  the difference and a dragged corner meets the cursor halfway. Both attempts
  are needed. **The ordering is load-bearing**: `FixConstraint` reads its target
  from the geometry at build time, so writing the cursor into the sketch first
  re-baselines every Fix onto the cursor and a pinned point follows the mouse
  anywhere while reporting Solved. The cursor is an INITIAL GUESS only.
  Previews record nothing; `commitSketchDrag` records every entity that MOVED,
  not just the grabbed one, as one undo step.
- **Symmetric is TWO residuals** (ADR-M17-015, schema **v16**): the signed
  distances to the line SUM to zero, and the segment joining the pair is square
  to it. Either alone admits points that are plainly not mirror images. It takes
  two POINTS and a line entity, not two entities and a stored axis -- so moving
  the mirror moves everything mirrored across it.
- **Mirror creates TIED copies** (ADR-M17-016), not stamps: both ends of a line,
  centre + Equal for a circle, and for an ARC the two tips **crossed** plus Equal
  and NOT the centre (ADR-M17-020) -- five equations for five freedoms, because
  adding the centre too would make every mirrored arc read as over-constrained.
  A reflection REVERSES the sweep, so the angles swap; not swapping them draws
  the complementary arc. The mirror is the LAST line selected. One transaction.
- **Trim handles arcs by moving an ANGLE** (ADR-M17-020); cuts are a fraction
  along the sweep, the same vocabulary lines use. A CIRCLE is still refused --
  trimming one changes its KIND, not its extent.
- **Extend shares Trim's machinery and its rules** (ADR-M17-013): lines only,
  in place, the end chosen by where you pointed. It grows FORWARD only -- a
  boundary behind the end is not somewhere to grow to -- and stops at the
  nearest one. Trim and Extend are mutually exclusive modes.
- **Chamfer DELETES the corner's own Coincident** (ADR-M17-014) and adds two.
  The old one said the two ends are one point, which the chamfer has just made
  false; leaving it hands the user a conflict they did not create. The whole
  command is ONE transaction and one Ctrl+Z, and it adds +2 DOF -- the two
  setbacks, which the user may then dimension.
- **Trim keeps the constraints and only trims LINES** (ADR-M17-011). The moved
  endpoint keeps its identity, so constraints on it still mean what they meant.
  Curves are refused because an arc's ends carry no solver variables; a middle
  piece is refused because splitting raises "which half keeps the constraints",
  which is not a guess to make.
- **A new command is not done until the shell smoke test drives it**
  (ADR-M17-012). Offset shipped with a decision layer, passing tests and NO
  BUTTON. Also: **Trim is a mode, so picking any drawing tool leaves it** --
  otherwise trim eats every click and every tool looks broken -- and
  `syncSketchToolButtons()` must SKIP the Trim button, which is checkable but
  carries no tool data.
- **Signed dimensions may be NEGATIVE OR ZERO** (ADR-M17-009, a fix). ADR-M5-002's
  "strictly positive" rule covers lengths, radii and point-to-point distances --
  quantities with no meaning at zero. It does NOT cover the signed separations
  (Horizontal/VerticalDistance, PointLineDistance), where zero means "aligned"
  and the sign records which side. `DimensionValueValid` rejected them, which
  made the behaviour HorizontalDistanceConstraint documents unreachable.
- **Offset creates a RELATIONSHIP** (ADR-M17-008): Parallel + Equal +
  PointLineDistance for a line, Concentric + Radius for a curve. Its parameter
  is seeded from the RESIDUAL'S formula, not the requested distance -- the
  formula is positive to the RIGHT of start->end, so a copy on the left measures
  negative, and seeding the requested value makes the first solve flip the copy
  across the source.
- **Construction geometry is a FLAG on SketchEntity** (ADR-M17-005, schema
  **v14**), not a type. It is drawn, snapped to, constrained and dimensioned
  like anything else; the only difference is that `BuildProfile` skips it. The
  flag is written only when true, so a sketch without construction geometry is
  byte-identical to v13. Switching is one undo step for the whole selection but
  **one delta per entity**, so a mixed selection undoes to what each one was.
- **An inferred constraint whose EXISTING target is gone is DROPPED, not failed**
  (ADR-M17-006). Undo mid-polyline deletes the segment the chain is holding, and
  failing the edit left the user unable to draw at all. This is not roadmap
  4.2's case: the thing that was snapped to no longer exists, so there is no
  relationship to record and the DOF is the same either way.
- **Horizontal/Vertical distance are their own SIGNED constraint kinds**
  (ADR-M17-001, schema **v13**). Residual is `(b - a) - target`, never
  `|b - a| - target`: the absolute form has no derivative at zero, which is
  exactly where the solver sits when asked for a horizontal gap between two
  vertically-aligned points. The consequence is that ref ORDER is semantic --
  `requestDimension` orders the pair so a new dimension reads positive, and the
  serializer writes `a`/`b` as-is. Neither kind is ever INFERRED from a
  selection; two points admit three measurements and picking one would be a
  silent guess.
- **Esc leaves the tool in ONE press** (ADR-M17-002), dropping any half-drawn
  shape with it.
- **Clicking a handle of an already-selected entity NARROWS the selection**
  (ADR-M17-003) rather than adding to it -- otherwise the click-the-line-then-
  click-its-end flow ends in "one line plus one point", which roadmap 7.1 has no
  dimension for. Handles are drawn only for points the solver has variables for.
- **A sketch has a REAL origin point** (ADR-M12-017): New Sketch materialises a
  fixed `SketchPoint` at (0,0), and the `Origin Point` toolbar command adds one
  to a sketch that lacks it (DXF imports, programmatic sketches, or one the user
  deleted). It exists so the origin can be SELECTED and dimensioned from -- the
  canvas always drew a marker there, but a marker is not a thing. Teaching the
  solver a constant point would mean a new residual variant per constraint kind;
  a fixed Point costs nothing and changes no DOF. **Consequence**: a corner drawn
  on the origin now snaps to that POINT and earns a Coincident, so ADR-M12-011's
  Fix path applies only to sketches without an origin point.
- **Constraint badges are CLICKABLE** (ADR-M12-015/016). Their layout comes from
  `ConstraintBadgesFor()` in the Qt-free layer, because the painter and the
  hit-test have to read the SAME layout -- two copies drift and the symptom is a
  badge you cannot click where it is drawn. Clicking one picks that constraint,
  moves the panel's selection to it, and **clears the geometry selection**, so
  `Del` has exactly one meaning. **`Del` has two entry points** -- the canvas's
  own `keyPressEvent` and a toolbar QAction scoped to the canvas -- and Qt gives
  the action first refusal, so both must call
  `deleteSelectionOrHighlightedConstraint()`. A decision written only in
  `keyPressEvent` is one the user never reaches; that shipped once and only the
  `--selftest` caught it.
- **Selecting a panel row HIGHLIGHTS, it does not select** (ADR-M12-013).
  `SketchCanvasWidget::setHighlightedConstraint()` rings the glyph and thickens
  the geometry the constraint names, leaving the canvas selection alone -- the
  rule §13 states for dialog fields. The ring is a SHAPE, not a colour swap:
  colour is already carrying solve status and `AT FAULT`. The highlight is
  remembered **by id and restored across every panel rebuild**; rebuilds happen
  on every recompute, and a row index would slide onto a different constraint
  after a delete.
- **The geometry itself changes colour with the solve status** (roadmap §8.1):
  blue under-constrained, near-black at DOF 0, red in trouble. Under A06 it is a
  SECOND channel, never the only one -- the status line's words carry the same
  fact. `SketchCanvasWidget::paintedGeometryColour()` reads the colour back from
  the pen that stroked, and `--selftest --sample m12-sketch` asserts it CHANGES
  when the sketch reaches DOF 0. A colour computed but never painted is the
  M6.14 shape, and nothing that asks the document can see it.

**READ THIS BEFORE TOUCHING SKETCH SNAPPING (ADR-M12-003).** A `SnapResult`'s
reference is only ever a point the SOLVER has variables for: a Point's `Whole`,
a line's `StartPoint`/`EndPoint`, a circle's or arc's `CenterPoint`. **An arc's
two tips are deliberately excluded.** `SketchSolveSession` gives an arc a centre
and a radius and nothing else, so a reference to an arc tip resolves to the
RADIUS variable. On-curve snapping still reaches those positions and reports no
reference, which is the honest answer while EP3D has no point-on-object
constraint.

**The origin is the one exception, and it is a Fix, not a reference**
(ADR-M12-011). Snapping to (0,0) reports `SnapKind::Origin` and NO reference --
there is no origin entity to be coincident with -- and the drawing tools turn
that into a `FixConstraint` on the point the user put there. Three rules bind
any change to it:

1. **Only points the geometry OWNS are eligible.** A circle's centre, yes; its
   rim click is a radius. An arc's centre, yes; neither tip, for exactly the
   reason above -- the third click contributes an angle, so the tip is a
   projection and pinning it would claim the origin for a point that is not
   there.
2. **A rectangle fixes only the two CLICKED corners**: the first click is side
   0's start, the second is side 2's. The other two corners are derived. The
   same two corners are the only ones that earn a **Coincident** when they land
   on an existing point (ADR-M12-012) -- so a rectangle carries 8 internal
   constraints plus 0, 1 or 2 inferred ones, and any count assertion has to say
   which case it means.
3. **Never emit a second Fix.** Defined points outrank the origin in
   `SnapCursor`, so anything drawn at (0,0) afterwards snaps to the already-fixed
   point and earns a Coincident instead; the line chain carries its end forward
   the same way. A second Fix is a redundancy roadmap 8.2 then has to keep
   distinguishable from a real conflict, for no gain.

Inference suppression suppresses it too: an inferred Fix is inference.

**M12.0 changed Core, and it had to.** `UndoDelta` had no case for sketch
geometry or constraints -- invisible while sketches could only be built in code,
and the first thing a mouse hits. Added `SketchEntityExistenceEdit` and
`SketchConstraintExistenceEdit` plus `PartDocument::addSketchEntity`. Two rules
that bind future work:

1. **Undo restores through `restoreEntity` / `restoreConstraint`, never `add*`**,
   so an entity comes back under the SAME id. Reissuing ids would orphan every
   constraint on it (A03) and make redo produce a document the stack no longer
   describes.
2. **Deleting an entity records its cascaded constraints BEFORE its own delta.**
   Deltas undo in reverse, and a constraint cannot be restored onto geometry
   that is not back yet.

**Creating a sketch is still NOT undoable** (ADR-M12-008). `UndoDelta` has no
sketch-existence case, and adding one means first deciding what an undo does to
the features that reference the sketch -- a Core decision M12 did not take.
`newSketchCommand()` says so in the status line. Do not report sketch creation
as undoable.

**Owner UI validation for M12 is NOT EXECUTED**
(`docs/reviews/M12_UI_UserValidation.md`, every Result cell blank). The agent
checks -- 45 `SketchCanvasTests` cases, 7 solver-backed `SketchCanvasSolveTests`
cases, and the `--selftest --sample m12-sketch` assertions inside the running
window -- are NOT owner validation (ADR-M4-016) and must never be cited as if
they were. **This is now the FIFTH milestone with an open owner UI validation**
(M6, M7, M8, M11, M12). **No independent agent reviewed M12** either; sub-agent
spawning was not used, per the fallback in `docs/OrchestratorGuide.md`.

Still NOT done, and not claimed: Parallel / Perpendicular / Tangent / Equal /
Concentric / Midpoint / Point-on-object do not exist in Core at all (`todo.md`
§2.1), so the UI can only offer the nine constraints M5 shipped; there is no
driving/driven distinction and no automatic demotion on over-definition
(roadmap §7.2, owner decision still open); geometry cannot be dragged; a new
sketch is always on world XY; and the OCCT overlay -- stage two of the owner's
decision -- is not written.

**Two defects M12's own checks found, worth knowing because both classes recur:**

1. **`refreshAll()` did not touch the sketch canvas or the constraint panel**, so
   Ctrl+Z in sketch mode undid the geometry in the document while the canvas
   went on drawing it. Undo routes through `onRecomputeRequested` -> `refreshAll`
   and never told the canvas anything. Only a widget-level assertion can see
   this shape (ADR-M12-009). Any new path that rebuilds the shell from the
   document must cover them.
2. **Window-scoped single-letter shortcuts steal keystrokes from line editors.**
   Qt processes shortcuts BEFORE the focus widget sees the key, so typing
   `#Width / 2` into a dimension fired Horizontal on the `h`. Sketch shortcuts
   are now `Qt::WidgetWithChildrenShortcut` and added to the canvas
   (ADR-M12-010).

**An unrelated half-finished edit was completed to make the tree build at all**:
`ParameterManager.h` had gained a `ValueDomain` parameter on `restore()` that
`ParameterManager.cpp` never received, so `ParametricCADCore` did not compile.
That is M11.4 work in progress, not M12's, and only the signature was closed.

---

**M13 — the seven geometric sketch constraints — is implemented**
(`docs/M13_SPEC.md`), at the owner's direction on 2026-08-20 immediately after
M12. **1088/1088 in Debug and Release.** Parallel, Perpendicular, Equal, Concentric, Midpoint, Point-on-object and
Tangent. The sketch model now has SIXTEEN constraint types; `todo.md` 2.1 is
down from eleven missing to four (Symmetric, plus Normal / Pierce / Curvature,
which all need a spline or an out-of-plane 3D reference Core does not have).

**Schema is now v11.** `Tangent.internal` is a REQUIRED field on load, never
defaulted: defaulting it turns a truncated file into a valid document
describing the OTHER tangency, silently.

Four decisions that bind future work:

- **Tangent's inner/outer branch is STORED, decided once at creation from the
  configuration then (ADR-M13-003).** The solver must never re-derive it.
  Re-deriving would let a drag that pushes one circle through another silently
  swap the model for its opposite -- a different definition, not a different
  pose. This is A03's reasoning applied to a boolean.
- **Direction-based residuals are NORMALISED** to the sine or cosine of the
  angle between the lines (ADR-M13-004). The raw cross and dot products have
  units of mm^2 and a magnitude that grows with the lines, so one residual
  tolerance cannot serve both a 1 mm pair and a 100 mm pair.
- **Self-referential pair constraints are REFUSED** (ADR-M13-006), because their
  residual is identically zero: always satisfied, constraining nothing, and
  quietly making the DOF report one fewer freedom than the sketch has. A
  constraint that lies about the DOF is worse than one that is refused.
- **Equal is one type, not two** (ADR-M13-001), and **Concentric takes two
  curves only** (ADR-M13-002) -- a point at a curve's centre is already
  `Coincident(point, curve.CenterPoint)`, and the UI's refusal names it rather
  than leaving a dead end.

**The gap this opens next**: an arc still contributes only a centre and a
radius to the solve (ADR-M12-003), so **an arc's endpoints cannot be
constrained at all**. That limits Tangent exactly where mechanical work wants
it -- a line meeting an arc's tip -- and it is now the most load-bearing hole in
the sketch model.

**RULE 9 FIRED AGAIN, IN A THIRD FLAVOUR.** M13 bumped the schema to v11, which
five older tests pin as a literal. After fixing them, Debug was green and
Release failed on three of the five -- because a Release build had been KILLED
midway through an earlier run, leaving object files whose timestamps were newer
than the source edits that came afterwards. MSBuild saw them as up to date and
never recompiled, so the Release binary tested the OLD assertions. The two
flavours already recorded are timestamp-preserving restores and stale test
binaries; **an interrupted build is the third**. The prescription is unchanged
and it worked: delete the test binary AND its object directory, rebuild, and
assert the binary exists before trusting a single result.

**Owner UI validation for M13 is NOT EXECUTED**
(`docs/reviews/M13_UI_UserValidation.md`, every Result cell blank), and **no
independent agent reviewed it**. The agent checks -- 20 solver-backed cases, 10
selection-rule cases, 7 serialization cases, and the tangent assertions inside
`--selftest --sample m12-sketch` -- are NOT owner validation (ADR-M4-016). This
is the SIXTH milestone with an open owner UI validation (M6, M7, M8, M11, M12,
M13).

---

**M14 - dimensions are drawn AS dimensions** (`docs/M14_SPEC.md`), at the
owner's request on 2026-08-20. What M12 shipped was a dashed line and a
floating number; what a drawing needs is extension lines, an offset dimension
line, filled arrowheads pointing outward, and the value knocked out of the line
it sits on. Radius carries `R`, diameter carries `D`, and an angle is an arc
swept about the corner where the two lines actually meet.

**The unit split is the load-bearing decision** (ADR-M14-001/002). Extension
lines, dimension lines and angular arcs are in MILLIMETRES and scale with the
zoom, because they are part of the drawing. Arrowheads and text are drawn at a
fixed PIXEL size, because they are READ rather than measured -- one that scaled
would be an invisible speck at 1:10 and would swallow a small feature at 50:1,
which is exactly when someone is zoomed in to check it. `DimensionArrow`
therefore carries a tip and a direction and NO size: size is the widget's
business, because only the widget knows about pixels. Everything else is
computed in the Qt-free layer (ADR-M12-001).

**`--screenshot <path>` was added to the selftest, and it earned its keep
immediately.** Dimension rendering is the one part of this UI whose correctness
is a JUDGEMENT: geometry assertions can confirm an arrowhead exists and points
the right way and still not say whether the result reads as a drawing. Two
defects came straight out of looking at the PNG, and every coordinate involved
was already correct:

1. **The angular dimension erased its own arc.** The value sat at 1.18x radius
   on the bisector -- right on top of the arc -- and text is drawn with the
   background knocked out behind it. Only the number survived. It is 1.55x now.
2. **Constraint glyphs and dimension values fought for the same point.**
   `ResolveElementPoint` gives a line its MIDPOINT and so does a dimension
   label, so every dimensioned line printed its number over its H/V/o badges.
   Glyphs now anchor at the quarter point.

The generalisation, and it is M6.14 again: the widget now counts
`paintedDimensionArrows()` and `paintedDimensionArcs()` SEPARATELY from
`paintedDimensions()`. "A dimension was drawn" and "it was drawn as a
dimension" are different claims, and the first one survives a version that
prints nothing but the number.

**Not done, and the gap a user meets first: dimension placement cannot be
dragged.** The offset is computed from the measured value and the sketch's
centroid, not put where the user wants it. Also absent: collision avoidance
between dimensions, tolerances and prefixes, arrowheads flipping outside a
narrow span, and arc-length / ordinate / chamfer dimensions (roadmap 24.4).

**Owner UI validation for M14 is NOT EXECUTED and no independent agent reviewed
it.** The screenshots are agent checks, not validation (ADR-M4-016).
