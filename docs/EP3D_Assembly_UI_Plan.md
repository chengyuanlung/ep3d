# EP3D 組合件 UI 實作計畫（區塊 G，M27–M30）

> **現況（2026-08-24 查證，不是憑印象）**
>
> ```
> grep -rn "ep3da" src/Viewer/   →  沒有任何一筆
> ```
>
> **Core 是完整的。** `AssemblyDocument` 有 instance、七種 mate、接地、組合件層級陣列、
> 具名姿態、爆炸圖、干涉檢查、次組合件、Gauss-Newton 求解器，序列化到 v33。
> `examples/hinge.ep3ds` 120 行、`four-bar.ep3ds` 解閉環、`sub-assembly.ep3ds` 154 行，
> 全部從腳本跑得起來。
>
> **而視窗連 `.ep3da` 這個副檔名都不認識。** 組合件目前只活在腳本和檔案裡。
>
> 這份計畫就是把那件事補起來，並且順便付掉 P3 欠的最後兩項。

---

## 一、為什麼這件事一定要從「第二個文件型別」開始

ADR-M23-006 把 P3 的五項裡的兩項——**選取**和 **UI 樹**——刻意記成一筆帳，
理由寫得很清楚：

> 組合件還沒有 UI，而替一個還沒有第二個使用者的東西做抽象是猜測，
> 猜錯的抽象比重複更難拆。

**這份計畫就是那個第二個使用者。** 所以第一個里程碑不是「畫組合件的 UI」，
而是「讓 shell 知道文件有兩種」——否則後面每一項都會被寫成第二份。

現況的耦合是具體的、可量的：

```
MainWindow(PartDocument& document, DocumentPresenter& presenter)
PartDocument* document_;                    // 146 處 document_->
DocumentPresenter(PartDocument& document)   // displayableSolids / displayableSketches
DocumentOutline(const PartDocument& document)
```

**兩條路，只有一條走得通：**

| | 做法 | 代價 |
|---|---|---|
| ❌ | 另開一個 `AssemblyWindow` | 選單、工具列、樹、屬性面板、undo、存檔**各一份**——正是 P3 點名的那種重複，而且是它列出的五項裡的四項 |
| ✅ | 把 shell 抬到 `DocumentBase`，用**能力**分辨兩種文件 | 一次性的抬升成本，之後第三個文件型別（Drawing）幾乎免費 |

走第二條。而且**由組合件的真實需求驅動**，不是先猜一個抽象再去找使用者。

---

## 二、路線圖對這件事的約束（不是我發明的規則）

實作前先把 roadmap 已經裁決過的事列出來，免得在 UI 層重新發明一次：

| 出處 | 規則 | 對 UI 的意思 |
|---|---|---|
| **§19** | Assembly placement **不可 bake 回 Part geometry** | 拖動 instance 改的是 `instanceTransform`，永遠不碰零件檔 |
| **§14 / §14.1** | Preselection 是**候選點產生器**，不只是變色 | Mate 指令進行中 hover 一個面，要浮出形心／邊中點／圓孔圓心等**推導出來的**候選點 |
| **§14.1** | **推導候選點不是文件物件** | 它們**沒有 ObjectId**，不進 document；已建立的 Connector 才有。兩者視覺上也必須分得開 |
| **§20.6** | Snap mode：把 connector 拖到 connector 上 | 放開後**開對話框讓使用者選型別**，**絕不自動決定 mate 型別**；被拖的 instance 變半透明 |
| **§20.3** | DOF 回報要把「mate 提供的 DOF」和「relation 耦合後的實際 DOF」**分開** | 否則使用者看不懂為什麼還有自由度卻動不了 |
| **§49** | 姿態／顯示狀態／爆炸圖是**三個獨立機制** | 不可合併成一個「view state」，那會讓 A02 的 document/presentation 分界失守 |
| **§49** | Exploded view **自帶 rollback bar** | 已經有 `EvaluationCut` 了（M26 抽的），直接用，不要再寫一份 |
| **§10.1 / §44** | 對話框標題**紅=不可 commit**；欄位**藍底=要從畫面選取** | Mate 對話框照這個規則，狀態詞彙用 §44.2 那張表，不要每個里程碑自己發明 |
| **§20.1** | Tangent mate **不使用 mate connector** | 它的對話框跟其他六種**不一樣**，不要硬塞進同一個流程（ADR-M25-007 已經為此拒絕實作它） |

