# EP3D M5 — UI 範例與 User-Assisted Validation Guide

**目的：** M5 的 UI 由 Agent 負責實作、建立可重現範例、提供驗證步驟與記錄結果；由 User 實際觀看畫面、操作滑鼠鍵盤並從旁協助驗證。  
**原則：** User 驗證屬於 `User-Assisted / Owner Manual Validation`，不得在文件中誤寫成 Independent Agent Review。

## 1. 驗證分工

```text
Developer Agent
  ↓
完成 UI
  ↓
建立 Sample Documents
  ↓
Qt automated tests
  ↓
產生 User Validation Instructions
  ↓
User 實際操作 EP3D
  ↓
User 回報 PASS / FAIL / 現象
  ↓
Agent 記錄 evidence
  ↓
有問題 → 修正 → 再驗證
```

Agent 負責：
- 建立範例資料。
- 說明每一步要按哪裡。
- 說明正確預期畫面/數值。
- 自動測試可自動驗證的邏輯。
- 收集 User 回報。
- 將結果寫入驗證報告。
- 對 FAIL 建立可重現步驟並修正。

User 負責：
- 看實際 Qt 畫面。
- 操作滑鼠/鍵盤。
- 確認視覺、選取、數值、單位、3D 更新是否符合預期。
- 回報 PASS / FAIL；FAIL 時提供看到的現象，必要時附 screenshot。

## 2. 必須提供的 M5 UI Sample

建議加入：

```text
examples/m5/
├─ fully_constrained_rectangle.ep3d
├─ under_constrained_rectangle.ep3d
├─ conflicting_rectangle.ep3d
├─ constrained_circle.ep3d
└─ README.md
```

若目前檔案格式/載入方式不適合 committed sample file，可改成 deterministic sample generator：

```text
--sample m5-rectangle
--sample m5-underconstrained
--sample m5-conflict
--sample m5-circle
```

但 Sample 必須可由 Reviewer/User 重複建立，不能依靠開發者記憶手動建模。

## 3. Sample A — Fully Constrained Rectangle

參數：

```text
Width       100 mm
Height       50 mm
PadLength    20 mm
Density    2700 kg/m³
```

Sketch 應包含四條線及 M5 約束，最終：

```text
Status = Solved / Fully constrained
DOF = 0
Volume = 100000 mm³
Mass = 0.270 kg
```

預期 UI 概念：

```text
Model Tree                     Properties
Part001                        Sketch001
├─ Sketch001                   Status    Fully constrained
├─ Pad001                      DOF       0
└─ MassProperties
                               Dimensions
                               Width     100.000 mm
                               Height     50.000 mm

            [ 3D VIEWER ]
          rectangular solid
```

實際 widget/layout 可依目前 UI 架構調整，不要求像素完全相同。

## 4. User Test A — 修改 Width

操作：

```text
1. 開啟 fully_constrained_rectangle sample。
2. 選 Sketch001 或 Width 對應的 Parameter/Property。
3. 找到 Width。
4. 將 100 改成 120。
5. 按 Enter/Apply（依 UI 實際設計）。
```

User 應看到：

```text
Width = 120 mm
Status = Fully constrained
DOF = 0
3D solid 變寬
Volume = 120000 mm³
Mass = 0.324 kg
```

驗證：

```text
[ ] Width 編輯容易找到
[ ] 單位清楚
[ ] 修改後沒有多餘 modal dialog
[ ] Sketch status 正確
[ ] DOF = 0
[ ] 3D 立即/合理時間內更新
[ ] Viewer 沒有顯示舊 geometry 當成 current
[ ] Volume 正確
[ ] Mass 正確
[ ] Tree / Property / Viewer selection 一致
```

## 5. User Test B — 修改 Height

從 Width=120 開始：

```text
Height 50 → 80 mm
```

預期：

```text
Volume = 192000 mm³
Mass = 0.5184 kg
Status = Fully constrained
DOF = 0
```

User 同時確認 3D 是高度方向的預期改變，而不是錯誤修改 Width 或 PadLength。

## 6. Sample B — Under-Constrained Rectangle

建立故意少一個或多個必要約束的 Sketch。

預期：

```text
Status = Under constrained
DOF > 0
```

UI 必須讓 User 不用開 developer console 就能知道 Sketch 尚未 fully constrained。

User 驗證：

```text
[ ] Under constrained 狀態容易辨認
[ ] DOF 數值可找到
[ ] 狀態不是只靠顏色
[ ] Sketch geometry 仍為有限、合理的 geometry
[ ] UI 沒有錯誤宣稱 Fully constrained
```

## 7. Sample C — Conflicting Rectangle

建立明確衝突，例如：

```text
Length(E1) = 100 mm
Length(E1) = 120 mm
```

依 solver ADR，顯示 `Conflicting` 或 documented `OverConstrained`。

UI 必須提供至少：

```text
Sketch001
Status: Conflicting
Constraint: Cxx / Cyy
Diagnostic: conflicting dimensional constraints
```

