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

Current target: **M7 — DXF Dimension and Constraint Reconstruction**
(`docs/M7_SPEC.md`), on branch `m7-wip`. **Functionally complete, NOT complete.**

All ten slices are delivered, Gates A-L pass, 668/668 in Debug and Release, nine
mutations verified. What is missing is the half this project's history says
matters most:

- **Independent review: ZERO ROUNDS.** M5 needed four and M6 needed three, and
  every one of them found defects the previous round's fixes had introduced.
- **M7 owner UI validation: NOT EXECUTED** (`docs/reviews/M7_UI_UserValidation.md`).
- Plus the two inherited M6 items above, which merging M6 did not close.

Read `docs/reviews/M7_SelfValidationReport.md` as CLAIMS. It names what I am
least confident about; start there. One entry is not hypothetical: a mutation
disproved a claim I had written in the code, and the test I had written to prove
that claim passed under the mutation.