---

## 三、里程碑

### M27 — shell 知道文件有兩種（付 P3 的帳）

**這一個里程碑不加任何組合件功能**，它只讓現有的 shell 能拿著一個 `AssemblyDocument`
而不崩潰。這是刻意的：混在一起做，就分不清哪個失敗是抬升造成的、哪個是新功能造成的。

1. **`MainWindow` 持有 `DocumentBase*`**，加上 `part()` / `assembly()` 兩個**有檢查**的存取器
   （`RecomputeContext::part()` 已經是這個形狀，照抄它，不要發明第二種）。
   146 處 `document_->` 逐一分類：屬於 `DocumentBase` 的直接留下，
   屬於零件的走 `part()`，而**走不通的那些就是這個里程碑真正的產出**——
   它們是 shell 裡真正假設了「文件就是零件」的地方。

2. **`DocumentOutline` 拆成兩個 builder，共用一個節點型別。**
   樹的**節點**（`OutlineNode`、`OutlineState`、狀態標記）是共用詞彙，
   **怎麼組出來**是每個文件型別自己的事。組合件的樹是：

   ```
   [Asm] Hinge
     ├ [Ins] Base           Grounded
     ├ [Ins] Swing          3 DOF
     ├ [Mat] Hinge          Revolute
     ├ [Pos] Open / Closed
     └ [Exp] Assembly steps
   ```

3. **`DocumentPresenter` 的顯示單位從「實體」變成「擺好位置的形狀」。**
   零件：形狀 + 單位變換。組合件：`instance->currentShape()` + `instanceWorldTransform()`。
   kernel 的 `placeShape` 已經有了。

4. **`File > Open` 問檔案，不問副檔名**（ADR-M26-005 已經裁決過這條）。
   `readHeader` 讀 `documentType`，再決定叫 `loadPartDocumentFromFile` 還是
   `loadAssemblyDocumentFromFile`。

5. **選取**：`selectedId_` 已經是 `ObjectId`，而 instance／mate 的 id 來自同一個產生器，
   所以選取本身**可能不用改**——這件事要**先量再說**，不要假設。

**Gate**：在視窗裡開 `examples/hinge.ep3da`，樹上看到兩個 instance 和一個 mate，
畫面上兩個零件**在 mate 解出來的位置**，`--selftest` 有一個組合件樣本走完全程。

**風險**：這是唯一一個「大改既有程式碼」的里程碑。做法是**先讓測試變成兩份**
（同一組 shell 斷言，一次餵零件、一次餵組合件），再動 shell。

---

### M28 — Instance：插入、擺放、接地、陣列

Core 全部就緒（`addInstance` / `setInstanceTransform` / `setInstanceGrounded` /
`addInstancePattern`），這個里程碑是把它們接到手上。

- **Insert > Part…** 檔案對話框 → `addInstance`。放置預設對齊來源原點（§1973）。
- **拖動 instance**：改 `instanceTransform`，**永不** bake 回零件（§19）。
  被 mate 定住的自由度拖不動，而**拖不動要說得出理由**（不是默默不動）。
- **接地／解除接地**：右鍵 + 樹上的徽章。
- **組合件層級陣列**：`addInstancePattern`，複本掛在原件座標系下（ADR-M26-003）。
- **刪除 instance**：連同**指向它的 mate**一起處理——刪除前要說清楚會帶走什麼。

**Gate**：完全用滑鼠組出一個三件式組合件，存檔、重開，位置一模一樣。

---

### M29 — Mate：connector 選取器與 mate 對話框

**這是整個計畫最難、也最能決定「組合件到底能不能用」的一塊。**

1. **Connector 的顯示與選取（§18）**
   - 已建立的 Connector：畫出來、可點、**有 ObjectId**。
   - **推導候選點（§14.1）**：mate 指令進行中 hover 才出現——面／輪廓形心、邊中點、
     頂點、圓孔圓心。**不進 document、沒有 ObjectId**，視覺上與已建立的明顯不同。
   - 這條分界是 §14.1 明文要求的，**在第一版就要分清楚**，
     因為事後要把「使用者以為自己建立了一個 connector」拆開，是改資料模型。

