# EP3D — Onshape Alignment Development Plan

## 1. 定位

EP3D 將 Onshape 作為參數式機械 CAD 的主要「行為、工作流程、UX」參考，但不複製其程式實作、品牌、圖示或雲端架構。

目標不是製作 Onshape clone，而是：

```text
Onshape-class parametric workflow
        +
EP3D semantic/recompute architecture
        +
Local/offline CAD
        +
2D DXF / 工程圖 → 可編輯參數式 3D
        +
未來 USER / TOOL / Assembly coordinate systems
```

Onshape 的 Sketch 以平面上的曲線、dimensions 與 constraints 組成；Sketch 是 Part Studio 中參數式 feature。Feature 則以參數形式保存在 Feature List。Assembly 使用 Mate Connector（local coordinate system）與 Mate 定義零件定位與運動關係。

## 2. 對齊原則

### A01 — 對齊行為，不對齊內部實作
參考：
- Sketch workflow
- Constraints / Dimensions
- Feature history
- Feature edit / preview / accept / cancel
- Selection behavior
- Assembly Mate / Connector concept
- Error/status UX
- Configurations

### A02 — EP3D 架構不因模仿 UI 而破壞

```text
Core semantic model
      ↓
Dependency / Recompute
      ↓
Kernel-neutral API
      ↓
OCCT
```

Qt 與 OCCT presentation 不得進入 Core semantic layer。

### A03 — Stable semantic identity
禁止將 vector index、pointer、Qt object、solver variable index、transient OCCT Edge/Face 當永久 CAD identity。

### A04 — User-assisted UI validation
Agent 建立 deterministic samples、測試與操作說明；User 實際操作 Qt UI 回報 PASS/FAIL。文件記為 `User-Assisted Manual Validation`。

## 3. 目標文件模型

```text
EP3D Document
├─ Part Studio / Part Design Context
│  ├─ Origin
│  ├─ XY / XZ / YZ Plane
│  ├─ Sketch001
│  ├─ Pad/Extrude001
│  ├─ Sketch002
│  ├─ Pocket001
│  └─ Fillet001
├─ Parts
├─ Assembly
│  ├─ Instances
│  ├─ Frame/Mate Connectors
│  └─ Mates/Joints
└─ Drawing
   ├─ Front / Top / Side / Section
   └─ Dimensions / Annotations
```

不要求照抄 Onshape browser/tab UI，但 semantic organization 應保持一致且容易理解。

## 4. Sketch 工作流程

```text
Create Sketch
→ Select Plane / ReferenceFrame
→ Draw Geometry
→ Automatic Inference（逐步加入）
→ Constraints
→ Dimensions
→ DOF / Constraint Status
→ Accept Sketch
```

M5 基本 entity：
- Point
- Line
- Circle
- Arc

Rectangle 應優先視為普通 Lines + constraints，而不是依賴特殊矩形 topology。

## 5. Sketch Coordinate Model

```text
Sketch (u,v)
→ Sketch ReferenceFrame
→ Part local XYZ
→ Assembly Instance Transform
→ World
```

初期支援 XY/XZ/YZ 與 explicit ReferenceFrame。未來可擴充 planar face、Connector。ReferenceFrame 應成為 Sketch、Assembly、USER、TOOL 共用抽象。

## 6. Constraint Roadmap

M5 mandatory：

```text
Coincident
Horizontal
Vertical
Distance
Length
Radius
Diameter
Angle
Fix
```

下一階段對齊：

```text
Parallel
Perpendicular
Tangent
Equal
Concentric
Midpoint
Symmetric
Point-on-object
```

所有 references 必須使用 stable SketchEntityId / SketchElementRef。

## 7. Dimension Model

正式區分：

```text
Driving Dimension
→ 控制 geometry

Driven / Reference Dimension
→ 只量測/顯示 geometry
```

目標 UX：

```text
double-click dimension
→ numeric editor
→ value / expression
→ Enter
→ solve
→ downstream recompute
```

Dimensional constraint 綁 existing Parameter ObjectId。長度以 mm，角度內部 radians，UI 可顯示 degrees。

未來 expression 可支援 `Width`, `Width/2`, `BaseWidth + 20 mm`，但不可阻塞 M5。

## 8. Constraint State UX

至少清楚顯示：

```text
Fully constrained / DOF=0
Under constrained / DOF>0
Conflicting
Invalid
Numerical failure
```

錯誤應能指出 Sketch 與相關 ConstraintId；不能只顯示 `Solver failed`，也不能只靠顏色。

## 9. Feature History

Feature List 是主要 parametric history：

