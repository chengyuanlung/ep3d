# M8 Completion Report — Core Feature Modeling

> **STATUS: NOT CLOSED.** Every required capability is implemented, every
> release gate passes, and four independent review rounds have run. What stands
> between this and "complete" is stated in §7, and it is not more testing by the
> author.

**Head:** `m8-wip`, after review round 4's fixes. `git log` is the authority.
**Baseline:** `7e93067` (m7-wip after review round 1's fixes), 690 tests.

---

## 1. What M8 delivered

M7 proved a 2D drawing can become one parametric solid. M8 makes the solid the
*start* of a model: a Body's shape is the ordered result of a **feature
history**, where every feature consumes the result of the one before it, carries
stable identity and semantic input references, and rebuilds selectively when its
inputs change.

| Slice | Content |
|---|---|
| M8.1 | kernel `subtractShape`; **PocketFeature**; the feature chain (base by ObjectId via `ISolidFeature`, `consumedSolidId()`, tail display and physics); schema v6 |
| M8.2 | kernel `revolveProfile`; **RevolveFeature**, its axis a `SketchEntityId` treated as construction geometry; schema v7 |
| M8.3 | kernel `filletAllEdges`/`chamferAllEdges` with a shared `BRepCheck_Analyzer` guard; **FilletFeature/ChamferFeature** on one `EdgeDressFeature` base; schema v8 |
| M8.4 | explicit deferrals with demonstrations (ADR-M8-007): Hole shown expressible as a circular Pocket by gate; Mirror/Pattern → M9; Shell → with edge/face selection |
| M8.5 | the shell samples: `m8-chain`, and — added during round 4 — `m8-revolve` and `m8-dress` |
| M8.6 | self-validation report, written as claims |
| M8.7 | **four** independent review rounds |

The chain composes four kinds: `Sketch → {Pad|Revolve|Box} → Pocket →
{Fillet|Chamfer}`, with dress-on-dress round-tripping in v8.

---

## 2. Release gates (M8 spec §7)

All pass, all analytically oracled, all selectivity proven by **counters** rather
than by equal final values.

| Gate | Proof | Result |
|---|---|---|
| A | subtract at the kernel boundary, hand-computed | PASS |
| B | Pad 100×50×20 − Pocket 20×30×10 = **94000 mm³**, 0.25380 kg | PASS |
| C | Width 100→120 rebuilds pad AND pocket → **114000 mm³** | PASS |
| D | depth 10→20 (through) → **88000 mm³**; solver calls unchanged, one tool extrude, pad not rebuilt | PASS |
| E | NaN/negative/zero depth → pocket Failed, base Valid, mass not current; recovery restores 94000 | PASS |
| F | save → fresh load → edit Width → correct volume; the chain reference round-trips semantically | PASS |
| G | deleting the pocket restores the pad as tail; deleting the BASE fails the pocket with a diagnostic, no crash | PASS |
| H | the viewer shows the chain TAIL, not tail plus intermediates | PASS |
| I | full M0–M7 regression, both configs, Release proven, single-process green | PASS |

Beyond the required set: **GATE_RB/RB2/RC/RC2/RG** (revolve, including "entity
order is not identity"), **GATE_FB/FC/FD/FE/FF** (fillet chain, Minkowski
oracle, impossible-radius refusal and recovery), **GATE_CB** (a three-kind
Sketch→Revolve→Chamfer chain against a Pappus oracle), **GATE_BB** (Box as a
chain base), **GATE_HOLE** (the deferral, demonstrated rather than asserted).

---

## 3. Verification, as measured

Both configurations, measured on the round-4 fix head with nothing else running:

| | Debug | Release |
|---|---|---|
| ctest entries registered | **808** | **808** |
| of those, executing | **804** | **804** |
| registered-Skipped children (spawned by their parents) | 4 | 4 |
| result | 808/808 pass | 808/808 pass |
| Single-process `ParametricCADCoreTests.exe` | 444 registered / **441 pass** / 3 child-Skipped | identical |
| Viewer smoke entries | 21 | 21 |
| Build errors | 0 | 0 |

Baseline 690 → **118 tests added across M8**. Schema pins moved 5→6→7→8.

A caution worth carrying forward: a single-process count taken **while a second
test run is in flight** reads one test lower, because the two contend for the
generator-limit proof file. That is a measurement hazard, not a product defect,
and it is recorded because a number measured under contention is exactly the
kind of thing this project's record has been wrong about before.

---

## 4. Independent review — four rounds

Every round found defects the previous round's fixes had introduced. That is not
a lapse; it is the pattern, and it is the reason the protocol exists.

| Round | Commit | Verdict | Scores | Headline |
|---|---|---|---|---|
| 1 | `880e6cc` | REQUEST CHANGES | 73/70/88 | A consumption **diamond** silently accepted; six save/load-symmetry gaps (ADR-M3-008, 4th recurrence) |
| 2 | `ab8513a` | REQUEST CHANGES | 89/78/81 | The `bodies()` **accessor bypass**; solid-type frontier drift; a vacuous test; a twice-miscredited guard |
| 3 | `c555269` | REQUEST CHANGES | 77/84/78 | Round 2's own new facade was the **seventh restore path with no duplicate-id guard** (5th recurrence, introduced by a fix); `M8_REV_322` pinned half its table |
| 4 | `7a60b6b` | REQUEST CHANGES | 71/68/76 | The round-3 guard was **one-directional** — its own comment named the sibling hole and closed none of them; a **sixth** ADR-M3-008 recurrence by a different route; the const-accessor class still open at its two largest doors |

Round 4's mandate deliberately covered **two** unreviewed change sets — M8's
round-3 fixes and M7's round-2 fixes — because M8 cannot close while M7's review
is open, and both were work nobody but their author had read.

Two round-4 findings were reached **independently by two reviewers each**: the
const-accessor class (two different doors) and the consumer-table drift. That
convergence is the strongest signal this protocol produces.

The full register, the masked-by-design list, the Z-battery and the honest
negatives are in `M8_IndependentReview.md`.

### What the rounds keep proving about this project

Three recurring classes, each now with a named counter-measure:

1. **`savePartDocument` writes a file its own loader refuses** (ADR-M3-008) —
   six recurrences, two of them *introduced by a fix for a previous
   recurrence*. Counter-measure: `validateSaveable` mirrors the loader
   endpoint for endpoint, now including the dependency graph.
2. **A table or enumeration pinned for some members with a comment claiming
   all** — the solid-type table (round 3), then the consumer table (round 4,
   in the commit that fixed the first). Counter-measure: each member credited
   to its own test, and the rule for adding a type stated at the table.
3. **A test that asserts non-emptiness where it claims to assert a value** —
   penalised in M7 round 1, fixed for the chain panel in M8 round 1,
   reintroduced for the skip rows in M7 round 2's fix, found again in round 4.
   Counter-measure: assert against the model's own composed string.

A fourth is worth naming because it happened to the fixes in **this** round: a
regression test written for round 4's fingerprint finding was itself
non-discriminating — it built each variant in a fresh document, so every row
passed on the entity-id contribution alone. It was caught by mutating the exact
line the test claimed to pin. **A test is not evidence until a mutation has
killed it.**

---

## 5. ADRs recorded

ADR-M8-001 (the feature chain: base by ObjectId, wiring, tail semantics) ·
ADR-M8-002 (tool direction and the legal cut) · ADR-M8-003 (display and physics
follow the tail) · ADR-M8-004 (blocked consumers, and where the truth lives) ·
ADR-M8-005 (Revolve's axis is a sketch line treated as construction geometry) ·
ADR-M8-006 (Fillet/Chamfer dress ALL edges; per-edge selection deferred) ·
ADR-M8-007 (what M8 defers, and why each deferral is safe) · ADR-M8-008 (a solid
may be consumed once, by its own body).

---

## 6. Known limitations

- **Fillet/Chamfer are all-edges only.** Per-edge selection is deferred with the
  selection architecture (ADR-M8-006, M10).
- **No feature-creation dialogs.** Deferred to M9's edit-transaction work
  (ADR-M8-007); the chain is reached today through the three shell samples.
- **Hole, Mirror, Pattern, Shell deferred** by ADR-M8-007, each with a
  demonstration or a named successor milestone.
- **`removeObject` does not re-point mass properties at the new tail.** Removing
  the tail detaches the mass source and invalidates it — nothing dangles — but
  the friendlier re-point is M9's.
- **Pocket and dress features do not unit-check their mm parameters**
  (consistent with Pad); only Revolve checks Radian.
- **The fake kernel models no fillets/chamfers**, so v8 Core tests assert
  round-trip identity without recompute; real-geometry checks live in the OCCT
  suite and GATE_FD.
- **Suppression is not correct for a chain.** M2's rule — dirtiness propagates
  *through* a suppressed node and its dependents run normally — was safe when
  nodes had no output contract and is wrong for a feature chain. Stated as an
  inspection finding, not demonstrated; it is M9.3's, with an ADR owed
  (`M9_SPEC.md` §3.1).
- **An edge with a non-persisted endpoint is silently dropped at save**
  (R2R4-m3). Save succeeds, load succeeds, and the document differs. Recorded
  and left open deliberately: the honest fix is to refuse, and that changes what
  existing documents can save, which is the owner's decision rather than a
  review-fix round's.
- **Chained-feature performance is unmeasured** beyond the fixtures, and the
  all-edges `BRepCheck_Analyzer` is O(shape) on every dress result.

---

## 7. What stands between this and closed

**None of it is more testing by the author.**

1. **Round 4's fixes are unreviewed.** Every round's have been, and every
   subsequent round has found something in them. The change set shrinks each
   time. Round 5, or accepted-and-recorded risk, is the owner's call.
2. **Owner UI validation is not executed, for three milestones**: M6, M7 and
   M8. The checklists are written and waiting, every Result cell blank. Only the
   owner may fill them (ADR-M4-016); an agent may prepare them and run the
   mechanical checks, and has. M8's checklist covers all four required features
   only because the shell gained `--sample m8-revolve` and `--sample m8-dress`
   during round 4 — before those, three of the four were unreachable from the
   running application and this validation could have covered the pocket alone.
3. **M6.11–M6.14 remain unreviewed**, merged to `master` at the owner's
   direction as a scheduling decision. That is not evidence and must not be
   read as one.

M8 spec §11 requires all three. **M8 is functionally complete and not
certifiable**, and those are two different statements.

---

## 8. Note on the M9 overlap

`docs/M9_SPEC.md` exists and M9.1 is authorized in detail, at the owner's
explicit direction, while the items in §7 are open. That is the same scheduling
decision taken at M6 and at M7, now three milestones deep. It is recorded here
so it is never later read as though the debt had been paid.
