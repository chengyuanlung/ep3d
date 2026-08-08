# ParametricCAD Reviewer Guide

## Purpose

This document is for a second AI agent or human reviewer. Its job is not to implement features. Its job is to evaluate whether a proposed implementation preserves the architecture, correctness, maintainability, and future extensibility of ParametricCAD.

The reviewer should be conservative: prefer identifying architectural drift early rather than accepting code that merely compiles.

## Review Inputs

Before reviewing code, read these files in order:

1. `docs/Architecture.md`
2. `docs/CodingRules.md`
3. `docs/Roadmap.md`
4. `docs/DecisionLog.md`
5. `AGENTS.md`
6. The changed source files and tests

## Core Architectural Invariants

A change must be rejected or sent back for revision if it violates any of these invariants without an explicit architecture decision update.

### 1. Core independence

`src/Core` must not depend on Qt, OpenCASCADE, UI frameworks, rendering APIs, or platform-specific APIs.

### 2. Stable identity

Persistent model objects use stable IDs. Vector indices, pointer addresses, and temporary OpenCASCADE sub-shape numbers must not be used as persistent references.

### 3. Model vs computed geometry

The parametric document model is the source of truth. Generated B-Rep, meshes, mass properties, and other derived data are computed results and may be invalidated/rebuilt.

### 4. Dependency-driven recompute

A parameter or feature change must eventually support dirty propagation through the dependency graph. New design choices must not make global full recompute the only possible implementation.

### 5. Command/transaction boundary

User-visible model mutations should be representable as commands/transactions so Undo/Redo can be added or maintained consistently.

### 6. Units are explicit

Do not introduce bare numeric values whose unit is ambiguous at API boundaries. Internal unit conventions must remain consistent with architecture documentation.

### 7. Coordinate frames are first-class

Part, assembly, joint, user, tool, and robot coordinate systems must use the common transform/frame model. Do not add one-off coordinate conversion logic inside individual features.

### 8. Physics metadata remains separate from solver

Material, density, mass, center of mass, inertia, and contact properties belong to the model. A future physics engine must remain replaceable and external to the core model.

### 9. Assembly behavior belongs to instances/relations

Motion behavior is defined by assembly joints/relations, not hard-coded into reusable part geometry. Parts may expose connectors/reference frames that describe intended attachment points.

### 10. Collision is a subsystem

Collision/interference state must not be mixed into geometric feature classes or joint classes. Collision should operate on component instances and collision geometry/caches.

## Evaluation Categories

Score each category from 0 to 5.

### A. Architectural Compliance — weight 25%

- 5: Fully follows documented layering and invariants.
- 4: Minor non-structural issues.
- 3: Some coupling or unclear ownership, but repairable.
- 2: Significant architectural drift.
- 1: Major violation of core boundaries.
- 0: Fundamentally incompatible design.

### B. Correctness — weight 20%

Check object lifetime, IDs, transforms, units, state transitions, error handling, and edge cases.

### C. Extensibility — weight 15%

Ask whether the implementation can later support Assembly, Joint, Collision, Robot, Material/Mass, serialization, and scripting without redesigning the feature.

### D. Testability — weight 15%

Core logic should be testable without UI. New behavior should include deterministic unit tests where practical.

### E. API Quality — weight 10%

Check naming, ownership, const-correctness, explicitness, error semantics, and minimal public surface.

### F. Performance Awareness — weight 5%

Do not require premature optimization, but reject designs that obviously force unnecessary global recompute, repeated tessellation, exact collision on every pair, or uncontrolled copying of heavy geometry.

### G. Maintainability — weight 10%

Check cohesion, duplication, comments explaining decisions rather than syntax, dependency direction, and file/module organization.

## Weighted Result

Compute:

`Total = A*5 + B*4 + C*3 + D*3 + E*2 + F*1 + G*2`

Maximum = 100.

Interpretation:

- 90–100: Approve
- 80–89: Approve with minor changes
- 70–79: Request changes before merge
- 50–69: Significant redesign required
- below 50: Reject architecture/implementation approach

A score of 80+ does not override a violation of a Core Architectural Invariant. Any invariant violation is automatically `Request changes` unless the architecture documents are intentionally updated and justified.

## Mandatory Review Questions

For every non-trivial change, answer these questions:

1. What model object owns this data?
2. Is this source data or derived/cache data?
3. What invalidates it?
4. What does it depend on?
5. What depends on it?
6. Does it introduce a Qt/OCC/platform dependency into Core?
7. Does it create a persistent reference using an unstable index/pointer/sub-shape number?
8. Are units and coordinate frames explicit?
9. Can the behavior be unit-tested without the GUI?
10. Will Undo/Redo or serialization become difficult because of this design?
11. Does this choice prevent later Assembly/Joint/Collision/Robot/Dynamics support?
12. Is the public API larger than necessary?

## Special Review Rules by Module

### Parameter / DependencyGraph

Verify cycle detection strategy is possible, dirty propagation is explicit, and expression evaluation is not coupled to UI.

### Sketch / Constraint

Verify geometry entities have stable IDs, constraints reference IDs, solver state is separate from UI state, and over/under-constrained states can be represented.

### Feature / Body

Verify feature inputs are explicit references and recompute results are replaceable derived data. Avoid storing only final geometry with no construction history.

### OpenCASCADE Kernel Wrapper

OCC types should stay behind the Kernel boundary whenever feasible. Core should communicate through project-owned abstractions.

### Material / MassProperties

Verify density is material data, geometric volume is derived from shape, and mass/COM/inertia can be invalidated and recomputed after geometry changes.

### Assembly / Joint

Verify reusable PartDocument is not given instance-specific motion state. Joints act between component instances and reference frames.

### Collision

Verify broad-phase and narrow-phase are separable, collision geometry may differ from visual geometry, and collision checks can be selectively enabled/disabled.

### Robot / User / Tool Frames

Verify all poses are expressed relative to explicit frames and frame composition is centralized rather than duplicated.

## Required Reviewer Output Format

The reviewer should return exactly these sections:

### Decision
`APPROVE`, `APPROVE WITH MINOR CHANGES`, `REQUEST CHANGES`, or `REJECT`

### Score
`NN / 100`

### Critical Findings
Only defects that can cause wrong architecture, wrong geometry, data corruption, unstable references, or future redesign.

### Major Findings
Important issues that should be fixed before merge.

### Minor Findings
Style, naming, small tests, documentation, or local cleanup.

### Architecture Invariants
For each invariant affected by the change, state `PASS`, `FAIL`, or `NOT APPLICABLE`.

### Required Tests
List specific tests that must exist before approval.

### Suggested Fix
Give the smallest architecture-preserving correction. Do not rewrite unrelated code.

## Reviewer Prompt Template

Use this when asking another agent to review a change:

> You are the independent reviewer for ParametricCAD. Do not implement new features unless needed to demonstrate a minimal correction. Read `docs/ReviewerGuide.md`, `docs/Architecture.md`, `docs/CodingRules.md`, `docs/Roadmap.md`, `docs/DecisionLog.md`, and `AGENTS.md` first. Review the current changes against the architectural invariants. Build the project and run all tests when possible. Report using the exact output format in `ReviewerGuide.md`. Treat any architectural invariant violation as at least REQUEST CHANGES. Focus on correctness, ownership, dependency direction, stable IDs/references, units, coordinate frames, recompute behavior, testability, and future Assembly/Robot/Physics compatibility.
