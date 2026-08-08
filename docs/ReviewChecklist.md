# ParametricCAD Review Checklist

Use this as a fast pass before a full review.

## Build and Tests

- [ ] Project configures with CMake.
- [ ] Project builds cleanly.
- [ ] Existing tests pass.
- [ ] New behavior has tests where practical.
- [ ] Core tests do not require Qt or a GUI.

## Architecture

- [ ] `src/Core` has no Qt dependency.
- [ ] `src/Core` has no OpenCASCADE dependency.
- [ ] UI does not become the source of truth for model state.
- [ ] Generated geometry/cache is not treated as persistent model history.
- [ ] Ownership of new data is clear.
- [ ] Dependencies point in the intended direction.

## IDs and References

- [ ] Persistent objects use stable IDs.
- [ ] No vector index is stored as persistent identity.
- [ ] No raw pointer/address is serialized or treated as identity.
- [ ] No OCC Face/Edge numeric index is treated as a permanent reference.
- [ ] New references have a future-safe persistent-reference strategy.

## Parameters and Recompute

- [ ] Units are known and explicit.
- [ ] Parameter changes can mark dependent data dirty.
- [ ] Derived values can be recomputed.
- [ ] Failed recompute can be represented without corrupting prior state.
- [ ] Design does not force full-document recompute forever.

## Geometry and Features

- [ ] Feature inputs are explicit.
- [ ] Feature result is derived data.
- [ ] Body/Feature/Sketch responsibilities are not mixed.
- [ ] Sketch entities and constraints use stable references.

## Frames and Motion

- [ ] Coordinate frame is explicit for poses/transforms.
- [ ] No duplicated ad-hoc User/Tool conversion math.
- [ ] Joint behavior belongs to Assembly instances, not reusable Part geometry.
- [ ] Joint limits and collision constraints are separate concepts.

## Material and Physics Readiness

- [ ] Material density is distinct from mass.
- [ ] Volume/mass/COM/inertia are modeled as derived physical properties.
- [ ] Physics-engine-specific types are not embedded into Core model classes.
- [ ] Contact/friction data is separable from bulk engineering material.

## Collision

- [ ] Collision system is separate from Feature and Joint classes.
- [ ] Visual geometry and collision geometry can differ.
- [ ] Broad-phase can exist before exact narrow-phase checks.
- [ ] Pair/group filtering is possible in the design.

## Undo / Serialization

- [ ] Mutation can be represented as a command/transaction.
- [ ] A future Undo operation is not made impossible by hidden side effects.
- [ ] New persistent data has an obvious serialization path.
- [ ] File format versioning/migration is considered for schema changes.

## API Quality

- [ ] Public API is minimal.
- [ ] Ownership/lifetime is clear.
- [ ] `const` is used appropriately.
- [ ] Error state/result semantics are explicit.
- [ ] No unnecessary framework type leaks across module boundaries.

## Final Gate

- [ ] No Core Architectural Invariant from `ReviewerGuide.md` is violated.
- [ ] Reviewer score is 80/100 or higher for merge approval.
- [ ] Any architecture decision change is recorded in `DecisionLog.md`.
