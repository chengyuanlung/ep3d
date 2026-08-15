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
sample, M8.6 self-validation). M8.7's independent review ROUND 1 is COMPLETE:
REQUEST CHANGES (R1 73, R2 70, R3 88) -- 2 Criticals (a silently accepted
consumption diamond; six save/load-symmetry gaps across all four new types)
and 10 Majors, ALL FIXED with one regression test per finding (M8_REV_*,
GATE_RH/RC2/E3/FE/FF/KC, ADV_A-D) and an 8+1-mutation verification battery
(record in `docs/reviews/M8_IndependentReview.md` -- read it before trusting
any M8 claim). The chain rule is now ADR-M8-008: a base must be a solid
feature of its own body, consumed at most once, enforced at creation, save,
load, and display. **M8 still CANNOT CLOSE**: round 2 has not run, and M7's
round 2 + owner UI validation remain open. Schema is at v8; the "unreserved
type name" examples in old tests have been renamed three times now
(Radius->, Revolve->Loft, Fillet->Sweep) -- when adding a feature type, grep
tests for its name as a placeholder first.

M7 state: functionally complete PLUS review round 1's fixes (all 4 Criticals,
14 Majors closed). **Still open, and M8 cannot close before they do:**

- M7 round 2 independent review -- this project has never had a review round
  that did not find defects introduced by the previous round's fixes.
- M7 owner UI validation (`docs/reviews/M7_UI_UserValidation.md`, every row
  blank; Test B is runnable now).
- The two inherited M6 items (M6.11-M6.14 unreviewed; M6 owner UI validation
  not run).

Read `docs/reviews/M7_IndependentReview.md` before trusting any M7 claim, and
`docs/reviews/M7_SelfValidationReport.md` only as corrected.
