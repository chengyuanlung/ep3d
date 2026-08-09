# Development Roadmap

## M0 — Repository and Core conventions
- CMake builds on VS2022 x64.
- Core has no Qt/OCC dependency.
- Stable ObjectId.
- Vec2/Vec3/Matrix3/Transform3D.
- Units convention documented.
- GoogleTest enabled.

Exit criterion: tests compile and run.

## M1 — Parametric Part Core
- PartDocument.
- ParameterManager.
- Body / Feature base classes.
- Compute state and dirty propagation (generic DependencyGraph).
- Native JSON serialization (schema v1) with stable-id restore.

Exit criterion: create/save/load a Part with scalar parameters and feature metadata.

## M2 — Document Recompute Infrastructure
- Document-local ObjectRegistry (stable ObjectId → object lookup, non-owning handles).
- PartDocument integrates the DependencyGraph and a DocumentRecomputeEngine behind a façade (single registration path, safe deletion).
- Parameters participate as dirty-source nodes; ParameterState vs ComputeState semantics documented (ADR-011).
- Topological incremental recompute with failure blocking (`BlockedByDependency`), deterministic retry, suppression rule.
- Schema v2 persists explicit dependency edges; registry and graph are rebuilt during load (ADR-012).

Exit criterion: change a Parameter and only its dependent nodes recompute, in order, with failures blocking downstream and retry recovering — proven by the release-gate scenario test.

## M3 — Geometry Kernel Adapter & First Parametric Solid
- Kernel-neutral `IGeometryKernel`/`KernelShape` live in `src/Core/Kernel` (zero OCCT); `src/Kernel/Occt` is the only OCCT-linked target (`OcctGeometryKernel`, `BRepPrimAPI_MakeBox`/`BRepGProp`).
- `BoxFeature` (Width/Height/Depth) and the document-singleton `MassPropertiesNode` join the M2 dependency graph and recompute engine; kernel access is injected via `RecomputeContext::kernel`, never instantiated by `BoxFeature` directly.
- Required graph shape: Width/Height/Depth → BoxFeature → MassPropertiesNode ← Material. Density-only changes recompute mass/COM/inertia without rebuilding geometry.
- Exact Volume, Mass, COM, and inertia tensor (kg·m²) with a single traceable mm→m conversion site; invalid dimensions/density fail transactionally (last valid shape retained, marked stale) and recover deterministically.
- Schema v3 persists BoxFeature/Material semantic records; Feature-owned and MassPropertiesNode edges are always re-derived from semantic id fields on load (never replayed from the generic edge list).

Exit criterion: change Width and receive rebuilt mass properties through real OCCT geometry — proven by the release-gate scenario test (spec §19). **MET** — OCCT 8.0.1, 180/180 tests in Debug and Release, independent review APPROVE 97/100.

## M4 — Sketch/Profile foundation, Pad/Extrude & basic 3D viewer
- Kernel-neutral Sketch model in `src/Core/Sketch`: Point/Line/Circle/Arc in
  sketch-local (u,v) mm on an explicit `SketchFrame`, with `SketchEntityId`
  stable across insertion, removal and reordering (ADR-M4-001/002).
- Semantic `Profile` validation: deterministic loop ordering and orientation
  independent of storage order, a documented connectivity tolerance that is
  never silently healed, and rejection of open/disconnected/branched/duplicate/
  degenerate/self-intersecting outlines (ADR-M4-005).
- `extrudeProfile` on the kernel-neutral interface; `Kernel/Occt` builds
  Wire/Face/Prism internally and no OCCT type crosses the boundary
  (ADR-M4-003).
- `PadFeature` joins the M2 graph: Sketch → Pad → MassProperties,
  PadLength → Pad, Material → MassProperties. Pad-length-only edits do not
  rebuild sketch semantics; density-only edits do not rebuild geometry.
- Schema v4 persists Sketch/Pad semantics only; no OCCT topology, index or
  address is ever an identity (ADR-M4-004).
- Minimal Qt 6 viewer outside Core: display, rotate, pan, zoom, fit-all,
  whole-object selection, refresh after recompute. Core links neither Qt nor
  OCCT (ADR-M4-006).

Deferred from the original M4 sketch, now M5+: feature tree and property panel.

Exit criterion: mandatory release gates A-E (rectangle, circle,
failure/recovery, transformed frames, save/load) pass against real OCCT
geometry. **MET** — OCCT 8.0.1 + Qt 6.11.1, 297/297 tests in Debug and Release.

## M5 — Sketch v1
- Points, lines, circles, arcs.
- 2D view.
- Basic constraints.
- Solver interface.

Exit criterion: constrained rectangle controlled by Width/Height.

## M6 — Features v1
- Sketch-to-wire conversion.
- Pad.
- Pocket.

Exit criterion: rectangular block with editable circular through-hole.

## M7 — DXF v1
- LINE, ARC, CIRCLE, LWPOLYLINE.
- DIMENSION parsing.
- First dimension-to-constraint mapping.

Exit criterion: import a simple dimensioned flange sketch, edit a recognized dimension, rebuild 3D.

## M8 — Assembly foundation
- AssemblyDocument.
- ComponentInstance.
- Reference frame linkage.
- Fixed/Revolute/Prismatic Joint model.

Exit criterion: assemble several Parts and interactively change one joint coordinate.

## M9 — Collision
- Broad phase bounding boxes/BVH.
- Minimum clearance.
- Exact interference.
- Collision rules.
- Last-safe joint position.

## M10 — Robot foundation
- Robot link/joint chain.
- FK.
- User/Tool frames.
- Joint jogging.
- IK interface.

## M11 — Dynamics / Digital Twin
- Consume Part mass/COM/inertia.
- Gravity/load calculations.
- Physics engine adapter.
- Real-controller state bridge.
