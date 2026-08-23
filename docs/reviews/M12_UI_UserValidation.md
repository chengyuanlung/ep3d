# M12 — Owner Manual UI Validation（2D Sketch 繪圖 UI）

**Status: NOT EXECUTED.** 每一格 Result 都是空的，且只有 owner 可以填（ADR-M4-016）。

Agent-executed checks 存在 —— `tests/SketchCanvasTests.cpp` 45 例、
`tests/Solver/SketchCanvasSolveTests.cpp` 7 例、以及 `--selftest --sample m12-sketch`
在真實視窗裡對「畫了幾條線、幾個約束圖示、幾列面板、幾個尺寸標註」的斷言 ——
**它們不是這份文件**，不能填任何一列判斷題，也不得被當成 owner validation 引用。

Agent 檢查**能**說的：畫得出來、約束真的建立了、DOF 正確、尺寸能驅動幾何、
拒絕時有訊息、畫面上確實有那些像素。
只有**你**能說的：這些操作**好不好用** —— 沒人教的情況下找不找得到、
在你的 DPI 與主題下看不看得清、值不值得這些鍵盤與滑鼠動作。

## 怎麼開始

```
build\Debug\ParametricCADViewer.exe --sample m12-sketch
```

開起來是 M4 的 100 × 50 矩形 pad。M12 要驗的不是它，而是接下來你自己畫的東西。

從選單 **Sketch → New Sketch**（Ctrl+Shift+N）開始。

---

## A — 進入 sketch 模式

| # | 做這個 | 應該看到 | Result | Notes |
|---|---|---|---|---|
| A1 | Sketch → New Sketch | 中央換成 2D 畫布：格線、兩條軸、原點方框 | | |
| A2 | 看工具列 | 多了一排 sketch 工具（Select / Line / Rectangle / Circle / Arc / Point），且 Select 是按下狀態 | | |
| A3 | 看右側 | 出現 **Constraints** 面板（此時空的） | | |
| A4 | 看狀態列 | 形如 `[DOF] Under constrained — ...   |   ...`，而且明講建立 sketch 目前**不可 undo** | | |
| A5 | 滾輪縮放、中鍵拖曳 | 縮放以游標為中心，平移跟手 | | |
| A6 | **在沒有讀這份文件的前提下**，你找得到怎麼開始畫嗎？ | 老實回答——這是可發現性，其他任何檢查都問不到 | | |

## B — 畫

| # | 做這個 | 應該看到 | Result | Notes |
|---|---|---|---|---|
| B1 | 按 Rectangle（或 R），點兩個對角 | 出現 4 條線；Constraints 面板出現 **8 列**（2 H、2 V、4 Coincident） | | |
| B2 | 看畫布上的小方框標記 | 線旁有 `H` / `V` / `o` 字母標記 —— 這是約束的第二通道，不是只有顏色 | | |
| B3 | 按 L 畫連續線，畫 3 段後按 Esc | 每一段接著上一段；Esc 結束鏈，不會多出一段 | | |
| B4 | 把游標移到既有線的端點附近 | 出現**方形**吸附標記；移到圓心出現**圓形**；移到原點出現**菱形**；在線上出現**叉** | | |
| B5 | 吸到某個端點畫一條新線，然後看 Constraints 面板 | 多了一列 Coincident —— 吸附產生的是**真的約束**，不是畫圖時的磁吸 | | |
| B6 | 按住 Shift 再畫一次 | 不吸附，且狀態列顯示 `inference suppressed` | | |
| B7 | 按 C 畫圓、按 A 畫弧（圓心→起點→終點） | 都畫得出來，形狀符合預期 | | |
| B8 | 同一點連點兩下試圖畫零長度的線 | **不會**產生東西，也不會跳出無法理解的錯誤；工具還在等你 | | |

## C — 選取與約束

| # | 做這個 | 應該看到 | Result | Notes |
|---|---|---|---|---|
| C1 | 按 S 回到 Select，點一條線 | 線變粗並變色（兩個通道，不是只有顏色） | | |
| C2 | 再點同一條線 | 取消選取。**多選不需要按 Ctrl** | | |
| C3 | 選一條線，按 H | 線變水平；面板多一列 Horizontal | | |
| C4 | 選一個**圓**，按 H | **拒絕**，且狀態列說明「這不是線」 | | |
| C5 | 選兩條線的相鄰端點，按 K（Coincident） | 兩點接在一起 | | |
| C6 | 只選一個點，按 K | 拒絕，並說明需要兩個點 | | |
| C7 | 選一個點，按 F（Fix） | 點被釘住；狀態列 DOF 數字下降 | | |
| C8 | 什麼都不選，按任一個約束鍵 | **有訊息**，不是靜悄悄什麼都沒發生 | | |

## D — 尺寸

