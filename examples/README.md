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

## `spline-handle.ep3ds` (M18)

Two splines through the **same three points**; only the second has been told
which way to leave the middle one. Their lengths differ, which is the whole
point of a handle: it changes the shape without adding a point that would then
need constraining itself.

It goes on to hold the tangent level with a vertical distance of nought between
the point and its handle's tip -- no constraint was written for the purpose,
because a handle's tip is an ordinary point.

```
ep3d --script examples/spline-handle.ep3ds
```

## `sweep-and-loft.ep3ds` (M19)

A bent pipe and a tapered tower, in one script. Both need sketches on
**different planes** -- which is why `sketch` learned to take one: a section
swept along a spine drawn on its own plane has no volume at all, and a loft
between two sections in the same place has nothing to run through.

It ends with a bare `measure`, which reports the mass properties of the solid
that was built. That is the evidence a script has that a feature produced
anything; before it, the only signal was that nothing complained.

```
ep3d --script examples/sweep-and-loft.ep3ds
```

## `shell-draft-hole.ep3ds` (M20)

An enclosure: a block, tapered on all four walls, hollowed to a 3 mm shell with
an open top, and drilled with four mounting holes through the floor.

All three features name **faces**, and none of them names an index -- what is
stored is a sentence (`the top face`, `every face pointing +y`) that is
answered again against whatever the part currently is.

The order matters and the script says why: drafting before shelling tapers the
walls, and shelling first would taper the cavity as well.

```
ep3d --script examples/shell-draft-hole.ep3ds
```

## `patterns-and-booleans.ep3ds` (M21)

One body holding **two separate solids**, cut into one with a boolean; a
six-tooth rotor from a circular pattern; and five studs spaced along a curve.

Multi-body here means what it means in a Part Studio: a Body is a feature
chain, and two features in it that nothing consumes are two disjoint parts.
`pad` twice into the same body to make them, and `subtract` to join them.

Every step ends in a `measure` whose expected value is written in the script
as arithmetic, so the numbers can be checked by reading.

```
ep3d --script examples/patterns-and-booleans.ep3ds
```

## `step-round-trip.ep3ds` (M22)

The way **out**, and the way back. A shelled, drilled part goes out as STEP and
comes back as an import; the same part goes out twice as STL at two
deflections, coarse and fine, and the coarse file is visibly the smaller one.

The format comes from the **extension**, because there is no format argument:
a `.step` written as STL would be a file whose name lies about it, and the
reader at the far end would refuse it for a reason naming neither EP3D nor the
choice that caused it.

An import stores the **path**, not the geometry -- the same decision as a face
query storing `the top face` rather than face #4. So the file is re-read on
every rebuild, a re-exported source shows up in the model, and a source that
went away stops the feature by name. The script ends by drilling the imported
solid, which is the part a mesh could not have done.

```
ep3d --script examples/step-round-trip.ep3ds
```

## `assembly-three-parts.ep3ds` (M23)

Three parts put together. One script does both halves -- it **builds** the
parts, saves them as files, and then assembles them -- because an assembly
stores the **path** to each part, so the parts have to be real files before
anything can be inserted.

An instance names a file and a body inside it: the sentence "that body, in that
file". Not a copy of the geometry. A part that is edited shows up in the
assembly on the next rebuild, and a part whose file goes away stops its
instance **by name** rather than leaving a copy nobody can trace back to
anything.

Where an instance sits is a **reference frame** -- there is no second transform
anywhere, in memory or in the file -- so moving one is undoable, dirties
exactly what it should through an ordinary graph edge, and sub-assemblies in
M26 will be a frame parented to a frame rather than a new way of saying where
something is.

After `assembly NAME`, the words `solve`, `measure` and `save` are about the
assembly rather than the part, and the log says so at the moment it changes.

```
ep3d --script examples/assembly-three-parts.ep3ds
```

## `hinge.ep3ds` (M24)

A hinge that turns. Read the gate carefully: **it turns, and it does not fall
apart while turning.** Turning needs one number. Not falling apart needs the
solver to put the arm where the mate actually says it goes, at every angle.

One script draws a bracket with a pin, draws an arm, puts a **mate connector**
on each, assembles them, and drives the joint. The connector lives on the
**part** -- define "the pivot" once and every instance of that bracket in every
assembly has it -- and the three numbers after its position are where its **+Z
points**, which is the whole act of saying where the hinge is: a revolute turns
about +Z and a slider slides along it.

Something has to be **grounded**. A mate says where something is relative to
something else, so a chain of them has to start from something that is not
relative to anything; an assembly whose mates reach no ground is refused rather
than started from whichever instance was typed first.

The arm's centre of mass is what the script measures at each angle. Its z never
changes, because the hinge axis is z -- an arm that had come off the pin would
report a plausible number rather than an error, which is why the measurement is
the test and the picture is not.

```
ep3d --script examples/hinge.ep3ds
```

## `four-bar.ep3ds` (M25)

A four-bar linkage. Drive one link and the other three follow.

This is what M24 could not do and said so by name: a tree of mates is solved
exactly, one step at a time outwards from the ground, but a closed loop has no
first step. M24 refused it rather than approximating, because an approximate
linkage is one that does not close and it looks perfectly convincing in a
picture. M25 buys the iterative solver.

The lengths are 100, 30, 110, 60 -- a **Grashof crank-rocker**, so the crank
goes all the way round. Pick them wrong and "turn it a full circle" is a demand
the mechanism cannot meet.