不要求文字完全相同，但必須足以讓使用者知道哪個 Sketch/constraint 有問題。

User 驗證：

```text
[ ] Tree 能看到 failure 狀態
[ ] Property/diagnostic 能找到原因
[ ] 沒有假裝 recompute success
[ ] 3D 舊結果若保留，有明確 stale/failed indication
[ ] 沒有 crash
```

然後刪除/修正衝突：

```text
120 mm conflict → remove/correct
```

預期：

```text
Status 回到 Solved/Fully constrained
DOF 正確
Profile/Pad 更新
Viewer 更新
```

## 8. Sample D — Constrained Circle

```text
Center fixed
Radius = 10 mm
PadLength = 30 mm
```

預期：

```text
DOF = 0
Volume ≈ 9424.77796 mm³
```

User 將：

```text
Radius 10 → 20 mm
```

預期：

```text
Volume ≈ 37699.11184 mm³
```

也就是 Volume 約為原本 4 倍。

User 驗證：

```text
[ ] Radius Property/Dimension 可找到
[ ] mm 單位正確
[ ] 3D 圓柱半徑明顯變大
[ ] PadLength 沒有被改變
[ ] Volume 約 ×4
[ ] DOF 保持 0
```

## 9. Selection Synchronization Test

User 依序：

```text
Tree 點 Sketch001
Tree 點 Pad001
Viewer 點 Solid
點 Viewer 空白區
重新選 Pad001
```

每一步確認：

```text
Tree selection
Viewer highlight
Property target
Status/selection text
```

指向同一 semantic object。

Critical failure：

```text
畫面顯示選 A
實際 Property 修改 B
```

## 10. Show / Hide / Fit All

User：

```text
1. Select Pad001。
2. Show/Hide。
3. 確認 solid 消失。
4. 再 Show/Hide。
5. 確認 solid 回來。
6. Rotate/Pan/Zoom。
7. Fit All。
```

驗證：

```text
[ ] hide 正確
[ ] show 正確
[ ] hidden state 清楚
[ ] Fit All 恢復合理視角
[ ] Rotate/Pan/Zoom 正常
```

## 11. Failure / Recovery UI Test

User 輸入非法值，例如規格允許的測試入口中：

```text
Radius = -10
或
Width = invalid/negative value
```

UI 應：

```text
拒絕輸入
或
接受 semantic edit 後明確標 Failed
```

實際政策依 M5 ADR。

不可：

```text
crash
NaN 顯示成正常 geometry
錯誤 solid 被標成 current success
無任何錯誤提示
```

修正成合法值後必須恢復。

## 12. DPI / Layout User Validation

若 User 環境可調 Windows Display Scale，至少協助看：

```text
100%
150%
200%
```

以及一般 desktop resolution。

檢查：

```text
[ ] Model Tree 文字未截斷到不可用
[ ] Property value 可完整編輯
[ ] mm/deg/kg 等單位可讀
[ ] toolbar icon 大小合理
[ ] Viewer 可正常 pick
[ ] dialog/button 沒有重疊
[ ] DOF/status 可讀
```

如果某個 DPI 沒有實際測，報告寫 `NOT EXECUTED`，不可猜 PASS。

## 13. Agent 必須產生的 User Test Sheet

實作完成後建立：

`docs/reviews/M5_UI_UserValidation.md`

格式：

```text
# M5 UI User-Assisted Validation

Build/Commit:
OS:
Resolution:
Display Scale:
Qt:

## Sample A — Fully Constrained Rectangle
Load sample: PASS/FAIL
Width edit 100→120: PASS/FAIL
DOF=0: PASS/FAIL
Volume 120000: PASS/FAIL
Mass 0.324: PASS/FAIL
3D update: PASS/FAIL
Notes:

## Sample B — Under Constrained
Status visible: PASS/FAIL
DOF > 0: PASS/FAIL
Notes:

## Sample C — Conflict
Conflict visible: PASS/FAIL
Useful diagnostic: PASS/FAIL
No false current success: PASS/FAIL
Recovery: PASS/FAIL
Notes:

## Sample D — Circle
R10 result: PASS/FAIL
R10→R20 update: PASS/FAIL
Volume ×4: PASS/FAIL
DOF=0: PASS/FAIL
Notes:

## Selection
Tree→Viewer: PASS/FAIL
Viewer→Tree: PASS/FAIL
Property target: PASS/FAIL
Clear/reselect: PASS/FAIL

## Viewer
Show/Hide: PASS/FAIL
Rotate: PASS/FAIL
Pan: PASS/FAIL
Zoom: PASS/FAIL
Fit All: PASS/FAIL

## DPI
100%: PASS/FAIL/NOT EXECUTED
150%: PASS/FAIL/NOT EXECUTED
200%: PASS/FAIL/NOT EXECUTED

## User Findings
Critical:
Major:
Minor:

## User Validation Result
ACCEPTED / NEEDS FIXES

Validated by:
Project owner/user manual validation
```

## 14. Screenshot Evidence

