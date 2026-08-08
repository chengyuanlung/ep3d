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
- Compute state and dirty propagation.
- Native JSON serialization skeleton.

Exit criterion: create/save/load a Part with scalar parameters and feature metadata.

## M2 — Kernel adapter + primitive solid
- Add OpenCASCADE wrapper target.
- Create a box from Width/Height/Length.
- Keep TopoDS_Shape inside Kernel implementation only.
- Compute volume, center of mass, inertia.

Exit criterion: change Width and receive rebuilt mass properties.

## M3 — Qt viewer
- Qt Widgets shell.
- Feature tree.
- Property panel.
- OCC-based 3D view through adapter.
- Parameter edit triggers recompute.

Exit criterion: edit Width 100 → 120 and see 3D update.

## M4 — Sketch v1
- Points, lines, circles, arcs.
- 2D view.
- Basic constraints.
- Solver interface.

Exit criterion: constrained rectangle controlled by Width/Height.

## M5 — Features v1
- Sketch-to-wire conversion.
- Pad.
- Pocket.

Exit criterion: rectangular block with editable circular through-hole.

## M6 — DXF v1
- LINE, ARC, CIRCLE, LWPOLYLINE.
- DIMENSION parsing.
- First dimension-to-constraint mapping.

Exit criterion: import a simple dimensioned flange sketch, edit a recognized dimension, rebuild 3D.

## M7 — Assembly foundation
- AssemblyDocument.
- ComponentInstance.
- Reference frame linkage.
- Fixed/Revolute/Prismatic Joint model.

Exit criterion: assemble several Parts and interactively change one joint coordinate.

## M8 — Collision
- Broad phase bounding boxes/BVH.
- Minimum clearance.
- Exact interference.
- Collision rules.
- Last-safe joint position.

## M9 — Robot foundation
- Robot link/joint chain.
- FK.
- User/Tool frames.
- Joint jogging.
- IK interface.

## M10 — Dynamics / Digital Twin
- Consume Part mass/COM/inertia.
- Gravity/load calculations.
- Physics engine adapter.
- Real-controller state bridge.
