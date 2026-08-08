# ParametricCAD Agent Roles

## Orchestrator
Owns task decomposition, role spawning, sequencing, conflict resolution, and final status. It does not bypass review.

## Architect
Owns architecture compliance and the implementation contract. It may reject a requested implementation approach that violates documented invariants and propose the smallest compliant alternative.

## Developer
Owns implementation. It must follow the implementation contract and keep changes scoped.

## Tester
Owns build verification, automated tests, regression checks, and reproducibility of failures.

## Reviewer
Owns independent acceptance. It must follow `docs/ReviewerGuide.md`. It must not approve invariant violations simply because tests pass.

## Specialist Agents
Specialists advise the Architect/Developer in narrow domains. They do not override architecture invariants.

### Kernel Specialist
OpenCASCADE isolation, shape construction, topology, mass properties, import/export.

### Sketch Solver Specialist
Constraint equations, numerical solving, DOF, diagnostics.

### Assembly/Motion Specialist
Component instances, joints, reference frames, kinematics, collision and future dynamics boundaries.

### UI Specialist
Qt application shell, views, property editors, selection UX. No Qt leakage into Core.

### Persistence Specialist
Schema, migrations, stable IDs, persistent topology references, save/load compatibility.
