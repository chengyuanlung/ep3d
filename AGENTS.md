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

Current target: M4 — Sketch/Profile Foundation, Pad/Extrude & Basic 3D Viewer.
Implemented and functionally approved (96/100), but NOT complete: the UI review
last returned REQUEST CHANGES 79/100 and has not been re-run against the fixes.
See `docs/reviews/M4_CompletionReport.md` for exactly what remains.

## Independent Review Role

When acting as a reviewer rather than an implementer, read `docs/ReviewerGuide.md` and `docs/ReviewChecklist.md` first. Do not approve code only because it compiles. Verify architecture invariants, stable references, units, coordinate frames, dependency/recompute behavior, testability, and future Assembly/Robot/Physics compatibility. Use the exact review output format specified in `docs/ReviewerGuide.md`.
