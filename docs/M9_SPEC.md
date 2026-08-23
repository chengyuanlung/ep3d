# M9 — History & Editing Infrastructure

## 1. Mission

M8 made a Body's shape the ordered result of a feature history. M9 makes that
history **editable** — and makes every edit reversible:

> Every change to the document is a **semantic transaction** that can be undone
> and redone; a feature can be **edited with preview, accept and cancel** without
> the preview ever becoming document state; a feature can be **suppressed**
> instead of deleted, and the chain closes over the gap; and the history can be
> **rolled back** to an earlier evaluation position to see the model as it was
> at that step.

This is roadmap §10 (feature edit transaction), §15 (undo/redo), §16 (rollback),
§17 (suppression) and §33's M9 rung. The central release proof:

> On `Sketch001 → Pad001 → Pocket001` (94000 mm³): change Width 100 → 120 and
> verify 114000; **Undo** and verify 94000 exactly; **Redo** and verify 114000.
> Then **suppress** Pocket001 and verify the tail becomes Pad001 at 120000;
> unsuppress and verify 114000 — with counters proving that undo did not
> re-solve a sketch it did not need to.

## 2. Baseline

M9 starts from `m8-wip` head — M8 functionally complete through all its slices,
plus M7's round-2 fixes and follow-up.

**Stated plainly rather than implied:** M8 is NOT yet accepted, and neither is
M7 nor, fully, M6. At the time this spec is written the open items are:

1. **M8 independent review round 4 has RUN** against `7a60b6b` (M8 round-3
   fixes plus M7's round-2 fixes, the two change sets nobody but their author
   had read): REQUEST CHANGES 71/68/76, three Criticals, all fixed. **Round
   4's own fixes are unreviewed** — the same position, one level up, for the
   fourth time. Round 5 or accepted-and-recorded risk is the owner's call.
2. **Owner UI validation, three milestones deep**: M6
   (`M6_UI_UserValidation.md`), M7 (`M7_UI_UserValidation.md`) and M8
   (`M8_UI_UserValidation.md`) all have every Result cell blank. Only the owner
   may fill them (ADR-M4-016).
3. **M6.11–M6.14 unreviewed** — merged to `master` at the owner's direction as a
   scheduling decision, which is not evidence.

M9 building on this baseline is, again, a scheduling decision made explicitly by
the owner. **M9 cannot close before M8 closes**, M8 cannot close before M7, and
nothing in this spec supersedes those obligations. The pattern this project has
repeated since M6 — start the next milestone while the previous one's validation
stays open — is now three deep. That is recorded here so it is never later read
as though the debt had been paid.

M9 must preserve every accepted M0–M8 contract.

## 3. Alignment (roadmap §32)

