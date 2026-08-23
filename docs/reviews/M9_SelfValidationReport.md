# M9 Self-Validation Report — History & Editing Infrastructure

> **These are CLAIMS, not facts.** The rule carries over from M7 and M8, and
> both proved it on this author: M7's report contained two false claims, M8's
> contained five stale counts and a miscredited guard, and review round 4 found
> a regression test written *in that round* that was itself non-discriminating.
> Read accordingly, and read `docs/DecisionLog.md` ADR-M9-001..006 for the
> decisions rather than the summary.

**Baseline:** `7a60b6b` plus M8 review round 4's fixes.
**Branch:** `m8-wip`, not pushed. `git log` is the authority for the head.

**Standing obligations, restated:** M8 is NOT closed. Round 4's fixes are
unreviewed, owner UI validation is open for M6, M7 **and** M8, and M6.11–M6.14
have never been reviewed by anyone but their author. **M9 cannot close before
M8 closes** (M9 spec §2). Nothing below supersedes that, and M9 having gone
further than M8 does not make M8 finished.

---

## What M9 delivered

| Slice | Content |
|---|---|
| M9.1 | `FeatureSnapshot` (shared with the serializer's loader); `UndoRecord`; transaction + undo/redo on `PartDocument`; `Body::moveFeatureToIndex`; mass re-pointing to the chain tail on removal |
| M9.2 | `FeatureEditSession` — preview on a semantic copy, accept as one transaction, cancel that leaves no trace |
| M9.3 | Suppression through the chain: one `isFeatureActive` predicate, base resolution that walks past inactive links, suppression as an undo delta |
| M9.4 | Rollback position per Body: a per-pass skip predicate on the graph, schema **v9**, undoable, clamped at both ends |
| M9.5 | The shell: Undo/Redo (Ctrl+Z/Ctrl+Y), Suppress/Unsuppress, Roll Back / Roll Forward, and **feature creation commands** (Insert ▸ Pad / Pocket / Fillet / Chamfer), closing ADR-M8-007's deferral |
| M9.6 | Mirror and Pattern **deferred to M10** with ADR-M9-006 — stated with no demonstration, because there is nothing to demonstrate with |
| M9.7 | this report |

Six ADRs: M9-001 (undo records are semantic deltas), M9-002 (suppression closes
the chain by resolution), M9-003 (preview state is not document state), M9-004
(rollback is a position, and document state), M9-005 (creation is commands, not
dialogs), M9-006 (Mirror/Pattern deferred, with the plan M10 inherits).

---

## Release gates (M9 spec §7)

All analytically oracled; selectivity by counters, never by equal final values.

| Gate | Proof | Result |
|---|---|---|
| **A** | Width 100→120 → 114000; Undo → **94000 exactly**; Redo → 114000 | PASS |
| **B** | five edits, five undos walking back through every intermediate value in order; one more undo is a no-op | PASS |
| **C** | delete Pocket001 → tail is Pad001 at 100000; Undo → the pocket is back with its **original ObjectId**, chained, at 94000, and the document still **saves** | PASS |
| **D** | Undo of a Depth edit: solver calls unchanged, one tool extrude, one subtract | PASS |
| **E** | preview shows 88000 while the document still reports 94000; Cancel leaves no undo record; Accept is exactly one | PASS |
| **F** | suppress Pocket001 → tail is Pad001 at 100000, reported `Suppressed` through the FEATURE; unsuppress → 94000; suppressing the only base fails the consumer loudly | PASS |
| **G** | position after Pad001 → 100000 and the pocket is not displayed; a save at that position round-trips the **full history** and reopens at the same step | PASS |
| **H** | Depth → −5 fails the pocket; Undo → 94000 and Valid; Redo → Failed again **with the same diagnostic** | PASS |
| **I** | v9 round-trips the rollback position; suppression round-trips on v8's existing `ComputeState` | PASS |
| **J** | full M0–M8 regression, both configs, single-process and shuffled seeds | PASS |

Beyond the required set: A2 (a new edit discards the redo branch), B2/B3
(transaction grouping; an aborted transaction leaves nothing behind), C2 (undo
of an addition), C3 (an unreplayable removal clears the history rather than
lying), C4 (a middle feature goes back to its own index), E2–E5 (cancel,
one-step accept, an abandoned session, a preview that does not build), F2 (the
chain closes over a suppressed middle feature), F3/F4, G2–G4 (round-trip, clamping,
and **rolling back past a failing feature leaves a healthy document**), and
X1/X2 — the two COMBINATIONS this report first listed as things it was least
sure of: undo of a deletion against a live rollback position, and chain
resolution walking past a suppressed feature AND a rolled-back one at once.

---

## Mutation record (23 total, plus 6 schema pins)

Every mutation: binaries deleted before the rebuild and asserted present after;
restores by plain copy + `touch`; every edit `cmp`-verified as landed before any
verdict was believed (AGENTS.md rule 9).

**M9.1 battery (13)** — M1 undo applies nothing · M2 redo branch not discarded ·
M3 mass does not follow the tail · M4 empty transaction consumes a step ·
M5 `moveFeatureToIndex` neutered · M6 re-entrancy guard removed · M7 abort
records instead of reverting · M8 parameter undo applies the new value ·
M9 additions unrecorded · M10 unreplayable removal keeps the history ·
S1–S3 three `SnapshotFeature` fields dropped. **All guarded**, two of them only
after the tests were fixed (below).

**M9.2–M9.4 battery (10)** — N1 `isFeatureActive` always true · N2
`activeChainBase` never skips · N3 presenter ignores activity · N4 graph ignores
the skip predicate · N5 suppression does not move the tail · N6 rollback not
persisted · N7 load records an undo step · N8 cancel applies the edits ·
N9 accept is not one transaction · N10 preview edits the real document.
**All guarded**, two of them only after the tests were fixed (below).

**The schema pins (6)** — not mutations but the same evidence: bumping v8→v9
turned six tests red immediately, including the "refuse a version newer than
this loader" fixture, whose "too new" number had become loadable. That is those
tests working, and a version bump is exactly when such a fixture rots.

---

## Four defects in my own tests, found by mutation

Recorded because the standard is the same whoever wrote the test, and because
three of the four are this project's own named recurring shapes.

1. **`Body::moveFeatureToIndex` was untestable** (M9.1, mutation M5). Every gate
   removed the LAST feature in its body, so neutering the function to
   `return true` left all nine gates green. GATE_C4 removes a MIDDLE feature.
   Note what does *not* catch it: the document still saves with the feature at
   the wrong index, because the save-side chain walk only constrains a consumer
   relative to its base.
2. **A test that HUNG instead of failing** (M9.1, mutation M6). Gate B drained
   the undo stack with `while (undo()) {}`. With the re-entrancy guard removed
   every undo recorded its own inverse, the stack never emptied, and the suite
   ran for ever — a CI timeout with no finding, which the next reader blames on
   the machine. The loop is bounded now and **the bound is the assertion**.
3. **Rollback's skip predicate was unobservable** (M9.4, mutation N4). The tail
   and the presenter already filter on activity, so a rolled-back feature that
   was still being COMPUTED produced the same volumes. What that breaks is the
   reason rollback exists: GATE_G4 rolls back past a *failing* feature and
   requires the document to be healthy.
4. **The load path's no-recording rule was unpinned for rollback** (M9.4,
   mutation N7). `M9_UNDO_402`'s fixture has no rollback position, so the
   restore path was never reached. GATE_G2 now asserts `undoDepth() == 0` on a
   document loaded *with* a position.

