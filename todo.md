# EP3D — 功能落差清單（對照 Onshape 掃描結果）

> 依據：`docs/EP3D_Onshape_Alignment_Roadmap.md`（2026-08-20 全站掃描版，§1–§53）
> 方針：**A07 — 功能求全，介面重畫**。「不做」需要理由，「做」不需要。
> 現況來源：`src/` 實際程式碼盤點（107 個檔案），非文件宣稱。
> 建立日期：2026-08-20

---

## 0. 現況基準（已驗證存在於程式碼）

| 區塊 | 現況 |
|---|---|
| Sketch entity | `SketchPoint` / `SketchLine` / `SketchCircle` / `SketchArc`（4 種） |
| Sketch constraint | `Coincident` `Horizontal` `Vertical` `Fix` `Distance` `Length` `Radius` `Diameter` `Angle`（M5 mandatory 全集）**＋（M13）`Parallel` `Perpendicular` `Equal` `Concentric` `Midpoint` `PointOnObject` `Tangent`**，共 16 種 |
| Solver | `GaussNewtonSketchSolver`；狀態 `Solved` / `UnderConstrained` / `OverConstrained`（相容冗餘）/ `Conflicting`；回報 `degreesOfFreedom` |
| Parameter | `Parameter` + `ParameterManager`；`UnitType` = Unitless / Millimeter / Radian / Kilogram / Second / KilogramPerCubicMeter |
| Feature | `Box` `Pad` `Pocket` `Revolve` `Fillet` `Chamfer` `Mirror` `Pattern` `Placeholder`（9 類） |
| ComputeState | `Valid` `Dirty` `Failed` `Suppressed` |
| 歷史編輯 | `undo()` / `redo()` / transaction / `nextUndoLabel()`；記錄 5 種 edit；每個 Body 有 `rollbackCut()` |
| 編輯交易 | `FeatureEditSession`：preview 文件 + `previewMassProperties()` + `accept()` / `cancel()` |
| 座標抽象 | `ReferenceFrame`（可巢狀、有 world transform）+ `Connector`（role、owner = PartDefinition/Assembly）+ sketch-on-frame |
| 量測 | `MassProperties`：volumeMm3、massKg、centerOfMassMm、inertiaTensorKgM2 |
| DXF 匯入 | `LINE` / `ARC` / `CIRCLE` + 尺寸（Linear / Aligned / Radius / Diameter / Angular）；`$INSUNITS` 全單位映射；skip 原因分類回報 |
| 重建 | `SketchReconstructor` + `ReconstructionPlan` |
| 序列化 | v3–**v11** 版本化，含 round-trip 測試 |
| UI | Model Tree、OCCT 視圖、Import DXF、Insert Pad/Pocket/Fillet/Chamfer、Undo/Redo、Suppress、Roll Back/Forward、Show/Hide、Properties、Recompute、Fit All；**（M12）2D Sketch 畫布：繪圖 5 種工具、吸附、4 種約束、5 種尺寸、constraint 面板、DOF 狀態列** |

**尚未存在任何程式碼的整個區塊**：Assembly、Drawing、Configuration、Derived、In-context、Export。

---

## 1. 阻塞型缺口（擋住多個下游功能，優先處理）

### 1.1 運算式求值器 — §42

**M11.1 已完成（2026-08-20）**：`src/Core/Expression/`，Core-only，939/939 Debug+Release，17/17 突變殺死。ADR-M11-001..004。

- [x] **運算式 parser + evaluator**（Core 層，UI 不得自己解析）
  - [x] 算術 `+ - * / ^`（含結合律：`-` `/` 左結合、`^` 右結合、`-2^2 = -4`）
  - [x] 函式 `ceil floor round sqrt abs max min log`（另加 `log10`，理由見 ADR-M11-004）
  - [x] 三角函式 `sin cos tan` 與反函數 `asin acos atan`
  - [x] 單位後綴 `mm cm m inch(es) foot/feet yard(s)` / `degree(s) deg radian(s) rad`
  - [x] **單位代數檢查**：`3mm+2deg`、`3mm*3mm`、`1/2mm`、`2mm^2` 全部在該運算當場拒絕
  - [x] 錯誤訊息帶字元位置與跨度（18 種錯誤碼，`DescribeExpressionError` 給 1-based 欄號）
  - [x] 保留使用者輸入原文（`ParsedExpression::sourceText()`）
  - [x] 三元運算 `a>b?x:y` 與六種比較運算（比較不可串接）
  - [x] 遞迴與規模界限（`kExpressionMaxDepth` 64、`kExpressionMaxNodes` 512）
