# M13 — 七種幾何約束（Parallel / Perpendicular / Tangent / Equal / Concentric / Midpoint / Point-on-object）

> 依據：`docs/EP3D_Onshape_Alignment_Roadmap.md` §6.1「次階段」的約束全集
> 對應 `todo.md`：§2.1（M13 之前是 11 項全空）
> 前置：M12 的 sketch UI（`docs/M12_SPEC.md`）
> 狀態：**已實作**，Debug + Release 各 1088/1088；獨立審查與 owner UI validation **未執行**

---

## 1. 範圍

`todo.md` §2.1 列了 11 種缺少的 constraint。本 milestone 做**其中 7 種**：

| Constraint | 做了 | 說明 |
|---|---|---|
| Parallel | ✅ | 兩條線同向（含反向） |
| Perpendicular | ✅ | 兩條線垂直 |
| Equal | ✅ | 兩線等長，或兩曲線等半徑 |
| Concentric | ✅ | 兩個圓／弧共圓心 |
| Midpoint | ✅ | 點位於線的中點 |
| Point-on-object | ✅ | 點落在線、圓或弧上 |
| Tangent | ✅ | 線與曲線相切，或兩曲線相切 |
| Normal | ❌ | 需要 spline，Core 尚無 spline |
| Pierce | ❌ | 需要平面外的 3D 參考（§5 / §10） |
| Curvature | ❌ | 需要 spline / conic |

剩下 3 種的前置條件在 Core 裡都還不存在，這是排除理由，不是遺漏。

---

## 2. 設計裁決

### 2.1 Equal 是**一個**型別，不是兩個

「這兩個一樣大」對使用者是一個念頭。型別由實際的 entity 決定：兩線比長度、兩曲線比半徑。
**一線一曲線被拒絕**——長度與半徑不是同一個量，默默把它們畫上等號會讓約束的意義取決於
使用者先點了哪一個。

### 2.2 Concentric 只吃兩個曲線

「點與圓心重合」已經可以用 `Coincident(point, curve.CenterPoint)` 表達，而且早已實作。
再給同一個關係第二種拼法，只會多出一個 solver 分不出來的型別。
Concentric 加的是**曲線對曲線**這個具名概念，讓約束清單寫得出使用者當初的意圖。

UI 在使用者選了「一個曲線 + 一個點」時，refusal 訊息會**指名 Coincident**，而不是只說不行。

### 2.3 Tangent 的分支是**存下來的**，不是每次求解重新猜

兩個圓相切有兩種真正不同的構型：

```text
外切   圓心距 = r1 + r2
內切   圓心距 = |r1 - r2|
```

`TangentConstraint::internal` 記錄使用者要的是哪一種。**UI 在建立當下依當時幾何決定一次**
（目前兩圓是套疊還是分開），之後就是約束的屬性。

若改成求解時重新推導，拖曳一個圓穿過另一個就會**默默把模型換成另一個模型**——
那不是換姿態，是換定義。這與 A03「identity 不可由暫態推導」是同一條精神。

### 2.4 Point-on-object 針對的是**無限長的線**，不是線段

把點限制在兩端點之間需要不等式，而這個 solver 只有等式。
宣稱做得到、實際只做一半，比明說做不到更糟。

### 2.5 Parallel ≠ Angle(0)

Parallel 用**正規化的外積**（= 兩線夾角的 sin），所以 0° 與 180° 都成立。
Angle 是有向的（ADR-M5-006），Angle(0) 會拒絕一對反向畫的平行線。
測試 `M13_PAR_002` 就是釘這個差異。

---

## 3. Residual 設計

新增 11 種 `SolveResidual::Kind`。全部沿用既有的數值 Jacobian，所以**不需要手推導數**——
這正是 ADR-M5-003 選中央差分的理由：手寫的 Jacobian 出符號錯誤時，
solver 會收斂到錯的答案，而所有「有沒有收斂」的測試都會通過。

| Kind | 式子 | slots |
|---|---|---|
| `LinesParallel` | `cross(dirA, dirB) / (|A| |B|)` | 8 |
| `LinesPerpendicular` | `dot(dirA, dirB) / (|A| |B|)` | 8 |
| `LengthsEqual` | `|B| - |A|` | 8 |
| `RadiiEqual` | `rB - rA` | 2 |
| `MidpointU` / `MidpointV` | `p - (start + end)/2` | 3 |
| `PointOnLine` | 點到無限直線的帶號垂距，除以線長 | 6 |
| `PointOnCircle` | `|P - C| - r` | 5 |
| `TangentLineCircle` | `|圓心到線的垂距| - r` | 7 |
| `TangentCirclesOuter` | `|C1 - C2| - (r1 + r2)` | 6 |
| `TangentCirclesInner` | `|C1 - C2| - |r1 - r2|` | 6 |

三個要點：

1. **兩個方向類的 residual 都做了正規化**，所以值是夾角的 sin / cos：無因次、上限為 1、
   在關係成立處恰為 0。未正規化的外積與內積單位是 mm²，量值隨線長增長——
   為毫米挑的殘差容忍度會接受一對明顯不平行的 100 mm 線，卻拒絕一對 1 mm 的。
2. **`TangentLineCircle` 用絕對值**。折點恰在圓心落在線上時，那是測度零的構型，
   而且永遠不是解（r > 0）。改用帶號距離會把「相切」偷偷變成「從左邊相切」。