User 不需要每個步驟都截圖。

建議 Agent/User 對重要狀態保留：

```text
M5-UI-01 FullyConstrained-100x50
M5-UI-02 Width120
M5-UI-03 UnderConstrained
M5-UI-04 Conflict
M5-UI-05 ConflictRecovered
M5-UI-06 Circle-R10
M5-UI-07 Circle-R20
M5-UI-08 HighDPI
```

若 User 回報 FAIL，優先截 FAIL 畫面。

## 15. User 回報的最簡格式

User 可以直接回覆 Agent：

```text
A PASS
B PASS
C FAIL - conflict 有顯示紅色，但是看不到是哪一個 constraint
D PASS
Selection PASS
Show/Hide PASS
DPI150 PASS
DPI200 NOT TESTED
```

Agent 必須把它轉成正式 validation report，而不是要求 User 自己寫完整 review 文件。

## 16. 修正循環

```text
User reports FAIL
    ↓
Agent records exact scenario
    ↓
classify Critical/Major/Minor
    ↓
Developer reproduces
    ↓
fix + regression test
    ↓
build/test
    ↓
only affected User test repeated
    ↓
report updated
```

若修正可能影響其他 UI workflow，Agent 應要求重跑相關 tests，而不是只測單一畫面。

## 17. 驗證責任界線

Agent automated tests 可證明：

```text
ObjectId mapping
property commit
solver/recompute result
numeric value
state transitions
serialization
```

User-assisted validation主要證明：

```text
畫面是否看得懂
操作是否合理
selection/highlight 是否符合實際觀察
layout/DPI 是否可用
錯誤提示是否足以理解
3D 更新是否符合使用者預期
```

兩者不可互相冒充。

## 18. M5 Completion Report 記錄方式

正確：

```text
M5 UI Validation:
User-Assisted Manual Validation: PASS
Validated by project owner using documented Samples A-D.
Automated UI/logic tests: PASS.
Independent UI agent review: not required / not performed according to project workflow.
```

錯誤：

```text
Independent Reviewer: PASS
```

除非真的有獨立 Reviewer 執行。

## 19. Codex Prompt — 建立範例並請 User 驗證

```text
Implement the M5 UI validation workflow using:
docs/M5_UI_User_Assisted_Validation_Guide.md

The project owner/user will assist with real visual and mouse/keyboard validation.

Your responsibilities:
1. Create deterministic M5 UI samples for:
   - fully constrained rectangle,
   - under-constrained rectangle,
   - conflicting rectangle,
   - constrained circle.
2. Add README/instructions for loading or generating each sample.
3. Run all automated tests that can verify semantic state, ObjectId mapping, property edits, solver/recompute results and numeric outputs.
4. Create/update:
   docs/reviews/M5_UI_UserValidation.md
5. Give the user short step-by-step instructions, one validation group at a time.
6. State the exact expected UI state and expected numerical result.
7. Wait for the user's PASS/FAIL observation for visual/manual items.
8. Record user observations accurately.
9. For any FAIL, reproduce, classify, fix, add regression coverage and ask the user to repeat only the affected validation plus related workflows.
10. Never mark an unobserved visual/DPI/manual test PASS.
11. Never describe owner/user manual validation as independent agent review.

Required samples:
A Fully constrained rectangle:
Width=100, Height=50, Pad=20, Density=2700.
DOF=0, Volume=100000 mm^3, Mass=0.27 kg.
User changes Width 100→120:
Volume=120000 mm^3, Mass=0.324 kg.

B Under-constrained sketch:
DOF>0 and visible UnderConstrained state.

C Conflicting rectangle:
contradictory dimensions, visible conflict/diagnostic, no false current success, then repair/recovery.

D Constrained Circle:
R=10, Pad=30, then R=20; volume must become approximately 4x.

Also guide user through:
Tree/Viewer/Property selection synchronization,
Show/Hide,
Rotate/Pan/Zoom/Fit All,
failure/recovery,
available DPI/layout checks.

At completion, incorporate the factual User-Assisted Validation result into M5_CompletionReport.md.
```

## 20. Acceptance

M5 UI user-assisted validation is accepted when:

```text
[ ] deterministic samples exist
[ ] sample instructions exist
[ ] automated semantic/UI logic tests pass
[ ] Rectangle 100→120 observed
[ ] correct Volume/Mass observed
[ ] DOF=0 observed
[ ] UnderConstrained + DOF>0 observed
[ ] Conflict state/diagnostic observed
[ ] Conflict recovery observed
[ ] Circle R10→R20 observed
[ ] Tree/Viewer/Property sync observed
[ ] Show/Hide observed
[ ] Rotate/Pan/Zoom/Fit observed
[ ] available DPI checks recorded
[ ] no Critical UI finding
[ ] unresolved Major findings explicitly tracked
[ ] M5_UI_UserValidation.md completed
[ ] CompletionReport identifies validation as User-Assisted
```

This workflow should be reused and expanded in M6/M7, especially when validating imported DXF geometry and reconstructed dimensions.