- [x] **`#name` 參照的擷取**：`ParsedExpression::referencedVariables()` 回傳排序去重的清單 — 這就是 dependency edge 集合
- [x] **把那些邊真的接進 dependency graph**（M11.2）
- [x] **循環參照偵測**，錯誤訊息列出環路（`describeDependencyPath` 以有界 BFS 印出 `A -> B -> A`）

**M11.2 已完成（2026-08-20）**：978/978 Debug+Release，16/16 突變殺死。ADR-M11-005..010。
- [x] `Parameter` 的量綱映射 `ExpressionDimensionOf`（Unitless / Millimeter→Length / Radian→Angle；kg、s、kg/m³ 明確不支援運算式）
- [x] `PartDocument::setParameterExpression` 改為解析 → 驗證 → 試算 → 建立 `#name` 相依邊；任何失敗**文件完全不變**並回傳帶字元位置的錯誤
- [x] `DocumentRecomputeEngine` 求值 Parameter（ADR-011 的已知限制關閉）
- [x] 求值失敗 → `ParameterState::Failed` + 帶位置的診斷；下游被 `BlockedByDependency` 擋住
- [x] 序列化：載入後重建相依邊（一次性後處理，支援前向參照）；存檔與載入兩端都拒絕帶不可解析運算式的文件
- [x] undo：運算式編輯可逆，相依邊隨之復原；輸入數值會**取代**運算式（ADR-M11-006）
- [x] 刪除被其他運算式引用的參數 → 拒絕（ADR-M11-007，沿用 ADR-M5-009）

**M11.3（UI）已完成（2026-08-20）**：998/998 Debug+Release，12/12 突變殺死（其中 3 個**只有** viewer selftest 抓得到）。ADR-M11-011..013。
- [x] 屬性面板為參數提供 `Value` 與 `Expression` **兩列**（不是一個雙模式欄位，理由見 ADR-M11-011）
- [x] 被運算式驅動時 `Value` 列鎖住，且 tooltip **指名**驅動它的運算式
- [x] 被拒絕的運算式**保留使用者輸入的文字**，並在 tooltip 給出三行 caret 呈現（原文 / `^^^^` / 訊息）
- [x] 判斷邏輯全在 Qt-free 的 `src/Viewer/PropertyEditing.*`，MainWindow 只負責繪製
- [x] 可在執行中的程式裡操作：`--sample m11-expression`

**仍未完成**：
- [ ] **Owner UI validation 未執行**（`docs/reviews/M11_UI_UserValidation.md` 全部空白，只有 owner 能填）。這是**第四個**帶著未完成 owner UI validation 的 milestone（M6、M7、M8、M11）
- [ ] 語法highlight 與 `#` 自動完成（打 `#` 不會列出參數名）
- [ ] 儲存格本身沒有錯誤標示（訊息只在狀態列與 tooltip）
- [ ] §1.2 的 document-scope 具名參數仍是另一件事——目前解析的是 Part Studio 既有參數的名稱

**擋住的線現在解開了幾條**：§7 dimension expression 的 Core 側已通（缺 UI）；§17.3 dynamic suppression 與 §17.1 configuration variable 的運算式前置已備妥，缺的是 configuration 本身；§43 變數缺的是型別與作用域（§1.2）。

### 1.2 具名 / document-scope Parameter — §43

現況：Parameter 綁在 feature 上，無跨 feature 具名參照；型別只有量綱列舉，無 Integer / Real / Text / Boolean。

- [ ] Parameter 增加**型別**（Length / Angle / Integer / Real / Text / Boolean）以支撐 §42.2 檢查
- [ ] 新增 **document-scope 具名 Parameter**（`#width` 可跨 feature 使用）
- [ ] 命名規則：英數與底線、區分大小寫、不可數字開頭
- [ ] 裁決並實作作用域規則：**建議沿用「宣告必須在使用之前」的線性順序**（與 rollback 語意相容），記入 §32
- [ ] 變數可引用其他變數

### 1.3 Stable topological naming — §12.2、§13、§20

現況：`Fillet` / `Chamfer` 只能 dress **全部邊**（ADR-M8-006 已記錄），因為沒有穩定的 edge/face 識別。

- [ ] 設計並實作 persistent topological naming
- [ ] 之後才可解鎖：per-edge Fillet/Chamfer、`Up to face/part/vertex/next`、face/edge 層級選取、非 connector mate

**這是目前最容易被 UI 需求推著違反 A03 的地方**（roadmap §12.2 已標明）。

---

## 2. Sketch — §4、§6、§7、§8

### 2.1 缺少的 constraint（11 種 → 剩 4 種）