3. **Concentric 重用 `PointsEqualU/V`**。它是不同的**約束型別**（清單要寫對），
   但不是不同的方程式。

---

## 4. 拒絕規則（Core 側）

這些在 `BuildSolveProblem` 裡拒絕，並且**指名是哪一條 constraint**（§8.2 的可定位性）：

- **自我參照**（`Parallel(l, l)`、`Concentric(c, c)`…）—— 這種 residual 恆為零：
  永遠滿足、什麼都沒約束，卻會讓 DOF 少報一個自由度。**一條會對 DOF 說謊的約束，
  比一條被拒絕的約束更糟。**
- **零長線**用於 Parallel / Perpendicular / Point-on-line / Tangent-line-circle ——
  這些 residual 要除以線長，退化線不只是未定義，是 0/0。
  （`restoreEntity` 刻意不驗證，所以手改過的檔案可以帶進這種幾何，檢查必須放在**使用**的地方。）
- **型別不符**：Equal 收到一線一曲線、Concentric 收到線、Tangent 收到兩條線。

---

## 5. UI

- 工具列與 `Sketch → Constrain and Dimension` 各多 7 個命令，快捷鍵
  **G**(Parallel) **N**(Perpendicular) **E**(Equal) **O**(Concentric)
  **M**(Midpoint) **B**(On object) **T**(Tangent)。
  沿用 ADR-M12-010：`Qt::WidgetWithChildrenShortcut` 且掛在畫布上。
- 畫布圖示（ASCII，A06 的第二通道）：`//` `|_` `=` `()` `M` `-o` `T`。
  圖示框從 13 px 加寬到 19 px —— 兩字元的圖示被裁掉就不再是可讀的第二通道。
- **Midpoint 與 Point-on-object 不在乎點選順序**：使用者不必記得先點哪一個。
  `requestConstraint` 把它正規化成「點在前、承載體在後」，下游不必再推導一次。
- Constraint 面板的 Tangent 列會標 `(inner)` / `(outer)` —— 兩者是不同的模型，
  而清單正是使用者確認自己要的是哪一個的地方。

---

## 6. 序列化：schema v10 → **v11**

七種都會存檔與載入。`Tangent` 的 `internal` **一律寫出，且載入時為必填**——
預設它會讓一個被截斷的檔案變成一份描述**另一種相切**的合法文件，而且不會有任何提示。

---

## 7. 驗證

| 層級 | 位置 | 覆蓋 |
|---|---|---|
| Solver 在場 | `tests/Solver/SketchGeometricConstraintTests.cpp`（20 例） | 每一種都**從不滿足關係的幾何出發**，解完後檢查關係成立；DOF 確實減 1；平行四邊形（Parallel + Equal）；所有拒絕路徑 |
| Qt-free 決策層 | `tests/SketchCanvasTests.cpp`（+10 例，共 55） | 選取適用性、拒絕訊息內容、點選順序無關、Tangent 分支的判定、undo、面板列 |
| 序列化 | `tests/SerializationV11Tests.cpp`（7 例） | round-trip、參考與 sub-element、`internal` 雙向、缺欄位被拒、未知型別被拒 |
| 真實視窗 | `--selftest --sample m12-sketch` | 在執行中的程式裡畫圓、選圓與線、下 Tangent，並斷言**面板多一列、畫布多一個圖示**；以及被拒絕時使用者確實收到訊息 |

「從不滿足關係的幾何出發」是刻意的：畫成已經正確的 sketch，不論 residual 有沒有意義都會通過，
而本專案出過一個收斂到 4e-11 卻在量錯東西的 residual（ADR-M5-006）。

**Owner UI validation 未執行**（`docs/reviews/M13_UI_UserValidation.md`）。
上面四層全是 agent 檢查，依 ADR-M4-016 不得當作 owner validation 引用。

---

## 8. ADR

- **ADR-M13-001** — Equal 是單一型別，一線一曲線拒絕。
- **ADR-M13-002** — Concentric 只吃兩曲線；點對圓心走既有的 Coincident，UI 的拒絕訊息指名它。
- **ADR-M13-003** — Tangent 的內／外切分支**存在約束裡**，建立時決定一次，求解不得重新推導。
- **ADR-M13-004** — 方向類 residual 一律正規化為 sin / cos。
- **ADR-M13-005** — Point-on-object 針對無限長的線；線段內的限制需要不等式，不做也不假裝。
- **ADR-M13-006** — 自我參照的成對約束一律拒絕，理由是它會讓 DOF 說謊。
- **ADR-M13-007** — schema v11；`Tangent.internal` 為必填欄位，不得預設。

---

## 9. 本 milestone 沒有做的

1. **Normal / Pierce / Curvature** —— 前置（spline、3D 參考）在 Core 裡不存在。
2. **Tangent 的接觸點沒有被定位**：兩圓相切只約束圓心距，切點在哪由其他約束決定。
3. **弧的端點仍然不能被約束** —— solver 對弧只有圓心與半徑兩組變數（ADR-M12-003）。
   這限制了 Tangent 在「弧接線」這個常見用法上的實用性，是下一個該處理的缺口。
4. **自動推斷（§6.2 的 Quadrant / Use / Intersection）** 仍未做。
5. **Symmetric** —— §6.1 表中列為次階段，但不在 `todo.md` §2.1 的 11 項裡，本輪未處理。