```text
Origin
Sketch001
Pad001
Sketch002
Pocket001
Fillet001
```

每個 Feature 必須具有 stable ObjectId、type、input refs、Parameters、recompute state、diagnostics、computed result。

修改歷史 feature 只 recompute affected descendants。

## 10. Feature Edit Transaction

目標：

```text
Select Feature
→ Edit
→ temporary parameter state
→ Preview
→ ✓ Accept → commit/recompute
  or
→ ✕ Cancel → original state
```

Preview 不可直接成為 persistent document state。簡單 property quick-edit 可以保留。

## 11. Feature Roadmap

Foundation：
- Sketch
- Pad / Extrude

接著：
- Pocket / Remove Extrude
- Revolve
- Fillet
- Chamfer
- Hole
- Boolean
- Mirror
- Pattern
- Shell

之後：
- Sweep
- Loft
- Draft
- advanced surface tools

## 12. Generalized Extrude Direction

Pad 未來可演進成 generalized Extrude：

```text
Operation:
New / Add / Remove / Intersect

Extent:
Blind / Symmetric / Through All / Up To Face
```

M5 不需為此擴大 scope。

## 13. Selection Architecture

```text
Feature Tree
↔ Viewer
↔ Property / Feature Dialog
```

selection level roadmap：
- Document Object
- Part
- Feature
- Sketch
- Sketch Entity
- Vertex / Edge / Face
- Connector / Frame
- Assembly Instance

Transient viewer handle 與 persistent semantic reference 必須分離。

## 14. Preselection

未來：

```text
Hover → Pre-highlight
Click → Selected
```

兩者視覺必須不同，供 Sketch、Fillet edge、Face、Connector 選取使用。

## 15. Undo / Redo

逐步建立 semantic transaction：

```text
User Command
→ Document Transaction
→ Undo Record
```

Create Sketch、change dimension、create Pad、delete feature、change constraint 都應成為 atomic Undo 操作。不得把 raw OCCT state 當 semantic undo history。

## 16. Feature Rollback

未來 Feature List 支援 rollback/evaluation position，用於查看 intermediate state、插入/修改歷史 feature。只有 dependency/recompute semantics 足夠穩定後才實作。

## 17. Suppression / Configurations

Feature 狀態：

```text
Enabled
Suppressed
Failed
Dirty
```

Configurations 未來直接驅動 Parameter / feature suppression，而不是複製整份 Document：

```text
Small:  Width=80
Medium: Width=100
Large:  Width=120
```

未來也可配置 material、feature values、assembly mate values。

## 18. Assembly Alignment

採用 local coordinate connector + behavioral mate 的核心概念。

EP3D 可命名：

```text
FrameConnector / MateConnector
```

內容：

```text
Origin
X/Y/Z axes
Owner ObjectId
Attachment semantic reference
```

這與未來 USER/TOOL coordinate system 共用 ReferenceFrame architecture。

## 19. Assembly Instance Model

```text
Part Definition Geometry
→ Part Local Coordinates
→ Instance Transform
→ Assembly Coordinates
→ World
```

Assembly placement 不可 bake 回 Part geometry。同一 Part 可有多個 instances 與不同 transforms。

## 20. Mate / Motion Roadmap

Initial：
- Fastened / Fixed
- Revolute
- Slider
- Cylindrical
- Planar

Later：
- Ball
- Pin-slot
- Gear relation
- Rack-and-pinion
- Screw relation
- Motion limits

Mate 定義 allowed/restricted DOF。例如 Revolute 保留單一 rotation DOF。

## 21. Connector Reuse

Part definition 上的 Connector 可由所有 Assembly instances 重複使用：

```text
MotorPart
├─ ShaftAxisConnector
└─ MountConnector

MotorInstance.ShaftAxisConnector
↕ Revolute
ArmInstance.JointConnector
```

這比直接永久引用不穩定 transient topology 更適合作為運動 semantic foundation。

## 22. Motion Limits

未來：

```text
Revolute: -90° ≤ θ ≤ +90°
Slider:    0 mm ≤ d ≤ 200 mm
```

初期只做 kinematics，不需要 mass/dynamics simulation。

## 23. Interference

```text
Assembly pose
→ world-space instance B-Reps
→ broad-phase bounds
→ precise OCCT intersection
→ interference result
```

Interference 與 Mate 分離；合法 Mate 仍可能造成 collision。

## 24. Drawing Architecture

區分：

```text
Sketch Driving Dimension
vs
Drawing Annotation Dimension
```

正常方向：

```text
3D → Drawing Views → Annotation
```

