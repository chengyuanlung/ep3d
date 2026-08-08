# ParametricCAD Orchestrator Guide

## Purpose

This project uses a multi-agent development workflow when the execution environment supports sub-agents. The primary agent is the **Orchestrator**. After reading `AGENTS.md`, it should create the role agents below automatically for any non-trivial implementation task.

## Required Role Agents

1. **Architect Agent**
   - Reads architecture and decision documents.
   - Checks scope, ownership, dependencies, stable IDs, units, frames, and future extensibility.
   - Produces a short implementation contract before coding starts.

2. **Developer Agent**
   - Implements only the approved task scope.
   - Adds or updates tests.
   - Does not change architectural invariants without escalating to the Architect.

3. **Test Agent**
   - Builds the project.
   - Runs all tests.
   - Adds missing edge-case tests when appropriate.
   - Reports reproducible failures without silently changing architecture.

4. **Reviewer Agent**
   - Must be independent from the Developer role.
   - Reads `docs/ReviewerGuide.md` and `docs/ReviewChecklist.md`.
   - Reviews the finished change and issues APPROVE / REQUEST CHANGES using the required format.

## Optional Role Agents

Create these only when relevant:

- **Kernel Agent**: OpenCASCADE wrapper, B-Rep, topology, mass properties, STEP/IGES.
- **Sketch Solver Agent**: constraints, DOF, numerical solving, over/under-constrained analysis.
- **Assembly/Motion Agent**: instances, joints, frames, kinematics, collision integration.
- **UI Agent**: Qt UI only; must not move UI dependencies into Core.
- **Serialization Agent**: project format, schema migration, persistent references.

## Automatic Workflow

For a non-trivial task, run this pipeline:

```text
User Task
   ↓
Orchestrator
   ↓
Architect Agent
   ↓ implementation contract
Developer Agent
   ↓ code + tests
Test Agent
   ↓ build/test report
Reviewer Agent
   ↓
APPROVE ? ── yes ─→ Orchestrator final report
   │
   no
   ↓
Developer Agent fixes only required findings
   ↓
Test Agent
   ↓
Reviewer Agent
   ↓
Repeat until approved or blocked
```

## Definition of Non-Trivial

Treat a task as non-trivial if any of these are true:

- Changes public Core APIs.
- Adds or changes a model object.
- Touches persistence, stable references, units, transforms, dependency graph, recompute, Undo/Redo, or serialization.
- Adds Sketch, Feature, Body, Assembly, Joint, Collision, Robot, Material/Mass, Kernel, or file I/O behavior.
- Modifies more than one subsystem.
- Requires new tests or architecture decisions.

Tiny typo/comment/build-script-only changes may skip the full pipeline, but still require build/test when applicable.

## Parallelism Rules

The Orchestrator may run independent discovery tasks in parallel, but **Developer and Reviewer must not be the same agent or share an unreviewed conclusion**. The Reviewer receives the resulting diff and documents, not the Developer's rationale as authoritative truth.

Recommended parallel pattern:

```text
Architect ─────────────┐
Kernel Specialist ─────┤ (only if relevant)
Test Planner ──────────┘
          ↓
      Developer
          ↓
        Tester
          ↓
       Reviewer
```

## Fallback When Sub-Agents Are Unavailable

`AGENTS.md` cannot itself create operating-system processes or force an AI product to expose sub-agent tools. If the current environment does not provide agent spawning, the primary agent must emulate the roles sequentially:

1. Perform an explicit Architect pass.
2. Perform an isolated Developer pass.
3. Build and test.
4. Perform a fresh Reviewer pass using the reviewer rubric.

It must clearly label the limitation in its final report. Do not claim independent-agent review if no independent agent was actually created.

## Orchestrator Output

At completion, report:

- Task scope
- Agents/roles used
- Architecture decisions (if any)
- Files changed
- Build result
- Test result
- Reviewer decision and score
- Remaining risks / next milestone