**M13 已完成（2026-08-20）**：`docs/M13_SPEC.md`，schema v11。Debug + Release 各 1088/1088。ADR-M13-001..007。

- [x] `Parallel` — 正規化外積；0° 與 180° 都成立（刻意與 Angle(0) 不同）
- [x] `Perpendicular` — 正規化內積
- [x] `Tangent` — 線↔曲線、曲線↔曲線；內／外切分支**存在約束裡**，求解不重新推導
- [x] `Equal` — 單一型別：兩線比長度、兩曲線比半徑；一線一曲線拒絕
- [x] `Concentric` — 兩曲線共圓心；點對圓心走既有的 Coincident（拒絕訊息會指名）
- [x] `Midpoint`
- [x] `Point-on-object` — 針對**無限長**的線；線段內的限制需要不等式，不做也不假裝
- [ ] `Symmetric` — roadmap §6.1 有列，M13 未處理（不在本輪的 7 項內）
- [ ] `Normal`（需 spline，Core 尚無 spline）
- [ ] `Pierce`（需平面外 3D 參考）
- [ ] `Curvature`（需 spline / conic）

**M13 未做而且會被踩到的**：**弧的端點仍不能被約束** —— solver 對弧只有圓心與半徑兩組
變數（ADR-M12-003）。這限制了 Tangent 在「弧接線」這個常見用法上的實用性，
是這條線上的下一個缺口。

**Owner UI validation 未執行**（`docs/reviews/M13_UI_UserValidation.md`）；獨立審查未執行。

### 2.2 自動產生的 constraint（§6.2，全缺）

- [ ] `Quadrant`（吸附圓/弧四分點）
- [ ] `Use`（投影外部 edge 進 sketch，並自動約束到來源）
- [ ] `Intersection`（平面與實體相交產生曲線）
- [ ] 規則：自動產生者仍需 ConstraintId、計入 DOF、可列出可刪除

### 2.3 Automatic inference（§4.2，全缺）

- [ ] 推斷引擎：horizontal / vertical / midpoint / parallel / coincident / 對齊原點
- [ ] proximity wake-up
- [ ] 視覺提示（虛線 + 高亮 + constraint 圖示）
- [ ] 抑制推斷的修飾鍵 + status bar 提示
- [ ] **推斷成立必須產生真正的 ConstraintId**，不得只做視覺吸附（否則 DOF 說謊）

### 2.4 缺少的 sketch entity / 工具（§4.1.1）

- [ ] Rectangle（corner / center point，實作為 4 Lines + constraints）
- [ ] Polygon（內接 / 外切）
- [ ] Slot
- [ ] Ellipse / Elliptical arc
- [ ] Spline / Bezier / Conic
- [ ] Center point arc（目前只有 3 point 概念）
- [ ] Text
- [ ] **Construction geometry**（entity 上的 flag，不新增型別）
- [ ] Sketch Fillet / Chamfer
- [ ] Trim / Extend
- [ ] Offset
- [ ] Sketch Mirror
- [ ] Sketch Linear / Circular pattern

### 2.5 Dimension 模型（§7）

- [ ] **Driving / Driven（reference）區分** — 目前所有 dimensional constraint 都是 driving，模型中沒有這個概念
- [ ] 手動切換 driving ↔ driven
- [ ] **裁決 over-defining 時的行為**（§7.2）：建議「自動降級為 driven + 強制提示 + 可 undo」，不可靜默改變使用者意圖
- [ ] 裁決單一 Dimension 工具（依選取推斷型別）vs 現行多命令，記入 §32
- [ ] `=#name` 或等價的欄位引用語法（依賴 1.1 / 1.2）

### 2.6 Constraint Manager（§6.3，全缺）

- [ ] 列出 Sketch 內全部 constraint：Id / type / 參與 entity / driving-driven / status
- [ ] 依 type / status 篩選（篩出 conflicting 與 redundant）
- [ ] 畫布上點選 constraint 圖示後刪除
- [ ] 刪除為一筆 semantic transaction

### 2.7 狀態呈現（§8）

Core 已有 `SketchSolveStatus` 四態與 `degreesOfFreedom`，缺的是**呈現**。

- [ ] status bar 文字（`Under constrained — DOF 3`）
- [ ] Sketch 節點 icon
- [ ] conflicting 時列出涉及的 ConstraintId 集合並高亮幾何
- [ ] **區分 numerical failure 與 conflicting**（目前 `Conflicting` 涵蓋了「不收斂」，兩者混在一起）
- [ ] 依 A06 確保顏色不是唯一通道

---

## 3. Feature 建模 — §9、§10、§11、§12

### 3.1 缺少的 feature