EP3D 的核心差異化方向：

```text
Existing Drawing / DXF
→ semantic geometry reconstruction
→ dimension/constraint reconstruction
→ driving Sketch Parameters
→ editable 3D reconstruction
```

## 25. DXF — M6

```text
DXF
→ LINE / ARC / CIRCLE
→ unit normalization
→ stable source mapping
→ Sketch entities
```

保留 provenance：
- source file
- layer
- source handle/id where available
- original geometry
- import transform

DXF entity order 不可成為 EP3D semantic identity。

## 26. Dimension Reconstruction — M7

```text
2D geometry
+ dimension annotations
+ geometric relationships
→ Constraint Reconstruction
→ M5 Constraints + Parameters
```

Example：

```text
Rectangle + dimensions 100 / 50
→ Coincident
→ Horizontal / Vertical
→ Length=100 / Length=50
→ Fully constrained Sketch
```

Ambiguous reconstruction 必須報告/要求確認，不可 silent guess。

## 27. Multi-view Reconstruction — Long Term

```text
Front + Top + Side
→ view alignment
→ correspondence
→ profile/feature hypotheses
→ user confirmation when ambiguous
→ parametric Feature History
```

必須建立在 M5/M6/M7 stable semantics 之後。

## 28. UI Layout Direction

```text
┌─────────────────────────────────────────────────────┐
│ Menu / Context Toolbar                              │
├─────────────────────────────────────────────────────┤
│ Sketch / Feature Toolbar                            │
├──────────────┬──────────────────────┬───────────────┤
│ Feature List │                      │ Edit/Property │
│ Sketch001    │     3D / Sketch      │ Parameters    │
│ Pad001       │       Viewer         │ Constraints   │
│ Sketch002    │                      │ Status        │
├──────────────┴──────────────────────┴───────────────┤
│ Status / Selection / DOF / Coordinates              │
└─────────────────────────────────────────────────────┘
```

不 pixel-copy Onshape；對齊 hierarchy 與 interaction。

## 29. Contextual Toolbars

```text
Part mode     → Feature tools
Sketch mode   → Geometry + Constraints + Dimensions
Assembly mode → Insert + Mate + Motion
Drawing mode  → Views + Annotation
```

避免所有命令塞進單一巨大 toolbar。

## 30. Keyboard Baseline

```text
Esc     cancel tool
Enter   accept numeric edit / operation
Delete  delete selected semantic object
Ctrl+Z  undo
Ctrl+Y  redo
```

Shortcut 不能取代 discoverable menu/tool commands。

## 31. User-Assisted UX Validation

每個 milestone 由 Agent 建 deterministic samples + expected values，User 實際操作。

M5：
- Fully constrained rectangle
- Under-constrained rectangle
- Conflicting rectangle
- Constrained circle

M6：
- simple_rectangle.dxf
- circle_plate.dxf
- mixed_line_arc_profile.dxf
- invalid_open_profile.dxf

M7：
- dimensioned_rectangle
- dimensioned_hole_plate
- ambiguous_dimension_case

User 回報 PASS/FAIL；Agent 負責 formal report 與 regression。

## 32. Alignment Validation Matrix

每個功能文件化：

```text
Reference behavior
EP3D intended behavior
Implemented behavior
Intentional difference
Validation result
```

避免「向 Onshape 對齊」變成主觀外觀模仿。

## 33. Milestone Roadmap

### M5 — Sketch Constraints
- driving/reference dimension foundation
- solver
- DOF/status
- constraint diagnostics
- dimension-edit UX

Release proof：

```text
Width 100→120
→ solve Sketch
→ rebuild Pad
```

### M6 — DXF Import
`DXF → stable Sketch entities`

### M7 — Dimension / Constraint Reconstruction
`2D dimension → M5 Parameter/Constraint → 3D`

### M8 — Core Feature Modeling
Pocket, Revolve, Fillet, Chamfer, Hole, Boolean, Mirror, Pattern, Shell.

### M9 — History / Editing Infrastructure
Undo/Redo, rollback, suppression, diagnostics.

### M10 — Reference Frames / Connectors
ReferenceFrame, Part connectors, Sketch-on-frame, USER/TOOL-ready semantics.

### M11 — Assembly Foundation
Instances, transforms, Fastened, Revolute, Slider, connector mating.

### M12 — Assembly Motion / Limits
DOF, limits, kinematic drag/solve, additional mates/relations.

### M13 — Interference
Assembly collision/interference.

### M14 — Drawings
Orthographic/section views, annotations, linked dimensions.

### M15 — Configurations
Parameter variants, suppression variants, assembly variants.

