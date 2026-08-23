# `spline-and-ellipse.ep3d`

> 同一個零件也有**腳本版**：`spline-and-ellipse.ep3ds`。
> ```
> ep3d --script examples/spline-and-ellipse.ep3ds --out examples/from-script.ep3d
> ```
> `ep3d --help` 會列出全部的指令、工具名、約束名和尺寸名。
> 腳本裡的每一行都走**滑鼠那條路** —— `click` 會經過真正的吸附器。
>
> 也可以**直接控制開著的視窗**（幾何會一行一行出現在畫面上）：
> ```
> ParametricCADViewer --listen          # 預設 127.0.0.1:5310
> ep3d --connect --script examples/spline-and-ellipse.ep3ds
> ep3d --connect                        # 從 stdin 讀，當主控台用
> ```
> **只綁 loopback，而且沒有旗標可以放寬** —— 這條路會執行儲存檔案的指令。

用 EP3D 開啟它。裡面兩個草圖、兩個 Pad，示範**橢圓和雲形線目前能加哪些約束**，
以及**加不了哪些、為什麼**。

檔案是用 `tools/MakeExampleEp3d.cpp` 透過**一般的 document API** 產生的，
不是手寫 JSON —— 手寫的範例是這個格式的第二份定義，一開始一致、然後就不一致了。
重新產生：`cmake --build build --config Debug --target MakeExampleEp3d && ./build/Debug/MakeExampleEp3d.exe`

---

## 草圖 1：`EllipseRing` —— 橢圓，**完全約束（DOF 0）**

一個橢圓是**五個數字**：圓心（2）、長半軸、短半軸、旋轉。
所以要五條方程式，而 EP3D 每一條都有一個對應的指令。

| # | 約束 | 指令 | 拿掉 |
|---|---|---|---|
| 1–2 | `Fix` 在**圓心**上 | 選圓心 → Fix | 2 |
| 3 | `MajorAxis` = `MajorA` (40) | 選橢圓 → **Major axis** | 1 |
| 4 | `MinorAxis` = `MinorB` (15) | 選橢圓 → **Minor axis** | 1 |
| 5 | `EllipseRotation` = `EllipseAngle` (30°) | 選橢圓 → **Ellipse angle** | 1 |

**合計 5，DOF 歸 0。**

再加一個同心圓當作孔：

| 約束 | 說明 |
|---|---|
| `Concentric`（橢圓, 圓） | 橢圓**可以**同心 —— 共用圓心不牽涉任何一邊有幾個半徑 |
| `Radius` = `HoleR` (8) | 那是**圓**的半徑，不是橢圓的 |

Pad 之後是一個橢圓環。

### 橢圓上**不能**做的事，以及為什麼

| 你會想做的 | 會發生什麼 |
|---|---|
| **Radius / Diameter 標註** | **拒絕**，並告訴你改用 Major/Minor axis。橢圓有兩個半徑；照做會默默驅動長軸 |
| **相切**（線或曲線對橢圓） | 尚未支援 —— 需要在橢圓上解出接觸點 |
| **Trim / Split** | 尚未支援 —— 找出某物和橢圓的交點是四次方程式 |
| **Offset** | 離橢圓固定距離的曲線**不是橢圓**（是八次的偏置曲線），沒有形狀可以放 |
| **Mirror** | 反射本身簡單，但鏡射會**綁住**複本，而兩個橢圓之間沒有可以綁形狀的相等約束 |

每一個都會**說明原因**，不是靜靜地什麼都不做。

---

## 草圖 2：`SplineBlade` —— 五點雲形線，**DOF 6**

這個數字是**真的**，不是遺漏。

雲形線通過五個點 = **十個數字**。而 `SketchElementRef` 是
「entity + sub-element」，sub-element 只有四種 ——
所以**只有頭尾兩個點有名字可以給約束用**。中間三個點是求解器會動的變數，
但沒有任何約束叫得出它們。

| 約束 | 拿掉 |
|---|---|
| `Fix` 在弦線的一端 | 2 |
| `Horizontal`（弦線） | 1 |
| `Length` = `BladeSpan` (120) | 1 |
| `Coincident`（雲形線**起點** ↔ 弦線端點） | 2 |
| `Coincident`（雲形線**終點** ↔ 弦線起點） | 2 |

弦線 4 個自由度被前三條拿光；兩個 Coincident 把雲形線的兩端釘在上面。
雲形線的 10 − 4 = **6** 留給中間那三個點。

**這正是設計上的取捨，而且它是說出來的**：把端點接到鄰居上，是 profile 需要的，
而那個是能用的 —— 這個草圖照樣 Pad 得出實體。
要讓中間的點也能被約束，`SketchElementRef` 必須能帶一個索引，那還沒做。

### 雲形線上**不能**做的事

| | 為什麼 |
|---|---|
| **Trim / Split** | 縮短一條雲形線等於移動它通過的點，而哪些點該動不是一次點擊說得出來的；切出來的兩段會是「通過沒人點過的點」的曲線 |
| **Offset** | 離雲形線固定距離的曲線，不是通過任何一組點的雲形線 |
| **Mirror** | 只有兩端叫得出名字，複本的中間綁不住原件的中間 |

---

## 畫這兩個東西

**橢圓**（Shift+E）：點圓心 → 點**長軸**那一端 → 點一下設定寬度。
第二下同時給了長半軸**和**旋轉 —— 橢圓的旋轉是相對長軸量的，
所以指著長軸那一端，一個手勢就把必須一致的兩件事講完了。
第三下用的是它到長軸的**垂直**距離，沿著軸滑動不會改變寬度。

**雲形線**（Shift+N）：點每一個要通過的點，**雙擊**結束，
或**再點一次第一個點**把它封起來（封閉的雲形線像圓一樣自成一個 loop，沒有端點）。

---

## 這個範例找到的一個 bug

第一次產生它的時候，橢圓在存檔前是 DOF 0，**重新載入後卻拒絕求解**：

```
constraint 11: a dimensional constraint is not bound to any Parameter
```

序列化的**寫入端**是泛用的（`BoundParameterId`），
**讀取端**卻是每一種約束各自手寫一行 `c.parameterId = parameterRef();` —— 八份拷貝。
M17.25 加了兩個尺寸約束，兩份都沒寫。
它們**存檔完全正確**，回來時綁在空的上面。

讀取端現在也是泛用的：`IsDimensional` 加上 `VisitBoundParameter`，同一份清單反過來讀。
`M17_SER_043` 用**能力**（而不是一份要記得擴充的清單）走過每一種尺寸約束，把它釘住。

## `spline-tangent-profile.ep3ds` (M18)

A line and a spline that meet **smoothly**. Before M18 the tangency was refused
outright, so a spline could not be part of any profile without a visible kink --
and the solid built from it had the kink too, because the kernel was choosing
its own end condition rather than being told the sketch's.

It ends with `measure`, which is new in M18: a script can now check what it
built instead of the reader opening the saved file and reading numbers by hand.

```
ep3d --script examples/spline-tangent-profile.ep3ds
```
