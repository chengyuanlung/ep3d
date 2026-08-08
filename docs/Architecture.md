# ParametricCAD Architecture Specification — Draft v0.1

## 1. Product direction

The product begins as a parametric Part CAD system and is deliberately structured so it can grow into:

Parametric Part CAD → Assembly → Mechanism / Joint → Collision → Robot Kinematics → Dynamics / Physics → Simulation / Digital Twin.

The first usable workflow is:

2D Sketch → Constraints / Dimensions → Feature Tree → 3D B-Rep → Parameter Edit → Dependency Recompute → Updated 3D.

DXF import is intentionally deferred until the internal sketch/document model is stable.

## 2. Architectural rules

1. `Core` must not depend on Qt.
2. `Core` must not expose OpenCASCADE types.
3. OpenCASCADE is hidden behind the `Kernel` layer.
4. UI invokes Commands; Commands mutate Documents.
5. Every persistent CAD object has a stable ID; vector indices are never persistent references.
6. Derived data is recomputed, not treated as source-of-truth.
7. Geometry, visual appearance, material, collision representation, and physical properties are distinct concepts.
8. Part files are independent documents. Assembly files reference Part/Assembly documents through instances.
9. Movement is expressed through Joints and Frames, not arbitrary per-frame XYZ edits.
10. Mass properties are first-class derived data from the first version.

## 3. Core object model

```text
CadDocument
├─ PartDocument
│  ├─ ParameterManager
│  ├─ ReferenceFrameManager
│  ├─ BodyManager
│  │  └─ Body
│  │     └─ FeatureTree
│  ├─ MaterialAssignment
│  ├─ MassProperties
│  └─ ConnectorManager
│
└─ AssemblyDocument                 [future]
   ├─ ComponentInstances
   ├─ Joints / Mates
   ├─ MotionRelations
   ├─ CollisionSystem
   └─ AggregateMassProperties
```

## 4. Units and coordinates

Internal convention:
- Length: millimetres for CAD geometry.
- Angle: radians internally; degrees are display/input convenience only.
- Mass: kilograms.
- Time: seconds.
- Density: kg/m³.
- Right-handed 3D coordinates.
- X = right, Y = forward, Z = up for application-level convention.

Conversions must pass through a Units subsystem; do not scatter conversion constants through feature code.

## 5. IDs and persistent references

Every persistent object uses a stable `ObjectId`.

Examples:
- DocumentId
- BodyId
- FeatureId
- SketchId
- SketchEntityId
- ConstraintId
- ParameterId
- FrameId
- ConnectorId
- ComponentId
- JointId
- MaterialId

Topology references require a higher-level semantic reference. Do not store only `Face #7` or `Edge #12` from a transient kernel shape.

Future `GeometryReference` example:

```text
FeatureId = Pad001
SubElementRole = TopFace
FallbackGeometrySignature = ...
```

## 6. Parameter system

A parameter is not only a double. It has identity, unit, optional expression, source/derived state, and dependency links.

```text
Parameter
├─ id
├─ name
├─ value
├─ unit
├─ expression
└─ state
```

Future expression examples:
- `HoleX = Width / 2`
- `Wall = Height * 0.1`

The parameter system feeds the dependency graph.

## 7. Part, Body, Feature

A PartDocument can contain one or more Bodies. A Body owns an ordered feature history.

```text
PartDocument
└─ Body001
   ├─ Sketch001
   ├─ Pad001
   ├─ Sketch002
   ├─ Pocket001
   └─ Fillet001
```

`Feature` is a document-model object. Kernel shapes produced by a feature are derived/cache data.

Feature compute states:
- Valid
- Dirty
- Failed
- Suppressed

Failure must propagate downstream without crashing the application.

## 8. Sketch and constraints

Initial sketch entities:
- Point
- Line
- Circle
- Arc

Initial constraints:
- Coincident
- Horizontal
- Vertical
- Distance
- DistanceX
- DistanceY
- Radius
- Diameter
- Parallel
- Perpendicular
- Tangent
- Equal
- Angle

The first solver may support only a subset. The object model should not need redesign when more constraints are added.

## 9. Material and physical properties

Material data is source data; physical properties are derived from geometry and density.

