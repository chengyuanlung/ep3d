# EP3D — Onshape Alignment Development Plan

> **本文件的角色**：EP3D 的長期行為規格參考。Onshape 只作為 *behavioral / workflow / UX* 對照組，不是實作或視覺抄襲對象（§34）。
> **編號穩定性**：§ 編號是各 milestone SPEC 的引用錨點（M8_SPEC 引用 §9/§16，M9_SPEC 引用 §10/§15/§16/§17/§32/§33）。**既有節號與其主題不得重編**；新主題一律往後追加。
> **參考來源**：見 §46（分級索引：已讀內文／僅搜尋摘要／範圍外未讀）。掃描日期 2026-08-20。覆蓋率與待讀清單見 §53。

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

參考模型的四層切分（EP3D 沿用此切分，命名可在地化，見 §45）：

```text
Part Studio       Sketch + Feature history → 一或多個 Part
Assembly          Instance + Mate Connector + Mate → 定位與運動
Drawing           由 3D 產生的關聯式 2D 視圖與標註
Variable / Config 參數與變體，跨上述三者驅動
```

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

### A05 — 對齊主張必須可查證
凡在本文件宣稱「參考行為是 X」，必須能指回 §46 的來源章節。無法查證的行為描述應標記為 **EP3D 自訂**，不得偽裝成對齊需求。

### A06 — 顏色不得是唯一資訊通道
參考模型大量使用顏色編碼（藍/黑/紅、灰底黑字/紅底白字）。EP3D 沿用同一語意，但每個狀態必須另有 **icon + 文字**。理由見 §8 與 §44。

### A07 — 功能求全，介面重畫（Owner 裁決，2026-08-20）

這是本專案的**範圍總則**，優先於本文件其他敘述：

```text
功能面（What）    以「完整複製」為目標。
                  參考模型有的 CAD 功能，EP3D 原則上都要有，
                  差別只在排程先後，不在要不要做。
                  要排除某項功能，必須有明確理由並記入 §52。

介面面（How）      完全自由重新規劃。
                  版面、面板配置、命令組織、圖示、互動手勢、
                  快捷鍵配置一律不受參考模型拘束。
```

推論規則：

1. **「不做」需要理由，「做」不需要**。這與先前的預設相反。§11「之後」、§20「Later」等字眼一律解讀為排程，不是排除。
2. **UI 差異不需記入 §32 的 intentional difference**——介面本來就自訂。§32 只記錄**行為與語意**上的刻意差異。
3. **功能缺口才是 §32 的重點**：若某功能決定不做或延後很久，必須在 §52 留下裁決與理由。
4. A02（架構不因模仿 UI 而破壞）仍然優先：功能求全不等於接受破壞 Core 分層的實作方式。
5. 唯一的結構性例外是**雲端架構本身**——見 §52.3，那是 §35.2「local/offline」的直接後果，屬於既有的產品定位決策，不是本則的例外開口。

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

參考行為補充：Part Studio 的 Feature list 之下以分隔列接 **Parts list**，可含 part / surface / mesh / composite part 四類產物，各自可 hide、rename、指定 material、改 appearance、單獨匯出。EP3D 對應語意見 §9.1。

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

### 4.1 Entity roadmap

| 階段 | Entity |
|---|---|
| 已定義（M5） | Point、Line、Circle、Arc |
| 次階段 | Rectangle（= 4 Lines + constraints）、Polygon、Slot、Ellipse |
| 之後 | Spline / conic、Offset、Mirror、Trim / Extend、Construction geometry |
| 需要外部參考時 | **Use / Project**（把既有 edge 投影進 Sketch）、**Intersection**（平面與實體相交產生 Sketch 曲線） |

`Use` 與 `Intersection` 在參考模型中會**自動產生對應 constraint**（§6.2）。EP3D 亦應如此：投影得到的 entity 必須被自動約束到來源，不得是一份斷了關聯的複本。

#### 4.1.1 參考模型的完整 sketch 工具清單（查證後）

平面 sketch 工具：

```text
Line、Corner rectangle、Center point rectangle、
Center point circle、3 point circle、
Tangent arc、3 point arc、Center point arc、
Spline、Bezier、Conic、Ellipse、Elliptical arc、
Point、Polygon（含內接／外切）、Slot、Text、
Fillet、Chamfer、Trim、Extend、Offset、
Mirror、Linear pattern、Circular pattern、
Use（Project/Convert）、Construction（切換建構幾何）
```

3D curve 工具（獨立於 sketch，但同屬幾何輸入）：

```text
Helix、3D fit spline、Projected curve、Bridging curve、
Composite curve、Intersection curve、Trim curve、
Isocline、Offset curve、Isoparametric curve、
Edit curve、Routing curve
```

EP3D 判讀：

1. **平面工具是 M5–M6 的延伸範圍**；3D curve 整組屬 surface/advanced 領域，與 §11 的「之後」同期，不進中期規劃。
2. `Construction` 是**切換**而非獨立實體型別——建構幾何仍是 Line/Circle/Arc，只是不參與 profile。EP3D 應以 entity 上的 flag 實作，不新增型別。
3. Sketch 內的 `Fillet` / `Chamfer` / `Trim` / `Extend` / `Offset` / `Mirror` / `Pattern` 都是**產生 entity + constraint 的複合操作**，不是單一 entity。實作時必須決定它們產生的 constraint 是否可獨立刪除——建議可以，理由同 §6.2。

### 4.2 Automatic inference（自動推斷）

參考行為：

```text
游標接近既有 entity → inference「醒來」（proximity wake-up）
→ 虛線 + 橘色高亮 + constraint 圖示，預示將建立的關係
→ 放開滑鼠 → 該 constraint 真的寫進 Sketch
→ 按住 Shift 拖曳 → 抑制推斷，畫出不受吸附的幾何
```

常見推斷：horizontal、vertical、midpoint、parallel、coincident，以及與 Origin 的對齊。

EP3D 要求：

1. Inference 是 **constraint 產生器**，不是繪圖時的視覺吸附。推斷成立就要產生真正的 ConstraintId，否則 DOF 會說謊。
2. 推斷產生的 constraint 必須可在 constraint 清單中看到並刪除（§6.3）。
3. 必須有明確的「抑制推斷」修飾鍵，並在 status bar 顯示目前是否處於抑制狀態。
4. M5 階段可只做架構 placeholder（§37），但 placeholder 不得回報假的 DOF。

## 5. Sketch Coordinate Model

```text
Sketch (u,v)
→ Sketch ReferenceFrame
→ Part local XYZ
→ Assembly Instance Transform
→ World
```

初期支援 XY/XZ/YZ 與 explicit ReferenceFrame。未來可擴充 planar face、Connector。ReferenceFrame 應成為 Sketch、Assembly、USER、TOOL 共用抽象。

參考行為對照：Onshape 允許 Sketch 建立在 plane、planar face 或 **Mate Connector** 上——因為 Mate Connector 本身就是一個 local coordinate system。EP3D 的 ReferenceFrame 抽象一旦成立（M10），Sketch-on-frame 就是同一件事，不應另做一套平面型別（見 §18、§21）。

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

### 6.1 完整對照表（參考模型的手動 constraint 全集）

| Constraint | 參考定義 | EP3D 階段 |
|---|---|---|
| Coincident | 使 2 個以上 sketch entity 共用同一位置 | M5 |
| Horizontal | 使一或多條線、或一組點水平 | M5 |
| Vertical | 使一或多條線、或一組點垂直 | M5 |
| Fix | 將 entity 鎖在 sketch plane 上不動 | M5 |
| Concentric | 使任一點與 arc/circle 圓心重合；或使數個 arc/circle 共圓心 | 次階段 |
| Parallel | 使 2 條以上線平行 | 次階段 |
| Perpendicular | 使 2 條線垂直 | 次階段 |
| Tangent | 使 2 個 entity 相切，或 entity 與 plane 相切 | 次階段 |
| Equal | 使數條線等長，或數個 arc 等半徑 | 次階段 |
| Midpoint | 使一點位於線或 arc 的中點 | 次階段 |
| Symmetric | 使 2 個同型 entity 相對於線／平面／直邊對稱 | 次階段 |
| Point-on-object | 使一點落在另一 entity 上 | 次階段 |
| Normal | 使 curve 與 line、或 curve 與 plane 垂直 | 後期（需 spline） |
| Pierce | 使 sketch entity 與 **sketch plane 之外**的 entity 重合 | 後期（需 3D ref，配合 §5 / §10） |
| Curvature | 使 spline / conic 與周邊幾何達成曲率連續 | 後期（需 spline） |

Dimensional constraint（Distance / Length / Radius / Diameter / Angle）在參考模型中歸類為 dimension 而非 constraint，但在 solver 中同屬約束；EP3D 維持 §7 的 driving / driven 區分。

### 6.2 自動產生的 constraint

參考模型中有三種 constraint **不由使用者手動下達**，而是隨操作自動生成：

```text
Quadrant      吸附到圓／弧四分點時自動產生
Use           由 Use/Project 投影外部 edge 進 Sketch 時自動產生
Intersection  由平面與實體相交產生 Sketch 曲線時自動產生
```

EP3D 規則：自動產生的 constraint **仍是一等公民**——有 ConstraintId、計入 DOF、可被列出。可標記為 auto-generated 以便 UI 區分，但不得成為 solver 看不到的隱形關係（隱形關係等同不穩定 identity，違反 A03 的精神）。

### 6.3 Constraint Manager

參考行為：提供一個 constraint 管理面板，可依 **type / mode / status** 篩選，並以垃圾桶圖示刪除；在畫布上也可直接點選 constraint 圖示後按 Delete。

EP3D 要求（constraint 數量一旦超過 rectangle 等級的樣本就必須具備）：

- 列出 Sketch 內全部 constraint：ConstraintId、type、參與的 entity、driving/driven、status。
- 可依 status 篩出 conflicting / redundant 的那幾條——這是 §8「不能只顯示 Solver failed」的落地手段。
- 刪除 constraint 是一筆 semantic transaction（§15）。

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

視覺區分（參考行為）：driving dimension 以黑色顯示，driven / reference dimension 以淺灰顯示。EP3D 沿用此語意，但依 A06 另加非顏色標記（例如括號或 `(REF)` 前綴）。

未來 expression 可支援 `Width`、`Width/2`、`BaseWidth + 20 mm`，但不可阻塞 M5。完整運算式文法、單位規則與變數參照見 **§42**；變數本身的型別與作用域見 **§43**。

### 7.1 選取組合 → dimension 型別（查證後）

參考模型的 Dimension 是**單一工具**，依選取內容決定產生哪種 dimension：

| 選取 | 產生 |
|---|---|
| 單一線 | 長度 |
| 兩條平行線 | 線距（同時隱含平行關係） |
| 兩點 | 直線距離（最短路徑）或水平／垂直距離 |
| 圓 | 直徑 |
| 弧 | 半徑 |
| 兩條夾角線 | 角度 |
| 弧 + 其兩端點 | 弧長 |
| spline 上兩點 | 點間距離 |
| sketch 幾何 + 平面 | 到平面的距離 |
| 圓或點 + 建構線 | 中心線距離（拖過該線可切換） |

