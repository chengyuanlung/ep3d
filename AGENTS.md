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

Current target: **M8 — Core Feature Modeling** (`docs/M8_SPEC.md`), on branch
`m8-wip`, guided by the adopted `docs/EP3D_Onshape_Alignment_Roadmap.md`.

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

**M8 still CANNOT CLOSE**: round 3's fixes are themselves unreviewed (round
4 or accepted-risk is the owner's call -- the change set shrinks each round),
and M7's round 2 + owner UI validation remain open. Schema is at v8; the "unreserved type name" examples
in old tests have been renamed three times now (Radius->, Revolve->Loft,
Fillet->Sweep) -- when adding a feature type, grep tests for its name as a
placeholder first. When adding an ISolidFeature type, its name goes in
`kSolidFeatureTypeNames` (serializer) AND a consumed-as-base row goes in
M8_REV_322 -- the test only reminds you about names it consumes (round 3
proved a name absent from BOTH drifts silently). A consuming type also goes
in `kConsumingFeatureTypeNames`.

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

**Six Majors remain OPEN** (naming order-dependence with two dimensions on one
target; `placeFix=false` re-opens accumulation; cap-check breadth and the
id-uniqueness net stopping short of entities/constraints; no erase path for
`reconstructionReports_`; Gate J reported Skipped not Failed; no CI fixture
renders a skip row; `--gtest_shuffle` still fails on generator poisoning).
Read `docs/reviews/M7_IndependentReview.md` round 2 before trusting any M7
claim.

**Also still open, and M8 cannot close before they do:**

- M7 owner UI validation (`docs/reviews/M7_UI_UserValidation.md`, every row
  blank). Agent-executed mechanical checks live in
  `docs/reviews/M7_UI_AgentExecutedChecks.md` and are NOT owner validation
  (ADR-M4-016) -- they cannot fill a judgement row and must never be cited
  as if they had.
- The two inherited M6 items (M6.11-M6.14 unreviewed; M6 owner UI validation
  not run).

Read `docs/reviews/M7_IndependentReview.md` before trusting any M7 claim, and
`docs/reviews/M7_SelfValidationReport.md` only as corrected.