- [ ] `Hole`（ADR-M8-007 已記錄可用圓形 Pocket 表達，但缺專用 feature 與螺紋參數）
- [ ] `Boolean`（body 之間的 union / subtract / intersect）
- [ ] `Shell`
- [ ] `Draft`
- [ ] `Sweep`
- [ ] `Loft`
- [ ] `Curve pattern`
- [ ] **per-edge Fillet / Chamfer 選取**（依賴 1.3）
- [ ] 變半徑 Fillet、Face blend

### 3.2 Extrude 泛化（§12.2）

現況只有 Blind + New/Add/Remove（以 Pad / Pocket 兩型別表達）。

- [ ] `Symmetric`
- [ ] `Through all`
- [ ] `Opposite direction`
- [ ] `Starting offset`
- [ ] `Second end position`
- [ ] `Up to next / face / part / vertex`（依賴 1.3）
- [ ] `Intersect` operation
- [ ] `Merge scope`（與 pattern 共用的「作用範圍」型別）
- [ ] `Draft` / `Thin` / `Surface`
- [ ] **裁決**：維持 Pad+Pocket 雙型別，或合併為單一 Extrude + operation 參數（§12.3）。**建議在引入 Intersect 之前決定**，否則型別數會從 2 變 3

### 3.3 Pattern 家族（§11.1）

- [ ] `Skip instances`（以 pattern 座標 (i,j) 記錄，**不可用線性序號**——A03）
- [ ] `Merge scope`
- [ ] Curve pattern

### 3.4 Feature list 能力（§9.2）

- [ ] Rename（名稱是顯示屬性，不得成為 identity，不觸發 recompute）
- [ ] **Show dependencies**（parent / child）— EP3D 已有 dependency graph，這只是 UI 查詢，**投報率最高**
- [ ] Reorder（`Body::moveFeatureToIndex` 已存在，需接上 UI + 驗證輸入仍排在自己之前）
- [ ] Folder（可巢狀、可整組抑制、可 unpack；**不得改變 dependency 順序語意**）
- [ ] 搜尋框 + feature 計數
- [ ] **錯誤過濾**（只列出有診斷的節點）
- [ ] Dynamic suppression（依賴 1.1）

### 3.5 Feature dialog（§10.1）

- [ ] 標題列狀態（可 commit / 不可 commit）
- [ ] 欄位輸入來源提示（畫面選取 vs 鍵盤輸入）
- [ ] **Preview 不透明度滑桿**
- [ ] 編輯既有 feature 時**自動回捲到該 feature 當時狀態** + 「看最終結果」切換（與 §16 共用同一套 evaluation position，不可各做一份）
- [ ] `Incomplete` 狀態（尚未填完 ≠ 出錯）
- [ ] 右鍵欄位：清除失效參考、批次移除選取

### 3.6 Parts list（§9.1）

- [ ] Body / Part 之下的產物清單（part / surface / mesh / composite）
- [ ] 各自可 hide / rename / 指定 material / 匯出

---

## 4. 選取與互動 — §13、§14

- [ ] **選取層級**：目前只有 tree 物件層。需要 Part / Feature / Sketch / SketchEntity / Vertex / Edge / Face / Connector 各層（Edge/Face 依賴 1.3）
- [ ] 三處選取同步（tree ↔ viewer ↔ dialog）為同一個 selection set
- [ ] 選取欄位有型別，型別不符在放入前就拒絕並說明
- [ ] **失效選取可見**：保留原本參考字串，不得靜默丟棄
- [ ] **Preselection**：hover 預highlight，與 selected 視覺不同
- [ ] Hover 揭露 context-sensitive 候選點（形心、中點、頂點、孔心、負空間中心、conic virtual sharp）—— §18 connector 選取需要
- [ ] 裁決：多選是否需修飾鍵（參考模型不需要）。**擇一並全域一致**
- [ ] 穿透透明物件選取

---

## 5. Assembly — §18–§23（整個區塊尚無程式碼）

現況只有 `ReferenceFrame` + `Connector` 兩個基礎型別，沒有 Assembly 文件、Instance、Mate。

### 5.1 基礎

- [ ] Assembly document 型別
- [ ] `Instance`（Part definition + instance transform，同一 Part 可多實例）
- [ ] Instances list（含 folder、group、context menu、suppress → 連帶停用其 mate）
- [ ] **Fix / 固定實例**

### 5.2 Mate（11 種）