EP3D 判讀：EP3D 目前以「型別各自一個命令」實作（Distance / Length / Radius / Diameter / Angle）。單一工具的做法可發現性更高但需要選取型別推斷器。**兩者皆可，須擇一並記入 §32 的 intentional difference**；若維持多命令，至少要讓錯誤的選取給出「這個選取應該用哪個命令」的提示。

### 7.2 Over-defining 時的行為（重要裁決點）

參考行為：

```text
新增的 dimension 預設是 driving，顯示為黑色
右鍵 dimension 值 → Change to driving/driven dimension  可手動切換
若新增的 dimension 會使 sketch over-defined
   → 系統「自動」把它轉成 driven，而不是報錯
driven dimension 顯示為淺灰且不可直接編輯
```

這與 §8 的 conflicting 狀態是**互補而非重複**的機制：自動降級處理的是「多餘但相容」（redundant），真正矛盾的才進入 conflicting。

EP3D 必須明確裁決三種可能，並記入 §32：

| 方案 | 行為 | 代價 |
|---|---|---|
| A 沿用參考模型 | 自動降級為 driven | 使用者可能沒注意到自己的 dimension 不再驅動幾何 |
| B 拒絕並說明 | 報錯並指出衝突的既有約束 | 操作被打斷，但意圖明確 |
| C 詢問 | 彈出「設為參考 / 取消」 | 最清楚，但打斷流程 |

建議 **A + 強制提示**：自動降級，但在 status bar 與該 dimension 上明確標示「已轉為參考尺寸」，並讓這次降級可 undo（§15）。純 A 在無提示下等於靜默改變使用者意圖，違反 §26 「不可 silent guess」的同一條精神。

### 7.3 Expression 入口語法

參考模型在 dimension 欄位中以 `=#` 開頭引用變數（例如 `=#d`），`#` 觸發自動完成。EP3D 的欄位語法見 §42.1；若採用不同的前綴，必須在 §45 記錄映射。

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

### 8.1 參考模型的視覺編碼

```text
幾何顏色         藍 = under-constrained
                 黑 = fully constrained
                 紅 = 有問題（over-constrained / conflicting）

Constraint 圖示  灰底黑圖 = 正常
                 紅底白圖 = 有問題

Dimension        黑   = driving
                 淺灰 = driven / reference
```

### 8.2 EP3D 的落地要求

1. **語意沿用、通道加倍**（A06）：同樣的紅/藍/黑語意，但 status bar 必須同時有文字（`Under constrained — DOF 3`），Sketch 節點必須有 icon。
2. **可定位**：conflicting 狀態必須能列出涉及的 ConstraintId 集合，點選時高亮對應幾何。
3. **可區分 redundant 與 conflicting**：多餘但相容的約束與互相矛盾的約束是不同診斷，不可合併成同一訊息。
4. **numerical failure 要能與 conflicting 區分**：前者是求解器收斂問題，後者是模型問題；混為一談會讓使用者改錯地方。

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

### 9.1 Feature list 的內容組成（參考行為）

```text
[Configuration 下拉]          ← 若該 Part Studio 有 configuration（§17）
[feature 數量]  [搜尋框]
Origin / Default planes
Feature 1 … Feature N          ← 各帶 tool icon 與狀態
──── Rollback bar ────         ← §16
──── 分隔列 ────
Parts list                     ← part / surface / mesh / composite
```

### 9.2 Feature 的操作集（參考行為 → EP3D 需求）

| 操作 | 參考行為 | EP3D 要求 |
|---|---|---|
| Rename | 直接在清單內改名 | 名稱是顯示屬性，**不是 identity**（A03）；改名不觸發 recompute |
| Suppress | 由 context menu 抑制單一 feature | §17；抑制後 chain 需跨過該 feature 接合 |
| Dynamic suppression | 以邏輯運算式決定何時抑制 | 後期；需 §42 的運算式求值器與 boolean 型別 |
| Hide / Show | 眼睛圖示切換可見性 | 純 presentation，不得進 Core（A02） |
| Show dependencies | 顯示該 feature 的 parent / child | EP3D 已有 dependency graph，這只是 UI 查詢；**優先度高**，它是 §32 驗證的直接工具 |
| Expand / Collapse | 展開巢狀內容 | UI |
| Reorder | 拖曳重排，且會參數式重算 | 需 §16 的 evaluation position 語意先穩定；重排必須驗證每個輸入仍排在自己之前 |
| Folder | 選取多項加入資料夾，可巢狀、可整組抑制、可 unpack | 後期；資料夾是組織單位，**不得改變 dependency 順序語意** |

### 9.3 錯誤過濾

參考行為：Feature list 左上角在有錯誤時出現圖示，點擊即可只列出出錯的 feature。EP3D 應提供同等的「只看有診斷的節點」過濾，理由同 §8.2 的可定位性。

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

補充要求：

1. **編輯中的 feature 會被回捲到自己的位置**：參考模型在編輯歷史中的 feature 時，會把評估位置暫時移到該 feature，讓 preview 顯示「那一步當時的樣子」，而非整條 chain 的末端。這與 §16 是同一套機制，不應各做一份。
2. **Preview 期間的錯誤是 preview 的錯誤**：預覽失敗不得把 document 留在失敗狀態，Cancel 之後所有診斷都要一併消失。
3. **未完成的選取要能被表達**：參考模型以「缺少選取」的黃色三角、「定義不完整」的提示區分「這個 feature 壞了」與「你還沒填完」。EP3D 的 dialog 必須能表達 *incomplete*，且 incomplete ≠ error。

### 10.1 Feature dialog 的解剖（查證後）

```text
標題列顏色    紅 = 輸入不完整或有錯，擋住 commit
              黑 = 可以 commit
藍底欄位      必須從 3D 畫面選取（sketch / face / surface）
白底欄位      鍵盤輸入，可含單位與運算式（§42）
下拉選單      預設選項
勾選方塊      選用修飾（例如 Draft、Second end position）
方向切換      反轉 feature 方向
Preview 滑桿  0% ~ 100% 不透明度，在「施加前」與「施加後」之間漸變
開啟時焦點    第一個藍色選取欄位 + 第一個白色文字欄位同時啟用
✓ 或 Enter    commit
✕ 或 Esc      放棄
右鍵欄位      清除失效參考、移除多筆選取
```

編輯既有 feature 時：

```text
模型自動回捲到「該 feature 建立當時」的狀態，後續 feature 隱藏
另有 Final 按鈕可在編輯中切回看完整結果（最後一個 feature 除外）
新 feature 插入在 rollback 位置
```

EP3D 判讀：

1. **欄位顏色即輸入來源**（藍=畫面選取、白=鍵盤）是低成本高回報的設計，且與 A06 相容——顏色在此不是狀態，而是「該從哪裡輸入」的提示，仍應輔以 placeholder 文字。
2. **「Final」是 §16 rollback 的第二個消費者**。§10.1 與 §16 共用同一個 evaluation position 機制，這裡再次確認 M9 把兩者做成一套是正確的。
3. **標題列顏色 = §44 的 Incomplete / Error 兩態合併呈現**。EP3D 若沿用，仍須在 tooltip 區分兩者（§44.2.1）。

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

每個 feature 的參數集是獨立的規格工作。Extrude 的完整參數集見 §12，其餘 feature 在各自 milestone SPEC 中以同樣的粒度展開（operation / extent / direction / scope / 附加修飾）。

### 11.1 Pattern 家族（查證後）

參考模型的 pattern 是**四個並列的 feature**，不是一個帶模式參數的 feature：

```text
Linear pattern    沿一個方向產生實例，或以兩個方向產生陣列
Circular pattern  繞一軸產生實例
Curve pattern     沿曲線或路徑產生實例
Mirror            對一個鏡射平面反射
```

共通參數：

```text
Skip instances   略過個別實例（Mirror 沒有此選項）
                 用於避開幾何衝突或刻意排除某幾個
Merge scope      決定哪些既有 part 受此 pattern 影響
                 不在 scope 內的 part 完全不受影響
                 （例如：被 pattern 的 pocket 要切哪些 part）
```

EP3D 判讀：

1. **Merge scope 是 pattern 與 boolean 共用的概念**（§12.1 也有）。它應該是一個可重用的「作用範圍」型別，而不是在每個 feature 各寫一次。
2. **Skip instances 需要穩定的實例索引**。若以「第 3 個實例」記錄，改變數量就會跳到別的實例上——這是 A03 在 pattern 上的具體形式。建議以 pattern 座標（i, j）而非線性序號記錄。
3. Pattern 同時存在於 sketch 層（§4.1.1）與 feature 層，兩者語意不同：sketch pattern 產生 entity + constraint，feature pattern 產生幾何實例。不可共用實作。

### 11.2 相鄰工具族：納入功能全集，排程在後

依 A07，掃描確認的另外兩個完整工具族**屬於 EP3D 的目標功能範圍**，只是排在核心 feature 之後（清單與排程見 §52.1）：

```text
Surface modeling  Thicken / Enclose / Face blend / Delete face / Move face /
                  Replace face / Offset surface / Boundary surface / Fill /
                  Move boundary / Ruled surface / Mutual trim / Constrained surface
Sheet metal       Sheet metal model（Convert / Extrude / Thicken）/ Flange / Hem /
                  Tab / Bend / Form / Loft / Make joint / Corner / Bend relief /
                  Modify joint / Corner break / flat view / Finish sheet metal model
                  （含 K factor、bend allowance、bend deduction 等展開計算）
```

列出它們的目的是**界定 §11「之後」那一格到底有多大**——避免把「advanced surface tools」誤估成一兩個 feature 的工作量。實際上這兩族合計約 27 個 feature，規模與 M8 全部核心 feature 相當甚至更大。

Sheet metal 另有一項架構意涵：它需要**同一個 part 的兩種表示（成形 3D 與展開平面）同步存在並互相編輯**。這不是又一個 feature，而是對 §9 feature chain 的擴充，必須在排程時單獨評估。

## 12. Generalized Extrude Direction

Pad 未來可演進成 generalized Extrude：

```text
Operation:
New / Add / Remove / Intersect

Extent:
Blind / Symmetric / Through All / Up To Face
```

M5 不需為此擴大 scope。

### 12.1 參考模型的完整 Extrude 參數集

```text
Operation（boolean）
  New        產生新的 part / surface
  Add        加料到既有幾何
  Remove     從既有 part 減料
  Intersect  只保留相交的部分

End condition / Extent
  Blind          指定距離
  Through all    貫穿全部
  Up to next     到下一個遇到的幾何
  Up to face     到選定面（面視為無限延伸的平面）
  Up to part     到下一個 part 或 surface
  Up to vertex   到選定的點或 Mate Connector

Direction
  沿指定軸或 Mate Connector 的 Z 軸
  Opposite direction（反向）

Modifiers
  Offset            可用於 up-to-next / up-to-face / up-to-part / up-to-vertex，單向或雙向
  Starting offset   起始位置離開 sketch plane（Blind 或以 entity 決定）
  Symmetric         雙向等距（僅 Blind 與 Through all）
  Second end position  兩個方向各自不同的 extent（非對稱）
  Draft             拔模角，以 sketch plane 為中性面

Creation type
  Solid / Surface / Thin

Merge scope
  新幾何要與哪些既有 part 合併；可選 merge with all
```