A fifth, caught by the tests rather than by mutation: the first version of the
`ParameterExistenceEdit` recording went into `restoreParameter` instead of
`addParameter`, so every load arrived carrying a history of its own
construction. GATE_G2 and M9_UNDO_402 both went red on the next run.

---

## Verified totals

Measured on the M9.7 head, both configurations, with nothing else running (a
count taken while a second test run is in flight reads one lower -- the two
contend for the generator-limit proof file; a measurement hazard, not a product
defect, recorded so the next reader does not chase it):

| | Debug | Release |
|---|---|---|
| ctest entries registered | **836** | **836** |
| of those, executing | **832** | **832** |
| registered-Skipped children (spawned by their parents) | 4 | 4 |
| ctest result | 836/836 pass | 836/836 pass |
| Single-process `ParametricCADCoreTests.exe` | 445 registered / **442 pass** / 3 child-Skipped | identical |
| M9 release-gate tests | 27 | 27 |
| Viewer smoke entries | 21 | 21 |
| Build errors | 0 | 0 |

The M9 starting point was 820 registered / 816 executing and 445/442, so M9.2
through M9.7 added 16 ctest entries. Counts are stated as registered/executing
because three review rounds have had to correct a count in this project's
record.

---

## Known limitations

- **Undo cannot replay every removal.** Removing a Body, a Sketch or a Material,
  or a feature another feature CONSUMES, clears both stacks rather than keeping
  a history that would lie. The clearing is observable (`undoDepth()` → 0) and
  pinned by GATE_C3. Parameters ARE replayable (M9.5 needed them to be).