- [ ] `Fastened`（0 DOF）
- [ ] `Revolute`（1 rot）
- [ ] `Slider`（1 trans）
- [ ] `Cylindrical`（1 rot + 1 trans）
- [ ] `Planar`（2 trans + 1 rot）
- [ ] `Ball`（3 rot）
- [ ] `Pin slot`（1 rot + 1 trans，互相獨立）
- [ ] `Parallel`
- [ ] `Tangent`（**不使用 mate connector**）
- [ ] `Width`
- [ ] `Group`
- [ ] Mate value（除 Ball / Fastened / Tangent / Width 外皆可指定）
- [ ] Offset（Planar / Slider / Revolute / Pin slot / Fastened）
- [ ] **Limits**（依剩餘自由度提供上下限，3D 視圖可見，超界夾住而非報錯）

### 5.3 Relation（4 種，§20.5）

- [ ] `Gear`（兩個 revolute mate，固定角位移比）
- [ ] `Rack and pinion`（revolute + slider，每轉位移量）
- [ ] `Screw`（**單一** cylindrical mate，每轉位移量）
- [ ] `Linear`（兩個直線 mate，固定比）
- [ ] Relation 模型必須支援 **一元與二元兩種 arity**（Screw 只吃一個 mate）
- [ ] Relation 持有 MateId 而非 instance 對

### 5.4 Connector 補齊（§18.2）

現況 `Connector` 只有 name / role / frameId / owner。缺：

- [ ] Implicit vs explicit 的建立時機與清單呈現（兩者都要有 ObjectId）
- [ ] Realign（primary + secondary axis entity）
- [ ] Flip primary axis 180°
- [ ] Reorient secondary axis（90° 象限）
- [ ] X/Y/Z 平移 offset 與旋轉
- [ ] 可見性在 Part 與 Assembly 兩脈絡各自獨立

### 5.5 DOF 回報（§20.3）

- [ ] 逐 instance 的 triad 指示 + tooltip 列出未約束自由度
- [ ] **「mate 提供的 DOF」與「relation 耦合後的實際 DOF」分開顯示**
- [ ] mate 指示器狀態（正常 / 抑制 / 有問題）

### 5.6 其他

- [ ] Snap mode（拖 connector 到 connector，放開後開 mate 對話框讓使用者選型別）
- [ ] Interference detection（§23，broad-phase → 精確相交，與 mate 分離）
- [ ] Assembly pattern（linear / circular / mirror）
- [ ] Replicate
- [ ] Standard content（標準件庫，需自建資料來源）
- [ ] Subassembly

---

## 6. Drawing — §24（整個區塊尚無程式碼）

### 6.1 View（9 種）

- [ ] Projected / Auxiliary / Section / Aligned section / Broken-out section / Detail / Break / Crop / Flat pattern

### 6.2 View 屬性

- [ ] Scale（N:N 或 N/N；projected 繼承母視圖）
- [ ] Tangent edges（隱藏 / 實線 / 假想線）
- [ ] View label（detail 與 section 自動加，可設前後綴）
- [ ] Rotation 0–360°（有母子關係時停用）
- [ ] Simplification、Render mode
- [ ] Associativity：綁定版本、母子關係、模型變更自動更新、跨 sheet 箭頭

### 6.3 Dimension（§24.4）

- [ ] Snap point 形狀語意（方=端點 / 三角=中點 / 菱形=四分點 / 圓=圓心）
- [ ] linear / angular / radial / diameter / arc length / ordinate / chamfer / max-min / hole-thread callout
- [ ] Centerline / Centermark
- [ ] **Dimension palette**：上下文字、前後綴、精度 0–8、單位、符號、雙重單位、檢驗外框、半徑↔直徑切換
- [ ] 公差型別：None / Symmetrical / Deviation / Limits / Basic / MIN / MAX / Fit / Fit with tolerance / Fit (tolerance only)
- [ ] **Dangling dimension** 狀態（關聯斷開，不得靜默刪除）

### 6.4 Annotation（§24.5）

- [ ] Note / Callout(balloon) / Datum / Surface finish / Weld symbol / Hole-thread callout / Bend note / Inspection item / Image
- [ ] **Geometric tolerance**：必須有結構化資料模型（符號 + 值 + 最多 3 個 datum + 修飾符 MMC/LMC/RFS + 條件符號 + 上下文字 + 可堆疊 frame），**不可用自由文字實作**
  - 符號集：Position / Concentricity / Symmetry / Parallelism / Perpendicularity / Angularity / Cylindricity / Flatness / Circularity / Straightness / Square / Profile of surface / Profile of line / Circular runout / Total runout
- [ ] Tables：BOM / cut list / revision / custom

### 6.5 Drawing 屬性（§24.6）