### 12.2 EP3D 分期

| 階段 | 納入 |
|---|---|
| 已有（M8） | Blind、New/Add/Remove（Pad / Pocket 兩個 feature 型別） |
| 下一步 | Symmetric、Through all、Opposite direction、Starting offset |
| 需要 3D 選取穩定後 | Up to face / part / vertex / next（依賴 §13 的 persistent semantic reference——這是 A03 的硬邊界，不可用 transient face index 實作） |
| 需要 boolean 成熟後 | Intersect、Merge scope |
| 後期 | Draft、Thin、Surface |

**架構註記**：`Up to *` 系列會把 feature 的輸入從「數值」擴充成「幾何參考」。在 stable topological naming 之前不得實作，否則每次 recompute 都可能指到不同的面。這是 EP3D 目前最容易被 UI 需求推著違反 A03 的地方。

### 12.3 Pad 與 generalized Extrude 的關係

EP3D 目前有 Pad 與 Pocket 兩個獨立 feature 型別；參考模型只有一個 Extrude 加上 operation 參數。兩種做法都成立，但必須**明確選一個並記錄為 intentional difference**（§32）：

- 保留兩型別：UI 較直覺，但 Add/Remove 的切換需要換 feature 型別，等同刪除重建，會斷掉下游參考。
- 合併為一型別 + operation 參數：切換不斷參考，但需要一次遷移。

建議在引入 Intersect 之前決定，因為 Intersect 會使型別數量從 2 變 3。

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

補充要求：

1. **三處選取是同一個 selection set**：在 tree 選取要在 viewer 高亮，反之亦然；dialog 開啟時的選取要進入 dialog 的欄位而不是覆蓋全域選取。
2. **選取欄位有型別**：feature dialog 的每個選取欄位只接受特定 level（例如 Fillet 只吃 edge），型別不符要在放入前就拒絕並說明原因，而不是在 recompute 時才報錯。
3. **失效選取要可見**：參考模型會把失效的選取在清單與畫布上一起標紅。EP3D 必須能表達「這個參考指向的東西不見了」，並且**保留原本的參考字串**供使用者判讀，不可靜默丟棄。

### 13.1 查證後補充

```text
選取是 toggle          再次點擊即取消選取，多選不需要按修飾鍵
Space                  取消全部選取
欄位型別以顏色表示      藍底 = 需從畫面選取；白底 = 鍵盤輸入（§10.1）
Alt + Click            穿透透明物件選取
右鍵欄位                清除失效參考、批次移除選取
```

EP3D 判讀：

1. **「多選不需修飾鍵」是一個明確的互動決策**，與多數桌面 CAD（需 Ctrl）不同。EP3D 若沿用，必須全域一致；若不沿用，記入 §32。兩者都可以，混用不行。
2. **穿透透明物件選取**在 assembly 與 §14 preselection 同時需要，屬 viewer 能力而非 Core 能力（A02）。

## 14. Preselection

未來：

```text
Hover → Pre-highlight
Click → Selected
```

兩者視覺必須不同，供 Sketch、Fillet edge、Face、Connector 選取使用。

### 14.1 Hover 也會揭露「潛在的可選點」

參考行為：在 Mate 指令進行中 hover 到面或邊時，會顯示 **implicit mate connector 候選點**，出現位置包括：

```text
面／sketch profile 的形心
邊的中點
頂點
圓孔的圓心
被減料形成的負空間中心
錐面的 virtual sharp（虛擬尖點）
```

這對 EP3D 有兩個意義：

1. Preselection 不只是「把滑鼠下的東西變色」，它是 **context-sensitive 候選點產生器**。§18 的 connector 選取需要它。
2. 這些候選點是**從幾何推導出來的**，不是使用者建立的物件。EP3D 必須區分「推導候選點」與「已建立的 Connector」——前者不進 document，後者才有 ObjectId。

## 15. Undo / Redo

逐步建立 semantic transaction：

```text
User Command
→ Document Transaction
→ Undo Record
```

Create Sketch、change dimension、create Pad、delete feature、change constraint 都應成為 atomic Undo 操作。不得把 raw OCCT state 當 semantic undo history。

補充要求：

1. **presentation-only 的操作不進 undo stack**：hide/show、視角、選取不是 document 變更。參考模型把可見性當作視圖狀態，EP3D 應一致。
2. **rollback 位置的移動是否進 undo stack 需明確裁決**：它不改變 document 內容，但改變使用者所見。建議不進 undo stack，但要記錄為 intentional difference（§32）。
3. **一個使用者動作 = 一筆 undo**：拖曳 dimension 連續產生的中間值必須合併成一筆。

## 16. Feature Rollback

未來 Feature List 支援 rollback/evaluation position，用於查看 intermediate state、插入/修改歷史 feature。只有 dependency/recompute semantics 足夠穩定後才實作。

### 16.1 參考行為

```text
Rollback bar 位於 Feature list 中，可上下拖曳
Bar 之下的所有 feature 暫時被抑制（temporarily suppressed）
右鍵 feature → "Roll to here"      精準回捲到該步
右鍵 bar    → "Roll to end"        回到末端
選取 bar 後按 ↑ / ↓                 一次前進／後退一步
新建立的 feature 插入在 rollback 位置
```

### 16.2 EP3D 語意要求

1. **rollback 是 evaluation position，不是刪除**：被跳過的 feature 保持完整定義與 ObjectId，只是不參與本次求值。
2. **rollback ≠ suppression**：兩者都讓 feature 不參與求值，但 suppression 是 **document 狀態**（會被序列化、會被 configuration 驅動），rollback 是 **檢視狀態**。兩者的 ComputeState 必須可區分，否則存檔後會分不清使用者當初是抑制了它、還是只是回捲過。
3. **UI 形式是自由的**：M9_SPEC 已記錄 EP3D 不做可拖曳的 bar widget，而是「rollback position 是一個值 + 移動它的命令」。這是 intentional difference，不是缺漏。
4. **在 rollback 位置插入 feature** 是 §9.2 reorder 的孿生問題，兩者共用「輸入必須排在自己之前」的驗證。

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

### 17.1 參考模型的 Configuration 詞彙

必須照抄的是**結構**，不是 UI：

```text
Input        一個可配置的維度（在面板中是一張可收合的表）
Option       Input 的一個可選值（表中的一列）
Parameter    被該 Input 驅動的具體屬性（表中的一欄）
Option value 某個 Option 對某個 Parameter 指定的值（儲存格）
```

可被配置的對象：

```text
feature 參數（例如 extrude 深度）
feature 的 suppression 狀態
自訂 feature 的參數
part number / color / material
Variable Studio 的變數（§43）
part properties
```

進階：**visibility condition**——某個 Input 只在其他 Input 取特定值時才出現。

Input 共有三種型別（查證後）：

| Input 型別 | 行為 | EP3D 對應 |
|---|---|---|
| **List** | 具名選項（Short / Medium / Tall），插入時以下拉選單呈現 | 列舉型變體，落地順序 1 |
| **Checkbox** | 布林開關，典型用途是開關某個 feature（等同抑制／解除抑制） | 需要 §17 的 suppression 語意，落地順序 2 |
| **Configuration variable** | 具名變數（`#name`），型別為 Length / Angle / Integer / Real / Text，透過運算式散佈到多個 feature 參數 | 需要 §43 的具名 Parameter + §42 的運算式，落地順序 1 的進階形式 |

表格機制：configuration 為列、被驅動的參數為欄；在 feature dialog 中選定某參數要被配置時，該欄位以**黃色虛線框**標示，並在表中新增一欄；欄標題是「feature 名 + 參數名」的階層組合。

Assembly configuration 的範圍**明顯小於** Part Studio：只能配置 **mate（不含 mate connector）、instance、pattern**，不能改動幾何 feature。EP3D 的 M11/M15 排程應以此為準——assembly 變體不需要等 assembly 幾何 feature，因為參考模型根本沒有這一層。

下游行為：配置過的物件被插入到其他脈絡時，**configuration input 會成為插入對話框中的選項**。

### 17.2 EP3D 的落地順序

| 順序 | 內容 | 前置 |
|---|---|---|
| 1 | Parameter 變體（Small/Medium/Large 驅動數值） | Parameter ObjectId 已穩定 |
| 2 | Suppression 變體 | §17 的 suppression 語意（M9） |
| 3 | Property 變體（material、part number） | Part properties 模型 |
| 4 | Assembly 變體（instance、mate value） | M11 |
| 5 | Visibility condition | §42 的運算式求值器 |

**架構紅線**：configuration 絕不以「複製整份 document」實作。它是同一個 semantic model 在不同 input 下的求值結果——這也是為什麼它必須排在 recompute 語意穩定之後。

### 17.3 Dynamic suppression

參考模型允許以邏輯運算式決定 feature 何時被抑制（例如 `#length > 50`）。這是 §17 與 §42 的交集：需要運算式求值器、需要 boolean 結果型別、需要把「這個 feature 的抑制狀態依賴某個 Parameter」也畫進 dependency graph。列為後期，但**架構上必須預留**：suppression 不能被硬編為布林欄位，要能是「常數或運算式」。

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

### 18.1 Implicit vs Explicit（參考行為）

```text
Explicit connector
  以獨立工具建立，在 Feature list 中有自己的項目
  在 Part Studio 中建立時 → 該 Part 的所有 assembly instance 都能重複使用
  可被多個 mate 使用，但在清單中只列一次

Implicit connector
  在 mate 對話框進行中就地產生
  巢狀顯示在使用它的那個 mate 之下
```

EP3D 要求：兩者都必須有 ObjectId。implicit 的差別只在**建立時機與清單呈現位置**，不在於它是不是一等公民。若 implicit connector 沒有穩定 identity，mate 就會建立在無法重新解析的東西上，直接違反 A03。

### 18.2 Connector 的幾何定義與調整

```text
Z 軸 = primary axis
  同時是 mate 對位的主軸，也可被其他 feature 當成可選取的軸

Realign
  以 primary（與可選的 secondary）axis entity 重新定向

Flip primary axis
  primary 軸反轉 180°

Reorient secondary axis
  secondary 軸以 90° 為單位在象限間旋轉

Offset
  X / Y / Z 平移

Rotation
  指定角度旋轉
```

EP3D 的 ReferenceFrame 必須能表達以上全部，因為這正是機械對位所需的最小完整集合：**一個原點 + 一個主軸 + 一個次軸 + 平移旋轉修正**。USER / TOOL coordinate system 用的是同一組欄位（§35.3）。

### 18.3 Owner 與可見性