```text
Material
├─ name
├─ density
├─ elastic modulus
├─ poisson ratio
├─ yield strength
└─ contact properties [future]

MassProperties
├─ volume
├─ mass
├─ centerOfMass
└─ inertiaTensorAboutCOM
```

Geometry edit → B-Rep rebuild → volume/mass/COM/inertia recompute → dependent assembly/dynamics data marked dirty.

## 10. Reference Frames

Frames are a general subsystem, not a robot-only feature.

Frame hierarchy examples:

```text
World
└─ Assembly
   └─ RobotBase
      └─ Joint1
         └─ ...
            └─ Flange
               └─ Tool
```

Other frames may include Camera, Fixture, Workpiece, User, Tool, Mount, Shaft, and OutputFrame.

Each frame stores a transform relative to a parent frame.

## 11. Connectors and future assembly behavior

A Part may publish semantic connection locations without hard-coding assembly motion.

Examples:
- Mount connector
- Shaft output connector
- Linear rail connector
- Tool flange connector

A Connector describes a frame and intended role. An actual Joint is created at Assembly level.

This distinction lets one Part instance be fixed in one assembly and movable in another.

## 12. Assembly plan

Future ComponentInstance:

```text
ComponentInstance
├─ id
├─ sourceDocumentId
├─ localTransform
└─ instance metadata
```

Joint types planned:
- Fixed
- Revolute
- Prismatic
- Cylindrical
- Planar
- Spherical
- Free

Initial implementation should focus on Fixed, Revolute, and Prismatic.

Each movable Joint uses parent and child Joint Frames rather than only an axis vector.

## 13. Collision / interference plan

Collision is a separate subsystem.

Pipeline:
1. Broad phase: AABB/BVH.
2. Narrow phase: exact/sufficient geometry query.
3. Result: separated / touching / penetrating.
4. Optional motion behavior: detect-only, stop-on-collision, last-safe-position.

Visual geometry and collision geometry are independent. Complex models may use simplified collision shapes.

## 14. Commands, transactions, undo/redo

Document state must not be directly changed from random UI handlers.

Command examples:
- ChangeParameterCommand
- AddFeatureCommand
- DeleteFeatureCommand
- AddConstraintCommand

Commands participate in transactions so multi-object edits can be undone atomically.

## 15. Dependency graph

The dependency graph is central infrastructure.

Example:

```text
Width parameter
   ↓
Sketch001
   ↓
Pad001
   ↓
Body shape
   ↓
MassProperties
   ↓
Assembly instance
   ↓
Collision cache / dynamics
```

Only affected nodes should recompute.

## 16. File format

The native format must be versioned from day one.

Suggested structure:

```json
{
  "format": "ParametricCAD",
  "schemaVersion": 1,
  "documentType": "Part"
}
```

Use a serialization layer independent from UI and kernel. JSON is acceptable for the early prototype; later, a packaged/binary format can be added without changing the document model.

## 17. DXF strategy

DXF import occurs after the internal sketch system works.

Import pipeline:

```text
DXF Reader
→ Raw DXF entities
→ DxfEntityConverter
→ Sketch entities
→ Dimension/relationship analysis
→ Constraints
```

Initial DXF entities:
- LINE
- ARC
- CIRCLE
- LWPOLYLINE
- DIMENSION

A DXF dimension is not assumed to be a parametric constraint automatically. The converter must resolve what geometry the dimension refers to.

## 18. UI architecture

Planned desktop UI uses Qt Widgets:
- MainWindow
- FeatureTree
- PropertyEditor
- SketchView
- View3D
- SelectionModel

UI remains outside Core. The same Core should be usable by CLI tests, import utilities, or future services.

## 19. Physics and robotics roadmap

Do not implement a full physics engine in v0.1. Preserve the data needed for one:
- mass
- center of mass
- inertia tensor
- joint position/limits
- future damping/friction
- collision geometry

Later integration can target a physics/dynamics engine without redesigning Part files.

## 20. Non-goals for v0.1

Do not implement yet:
- Assembly solver
- Robot IK
- Dynamics
- FEA
- CAM
- Large-assembly optimization
- Advanced NURBS editing
- Sheet metal
- Full DXF compatibility

The first objective is a clean, testable Part Core.