- [ ] 文件層預設：Units and Precision / Dimensions / Annotations / Views / Construction Geometry / Formats / Tables / Inspection
- [ ] **樣式繼承鏈**：文件層預設 → 實體層覆寫 → 格式刷 → 存成範本（**M14 起始就要這樣建模，事後補要資料遷移**）
- [ ] Lock drawing properties

---

## 7. Configuration — §17（尚無程式碼）

- [ ] `List` input（具名選項）
- [ ] `Checkbox` input（開關 feature = 抑制/解除）
- [ ] `Configuration variable` input（`#name`，型別 Length/Angle/Integer/Real/Text，依賴 1.1 / 1.2）
- [ ] 表格：configuration 為列、被驅動參數為欄；欄標題 = feature 名 + 參數名
- [ ] 被配置的欄位在 dialog 中有明確標記
- [ ] Visibility condition（依賴 1.1）
- [ ] Property configuration（part number / color / material）
- [ ] Assembly configuration（**範圍較小**：只有 mate（不含 mate connector）、instance、pattern）
- [ ] **紅線：絕不以「複製整份 document」實作**

---

## 8. 跨 Studio 與情境 — §47、§48

- [ ] **Derived**：帶入 parts / surfaces / curves / sketches / planes / mate connectors
  - [ ] 單向關聯（來源→derived 傳播，反向不傳播）
  - [ ] 放置方式：Base origin 或 Base mate connector
  - [ ] 「來源已變更，尚未更新」狀態
  - [ ] **前置：本地版本／具名快照機制**（EP3D 離線，沒有雲端版本，必須自創——這是 A07 下第一個需自創的機制）
- [ ] **In-context 設計**
  - [ ] Context 是一等物件（有 identity、有更新狀態），不是編輯模式
  - [ ] 手動更新，絕不自動；過期時明確提示
  - [ ] 跨文件參照必須能序列化

---

## 9. Assembly 狀態集 — §49（尚無程式碼）

三者必須**分開**（捕捉的是三種不同性質的狀態）：

- [ ] **Named position**：mate 自由度的值 + 無 mate 實例的絕對變換（幾何求值輸入）
- [ ] **Display state**：哪些 part / subassembly / mate 隱藏（presentation，不得進 Core）
- [ ] **Exploded view**：有序 explode step，**自帶 rollback bar** 逐步預覽
- [ ] Subassembly 的 named position 可被上層加入協同
- [ ] 三者都可被 drawing view 指定

> **架構觀察**：`explode step 的 rollback` 是「有序步驟 + 評估位置」這個機制的**第三次**出現（feature chain、feature 編輯回捲、explode step）。建議抽成可重用機制，不要各實作一次。

---

## 10. 量測與分析 — §50（建議提前，理由見下）

現況有 `MassProperties` 結構，但沒有互動式量測工具。

- [ ] **Measure 工具**
  - [ ] 可量測 sketch / part / assembly / curve / surface / face / edge / vertex
  - [ ] 單一實體：直徑、半徑、長度、周長、面積、座標
  - [ ] 兩實體：中心距、最小距、最大距（含 X/Y/Z 分量）、角度
  - [ ] 單位切換（mm / cm / m / inch / foot / yard；deg / rad）
  - [ ] 精度切換（點一下看最大精度）
  - [ ] **只顯示不約束**，不進 document（與 driven dimension 分工明確）
- [ ] Mass properties 補齊：**表面積**、**面周長**、覆寫（override）與相依值重算、精度切換
- [ ] 材料未指定時排除並明確說明哪些被排除
- [ ] 近似值必須誠實標示容差

> **為什麼要提前**：§31 / §38 的使用者驗證目前靠人工算體積。有了量測工具，`Volume=120000 mm³` 從「人工核對」變成「工具讀數」，直接降低驗證成本、提高可信度。它在排程上是 M20，實際價值在現在。

---

## 11. 匯入 / 匯出 — §51

### 11.1 DXF 匯入補強

- [ ] 目前只吃 `LINE` / `ARC` / `CIRCLE`。至少補 `LWPOLYLINE` / `POLYLINE`（工程圖極常見）
- [ ] `SPLINE` / `ELLIPSE`（依賴 2.4 的 spline entity）
- [ ] `INSERT`（block）展開
- [ ] **比例處理裁決**：參考模型用「第一個標注的尺寸縮放整份 sketch」。**EP3D 不應照抄**——既然 §25 要求保留 provenance 與 import transform，比例應是**顯式的 import 參數**，否則 §26 的重建無法解釋自己為何得到某個尺寸
- [ ] 匯入前選單位（事後改單位 = 重新匯入，不是編輯）
- [ ] 「使用檔案原點 vs 置中」選項
- [ ] 目標 sketch 非空時的明確前置檢查