- Connector 屬於 **Part definition** 或 **Assembly**，兩者是不同的 owner。Part 上的 connector 可被所有 instance 重用（§21）；Assembly 上的 connector 只屬於該 assembly。
- 可見性在 Part Studio 與 Assembly 兩個脈絡中**各自獨立**——同一個 connector 可以在零件裡藏起來、在組立裡顯示。這是 presentation 狀態，依 A02 不得進 Core。

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

### 20.1 完整 mate 型別對照

| Mate | 保留的自由度 | 備註 |
|---|---|---|
| Fastened | 0 | 完全固定六個自由度 |
| Revolute | 1 rotation | 繞單一軸旋轉 |
| Slider | 1 translation | 沿主軸平移 |
| Cylindrical | 1 rotation + 1 translation | 同軸旋轉並滑動 |
| Planar | 2 translation + 1 rotation | 在平面內移動 |
| Ball | 3 rotation | 球接頭 |
| Pin slot | 1 rotation + 1 translation | 旋轉 + 沿槽方向平移，兩者互相獨立 |
| Parallel | — | 對齊型 mate，只約束方向 |
| Tangent | — | 維持相切；**不使用 mate connector** |
| Width | — | 置中／控制間距 |
| Group | — | 把多個 instance 綁成相對固定的一組 |

### 20.2 Mate value、offset 與 limit

```text
Mate value    可指定值的 mate = 除了 Ball / Fastened / Tangent / Width 之外的全部
Offset        Planar / Slider / Revolute / Pin slot / Fastened 支援沿軸平移或旋轉一個設定值
Limit         依該 mate 剩餘的自由度提供上下限
              畫面上以「兩端帶橫槓的虛線」呈現界限
Animation     可預覽受約束後的運動
```

### 20.3 狀態與 DOF 回報

```text
Mate 指示器   藍/白 = 正常
              灰     = 已抑制
              紅     = 有問題

Instance      仍有自由度的 instance 在清單中顯示三軸（triad）圖示
              tooltip 列出尚未被約束的自由度
```

EP3D 要求：**DOF 回報必須逐 instance、逐自由度可讀**，理由與 §8 相同——「這個組立還沒定位完」是無法行動的訊息，「Arm 這個 instance 還有 1 個繞 Z 的旋轉自由度」才是。

### 20.4 EP3D 分期

| 階段 | 內容 |
|---|---|
| M11 | Fastened、Revolute、Slider + connector 對位 + DOF 計數 |
| M11 後段 | Cylindrical、Planar、Group |
| M12 | Limits、mate value、kinematic drag、Ball、Pin slot |
| 之後 | Parallel、Tangent、Width，以及 relation 類（gear、rack-and-pinion、screw） |

Relation 類（齒輪比、齒條、螺紋）在參考模型中是與 mate 分離的另一種物件，EP3D 也應如此建模：**mate 約束自由度，relation 耦合兩個既有自由度**。把 relation 硬塞進 mate 型別會讓 DOF 計算失去意義。

### 20.5 Relation 完整清單（查證後，共 4 種）

| Relation | 輸入 | 耦合內容 | 參數 |
|---|---|---|---|
| **Gear** | 兩個具 revolute 自由度的 mate | 兩者的角位移成固定比 | 齒輪比（= 主動齒數 / 從動齒數）、反向 |
| **Rack and pinion** | 一個 revolute mate + 一個 slider mate | 旋轉 ↔ 直線位移 | 每轉位移量、反向 |
| **Screw** | **單一** cylindrical mate | 同一個 mate 內的旋轉 ↔ 平移 | 每轉位移量（含單位）、反向 |
| **Linear** | 兩個具直線自由度的 mate | 兩者的直線位移成固定比 | 比值、反向 |

沒有 cable relation；**Tangent mate 不支援任何 relation**。

EP3D 判讀：

1. **Relation 的輸入是 mate，不是 instance**。這使 relation 必然排在 mate 之後，且 relation 必須持有 MateId（穩定 identity，A03），不能持有 instance 對。
2. **Screw 只吃單一 mate** 是一個結構性線索：relation 不必然是「兩個東西的關係」，也可以是「一個 mate 內兩個自由度的耦合」。EP3D 的 relation 模型要能表達兩種 arity，不可硬寫成二元關係。
3. Relation 讓有效 DOF 下降，但 mate 本身的 DOF 不變。§20.3 的 DOF 回報必須把「mate 提供的 DOF」與「relation 耦合後的實際 DOF」分開顯示，否則使用者看不懂為什麼還有自由度卻動不了。

### 20.6 Snap mode

參考行為：把一個 instance 的 mate connector 拖到另一個 instance 的 mate connector 上，即建立 mate。拖曳時被拖的 instance 變半透明以便看見目標連接點；目標的候選連接點在接近時才顯示（同 §14.1）。放開後**開啟 mate 對話框讓使用者選型別與對齊**，而不是自動決定 mate 型別。

EP3D 判讀：這是 §14 preselection 與 §18 connector 的組合應用，是 assembly 可用性的關鍵互動；但它不引入任何新語意，屬純 UI，可安排在 M11 之後而不影響架構。

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

引申規則：**mate 應該盡量指向 connector，而不是指向 face/edge**。connector 有 ObjectId，face 沒有。這條規則同時解釋了 §12.2 為何 `Up to face` 要等 topological naming——兩者是同一個 identity 問題的兩個面向。

## 22. Motion Limits

未來：

```text
Revolute: -90° ≤ θ ≤ +90°
Slider:    0 mm ≤ d ≤ 200 mm
```

初期只做 kinematics，不需要 mass/dynamics simulation。

限制的呈現沿用 §20.2：界限在 3D 視圖中可見，而不是只存在於對話框的數字欄位裡。超出界限的拖曳應被夾住（clamp）而非報錯。

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

### 24.1 View 型別全集（參考模型）

```text
Projected view          正投影視圖
Auxiliary view          自母視圖選定邊向外折出 90° 的斜視圖
Section view            剖視圖
Aligned section view    對齊母視圖的剖視圖
Broken-out section view 以封閉輪廓（spline）局部剖開
Detail view             放大局部
Break view              中斷視圖（省略等截面段）
Crop view               裁掉視圖的一部分
Flat pattern view       鈑金展開圖
```

### 24.2 View 屬性

```text
Scale             N:N 或 N/N；projected view 預設繼承母視圖比例
Tangent edges     隱藏 / 實線 / 假想線（phantom）
View label        可選；detail 與 section 會自動加上標籤，可設前後綴
Rotation          0–360°；若該視圖有母子關係則停用
Simplification    auto / 絕對值 / 比例，用來濾除過小的細節
Render mode       品質優先 / 效能優先
```

### 24.3 關聯性（associativity）

```text
同一元件的所有視圖鎖定到同一個版本
母子關係保留（projected 繼承母視圖比例，除非另行覆寫）
模型變更 → 視圖自動更新
相關視圖在不同 sheet 上時，以方向箭頭標示
```

EP3D 要求：drawing view 是 **3D model 的 derived view**，其中的幾何沒有獨立 identity；而 annotation dimension 有自己的 ObjectId 且指向 model 的 semantic reference。這條分界正是 §24 開頭那個區分的技術意涵——也是 §26 反向重建能否成立的前提：重建的目標是把 annotation 轉成 driving，兩者的 identity 模型必須從一開始就分開。

### 24.4 Drawing dimension 工具與樣式（查證後）

放置方式：hover 幾何時出現橘色 **snap point**，形狀即語意——

```text
方形 = 端點    三角 = 中點    菱形 = 四分點    圓形 = 圓心
```

尺寸型別：linear（點對點、線對線）、angular、radial / diameter（可互相切換）、arc length、ordinate（X/Y 一組，選定基準點為 0,0，整組連動）、chamfer（可設 45° 樣式為 Note 或 Dimension）、maximum/minimum、hole / thread callout。另有 centerline（由兩條邊、兩個同心弧，或單一圓柱／圓錐輪廓邊產生）與 centermark。

**Dimension palette**（逐一尺寸的樣式覆寫）：

```text
上方文字 / 下方文字、前綴 / 後綴、
精度 0–8 位小數、單位、符號、
雙重單位（dual dimensions）、
檢驗尺寸外框（inspection framing）、
半徑／直徑切換
```

公差顯示型別：

```text
None、Symmetrical、Deviation、Limits、Basic、
MIN、MAX、Fit、Fit with tolerance、Fit (tolerance only)
```

**Dangling dimension**：當尺寸與其參考幾何的關聯斷開時，成為「懸空尺寸」。EP3D 必須有對應狀態——這是 §44 的 `Downstream` 在 drawing 層的形式，且**不得靜默刪除**（同 §13.3）。

### 24.5 Annotation 全集（查證後）

```text
Note                     文字註記
Callout（balloon）        含引線的泡泡，可附著幾何或置於空白處
                         可與 dimension / weld / surface finish / datum 群組
Datum                    基準符號
Geometric tolerance      幾何公差（見下）
Surface finish           表面粗糙度符號
Weld symbol              熔接符號
Hole / thread callout    孔與螺紋標註
Bend note                鈑金彎折註記
Inspection item          檢驗項目
Image / decal            影像
Tables                   BOM table、cut list table、revision table、custom table
```

**Geometric tolerance 的 feature control frame 組成**：

```text
上方補充文字
幾何特性符號 ── Location:    Position / Concentricity(coaxiality) / Symmetry
                Orientation: Parallelism / Perpendicularity / Angularity
                Form:        Cylindricity / Flatness / Circularity / Straightness / Square
                Profile:     Profile of surface / Profile of line
                Runout:      Circular runout / Total runout
公差值（可含 Ø、SØ、R、SR、CR）
最多三個 datum reference
前綴 / 後綴
修飾符 MMC / LMC / RFS
條件符號 Free state / Tangent plane / Projected tolerance zone
下方補充文字
```

可堆疊多個 frame，composite 符號可跨連續 frame。可附著於：邊、孔、尺寸（有無引線皆可）、尺寸延伸線、延伸線、表面區域，或不附著任何幾何。

EP3D 判讀：GD&T 不是「一個標註型別」，而是**一個帶結構的小型語法**。它必須有自己的資料模型（符號 + 值 + datum 列 + 修飾符），不能用自由文字實作，否則日後無法檢核也無法輸出到 MBD 或檢驗表。

### 24.6 Drawing 層級屬性

可在整份 drawing 設定預設值的類別（個別實體仍可覆寫）：

```text
Units and Precision、Dimensions、Annotations、Views、
Construction Geometry、Formats、Tables、Inspection
```

另有 **Format painter**（把一個尺寸／標註的格式複製到其他標註）、**Lock drawing properties**（鎖定避免誤改）、以及把整組設定存成**自訂範本**。

EP3D 判讀：「文件層預設 → 實體層覆寫 → 格式刷 → 存成範本」是一條完整的樣式繼承鏈。若一開始就把樣式硬寫在每個 annotation 上，日後補繼承鏈需要資料遷移。建議在 M14 起始就以「樣式參照 + 局部覆寫」建模。

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

### 25.1 參考模型的 DXF/DWG 匯入行為（查證後）