`drive` means **driven**: that mate holds its value and the solve moves the
rest. Saying which is which is the difference between a mechanism and a
contradiction -- four revolutes with four fixed angles is almost never a closed
loop.

The script also shows a **limit** (a drag past a stop stops, and says so) and an
**interference** check (separate from mates, because a perfectly legal set of
mates can still drive two parts through each other).

Two defects this example found, both of which it now guards:
`drive` used to set the number without marking the mate driven, so the solver
moved the crank anyway; and two connectors could share a name in one document,
which made every mate in the linkage resolve to the first link's pin.

```
ep3d --script examples/four-bar.ep3ds
```

## `sub-assembly.ep3ds` (M26)

An assembly inside an assembly, and three ways to show one.

`insert` does not care whether the file holds a part or an assembly — it reads
the file's own `documentType` and asks the right question. There is **one**
instance type either way, because a mate names an instance by id and two kinds
would mean every mate lookup, rename, deletion and save had to ask which.

Moving a sub-assembly moves everything inside it, and nothing was told about
the parts: that is what M23 bought by making a placement a **frame** rather than
a transform of its own.

The three state mechanisms stay three, because roadmap §49 says they capture
three different kinds of thing:

- a **named pose** is geometry evaluation input — the mates' freedoms plus where
  the loose parts were put. Apply it and the model rebuilds. Not a
  configuration: a configuration changes what the model *is*.
- an **exploded view** is a derived picture. It never changes the model, it has
  its own preview position, and its steps can be named, reordered and deleted.
- a **display state** is pure presentation, and is deliberately absent: A02
  keeps presentation out of Core and there is no assembly UI to hide anything
  in yet.

The script also shows a **row** of instances — each copy's placement frame hangs
off the original's, so moving the original moves the row.

Two things this example forced: `part`, to switch back to the part document
after starting an assembly (without it a rig built from sub-assemblies could not
be written by one script at all), and lifting a deleted frame's children onto
its parent so a patterned copy does not silently jump when its original goes.

```
ep3d --script examples/sub-assembly.ep3ds
```

## `gear-train.ep3ds` (M31)

Two arms on one plate, geared 2:1. Turn the first and the second turns twice as
far -- and **nobody drives the second one**.

That is the whole check, and it is why the script measures rather than draws.
The second arm's angle is not typed anywhere. A relation that were stored and
never applied -- the exact failure this milestone exists to avoid -- would let
this script run, save, and leave the second arm sitting at zero, and every test
that read the log instead of the geometry would pass.

A relation couples two **freedoms**, not two things (roadmap 20.5). A gear names
a rotation on each of two mates; a **screw** names the rotation and the
translation of ONE mate, because a screw turns and advances at once. Built on
"a relation joins two objects", the screw is a special case forever.

The ratio's unit comes from the type: turns per turn for a gear or a linear
ratio, **millimetres per turn** for a screw or a rack -- which is how a lead
screw and a rack are actually quoted. The conversion to radians happens in
exactly one place, so no caller can get 2*pi wrong on its own.

Which freedom on each mate is not typed either: a gear takes each mate's first
free rotation. One rule, shared by this script, the Assembly menu and the
loader, so a click and a script cannot produce different gears.

```
ep3d --script examples/gear-train.ep3ds
```

---

## `stepper-motor.ep3ds` (M26)

```
ep3d --script examples/stepper-motor.ep3ds
```

一台 **NEMA 23 步進馬達**。裡面每一個數字都是規格書上的數字，
所以做出來的東西可以拿**規格書**去對，而不是拿它自己去對：

| | |
|---|---|
| 機殼 | 56.4 mm 見方，四角切掉 4 mm |
| 本體長度 | 76 mm |
| 定位凸台 | Ø38.1 mm，凸出端面 1.6 mm |
| 軸 | Ø6.35 mm，自安裝面起 21 mm |
| 安裝孔 | Ø5.1 mm，47.14 mm 方陣，深 12 mm |

**它示範三件別的範例沒有的事：**

**一、八角形靠「鏈」閉合，不是靠數字剛好對上。**
`tool line` 連續九次 `click`，最後一次回到第一個點 ——
每個轉角拿到的是**真的 Coincident 約束**。

**二、螺栓孔位是用「約束」說出來的，不是打四組座標。**
下面兩點共用一個 v、上面兩點共用一個 v、左右各共用一個 u，
然後**一個橫向尺寸加一個縱向尺寸**定住整組的大小 —— `Mounts` 草圖解到 **DOF 0**。
這裡的 `constrain horizontal` 吃的是**兩個點**（M26.3 的點對點形式）。

**三、負的孔深往反方向鑽。** 孔的草圖在安裝面上（法線 +Z），
而孔要往**下**鑽進端蓋，所以深度寫 `-12`。

### 這個範例自己抓到的一個錯

第一版的四角切了 **8 mm**。結果：四個螺栓孔的孔壁**從倒角面破出去**了。

而零件照樣建得起來、照樣存得了檔、從正面看起來也完全正常。

抓到它的是 `measure` —— 體積顯示只少了 **2.87 個孔**的材料，而腳本要的是四個。
**算術對照封閉解是唯一看得見這件事的檢查**：數特徵數不出來，數實體數不出來，
問包圍盒也問不出來。`M26_CLI_008` 就是那道門。