### 11.2 匯出（**目前完全沒有**）

- [ ] 2D：DXF / DWG / PDF / SVG / PNG
- [ ] 3D：STEP、STL（機械與 3D 列印場景優先度最高）
- [ ] **URDF**（機器人描述格式）—— 與 §35.3 的 USER/TOOL 定位高度契合，**建議提前評估**，這是少數「參考模型也有、且正好打中 EP3D 差異化」的功能
- [ ] DXF 匯出選項：單位、spline 輸出為 polyline、z 歸零法線取正

### 11.3 3D 匯入與 direct edit

- [ ] STEP / STL 匯入
- [ ] **Direct edit 家族**（刪除面 / 移動面 / 取代面 / 修補）——匯入幾何與參數式歷史之間的橋。引入 3D 匯入時必須同批考慮，否則匯入的模型完全不能改
- [ ] 邊界邊高亮找破洞；Enclose / Fill / Move boundary / Bridging curve / Composite curve
- [ ] Composite part

---

## 12. 狀態與診斷 — §44

現況 `ComputeState` 只有 4 態。目標分類 8 態。

- [ ] `Incomplete`（尚未填完必要輸入，**≠ Error**）
- [ ] `Warning`（求值成功但結果可疑）
- [ ] `Downstream`（自身正確，因上游失敗而無法求值，**必須指名上游 ObjectId**）
- [ ] `RolledBack` 與 `Suppressed` 明確可區分並各自正確序列化（Body 層已有 `rollbackCut()`，但 `ComputeState` 中未反映）
- [ ] 每個非 `Ok` 狀態都要有 icon + 文字摘要 + 跳轉到肇因的動作
- [ ] 「忽略此警告」是 document 狀態變更 → 進 undo stack 並序列化

---

## 13. 材料與屬性

現況：`PartDocument` 只有**單一** `material()`，`assignMaterialToFeatures()` 是全域指派。

- [ ] **逐 part 材料指派**（目前是每份文件一種材料）
- [ ] 材料庫（library + custom）
- [ ] Part properties：name、description、part number、custom property
- [ ] Appearance（顏色）

---

## 14. UI 基礎建設 — §28、§29、§30（介面自由，但機制必要）

**M12 已完成（2026-08-20）**：`docs/M12_SPEC.md`。Debug + Release 各 1051/1051；Qt-free 決策層 45 例 +
solver 在場 7 例 + `--selftest --sample m12-sketch` 的畫面斷言。ADR-M12-001..008。

- [x] **Sketch 繪圖 UI**：Point / Line（連續）/ Rectangle / Circle / Arc，端點・圓心・原點・
      曲線・格線吸附，Shift 抑制推斷；吸附產生**真的 Coincident constraint**（§4.2）
- [x] Sketch 內的 dimension 顯示與雙擊編輯（含 M11 運算式；角度內部 radian、介面 degree）
- [x] **（M14）尺寸畫成真正的標註**：延伸線 + 偏移的標註線 + 兩端朝外的實心箭頭 +
      挖斷線的數值；半徑帶 R、直徑帶 D、角度是繞交點的弧。`docs/M14_SPEC.md`
- [ ] **標註位置不能拖曳** —— 偏移量是算出來的，AutoCAD 可以用滑鼠放，這裡還不行（M14 最明顯的缺口）
- [ ] 標註之間不會互相避讓；沒有公差／前後綴；箭頭放不下時不會翻到線外
- [x] **Mode 概念**（3D / Sketch）與 contextual toolbar —— sketch 工具列與 constraint dock
      只在 sketch 模式出現；Assembly / Drawing 模式尚不存在
- [x] Constraint manager（§6.3）：列出、標示 `AT FAULT`、可刪除
- [x] 狀態呈現（§8）：六種 solver 狀態各有 badge + 句子，redundant 與 conflicting 分開
- [x] **Core：sketch 幾何與約束納入 undo**（M12.0，ADR-M12-002）—— 這在 M12 之前完全不存在
- [ ] **建立 sketch 本身仍不可 undo**（`UndoDelta` 無 sketch-existence；要補得先裁決
      「undo 一個被 feature 引用的 sketch 時那些 feature 怎麼辦」）
- [ ] 拖曳幾何（目前只能用尺寸驅動）
- [ ] Sketch 平面選擇 UI（一律建在 world XY；M10 的 `supportFrameId` 已存在但無入口）
- [ ] **階段二：把同一套 `SketchCanvasModel` 接到 OCCT overlay**，在 3D 視圖的平面上繪製
- [ ] **單一 shortcut registry**（以 mode 為維度，可自動偵測衝突，說明頁由 registry 生成）
- [ ] 「commit 後再開一個同命令對話框」的連續建立（機械設計常態，省下大量操作）
- [ ] 快捷鍵清單可從程式內叫出
- [ ] **Owner UI validation 未執行**（`docs/reviews/M12_UI_UserValidation.md` 全部空白）——
      這是**第五個**帶著未完成 owner UI validation 的 milestone（M6、M7、M8、M11、M12）