```text
匯入前必須先選單位     選了檔案對話框就關閉，單位不能事後補
可選「使用檔案原點」    勾選 → 以 DXF 原點對齊 Part Studio 原點
                        不勾 → 幾何置中於 Part Studio 原點
建議插入到空的 sketch
插入後第一個標注的尺寸  會「縮放整個 sketch」，而不是只約束一條邊
檔案需已匯入文件內      或由共用文件連結；亦可在對話框中直接上傳
```

支援的 2D 格式版本：DWG ≤ 2018、DXF ≤ 2013／2018、DWT 2013／2018。

EP3D 判讀：

1. **「第一個尺寸縮放整份 sketch」是一個明確且大膽的設計決策**，用來處理來源圖檔比例不明的情況。EP3D 的 M6/M7 面對的正是這個問題，但 EP3D 的立場應該不同：既然 §25 已要求保留 provenance 與 import transform，就應該**把比例當成一個顯式的 import 參數**，而不是靠「第一個尺寸」隱式推定。隱式推定會讓 §26 的重建無法解釋自己為何得到某個尺寸。
2. **單位必須在匯入前決定**這點應沿用——事後改單位等同改變所有幾何，是重新匯入而非編輯。
3. 「建議插入空 sketch」在參考模型是建議，在 EP3D 應是**可檢查的前置條件**：若目標 sketch 非空，必須明確告知會發生什麼。

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

重建的目標型別集合就是 §6.1 的表——重建器不得產生表外的約束型別，也不得產生「只在重建器內部有意義」的私有約束。重建結果要能與手繪 Sketch 完全等價地被編輯，否則 §35.1 的差異化主張不成立。

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

**依 A07，本節是建議而非規範。** 版面、面板位置、命令分組、圖示與手勢完全自由重畫；上圖僅表達「有哪些資訊必須有地方放」。

必須有容身之處的資訊（這才是規範的部分）：

```text
feature 歷史與其狀態      §9
3D / sketch 檢視           §13、§14
目前編輯對象的參數與診斷    §10.1、§44
選取內容、DOF、座標         §8、§20.3
rollback 位置              §16
Parts / Instances 清單      §9.1、§19
```

Feature list 若以樹狀呈現，應具備 §9.1 的組成要素：搜尋、計數、錯誤過濾入口、rollback 位置指示、Parts 區。若改以其他形式呈現，需自行確認以上資訊仍可達。

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

補充：參考模型大量使用**單鍵快捷鍵**（例如 drawing 中 `p` 觸發 projected view），並集中維護一份快捷鍵表。EP3D 應同樣建立**單一 shortcut registry**，理由是：

1. 衝突偵測要能自動化，不能靠人記。
2. 快捷鍵表本身就是可發現性的一部分（說明頁要能從 registry 生成）。
3. 各 mode（§29）可有各自的鍵位，registry 需以 mode 為維度。

單鍵快捷鍵的引入不早於 §29 的 mode 概念落實，否則會與文字輸入衝突。

### 30.1 參考模型的快捷鍵規模（查證後）

實際清單分為 General / Part Studio / Assembly / 3D View / Sketch / Feature Studio / Drawings 七類，合計約 110 組。其中值得注意的結構性事實：

```text
Sketch 類幾乎全是單鍵      L=Line、C=Circle、R/G=Rectangle、A=Arc、D=Dimension、
                           H=Horizontal、V=Vertical、T=Tangent、E=Equal、
                           M=Trim、X=Extend、O=Offset、U=Use、Q=Construction …
同一鍵在不同 mode 意義不同   Sketch 的 A = 3 point arc；General 的 A = flip primary axis
                           Assembly 的 M = Fasten mate；Sketch 的 M = Trim
Shift+Enter                commit 後再開一個同命令的對話框（連續建立）
Space                      取消選取
Shift+/                    叫出快捷鍵清單本身
```

EP3D 判讀（介面自由，但這三點是機制而非外觀，仍建議沿用）：

1. **同鍵跨 mode 重用是刻意的**，前提是 mode 邊界清楚（§29）。這證明 §30 的 registry 必須以 mode 為維度，而不是全域唯一。
2. **「commit 後再開一次」值得納入**：連續建立同類 feature 是機械設計的常態，這比多按幾次工具鈕省下大量操作。
3. **快捷鍵清單要能從程式內叫出**，且由 registry 生成——否則 110 組永遠不會被記住，也不會被維護。

實際鍵位配置依 A07 完全自訂，不需與參考模型一致。

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

**填表規則**（避免此表淪為形式）：

- `Reference behavior` 必須可回溯到 §46 的來源，否則改標為 EP3D 自訂（A05）。
- `Intentional difference` 必須寫出**理由**，不能只寫「不做」。沒有理由的差異是缺漏，不是決策。
- `Validation result` 若涉及 UI，必須註明是 agent 自動檢查還是 owner 手動驗證——兩者不可互相冒充（A04）。

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

**跨 milestone 的架構前置條件**（違反其一就會逼出 A03 的破口）：

```text
stable topological naming   →  §12.2 Up-to-*、§13 face/edge 選取、§20 非 connector mate
ReferenceFrame（M10）        →  §5 Sketch-on-frame、§18 Connector、§35.3 USER/TOOL
運算式求值器（§42）           →  §7 expression、§17.3 dynamic suppression、§43 變數
evaluation position（M9）    →  §10 編輯期預覽、§16 rollback、§9.2 reorder
```

### 33.1 A07 之後的功能全集排程

A07 把「參考模型有的 CAD 功能都要有」定為目標，因此 Configurations 之後不是終點。掃描後可見的完整功能版圖，依相依性排序。

**本表刻意不編號**：repo 的 M 編號是實作進度（已到 M17.x，且焊進 ADR 與測試名稱），與本表的主題若共用 M 字母，同一個編號會有兩種讀法。實作排程見 `EP3D_Onshape_Parity_Plan.md`。

| 主題 | 依賴 |
|---|---|
| Sketch / DXF / 重建 / 核心 feature / 歷史編輯 | （已完成，repo M5–M9） |
| ReferenceFrame、Assembly、運動、干涉 | 核心 feature |
| **Drawing** views + 完整 annotation 集（§24.4、§24.5） | 干涉 |
| **Configurations**（三種 input，§17.1） | 歷史編輯 + §42 |
| **Variables** / 具名 Parameter + 運算式求值器（§42、§43） | 可提前，Configurations 的前置 |
| **Derived** / 跨 Part Studio 重用（§47） | ReferenceFrame |
| **In-context** / top-down 設計（§48） | Assembly + Derived |
| **Assembly 狀態集**：named position、display state、exploded view（§49） | Assembly |
| **Measure** / Mass properties / 分析工具（§50） | 可大幅提前，見 §50.3 |
| **Import / Export** 格式矩陣（§51） | DXF 起逐步 |
| **Pattern 家族補齊**：curve / assembly / sketch pattern（§11.1） | 核心 feature |
| **Surface modeling**（13 個 feature，§11.2） | 需 spline / 曲面核心 |
| **Sheet metal**（16 項 + 展開計算，§11.2） | Surface + 雙表示架構 |
| **MBD / 檢驗**：inspection item、custom table、cut list（§24.5） | Drawing |
| **自訂 feature 機制**（EP3D 版，非 FeatureScript 移植，§52.2） | Variables |

**排序原則**：Variables（§42、§43）與 Measure（§50）的實際依賴比表上位置早得多，若在早期順手完成，可以省掉後面數個 milestone 的返工。特別是 §50（量測），它同時是 §31 使用者驗證的基礎設施——現在每次驗證都靠人工算體積，這件事本身就該提前。

本表只排相依順序，不排時程；實作切分見 `EP3D_Onshape_Parity_Plan.md`。**相依順序不建議變動**。

## 34. 不應複製 Onshape 的項目

> 本節在 A07 之後重新定義：**限制的對象是「表現與實作」，不是「功能」**。功能面以完整複製為目標，範圍裁決一律在 §52。

不複製（法律／權利邊界）：
- branding、產品名稱、商標
- proprietary icons / assets / 說明文字
- 具體視覺樣式（配色、間距、圖示造形、版面比例）
- 未公開的專有實作
- FeatureScript 的語言實作與其原始碼
- 為模仿而模仿的雲端架構（EP3D 的離線定位見 §35.2、§52.3）

不受限制（依 A07 積極對齊）：
- 功能集合本身：sketch / constraint / feature / assembly / drawing / configuration 的**完整能力**
- 業界標準術語
- 參數式行為與語意
- connector-based assembly 抽象
- 錯誤處理與狀態回報的**資訊內容**（呈現方式自訂）
- 工作流程的步驟順序與前後置條件

介面自由（A07）：版面、面板、命令分組、圖示、手勢、快捷鍵一律自訂，不需理由，也不計入 §32。

## 35. EP3D 差異化

1. **2D → 3D reconstruction**：existing DXF/drawing 轉成 editable parametric model。
2. **Local/offline operation**：Core CAD/solver 不依賴 cloud。
3. **Machine/robot coordinate integration**：ReferenceFrame 同時服務 Sketch/Part/Assembly/USER/TOOL。
4. **User-verifiable reconstruction**：ambiguous 2D reconstruction 採 Agent proposal → User confirmation → commit。

### 35.3 ReferenceFrame 作為共用抽象（上列第 3 點的展開）

參考模型的 Mate Connector 已經是「原點 + 主軸 + 次軸 + 平移旋轉修正」（§18.2）。工具機／機器人的 USER frame 與 TOOL frame 需要的正是同一組欄位。因此 EP3D 不應為 CAD 與 machine coordinate 各做一套：

```text
ReferenceFrame
├─ 用於 Sketch plane          （§5）
├─ 用於 Part connector        （§18）
├─ 用於 Assembly mate         （§20）
├─ 用於 USER coordinate system
└─ 用於 TOOL coordinate system
```

這是 EP3D 相對於通用 CAD 的結構性優勢，也是 M10 必須先於 M11 的原因。

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
[ ] 顏色不是唯一的狀態通道（A06）
[ ] 自動產生的 constraint 仍有 ConstraintId 並計入 DOF（§6.2）
[ ] rollback 與 suppression 的狀態可區分且各自正確序列化（§16.2）
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
├─ M6_SPEC.md … M9_SPEC.md
├─ reviews/
└─ architecture/
   └─ ADRs...