| # | 做這個 | 應該看到 | Result | Notes |
|---|---|---|---|---|
| D1 | 選一條線，按 D | 畫布上出現尺寸標註（虛線 + 數值），面板多一列 Length，**幾何沒有移動** | | |
| D2 | 選一個圓，按 D | 產生 **Diameter**（不是 Radius） | | |
| D3 | 選一個弧，按 D | 產生 **Radius** | | |
| D4 | 選一個圓，按工具列的 Radius | 強制產生 Radius —— 覆寫推斷 | | |
| D5 | 選兩個點，按 D | Distance | | |
| D6 | 選兩條線，按 D | Angle，面板顯示的是**度**不是弧度 | | |
| D7 | 選三條線，按 D | 拒絕，且訊息**列出哪些選取組合是有效的** | | |
| D8 | 雙擊畫布上的尺寸數值 | 跳出編輯框，內容是目前的值 | | |
| D9 | 輸入一個新數字按 Enter | **幾何跟著動**；狀態列說明改了什麼 | | |
| D10 | 雙擊尺寸，輸入 `#d1 / 2`（用面板上看得到的參數名） | 接受，值變成一半 | | |
| D11 | 雙擊尺寸，輸入 `#Nope * 2` | 拒絕；訊息指出欄位位置；**原本的值沒有被改掉** | | |
| D12 | 對角度尺寸輸入 `45` | 存進去的是 45 度（不是 45 弧度） | | |

## E — 約束到完全定義

| # | 做這個 | 應該看到 | Result | Notes |
|---|---|---|---|---|
| E1 | 畫一個矩形，對底邊與右邊各下一個尺寸 | 狀態列仍是 `[DOF] Under constrained — DOF 2`（還能整體平移） | | |
| E2 | 對左下角的點下 Fix | 狀態列變成 `[OK] Fully constrained — DOF 0` | | |
| E3 | 對同一條線再下一個**不同值**的 Length | 狀態列變成 `[CONFLICT]` 或 `[REDUNDANT]`，**兩者是不同的訊息** | | |
| E4 | 看 Constraints 面板的 Status 欄 | 出問題的那幾列標 `AT FAULT`（文字，不是只有紅色） | | |
| E5 | 在面板選中那一列，Sketch → Constrain and Dimension → Delete Selected Constraint | 該約束消失，狀態回復 | | |
| E6 | 狀態列的 tooltip | 有 solver 的訊息與涉及的 ConstraintId | | |

## F — Undo

| # | 做這個 | 應該看到 | Result | Notes |
|---|---|---|---|---|
| F1 | 畫一條線，Ctrl+Z | 線消失 | | |
| F2 | Ctrl+Y | 線回來 | | |
| F3 | 畫一個矩形，Ctrl+Z **一次** | 4 條線與 8 個約束**一起**消失（一個動作 = 一步 undo） | | |
| F4 | 下一個尺寸，Ctrl+Z 一次 | 約束與它建立的 Parameter **一起**消失，model tree 沒有留下孤兒參數 | | |
| F5 | 刪掉一條帶約束的線，Ctrl+Z | 線與它的約束**都**回來 | | |
| F6 | 看 Edit 選單 | Undo 項目寫著它會 undo 什麼 | | |

## G — 出去，然後變成 3D

| # | 做這個 | 應該看到 | Result | Notes |
|---|---|---|---|---|
| G1 | 畫一個封閉矩形並標好尺寸，Sketch → Finish Sketch（Ctrl+Enter） | 回到 3D 視圖；sketch 工具列與 Constraints 面板消失；狀態列報告該 sketch 的約束狀態並提示下一步 | | |
| G2 | 該 sketch 已被選中；Insert → Pad from Selected Sketch | 產生實體 | | |
| G3 | 在 model tree 選回那個 sketch，Sketch → Edit Selected Sketch | 重新開啟畫布，幾何與約束都在 | | |
| G4 | 改一個尺寸，Finish Sketch | 3D 實體**跟著改變** | | |

## H — 整體判斷（只有 owner 能回答）

| # | 問題 | Result |
|---|---|---|
| H1 | 在你的 DPI 與主題下，尺寸數字、約束字母、吸附標記**看得清楚嗎**？ | |
| H2 | 吸附半徑（8 px）在你實際使用時是太黏還是太鬆？ | |
| H3 | 「選了再下命令」的順序，和你習慣的 CAD 一致嗎？ | |
| H4 | 尺寸用**單一 D 命令 + 推斷**（而不是每種尺寸一個命令）—— 你要留這個設計嗎？（`todo.md` §16 第 3 項的裁決） | |
| H5 | 「建立 sketch 不可 undo」你能接受多久？ | |
| H6 | 缺少 Parallel / Perpendicular / Tangent / Equal 對你的實際使用有多痛？（決定它們的排程） | |
| H7 | 整體上，你現在**能不能靠這個 UI 畫出一張你真的想要的圖**？ | |

---

## 已知缺口（不需在上面回報為失敗）

見 `docs/M12_SPEC.md` §9。摘要：

1. Parallel / Perpendicular / Tangent / Equal / Concentric / Midpoint / Point-on-object 尚未存在於 Core。
2. 沒有 driving / driven 區分，過約束不會自動降級（待 owner 裁決）。
3. 建立 sketch 本身不可 undo。
4. 不能拖曳幾何，只能用尺寸驅動。
5. 新 sketch 一律建在 world XY，尚無平面選擇 UI。
6. 尚未做 OCCT overlay（階段二）。