- [ ] **獨立審查未執行**

---

## 15. 排程建議

| 優先 | 項目 | 理由 |
|---|---|---|
| **P0** | ~~§1.1 求值器（M11.1）~~ ~~接線（M11.2）~~ ~~UI（M11.3）~~ **三段皆完成**；剩 **§1.2 具名 Parameter** 與 **owner UI validation** | 使用者現在可以在面板裡打運算式並看到結果與錯誤。剩下的是變數的型別與作用域，以及只有 owner 能做的實機驗證 |
| **P0** | §10 Measure / Mass properties 補齊 | 降低**現在**每個 milestone 的驗證成本 |
| **P0** | §3.4 Show dependencies | 已有 dependency graph，純 UI 查詢，是 §32 對齊驗證的直接工具 |
| **P1** | §1.3 topological naming | 解鎖 per-edge dress、Up-to-*、face/edge 選取；**愈晚做，愈多程式碼會繞過它** |
| **P1** | §2.1 constraint 補齊、§2.5 driving/driven | Sketch 可用性的門檻 |
| ~~**P1**~~ | ~~§14 Sketch 繪圖 UI~~ **已完成（M12）** | 現在可以用滑鼠畫圖、標尺寸、下約束，並 Pad 成 3D。剩 owner validation 與獨立審查 |
| ~~**P1**~~ | ~~§2.1 的 7 種 constraint~~ **已完成（M13）** | 16 種約束可用。剩 Symmetric（未排）與三種需 spline / 3D 參考的 |
| **P1** | **弧端點的 solver 變數** | 弧只有圓心與半徑，端點無法被約束——Tangent 與 Coincident 在弧上的用途因此受限 |
| **P2** | §3.1 Hole / Boolean / Shell、§3.2 Extrude 泛化 | 建模能力補齊 |
| **P2** | §12 狀態分類補齊 | 診斷品質 |
| **P3** | §5 Assembly、§9 狀態集 | 新的大區塊 |
| **P3** | §6 Drawing | 新的大區塊；**樣式繼承鏈要在起始就做對** |
| **P4** | §7 Configuration、§8 Derived / In-context | 依賴 P0 與本地版本機制 |
| **P5** | Surface（13 feature）、Sheet metal（16 項 + 展開計算） | 規模與 M8 全部核心 feature 相當甚至更大 |

---

## 16. 需要 Owner 裁決的項目

這些不是實作問題，是方向問題，**做下去之前需要決定**：

1. **Pad + Pocket 雙型別 vs 單一 Extrude + operation 參數**（§12.3）—— 建議在引入 Intersect 之前決定
2. **Over-defining 時自動降級為 driven，或拒絕並說明**（§7.2）—— 建議自動降級 + 強制提示 + 可 undo
3. **單一 Dimension 工具（依選取推斷）vs 現行多命令**（§7.1）—— **M12 的 UI 已採用
   「單一 D 命令 + 推斷 + Radius/Diameter 顯式覆寫」並記為 ADR-M12-005，待 owner 追認**。
   若否決，改動範圍限於 `SketchCanvasModel::requestDimension` 與工具列上的一個按鈕
4. **多選是否需要修飾鍵**（§13.1）—— 擇一並全域一致
5. **變數作用域走線性順序或走 graph**（§43.1.1）—— 建議線性順序，與 rollback 語意相容
6. **本地版本／具名快照的形式**（§47）—— Derived 的硬前置
7. **DXF 比例處理**：顯式 import 參數，或照抄「第一個尺寸縮放整份 sketch」（§11.1）
8. **Render / Simulation 是否納入**（§52.4）—— 建議不納入，若要做應另立 roadmap

---

## 17. 已知的既有債務（非本次掃描發現，但影響排程）

以下出自 `docs/M9_SPEC.md` §2，列在此處以免被功能清單淹沒：

- [ ] **M8 review round 4 的修正本身未經審查**（同一位置，第四次）
- [ ] **M6 / M7 / M8 的 owner UI validation 全部空白**（只有 owner 可填，ADR-M4-016）
- [ ] **M6.11–M6.14 未經審查**即依 owner 指示併入 master

功能擴張不會讓這三項消失。**M9 不能在 M8 關閉前關閉，M8 不能在 M7 之前。**