```

之後每個 milestone 應增加 Alignment section（§32 的表），而不是重新定義一套 CAD philosophy。

## 40. Codex Planning Prompt

> **注意**：以下是 M5 時期的原始 prompt，保留作為歷史記錄。其中的範圍限制（「不要把 M5 擴張成 DXF / Assembly / 進階建模」）在當時有效；但**整體範圍政策現由 A07 與 §52 決定**——功能求全、介面自由。撰寫新 milestone 的 prompt 時應以 A07 為準，並沿用此處的「先產出對齊分析再動工」與「不得為模仿 UI 而破壞架構」兩條做法。

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

---

# 附錄：本輪新增的深化章節

## 42. Numeric Field 與 Expression 文法

任何接受數值的欄位（dimension、feature 參數、mate offset、limit）應遵循同一套文法，而不是各欄位各自解析。

### 42.1 參考模型支援的內容

```text
數值        整數、小數（亦接受逗號作為小數分隔）
算術        + - * / ^
函式        ceil floor round sqrt abs max min log
三角        sin cos tan 及其反函數
長度單位    mm cm m inch foot yard（含複數形，如 feet）
角度單位    degree / radian（含複數形）
變數參照    #name，例如 #width
條件        三元運算，例如 #width>5?7:4
```

單位處理：可輸入任意支援單位，系統換算後以預設單位顯示。

### 42.2 單位代數規則

```text
合法    無單位的純量獨立使用            → 2 * 3
合法    同類單位相加減                  → 3mm + 2.5mm
合法    結果為一次方單位量               → #width / 2

非法    混用不同類單位相加               → 3mm + 2deg
非法    產生複合單位                    → 3mm * 3mm
非法    無單位除以有單位（產生倒數單位）    → 1 / 2mm
```

**核心規則：運算式的結果必須是該欄位期望的量綱，且為一次方。** 這條規則要在 parser 層強制，而不是等 solver 出錯。

### 42.3 EP3D 的落地要求

1. **一個 parser，一份文法**：由 Core 提供，UI 只負責輸入與錯誤呈現（A02）。
2. **內部單位固定**：長度 mm、角度 radian；顯示單位是 presentation 設定。輸入 `1 inch` 存成 25.4 mm，而**輸入的原始文字也要保留**，否則使用者下次打開會看到 25.4 而不是自己寫的 1 inch。
3. **錯誤訊息要指出位置**：`3mm + 2deg` 的錯誤是「第 7 字元：角度不能與長度相加」，不是「invalid expression」。
4. **運算式進 dependency graph**：`#width / 2` 建立一條從 Width Parameter 到本欄位的邊。這是 §7 的「綁 Parameter ObjectId」在多變數情況下的推廣。
5. **循環參照必須被偵測並拒絕**，錯誤訊息要列出環路。

### 42.4 實作狀態（M11.1，2026-08-20）

求值器本體已在 `src/Core/Expression/`，Core-only、939/939（Debug + Release）、17/17 突變殺死。ADR-M11-001..004。**尚未接入 Parameter / dependency graph / recompute**（那是 M11.2），因此產品層面運算式仍不可用。

**更正**：§42.2 原先列的「單位相除成為無單位卻用於長度欄位 → 需明確拒絕」是撰寫本文件時的誤記，不是查證得到的參考行為。實際規則相反——欄位裡打 `5` 必須等於 5 mm，否則最常見的輸入方式就不能用。真正非法的是**無單位除以有單位**（`1 / 2mm`，會產生倒數單位），該行已更正。

**已記錄的 intentional difference**（ADR-M11-002、ADR-M11-004）：

| 項目 | EP3D | 參考模型 | 理由 |
|---|---|---|---|
| 運算式**內部**的無單位提升 | 沒有；`3mm + 2` 報錯。提升只發生在欄位邊界 | 未明文，但其三元範例 `#width>5?7:4` 暗示有 | 提升規則會把 `#angle > 90` 讀成 90 **弧度**——靜默給錯答案比報錯更糟；只提升長度不提升角度則是兩條規則 |
| 逗號作小數點 | 不接受 | 接受 | `max(1,2)` 會變成真正的歧義 |
| `log` 的底數 | 自然對數；另提供 `log10` | 只寫 `log`，未指明底數 | 底數歧義應由實作明說，不該讓使用者試出來 |
| `round`/`ceil`/`floor` 吃有單位值 | 拒絕，訊息給出 `round(#x / 1mm)` | 未查證 | 在正則單位下取整會把角度捨入到最近的**弧度** |
| 比較串接 `1 < x < 10` | 拒絕 | 未查證 | 人讀成區間、C 讀成 `(1<x)<10`；拒絕是唯一不會誤導的解讀 |
| 三元運算的兩個分支 | 兩邊都求值 | 未查證 | 未取用分支裡的缺陷應該現在就報，而不是等某天參數變動選中它 |
| `in` / `ft` 縮寫 | 不支援 | 未列出 | 兩者也都是合理的識別字，暫不佔用 |

**規模界限**：巢狀深度上限 64，節點數上限 512。後者不是效能考量而是正確性——它是唯一界限住**求值器遞迴**的東西：`1+1+1+…` 由迴圈摺疊，parser 深度恆定但樹每多一項深一層（ADR-M11-003）。

## 43. Variables 與 Variable Studio

### 43.1 參考模型

```text
變數型別   Length、Angle、Integer、Real、Text
           Any（可容納上述任一種、帶不同單位的數值，
                或 boolean / map / array / string / function 等值）

宣告位置   Part Studio 內的 Variable feature
           或文件層級的 Variable Studio（跨 studio 共用）

參照方式   #name，可直接參照或作為運算式的一部分（§42）

表內順序   表中較前面定義的變數，可用來定義後面的變數

可配置     Variable Studio 的變數本身可以被 configuration 驅動（§17.1）
```

### 43.1.1 查證後的細節補充

```text
Variable feature 的型別    Length、Angle、Number、Any
Configuration variable 型別 Length、Angle、Integer、Real、Text
命名規則                   僅英數與底線，區分大小寫，不可數字開頭
宣告順序                   必須「在使用它的 feature 之前」建立
                           feature list 中位置較後的變數，前面的 feature 讀不到
引用                       dimension 欄位輸入 =#name；打 # 觸發自動完成
互相引用                   變數可用運算式引用其他變數，亦支援陣列索引
```

**「宣告必須在使用之前」是關鍵設計**：它把變數的相依關係壓成 feature list 的線性順序，用排序取代環路偵測。EP3D 有真正的 dependency graph，可以不受此限制——但那樣就必須自行處理環路（§42.3.5）。兩種做法都成立：

| 方案 | 說明 |
|---|---|
| 沿用線性順序 | 實作簡單，與 §16 rollback 語意一致（回捲到變數之前，變數就不存在） |
| 走 graph | 更自由，但需環路偵測，且必須定義「rollback 位置在變數之前時該變數的值」 |

建議沿用線性順序，理由是它與既有的 evaluation position 語意天然相容。此裁決記入 §32。

### 43.2 EP3D 對應

EP3D 已有 Parameter ObjectId 這一層，本節的工作不是新造一套東西，而是把 Parameter 補上三件事：

| 缺口 | 需求 |
|---|---|
| 型別 | Parameter 需要量綱型別（Length / Angle / Integer / Real / Text / Boolean），才能執行 §42.2 的檢查 |
| 作用域 | 目前 Parameter 屬於某個 feature；需要一層 **document-scope 的具名 Parameter**，才能做到 `#width` 跨 feature 共用 |
| 定義順序 | 具名 Parameter 之間可互相引用，需以 dependency graph 保證無環並決定求值順序（§42.3.5） |

命名建議：EP3D 內部沿用 `Parameter`，UI 上對使用者顯示為「變數 / Variable」，並在 §45 的詞彙表中記錄此映射。

## 44. Error / Warning / Status 分類

§8 處理 Sketch 的求解狀態，§9.3 處理 feature 清單的過濾，本節把整體分類收斂成一張表，避免各 milestone 各自發明狀態名稱。

### 44.1 參考模型的呈現手段

```text
Feature list 與對話框標題    出問題的 feature 顯示為橘色文字
個別欄位                    有問題的欄位以紅框標示
選取項目                    失效的選取在清單與畫布上同時變紅；
                            選中時呈更深的紅
Hover                       停在橘色 feature 上會顯示問題摘要
系統層級                    視窗頂端的通知泡泡
缺少選取                    黃色三角
定義不完整                  藍色指示
可忽略的幾何警告            右鍵「Ignore faulty part warning」→ 降級為 warning，
                            並可再以「Show faulty part warning」還原
```

補充（查證自 feature dialog，§10.1）：

```text
對話框標題列    紅 = 輸入不完整或有錯（擋住 commit）
                黑 = 可 commit
欄位底色        藍 = 需從畫面選取；白 = 鍵盤輸入
                （這是「輸入來源」提示，不是狀態）
Drawing         尺寸與幾何關聯斷開 → dangling dimension（§24.4）
```

### 44.2 EP3D 的狀態分類

| 分類 | 意義 | 是否阻擋 commit | 是否進 diagnostics |
|---|---|---|---|
| `Ok` | 正常求值完成 | 否 | 否 |
| `Incomplete` | 使用者尚未填完必要輸入 | 是（但不是錯誤） | 否 |
| `Warning` | 求值成功但結果可疑（例如極小面、被忽略的幾何問題） | 否 | 是 |
| `Error` | 求值失敗，本 feature 無結果 | 是 | 是 |
| `Downstream` | 本身定義正確，因上游失敗而無法求值 | 是 | 是（且必須指出上游來源） |
| `Suppressed` | 使用者或 configuration 抑制（document 狀態） | 否 | 否 |
| `RolledBack` | 因 evaluation position 而未參與本次求值（檢視狀態） | 否 | 否 |
| `Dirty` | 需重算但尚未重算 | 否 | 否 |

規則：

1. `Incomplete` 與 `Error` 必須分開——「你還沒選面」不是「這個 feature 壞了」。
2. `Downstream` 必須指名上游 ObjectId，否則使用者會在錯的地方找原因。
3. `Suppressed` 與 `RolledBack` 不可合併（§16.2.2）。
4. 每個非 `Ok` 狀態都要有：icon、文字摘要、可跳轉到肇因的動作（A06）。
5. 「忽略此警告」是 document 狀態變更，要進 undo stack（§15）並被序列化。

## 45. 詞彙對照表

| 參考模型用語 | EP3D 用語 | 備註 |
|---|---|---|
| Part Studio | Part Design Context / PartDocument | §3 |
| Feature list | Model Tree | M8_SPEC 已記錄此為 intentional difference |
| Parts list | Parts 區 | §9.1 |
| Rollback bar | Evaluation position | EP3D 不做拖曳 widget（M9_SPEC） |
| Mate connector | ReferenceFrame / FrameConnector | §18、§35.3 |
| Mate | Mate / Joint | §20 |
| Variable | Parameter（UI 顯示為「變數」） | §43.2 |
| Configuration Input / Option / Parameter / Option value | 同名沿用 | §17.1，結構照抄、UI 自訂 |
| Extrude（operation 參數） | Pad / Pocket（兩個型別） | §12.3，尚待裁決 |
| Driven dimension | Reference dimension | §7 |
| Sketch fully defined | Fully constrained / DOF=0 | §8 |
| Relation（Gear / Rack and pinion / Screw / Linear） | Relation | §20.5，與 Mate 分離的物件 |
| Derived | Derived / 跨 Studio 參考 | §47 |
| Context（in-context design） | Context | §48 |
| Named position | 具名姿態 | §49 |
| Display state | 顯示狀態 | §49 |
| Exploded view | 爆炸圖 | §49 |
| Merge scope | 作用範圍 | §11.1、§12.1 |
| Skip instances | 略過實例 | §11.1 |
| Dangling dimension | 懸空尺寸 | §24.4 |
| Snap point（drawing） | 吸附點 | §24.4 |
| Feature control frame | 幾何公差框 | §24.5 |
| Standard content | 標準件庫 | §52.1 |
| Direct edit | 直接編輯（無歷史幾何） | §51.2 |
| Composite part | 複合零件 | §51.2 |