2. **Mate 對話框（§10.1 的規則）**
   - 型別、兩個 connector、對齊方向。
   - **標題紅色 = 還不能 commit**；欄位**藍底 = 要從畫面選取**。
   - 六種用 connector 的 mate 走這個流程；**Tangent 不走**（§20.1，它不用 connector）。

3. **DOF 回報（§20.3）**：每個 instance 顯示剩餘自由度，
   而且「mate 給的 DOF」與「實際可動的 DOF」**分兩欄**。
   `MateSolveReport` 和 `NumericalRank` 已經算得出來了。

4. **極限（limits）**：`setMateLimit` 已經有，UI 要能設、能看見被夾住。

5. **Snap mode（§20.6）** ——可以放到最後，它「不引入任何新語意，屬純 UI」：
   把一個 connector 拖到另一個上，被拖的 instance 半透明，放開後**開對話框**。

**Gate**：完全用滑鼠做出 `examples/hinge.ep3ds` 那個鉸鏈，拖動它，
在極限內轉得動、到極限停得住。

---

### M30 — 組合件狀態集：姿態、爆炸、顯示狀態

§49 的三個機制，**保持三個**。

- **具名姿態**：擷取／套用／刪除。套用是**一步 undo**（M26 已經這樣做了）。
- **爆炸圖**：步驟清單，可命名、重排、刪除，**自帶 rollback bar**——
  直接用 M26 抽出來的 `EvaluationCut`，不要寫第二份。
- **顯示狀態**：**M26 刻意沒做的那一項**，理由是「沒有組合件 UI 可以隱藏任何東西」。
  現在有了，所以它現在可以做——而且它是 **presentation，不進 Core**（A02）：
  它住在 presenter 裡，跟 `hiddenIds` 同一層。
- **干涉檢查**：`checkInterference` 已經有，UI 要能跑、能把結果**指到那一對零件上**。

**Gate**：M26 的三個範例（姿態、爆炸、次組合件）全部從 GUI 重現得出來。

---

## 四、每個里程碑的完成條件

沿用計畫第四節那四條，一條都不減：

1. **可執行的樣本** —— `--selftest` 要有一個**組合件**樣本從頭走到尾。
2. **突變測試** —— 而且 `tools/mutate.py` 現在**看得見 shell** 了（ADR-M26-014），
   所以 viewer-only 的改動不再是隱形的。
3. **沒有新接縫** —— 特別是：零件和組合件之間任何「兩份必須一致」的東西，
   要用單一型別／capability 收掉，做不到就在 ADR 裡寫明為什麼。
4. **ADR** —— 決定了什麼、為什麼、放棄了什麼、突變結果。

---

## 五、已知的風險，先寫下來

1. **M27 是唯一的大手術。** 146 處 `document_->` 要分類。
   緩解：先把 shell 測試變成兩份（同一組斷言、兩種文件），再動程式碼。

2. **3D 選取要指到 instance，不是 feature。** OCCT 的 pick 目前回到 feature 的
   provenance。組合件裡使用者點的是「那個零件」，而同一個零件檔可能有五個 instance——
   **五個 instance 共用同一份幾何，但選取必須分得出是哪一個**。
   這件事要在 M27 就量清楚，不要拖到 M29 才發現。

3. **推導候選點 vs 已建立 Connector** 如果第一版沒分清楚，之後要拆的是資料模型，不是 UI。

4. **Tangent mate 沒有實作**（ADR-M25-007），所以 mate 對話框的型別清單會少一個。
   這是誠實的缺口，不是 UI 的問題——不要為了讓清單好看而假裝支援它。

5. **這份計畫不含 Drawing。** 區塊 D（M27–M29 原編號）現在往後排。
   兩者都要用 P3 抬升的成果，而組合件是**更成熟的那個**——它的 Core 已經完整，
   Drawing 的還一行都沒有。先做有 Core 的那個，抬升才有真實需求驗證。
