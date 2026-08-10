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

Current target: **M6 — DXF Import to Stable Sketch Entities**
(`docs/M6_SPEC.md`), on branch `m6-wip`. Two review rounds so far, with the same
pattern: the second found a Critical the first round's fixes had created. See
`docs/reviews/M6_CompletionReport.md`.

**The pattern is now the most reliable prediction this project makes.** Every
review round since M5 has found defects introduced by the previous round's
fixes, and in three of them the implementer's own claim that "every fix is
mutation-verified" was false. A mutation suite written by the author of a fix
measures the author's imagination; only a reviewer removing a line the author
did not think to remove has ever found an unguarded fix here. Plan for a review
round after every round of fixes, not after the implementation.

## Independent Review Role

When acting as a reviewer rather than an implementer, read `docs/ReviewerGuide.md` and `docs/ReviewChecklist.md` first. Do not approve code only because it compiles. Verify architecture invariants, stable references, units, coordinate frames, dependency/recompute behavior, testability, and future Assembly/Robot/Physics compatibility. Use the exact review output format specified in `docs/ReviewerGuide.md`.