新增對外可見的名詞前，先查此表；若參考模型已有業界標準用語（A01「industry-standard terminology」），優先沿用而非自創。

## 46. 參考來源索引（分級）

掃描日期 **2026-08-20**。依 A05，本文件所有「參考行為」主張都必須對應到 **T1** 的來源；T2 只能支撐概略敘述並須標明；T3 尚未查證。

### 46.1 T1 — 已讀內文（42 頁）

| # | 主題 | 對應節次 | URL |
|---|---|---|---|
| 1 | Help 首頁 | 全篇 | `/help/Content/Home/home.htm` |
| 2 | Getting Started | §1 | `/help/Content/Home/getting_started_with_onshape.htm` |
| 3 | Glossary（A–F 可用，G–Z 見 §46.4 警告） | §45 | `/help/Content/Home/glossary.htm` |
| 4 | Keyboard Shortcuts and Hotkeys | §30.1 | `/help/Content/Home/keyboard_shortcuts_and_hotkeys.htm` |
| 5 | Numeric Fields | §42.1、§42.2 | `/help/Content/Home/numeric_fields.htm` |
| 6 | User Interface Basics | §13.1、§28 | `/help/Content/ui-basics.htm` |
| 7 | Error Indicators | §8.1、§44.1 | `/help/Content/errorindicators.htm` |
| 8 | Working with Constraints | §6.1、§6.2、§6.3、§8.1 | `/help/Content/constraints.htm` |
| 9 | Automatic Inferencing | §4.2 | `/help/Content/Sketch/automatic_inferencing.htm` |
| 10 | Sketch Tools | §4.1.1 | `/help/Content/Sketch/sketch_tools.htm` |
| 11 | Sketch Dimension | §7.1、§7.2、§7.3 | `/help/Content/Sketch/dimension.htm` |
| 12 | Insert DWG or DXF | §25.1 | `/help/Content/Sketch/insert_dwg_or_dxf.htm` |
| 13 | Feature and Part Lists | §3、§9.1–§9.3、§16.1 | `/help/Content/PartStudio/features_and_parts_lists.htm` |
| 14 | Feature Basics | §10.1、§13.1、§44.1 | `/help/Content/PartStudio/feature_basics.htm` |
| 15 | Extrude | §12.1 | `/help/Content/extrude.htm` |
| 16 | Mate Connector | §5、§14.1、§18 | `/help/Content/PartStudio/mate_connector.htm` |
| 17 | Variable | §43.1.1 | `/help/Content/PartStudio/variable.htm` |
| 18 | Part Studio and Assembly Configurations | §17.1 | `/help/Content/PartStudio/part_studio_and_assembly_configurations.htm` |
| 19 | Configurations | §17.1 | `/help/Content/configurations.htm` |
| 20 | Assemblies | §19、§20 | `/help/Content/assembly.htm` |
| 21 | Mates | §20.1–§20.3 | `/help/Content/Assembly/mates.htm` |
| 22 | Mates（第二頁） | §20.1、§20.2 | `/help/Content/mate.htm` |
| 23 | Relations | §20.5 | `/help/Content/relations.htm` |
| 24 | Screw Relation | §20.5 | `/help/Content/Assembly/screw_relation.htm` |
| 25 | Assembly List / Instances List | §9.2、§19、§20.3 | `/help/Content/featurelistassembly.htm` |
| 26 | Snap Mode | §20.6 | `/help/Content/Assembly/snap_mode.htm` |
| 27 | Exploded Views | §49 | `/help/Content/Assembly/exploded_views.htm` |
| 28 | Display States | §49 | `/help/Content/Assembly/display_states.htm` |
| 29 | Named Positions | §49 | `/help/Content/named-positions.htm` |
| 30 | Modeling In-Context | §48 | `/help/Content/in-context.htm` |
| 31 | Derived | §47 | `/help/Content/derived.htm` |
| 32 | Drawing Views | §24.1–§24.3 | `/help/Content/drawings-views.htm` |
| 33 | Drawing Dimensions | §24.4 | `/help/Content/drawings-dimensions.htm` |
| 34 | Drawing Properties | §24.6 | `/help/Content/drawings-properties.htm` |
| 35 | Geometric Tolerances | §24.5 | `/help/Content/Drawing/geometric_tolerance.htm` |
| 36 | Supported File Formats | §51.1 | `/help/Content/translation.htm` |
| 37 | Working with Imported CAD | §51.2 | `/help/Content/Document/working_with_imported_cad.htm` |
| 38 | Surface modeling | §11.2 | `/help/Content/surfacing.htm` |
| 39 | Sheet Metal | §11.2 | `/help/Content/sheetmetal.htm` |
| 40 | Mass Properties Tool | §50.1 | `/help/Content/View/mass_properties_tool.htm` |
| 41 | Measure Tool（Primer） | §50.2 | `/help/Content/Primer/mass_properties_measure.htm` |
| 42 | Model Evaluation Tools | §50 | `/help/Content/View/model_evaluation_tools.htm` |

### 46.2 T2 — 僅搜尋摘要（尚未讀內文）

以下內容目前只有搜尋結果摘要支撐，敘述限於概略層級：

```text
Pattern 家族細節      linear_pattern / circular_pattern / curve_pattern / mirror
                      各自的完整參數（§11.1 僅有共通參數為 T1 等級）
Gear / Rack and pinion / Linear relation 的個別頁
                      （§20.5 的輸入與耦合為 T1，個別參數細節為 T2）
Interference Detection 面板細節
Replicate              種子實體與自動配對規則
Standard Content       標準件庫
Materials / Material library / Custom properties
Drawing 個別視圖頁      section / aligned section / detail / auxiliary / crop /
                      broken-out / break / flat pattern（§24.1 的清單為 T1）
Drawing 個別標註頁      callout / BOM table / surface finish / inspection item / styles
Sketch 個別工具頁       coincident / concentric / tangent / equal 等
Drawing construction tools
Assembly pattern（linear / circular / mirror）
```

### 46.3 T3 — 未讀（範圍裁決見 §52）

```text
Documents / Versions & branching / Release management /
Sharing & collaboration / Enterprise / Analytics / Permissions
Render Studio、Simulation、PCB Studio
App store / Integrations
FeatureScript 語言參考（/FsDoc/）
Mobile（iOS / Android）
Performance / Hardware recommendations
```

### 46.4 抓取品質警告

1. **Glossary G–Z 不可引用**。該次抓取的摘要中混入明顯非 Onshape 的條目（例如 "Linden Script"、"The Great Intersection"、"Macro"、"Polar array"），判定為摘要模型的幻覺污染。G–Z 僅供「有哪些主題值得查」的線索，任何條目在引用前都必須回到原頁查證。A–F 段落與已知事實一致，可信度較高，但同樣不作為單一來源。
2. **下列 URL 會被導回首頁或 404**，不存在或已改名，勿再引用：`mate-connector(s).htm`、`sketch_constraints.htm`、`partstudios.htm`、`variables.htm`、`measure.htm`、`massproperties.htm`、`draw-tools.htm`、`Data/Tocs/Toc.js`。
3. **`featuretools.htm` 與 `PartStudio/feature_tools.htm` 是操作說明頁，不是工具索引**；feature 全集需由個別頁與 glossary 交叉建立。
4. `sitemap.xml` 只列首頁區塊，**全站無公開 TOC**，故本索引以「索引頁 + 分區搜尋枚舉」建立，不保證窮盡。

### 46.5 使用限制

以上連結只供行為對照。禁止複製其文字、圖示或截圖進入 EP3D 的產品、說明文件或原始碼（§34）。

## 47. Derived — 跨 Part Studio 幾何重用

參考行為：

```text
可帶入的內容    parts、surfaces、curves、sketches、planes、
                active sheet metal models、mate connectors
來源            同文件的其他 Part Studio，或其他文件
版本綁定        跨文件必須參照「版本」（來源沒有版本就得先建一個）
                同文件可參照 workspace（即時更新）或版本（手動更新）
關聯方向        單向：來源改變會傳播到 derived；改 derived 不影響來源
放置方式        預設對齊來源原點，亦可指定 Base origin 或 Base mate connector
```

EP3D 判讀：

1. **這是 §21 connector reuse 的上位機制**。同一個 ReferenceFrame 抽象在此第三次出現（sketch plane、mate、derived 放置）——再次確認 §35.3 的判斷。
2. **單向關聯 + 版本綁定**是避免相依爆炸的關鍵。EP3D 是離線單機，沒有雲端版本系統，因此必須自行決定「版本」在本地是什麼：可能是文件內的具名快照。**這是 A07 之下第一個需要 EP3D 自創機制的功能**，記入 §32。
3. Derived 帶入的幾何**不是複本而是投影**：它必須隨來源重算，且必須能報告「來源已變更，尚未更新」的狀態（§44 的 `Dirty` 在跨文件層的形式）。

## 48. In-Context 設計（top-down）

參考行為：

```text
Context 是什麼   組立在「某一時刻」的全部幾何與位置的快照
建立方式         在 assembly 中對某零件按「在情境中編輯」，
                 Part Studio 開啟，被編輯零件不透明、其他零件半透明
                 一旦參照到半透明零件上的邊／面／軸，即擷取為新 context
更新方式         永遠手動，絕不自動；以「更新 context」命令選擇性更新
                 有上游變更時以藍點圖示提示
參考韌性         宣稱參照不會遺失或斷裂，零件不會因此失敗
限制             revision 實例不可 in-context 編輯；
                 workspace 或 version 參照才可以
```

EP3D 判讀：

1. **Context 是一個一等物件**（有 identity、有更新狀態），不是「編輯模式」。它本質上是「凍結的 assembly 求值結果」，與 §16 evaluation position 是同一族概念的不同軸向：一個凍結時間（feature 順序），一個凍結空間（instance 位置）。
2. **手動更新是刻意的**：自動更新會讓 top-down 設計變成不可預測的連鎖重算。EP3D 應沿用，並把「context 已過期」納入 §44 狀態表。
3. 這是 §35.1「2D → 3D 重建」之外，EP3D 第二個會產生**跨脈絡相依**的功能。兩者都要求 dependency graph 能跨文件邊界——建議在 M17/M18 之前先確認序列化格式能表達跨文件參照。

## 49. Assembly 狀態集：姿態、顯示、爆炸

參考模型把三種「同一個組立的不同呈現」分成三個獨立機制：