| | |
|---|---|
| Reference behavior | Onshape: every action is undoable; a feature dialog previews live and commits on ✓ or discards on ✕; features can be suppressed; the rollback bar moves the evaluation point through the feature list |
| EP3D intended behavior | identical semantics on the EP3D graph: undo records are semantic deltas over the Core model, preview state is separate from document state, suppression closes the chain, rollback is an evaluation position and never a destructive edit |
| Implemented before M9 | Suppression exists at the graph level since M2 (`ComputeState::Suppressed`, `DependencyGraph::setSuppressed`, `PartDocument::setSuppressed`, persisted by the serializer) — but see §3.1: what exists is M2 semantics that CAD features never revisited. There is no undo, no transaction and no rollback of any kind |
| Intentional differences | no rollback *bar* as a dragged widget — a rollback position is a value with commands to move it (the widget is a UI question, not a semantics question); no configurations (§17's variant half stays in M15) |
| Validation | Gates below + owner manual UI validation |

### 3.1 What M2's suppression actually does, and why M9 must revisit it

Two facts, established by reading the code and its own tests rather than by
assuming the feature works:

1. **Dirtiness propagates THROUGH a suppressed node and its dependents execute
   normally.** This is a deliberate, documented M2 rule
   (`M2_SUP_002_DownstreamOfSuppressedExecutes`), and that test's own comment
   says why it was safe then: *"M2 nodes have no output contract, so no false
   success is expressible; real cached-output semantics arrive with CAD features
   in M3."* M3 through M8 never revisited it. On an M8 chain the rule is
   actively wrong: suppress `Pocket001` and its consumer still runs, cutting
   against whatever shape the suppressed pocket last retained — the
   healthy-looking wrong solid that ADR-M8-004 exists to prevent, arrived at
   from the one direction nothing guards.

2. **`PartDocument::setSuppressed` sets the GRAPH node only.** It never calls
   `Feature::setSuppressed`, so the feature's own `ComputeState` cache — the
   manually-synchronised one of ADR-M3-004 — is left disagreeing with the graph.
   `syncFeatureStatesFromGraph` then corrects the feature *downward to Dirty*,
   because `Feature::markDirty()` deliberately refuses to overwrite `Suppressed`
   and the feature was never put in that state to begin with. The result: the
   graph says `Suppressed`, the feature says `Dirty`, and anything reading the
   feature — the model tree, the property panel, a diagnostic — cannot tell the
   user that the feature is suppressed at all.

Finding 1 is stated as an **inspection finding, not a demonstrated defect**: no
test covers it, and no UI command reaches suppression, so it is not reachable by
a user today. **M9.3 owes an executed demonstration before it fixes it** — a
defect this project has not proven by running is a hypothesis, and hypotheses
have been wrong here before, including one written into this very section.

Finding 2 was written here as an inspection finding too, and M8's review round 4
reached it independently BY EXECUTION (R1R4-m1), adding the consequence
inspection had missed: suppressing a Pocket removed the whole body from the
viewer. It is fixed — `PartDocument::setSuppressed` now writes the feature's own
state as well as the graph node — so M9 inherits only finding 1.

This is why suppression is a *required* M9 item rather than a "it already
works, expose it" item.

## 4. Scope

**Required for M9 to close:**

- **Undo / Redo** over semantic document transactions
- **Feature edit transaction** — preview, accept, cancel
- **Suppression** reachable through the document facade and correct through a
  feature chain
- **Rollback / evaluation position**
- diagnostics for all four: a user must be able to tell what an undo will undo,
  what a preview would change, why a suppressed feature is not contributing, and
  where the rollback position is

**In scope, closable as deferred only by explicit ADR:** Mirror and Pattern
(inherited from ADR-M8-007, which named M9 as their successor); feature
*creation* dialogs in the shell (ADR-M8-007 deferred them to "M9's
edit-transaction workflow", so M9 owes them or owes a new ADR); re-pointing mass
properties at the new tail when a chain member is removed (M8's recorded
limitation).

Out of scope: Configurations and parameter variants (M15); per-edge selection
and the selection architecture (M10); Assembly; Drawings; Sweep/Loft/Draft.

## 5. Architectural Rule — the transaction

```text
User Command
   └─▶ Document Transaction ──▶ semantic delta ──▶ Undo Record
                                                       │
                          Undo ◀────────────────────────┘
```

- An undo record is a **semantic delta over the Core model** — object ids,
  parameter values, constraint data, feature records. Never a serialized OCCT
  shape, never a kernel handle, never a whole-document snapshot taken because it
  was easier (roadmap §15, ADR-M4-004).
- **Preview state is not document state.** A preview may compute geometry, but
  until Accept, nothing in the document, the registry or the dependency graph
  has changed, and Cancel is indistinguishable from never having started
  (roadmap §10).
- **Suppression closes the chain.** A suppressed consumer's base becomes the
  tail for everything downstream of it; a suppressed feature in the middle of a
  chain does not orphan its consumer. What this must NOT do is silently rewrite
  the consumer's stored base reference — the reference is what the model says,
  and suppression is a state, not an edit. How the two are reconciled is the
  first ADR M9 owes.
- **Rollback is an evaluation position, not a deletion.** Features after the
  position are not computed and not displayed; they are not removed, not
  modified, and a save at any rollback position round-trips the whole history.
- Transactions compose with the M2 recompute machinery — undoing a parameter
  edit dirties exactly what the original edit dirtied, and nothing else.

Forbidden: an undo stack of document snapshots; a preview that writes into the
document and relies on a later undo to clean up; a rollback that removes
features; raw OCCT state anywhere in the undo history.

## 6. Failure semantics

- A transaction that fails **leaves nothing behind** — the document is exactly
  as it was, and no undo record is created for work that did not happen
  (M7's round-2 lesson: an unwind that no test can reach is dead code, and it
  must be honestly labelled as such if it stays).
- An undo whose redo would fail is still a legal undo; the failure surfaces at
  redo, with a diagnostic, and the document keeps its last valid state
  (ADR-M3-001/004).
- Undoing across a **failed** feature restores the failure faithfully. Undo is
  not a repair tool and must never present a healed model that the user never
  had.
- A suppressed feature reports `Suppressed`, not `Valid` and not `Failed`, and
  MassProperties describes the shape actually produced — never the shape that
  would exist if the suppressed feature were running.
- Rolling back past a feature that another feature consumes is legal: the
  consumer is simply not evaluated. Rolling forward restores it.

## 7. Release gates (all analytically oracled, counters not equal-values)

Fixture: the M8 chain — a 100 × 50 constrained rectangle, Pad 20 mm, Pocket
20 × 30 × 10. Base volume **94000 mm³**.

| Gate | Proof |
|---|---|
| **A** | undo/redo of a parameter edit: Width 100→120 → 114000; Undo → **94000 exactly**; Redo → **114000**. Byte-identical restoration, not merely equal volume |
| **B** | undo depth: N edits, N undos, the document walks back through every intermediate value in order; N+1 undos is a no-op, not a corruption |
| **C** | undo of a **structural** change: delete Pocket001 → tail is Pad001 at 100000; Undo → the pocket is back, chained, with its ORIGINAL ObjectId, and 94000 |
| **D** | selectivity: Undo of a Depth edit re-runs the pocket only — solver calls unchanged, exactly zero extrudes, one subtract |
| **E** | transaction: preview Depth 10→20 shows 88000 while `document` still reports 94000; **Cancel** → 94000 and **no undo record was created**; Accept → 88000 and exactly one record |
| **F** | suppression: suppress Pocket001 → tail is Pad001, **100000 mm³**, pocket reports `Suppressed` **through the feature, not only through the graph** (§3.1 finding 2); unsuppress → 94000. Suppressing the BASE of a consumer must NOT let the consumer run against the base's retained shape (§3.1 finding 1) — it produces a diagnostic, never a silent wrong solid |
| **G** | rollback: position set after Pad001 → 100000 and the pocket is not displayed; position to end → 94000; a save at the rolled-back position round-trips the FULL history and reloads at the same position |
| **H** | undo across a failure: Depth → −5 (pocket Failed, mass not current); Undo → 94000 and Valid; Redo → Failed again, with the same diagnostic |
| **I** | v9 round-trips suppression state and rollback position semantically |
| **J** | full M0–M8 regression, Debug and Release, Release proven, single-process runs green, shuffled seeds green |

Mutation minimums: undo record made a snapshot → Gate C fails on the id;
preview writing into the document → Gate E fails; suppression implemented as a
silent delete → Gate F fails on unsuppress; rollback implemented as truncation
→ Gate G fails on save; the undo stack's depth counter off by one → Gate B fails.

## 8. Adversarial (beyond the gates)

Undo with an empty stack; redo after a new edit (the redo branch must be
discarded, not silently replayed later); undo of an edit to a deleted object;
suppress a feature that is already suppressed; suppress every feature in a body;
rollback position beyond the end and before the start; rollback then edit (does
the redo branch survive?); nested transactions; a transaction left open when the
document is saved; undo after load (a loaded document has no history — it must
say so rather than undoing into someone else's session); preview of a change
that fails to compute; two consecutive identical edits (one record or two?);
whole-suite single-process runs.

## 9. Required ADRs

- ADR-M9-001 — the undo record: a semantic delta, and what "semantic" excludes
- ADR-M9-002 — suppression and the stored base reference: how the chain closes
  without rewriting what the model says
- ADR-M9-003 — preview state is not document state
- ADR-M9-004 — rollback is a position, and what a save at a position means
- more as implementation discovers durable decisions

## 10. Slices

```text
M9.1 — transaction + undo/redo core, parameter and structural edits, gates A-D  [DONE]
M9.2 — feature edit transaction: preview / accept / cancel (gate E)             [DONE]
M9.3 — suppression through the chain (gate F)                                   [DONE]
M9.4 — rollback / evaluation position (gate G) + schema v9                      [DONE]
M9.5 — UI: undo/redo, suppression, rollback, and feature CREATION COMMANDS
       in the shell (ADR-M9-005 -- commands, not dialogs)                      [DONE]
M9.6 — Mirror / Pattern DEFERRED to M10 with ADR-M9-006                         [DONE]
M9.7 — self-validation + mutation audit                                         [DONE]
M9.8 — independent review + close (blocked on M8 closing first)                 [OPEN]
```

All slices through M9.7 are implemented. M9.8 -- the independent review -- has
not run, and cannot close before M8 does.

### M9.1 as built, and one correction to this spec

**No schema bump.** This slice line said "v9", and that was wrong when it was
written: the undo history is SESSION state, not document state (ADR-M9-001,
following M7's provenance in ADR-M7-017), so nothing M9.1 adds is persisted and
the file format is untouched at v8. Gate I's v9 belongs to M9.3 and M9.4, which
persist suppression state and the rollback position -- the things that ARE
document state. Correcting the slice line here rather than bumping a version
number that would carry no content.

Delivered: `FeatureSnapshot` (shared with the serializer, which now restores
through it, so the per-type dispatch count went DOWN); `UndoRecord`;
`beginTransaction`/`commitTransaction`/`abortTransaction`/`undo`/`redo` with
depth and label accessors on `PartDocument`; `Body::moveFeatureToIndex`;
mass-source re-pointing to the chain tail on removal, which pays off M8's
recorded limitation because gate C requires it.

Gates A-D pass, plus A2 (the redo branch is discarded by a new edit), B2/B3
(transaction grouping and abort), C2 (undo of an ADDITION), C3 (an unreplayable
removal clears the history rather than lying) and C4 (a middle feature goes back
to its own index) -- eleven gate tests, plus `M9_UNDO_401` pinning
`SnapshotFeature` field by field. Twelve in all, each mutation-verified:

| # | Mutation | Verdict |
|---|---|---|
| M1 | `undo()` reports success and applies nothing | guarded (6 gates) |
| M2 | the redo branch is not discarded by a new edit | guarded (A2) |
| M3 | mass does not follow the new tail | guarded (C, C2) |
| M4 | an empty transaction consumes a step | guarded (B2) |
| M5 | `moveFeatureToIndex` neutered | **UNGUARDED at first** -- guarded once C4 existed |
| M6 | the `recordDelta` re-entrancy guard removed | **HUNG at first** -- guarded once B's drain loop was bounded |
| M7 | abort records the work instead of reverting it | guarded (B3) |
| M8 | parameter undo applies the NEW value | guarded (5 gates) |
| M9 | feature additions are not recorded | guarded (C2, C3) |
| M10 | an unreplayable removal keeps the history | guarded (C3) |
| S1-S3 | pocket base / dress size / revolve axis dropped from `SnapshotFeature` | guarded (M9_UNDO_401) |

**Not yet reachable from the running application**, and named here so it does
not become the thing M8 was caught doing: there is no Ctrl+Z, no Edit menu, no
undo button. The history IS already accumulating in the shell -- the property
panel commits through `setParameterValue`, which records -- so M9.5 has to
supply the commands, not the machinery. Until it does, undo is Core-only, and
no document may claim otherwise.

**Two of those tests exist because a mutation survived the first version**, and
both are this project's own recurring shapes turned on M9's new code:

- `moveFeatureToIndex` was written because feature ORDER is load-bearing, and
  then no test could see it -- every gate removed the LAST feature in its body,
  so neutering the function to `return true` left all nine gates green. GATE_C4
  removes a MIDDLE feature. Note what does not catch it: the document still
  saves with the feature at the wrong index, because the save-side chain walk
  only constrains a consumer relative to its base.
- Gate B drained the stack with `while (undo()) {}`. Under M6 every undo
  recorded its own inverse, so the stack never emptied and the suite **hung**
  instead of failing -- a timeout with no finding, which the next reader blames
  on the machine. The loop is bounded now and the bound is the assertion.

Recorded here rather than in a review document because the standard is the
same whoever finds the defect.

**Verified on the M9.1 head, both configurations, nothing else running:**
ctest **820 registered / 816 executing, 820/820 pass, identical in Debug and
Release** (the four non-executing entries are the registered-Skipped children
their parents spawn); single-process `ParametricCADCoreTests.exe` **445
registered / 442 pass / 3 child-Skipped**, identical in both. The round-4
baseline was 808/804 and 444/441, so M9.1 added 12 ctest entries and 1 Core
test.

## 11. Definition of done

Required capabilities implemented; gates A–J pass; adversarial matrix green;
mutation-verified; v9 round-trips; M0–M8 regressions pass both configs including
single-process and shuffled seeds; ADRs recorded; self-validation written as
claims; independent review with no unresolved Critical/Major; owner UI
validation — **and M8's own open items closed, which includes M7's, which
includes M6's.**