- **A preview costs a save/load per session open.** Proportional to document
  size; a session is opened per EDIT, not per keystroke. Nothing profiles it.
- **Rollback is per Body, not per document.** A document with several bodies has
  several positions. Onshape's bar is per Part Studio; whether EP3D wants one
  document-wide position is a UI question M10/M14 can settle.
- **No rollback *bar*.** The position is a value with commands to move it, as
  M9 spec §3 says. Dragging a bar in the tree is a widget, not semantics.
- **Mirror and Pattern do not exist** (ADR-M9-006), and no document claims they
  do.
- **Creation commands use fixed defaults** (Pad 20 mm, Pocket 10 mm, dress
  2 mm) and always target the first body. Extent and operation options are named
  in ADR-M9-005 as what would justify a dialog.
- **`FeatureEditSession` previews parameter edits only.** Previewing a
  structural change (adding a feature) would work the same way but has no
  command that needs it yet.
- **Undo history is not bounded.** A long session accumulates records. Each is
  small (ids and doubles), but nothing caps them.

---

## NOT EXECUTED

- **M9 owner UI validation** — the checklist is WRITTEN
  (`docs/reviews/M9_UI_UserValidation.md`, 50 rows across five tests) and
  **not run**. The M9 commands are covered by automated smoke assertions in the
  running shell, and ADR-M4-016's line between that and owner validation stands:
  an agent may write the rows and state the expected values, and may not fill a
  Result cell. Row A11 asks a question the automated assertions cannot even
  pose — the viewer builds its sample through the recording facade and nothing
  clears the history, so Ctrl+Z at startup dismantles the sample.
- **M9 independent review** — not launched. Everything above is unreviewed, and
  every previous milestone's review found defects in exactly this kind of
  fresh work.
- Performance of any kind: the preview copy, the activity predicate (which is a
  linear scan per query), undo memory.
- Undo across a DXF import, and undo interacting with M7 reconstruction.

---

## What I am least confident about

1. **`isFeatureActive` is called a lot and scans linearly** — from base
   resolution, from the tail computation, from the presenter, and from the graph
   skip predicate on every node of every pass. It is correct and it is O(features)
   per call. Nothing measures it, and a large part would feel it before any test
   would.
2. ~~The interaction between rollback and undo of a structural change.~~
   ~~`activeChainBase` walking past a suppressed feature whose own base is
   rolled back.~~ **Both were on this list and both are now tested** — GATE_X1
   and GATE_X2 — because a worry that can be turned into a test should be, and
   combinations are where four review rounds have found this project's defects.
   Both behaved correctly by construction, which is an honest negative and not
   a reason to have skipped the tests: the two mechanisms index the same array
   and share one predicate, and nothing but execution establishes that they
   compose.
3. **Everything about M9 that a second pair of eyes has not seen.** Four review
   rounds on M8 found, between them, three Criticals and eleven Majors in work
   its author believed finished — including two defects introduced by fixes for
   earlier defects. M9 is larger than any single M8 slice and has had no review
   at all.