| 機制 | 捕捉的內容 | 切換方式 | 與 Drawing 的關係 |
|---|---|---|---|
| **Named position** | mate 自由度的值 + 無 mate 實例的絕對變換 | 右鍵套用 | 視圖可指定要顯示哪個具名姿態 |
| **Display state** | 哪些 part / subassembly / mate 隱藏或顯示 | 下拉選單切換 | 視圖預設 Show all，可手動改；組立改變會讓 drawing 顯示待更新 |
| **Exploded view** | 一連串 explode step（平移／旋轉），每步可命名、重排、刪除 | 面板 + **自己的 rollback bar** 逐步預覽 | 視圖可指定爆炸圖；同時顯示所有步驟並繼承爆炸線 |

另：subassembly 的 named position 可被上層組立「加入」並協同（`Add to named positions`）。

EP3D 判讀：

1. **三者必須分開**，因為捕捉的是三種不同性質的狀態：姿態是**幾何求值輸入**、顯示是**presentation**、爆炸是**衍生的展示變換**。混成一個「view state」會讓 §44 的 document/presentation 分界失守（A02）。
2. **Exploded view 自帶 rollback bar** 是 §16 evaluation position 概念的第三次出現（feature chain、feature 編輯、explode step）。這強烈建議把「有序步驟 + 評估位置」抽成可重用機制，而不是各實作一次。
3. Named position 是 §17 configuration 的近親但**不同**：configuration 改變模型定義，named position 只改變自由度的值。EP3D 不可把兩者合併。

## 50. Measure 與 Mass Properties — 驗證基礎設施

### 50.1 Mass properties

```text
回報值      質量、體積、表面積、周長（面）、質心、慣性矩（相對重心）
材料未指定  該零件被排除於質量計算之外，並顯示註記說明哪些被排除
覆寫        可勾選覆寫並手動輸入質量，其餘相依值自動重算
精度        預設依 workspace 單位；點一下數值顯示最大精度，再點回預設
適用對象    Part Studio 的零件、面、截面、assembly 實例，可多選累加
準確性      文件明言計算結果為近似值
```

### 50.2 Measure

```text
可量測      sketch、part、assembly、curve、surface、face、edge、vertex
單一實體    直徑、半徑、長度、周長、面積、座標
兩個實體    中心距、最小距、最大距（含 X/Y/Z 分量）、角度、
            面切角、垂直軸夾角
組立多選    最小／最大距離、總面積、中心軸距、長度
單位        長度 cm / foot / inch / meter / mm / yard；角度 degree / radian
精度        點擊數值在最大精度與預設之間切換
與 driven dimension 的差別
            量測只顯示不約束，不改變模型，隨零件移動即時更新
```

### 50.3 EP3D 判讀：這應該提前做

1. **§31 與 §38 的使用者驗證目前依賴人工算體積**。有了 measure / mass properties，驗證腳本可以直接讀數值，`Volume=120000 mm³` 這類期望值就從「人工核對」變成「工具讀數」。這是**降低驗證成本、提高驗證可信度**的直接手段，其價值不在 M20 而在現在。
2. **「近似值」必須誠實呈現**。EP3D 的 release proof 用的是解析可驗證的簡單幾何，因此可以要求精確；但一般幾何要標明容差，不可讓使用者誤以為顯示值是精確值。
3. **精度切換（點一下看全精度）是低成本高價值的互動**，建議納入。
4. Measure 與 driven dimension 的分工要明確：measure 是**瞬時查詢**（不進 document），driven dimension 是**持久標註**（有 ObjectId）。這與 §14.1 「推導候選點 vs 已建立 Connector」是同一條原則。

## 51. Import / Export 格式矩陣

### 51.1 參考模型支援的格式

```text
匯入 3D    Parasolid（.x_t/.x_b，首選）、ACIS(.sat)、STEP(AP203/214/242)、
           IGES(≤5.3)、CATIA V5、SOLIDWORKS、Inventor、Pro/E & Creo、
           JT、Rhino(.3dm)、NX、Solid Edge、glTF 2.0、3MF、PVZ
匯入 mesh  STL、OBJ、Parasolid mesh（僅檢視／參考，不可編輯）
匯入 2D    DWG(≤2018)、DXF(≤2013/2018)、DWT
匯出 3D    Parasolid、ACIS、STEP、IGES、STL、JT、glTF、Rhino、PVZ、3MF、OBJ、
           URDF（僅 assembly）
匯出 2D    PDF、DWG、DXF、DWT、SVG、PNG、JPEG
保留限制   STEP 匯出保留「幾何、MBD 資料、面顏色」；參數式歷史一律不跨格式保留
DXF 匯出   可選匯出單位或用 workspace 單位；
           可勾「spline 匯出為 polyline」、「z 高度歸零、法線取正」
```

### 51.2 匯入 CAD 的既有處理方式

```text
會失去      參數式歷史；可能以「一堆面或零件」而非單一實體匯入；可能帶缺陷
編輯手段    匯入後新增的 feature 仍是參數式；既有幾何則靠 direct edit
            （刪除／重建面、移動孔、改形狀）
修復        邊界邊高亮找出破洞與斷開處；
            以 Enclose / Fill / Move boundary / Bridging curve / Composite curve 修補
            破碎零件可合併為 composite part 當成單一零件處理
下游整合    重複零件可匯出為獨立文件後以「取代實例」統一參照
```

### 51.3 EP3D 判讀

1. **EP3D 目前只做 DXF 匯入**（M6）。依 A07，這是排程起點而非終點；但 3D 匯入的優先序應由 EP3D 的定位決定：`STEP` 與 `STL` 對機械／3D 列印場景的價值遠高於各家原生格式。
2. **`URDF` 匯出值得特別注意**——它是機器人描述格式。這與 §35.3 的 USER/TOOL coordinate 定位高度契合，是 EP3D 少數「參考模型也有、且正好打中 EP3D 差異化」的功能。建議提前評估。
3. **Direct edit 是一整族 feature**（刪除面／移動面／取代面／修補），不是單一功能。它是「匯入幾何」與「參數式歷史」之間的橋，在 EP3D 引入 3D 匯入時必須同批考慮，否則匯入的模型完全不能改。
4. **「參數式歷史不跨格式保留」是產業現實**，正好反證 §35.1 的價值：EP3D 的 2D → 參數式 3D 重建，做的正是產業標準格式做不到的事。

## 52. 功能範圍裁決（A07）

A07 之下，「不做」需要理由。本節記錄全部裁決。

**本節只記錄「做不做」，不記錄「什麼時候做」。** 實作排程見 `EP3D_Onshape_Parity_Plan.md`；那份文件用 repo 的 M 編號，與本文件的 § 主題編號是兩套東西。

### 52.1 納入 — 排程問題，不是要不要做

```text
Surface modeling（13 feature）            §11.2
Sheet metal（16 項 + 展開計算）            §11.2
Pattern 家族補齊（curve / assembly / sketch）§11.1
完整 Drawing annotation 集                §24.5
MBD / inspection / custom table / cut list §24.5
Configurations 三種 input                 §17.1
Variables 與運算式                        §42、§43
Derived                                   §47
In-context                                §48
Named position / display state / exploded §49
Measure / mass properties / 分析工具       §50、建議提前
Import / Export 格式矩陣                   §51
Standard content（標準件庫）               需自建資料來源，Assembly 之後
Replicate、Interference、Assembly pattern  Assembly 運動之後
```

### 52.2 條件納入 — 做同等功能，不移植實作

| 項目 | 裁決 |
|---|---|
| **FeatureScript** | 不移植其語言與實作（§34 的權利邊界）。但「使用者可自訂 feature」本身是功能，依 A07 應有 EP3D 版本。語言選型另議，可能是既有嵌入式語言而非自創。 |
| **Custom tables** | 同上，依賴自訂 feature 機制。 |
| **AI Advisor** | 屬產品服務而非 CAD 功能，不納入本 roadmap。 |

### 52.3 不納入 — 與 EP3D 定位直接衝突

以下全部源自 §35.2「local/offline operation」這條既有的產品定位，不是 A07 的例外：

```text
雲端文件管理、即時協作、留言、分享權限
版本／分支／合併工作區（雲端形式）
Release management、Revision、Approval workflow
Enterprise：帳號、分析、專案、連線、Arena 整合
行動平台（iOS / Android）
```

**但要注意**：這些功能中有一部分的**本地等價物是必要的**——

1. **版本／快照**：§47 Derived 需要「版本」概念才能綁定參照。EP3D 必須有本地版本機制（文件內具名快照），否則 Derived 無法實作。
2. **Where used / 相依查詢**：這是 §9.2「Show dependencies」的跨文件形式，離線一樣需要。
3. **文件層 metadata**（part number、material、custom property）：BOM 與 drawing 標題欄都需要，與雲端無關。

### 52.4 待裁決

```text
Render Studio（照片級算圖）    非 CAD 核心，但屬產品功能。建議不納入。
Simulation（有限元分析）       同上。§23 的干涉檢查已涵蓋機構需求；
                              應力分析屬另一個產品領域。建議不納入。
PCB Studio                    明確不納入。
```

以上三項若要納入，應另立獨立 roadmap，不應混入本文件。

## 53. 掃描覆蓋率與後續待讀

### 53.1 本輪成果

```text
成功讀取內文        42 頁（§46.1）
搜尋枚舉            9 次
確認不存在的 URL     8 個（§46.4.2）
全站公開 TOC        無（§46.4.4）
```

### 53.2 覆蓋率評估（誠實版）

| 區塊 | 覆蓋 |
|---|---|
| Sketch（constraint / inference / dimension / 工具清單） | 高，個別工具頁未讀 |
| Feature（dialog 機制 / feature list / rollback / Extrude） | 高，**除 Extrude 外的個別 feature 參數頁未讀** |
| Assembly（mate / connector / relation / 清單 / 狀態集） | 高，個別 mate 頁未讀 |
| Drawing（view / dimension / annotation / properties） | 中高，個別視圖與標註頁未讀 |
| Configuration / Variable | 中高 |
| Import / Export | 中 |
| Measure / Mass properties | 中高 |
| Surface / Sheet metal | 僅索引層 |
| Render / Simulation / PLM / Enterprise | 未讀（§52 判定不納入） |

### 53.3 下一輪應讀（依對 EP3D 的價值排序）

1. **個別 feature 參數頁**：Revolve、Hole、Fillet、Chamfer、Boolean、Shell、Draft、Sweep、Loft、Pattern ×4。理由：M8 之後每個 milestone SPEC 都需要這個粒度，目前只有 Extrude（§12.1）達標。
2. **個別 mate 頁**：11 種 mate 的完整參數與對齊選項。理由：M11 排程需要。
3. **個別 drawing 視圖與標註頁**：§24.1 的 9 種視圖、§24.5 的 11 種標註。理由：M14。
4. **Interference detection、Replicate、Assembly pattern、Standard content**。理由：M12–M13。
5. **Materials / custom properties / part properties**。理由：BOM 與 mass properties 的前置。
6. **Surface 與 sheet metal 個別頁**。理由：M23–M24，可延後。

每輪掃描後應更新 §46 的分級與本節的覆蓋率表，避免下次又從零開始判斷讀過什麼。