Milestone numbering可依 repository 現況調整，但 dependency order 建議維持。

## 34. 不應複製 Onshape 的項目

不複製：
- branding
- proprietary icons/assets/text
- exact visual styling
- undocumented proprietary implementation
- cloud architecture purely for imitation
- FeatureScript source implementation

可學習：
- workflow
- industry-standard terminology
- Sketch constraint UX
- Feature history concepts
- connector-based assembly abstraction
- interaction consistency
- error handling expectations

## 35. EP3D 差異化

1. **2D → 3D reconstruction**：existing DXF/drawing 轉成 editable parametric model。
2. **Local/offline operation**：Core CAD/solver 不依賴 cloud。
3. **Machine/robot coordinate integration**：ReferenceFrame 同時服務 Sketch/Part/Assembly/USER/TOOL。
4. **User-verifiable reconstruction**：ambiguous 2D reconstruction 採 Agent proposal → User confirmation → commit。

## 36. Permanent Architecture Gates

```text
[ ] stable semantic IDs
[ ] no transient topology persistent identity
[ ] Core free of Qt
[ ] Core free of direct OCCT dependency
[ ] dependency-aware recompute
[ ] transactional failure
[ ] serialization round-trip
[ ] deterministic tests
[ ] no false PASS for unexecuted UI tests
[ ] User-assisted validation recorded accurately
```

## 37. M5 Immediate Alignment

M5 實作時必須評估：

```text
[ ] Driving vs Driven dimension semantics
[ ] dimension display in Sketch viewer
[ ] double-click/edit dimension workflow
[ ] Fully/Under constrained state
[ ] DOF presentation
[ ] automatic inference architecture placeholder
[ ] constraint icon/state representation
[ ] Accept/Cancel Sketch edit transaction
[ ] contextual Sketch toolbar
```

不應為追求 UX 一次完成而犧牲 M5 solver correctness；未完成者明確 defer。

## 38. M5 User Validation Scenario

Sample：

```text
Width=100
Height=50
Pad=20
```

User：

```text
open Sketch
→ see dimensions 100 / 50
→ verify Fully constrained / DOF=0
→ edit Width dimension
→ type 120
→ Enter
→ Accept Sketch if required
→ observe Pad rebuild
```

Expected：

```text
Width=120
Height=50
Pad=20
Volume=120000 mm³
DOF=0
```

User 同時評估 dimension discoverability、constraint state、Accept/Cancel、3D update 與整體 parametric workflow。

## 39. Documentation Set

```text
docs/
├─ EP3D_Onshape_Alignment_Roadmap.md
├─ M5_Implementation_SelfValidation_and_Evaluation.md
├─ M5_UI_User_Assisted_Validation_Guide.md
└─ architecture/
   └─ ADRs...
```

之後每個 milestone 應增加 Alignment section，而不是重新定義一套 CAD philosophy。

## 40. Codex Planning Prompt

```text
Read AGENTS.md first.

Then read:
- docs/EP3D_Onshape_Alignment_Roadmap.md
- docs/M5_Implementation_SelfValidation_and_Evaluation.md
- docs/M5_UI_User_Assisted_Validation_Guide.md
- all accepted M0-M4 ADR/review/completion documents.

Use Onshape as a behavioral and UX reference for EP3D's parametric CAD workflow, not as an implementation or visual-copy target.

For M5 prioritize:
1. stable Sketch/Constraint semantics,
2. driving dimensional constraints,
3. solver correctness + DOF/status,
4. dimension edit → Sketch → Profile → Pad → 3D,
5. discoverable dimensions/constraint state,
6. transactional Accept/Cancel where practical,
7. user-assisted UI validation.

Before coding produce a short alignment analysis:
- current EP3D behavior,
- desired reference behavior,
- proposed M5 behavior,
- intentional differences,
- deferred items.

Do not expand M5 into DXF, full production Sketcher, Assembly or advanced feature modeling.

Never compromise EP3D architecture merely to imitate UI behavior.

Release-critical proof:
Width 100→120 mm on a fully constrained rectangle rebuilds Pad volume 100000→120000 mm³ for Height=50 and Pad=20.

Record manual visual validation as User-Assisted Manual Validation.
```

## 41. Final Product Direction

```text
Stable semantic model
→ Parameters
→ Constraints
→ Features
→ Parts
→ Instances
→ Connectors / Mates
→ Drawings
```

加上 EP3D 的反向工程核心：

```text
2D Drawing / DXF
→ semantic recognition
→ constraints / parameters
→ feature reconstruction
→ editable 3D model
```
