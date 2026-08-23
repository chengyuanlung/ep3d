# M10 — Reference Frames & Connectors

## 1. Mission

M8 made a Body's shape a feature history. M9 made that history editable. M10
gives the model somewhere to *be*:

> A **ReferenceFrame** is a first-class document object with stable identity,
> a parent, a local transform, and a composed world transform. A Sketch can be
> **supported by a frame** instead of by an embedded plane, so moving the frame
> moves everything built on it. A **Connector** is a named, role-carrying frame
> on a part — the same abstraction that will serve Assembly mates, and USER and
> TOOL coordinate systems.

This is roadmap §5 (the sketch coordinate model), §18 (connector content), §21
(connector reuse), §35's third differentiator (one ReferenceFrame abstraction
serving Sketch / Part / Assembly / USER / TOOL) and §33's M10 rung.

The central release proof — chosen so that **volume cannot discriminate it**,
which is the mistake M8's GATE_RB2 was written to avoid:

> `Frame001` at the origin supports `Sketch001`, a 100 × 50 rectangle padded
> 20 mm: **100000 mm³, centre of mass (50, 25, 10)**. Translate the FRAME by
> +30 in Z and the volume is unchanged while the centre of mass becomes
> **(50, 25, 40)**. Rotate the frame 90° about X and it becomes
> **(50, −10, 25)**. The sketch is not edited, the pad is not edited, and the
> chain rebuilds selectively.

## 2. Baseline

M10 starts from `m8-wip` head — M9 implemented through M9.7.

**Stated plainly rather than implied, for the fourth milestone running:** M8 is
NOT closed and M9 has never been reviewed. The open items are:

1. **Owner UI validation: M6, M7 and M8**, every checklist row blank. Only the
   owner may fill them (ADR-M4-016). M9 has no checklist at all yet.
2. **M8 review round 4's fixes are unreviewed**, as every round's have been.
3. **M9 has had no independent review** — it is larger than any single M8
   slice, and the four M8 rounds found three Criticals and eleven Majors in
   work its author believed finished, two of them introduced by fixes.
4. **M6.11–M6.14 have never been reviewed by anyone but their author.**

**M10 cannot close before M9 closes, which cannot close before M8.** Starting
M10 here is a scheduling decision made explicitly by the owner, the fourth in a
row. Recorded so it is never later read as though the debt had been paid.

M10 must preserve every accepted M0–M9 contract.

## 3. Alignment (roadmap §32)

| | |
|---|---|
| Reference behavior | Roadmap §18 and §18.1–18.3, sourced to the Mate Connector page in §46 (A05): a connector is a named local coordinate system owned by a Part definition or an Assembly, reusable by every instance, with an explicit and an implicit creation route that differ only in *when* and *where listed*. Roadmap §5: a sketch may be placed on a plane, a planar face, or a Mate Connector — because a connector **is** a coordinate system |
| EP3D intended behavior | identical semantics on the EP3D graph: a frame is a registered, persisted, graph-participating object; a sketch references its support frame by ObjectId; a connector is a frame plus a role and an owner. Roadmap §5 is explicit that **sketch-on-frame is the same thing as sketch-on-connector and must not grow a second plane type** — M10 takes that route |
| Implemented before M10 | see §3.1 — the TYPES exist and nothing else does |
| Intentional differences | no attachment to a planar FACE yet (that needs the selection architecture and stable topological references — the same reason ADR-M8-006 deferred per-edge fillets); no Assembly, so a connector has no mate to participate in yet |
| Validation | Gates below + owner manual UI validation |

### 3.0 What the roadmap requires of a frame (§18.2, §35.3)

Roadmap §18.2 states the **minimum complete set** for mechanical alignment, and
§35.3 states that USER and TOOL coordinate systems need the same fields — which
is why one abstraction serves Sketch, Part connector, Assembly mate, USER and
TOOL, and why **§35.3 says M10 must precede M11**.

```text
origin + primary axis (Z) + secondary axis + offset + rotation
```

`ReferenceFrame` carries a `Transform3D` — a translation and a quaternion —
which expresses exactly an origin and an orientation, so the *data* is complete.
What §18.2 also lists are **operations** on it:

| §18.2 operation | M10 |
|---|---|
| Offset (X/Y/Z translation) | the transform's translation; `setFrameTransform` |
| Rotation by a given angle | the transform's rotation; `setFrameTransform` |
| Realign to a primary (and optional secondary) axis entity | **deferred** — it takes a *selected entity* as input, which needs the selection architecture the same way ADR-M8-006's per-edge fillets do |
| Flip primary axis 180° | expressible as a transform; a convenience command, M10.5 |
| Reorient secondary axis in 90° quadrants | same |

So M10 delivers the representation and the direct edits, and defers only the
operations whose *input* is a selected piece of topology. That boundary is the
same one M8 and M9 drew, and it is drawn here on purpose rather than discovered.

Roadmap §18.1 adds a rule M10.3 must honour: **implicit and explicit connectors
are both first-class** — the difference is when they are created and where they
are listed, never whether they have an ObjectId, because a mate built on
something that cannot be re-resolved violates A03 directly.

Roadmap §18.3 adds one M10 must NOT do: connector **visibility** is per-context
presentation state and stays out of Core (A02).

### 3.1 What exists today, verified by reading the code

Not assumed from the comments — checked, because M9's equivalent section found
two facts its own comments had wrong.

1. **`ReferenceFrame` and `Connector` are types with no document identity.**
   `PartDocument::addFrame` constructs the object and pushes it into a vector.
   It does **not** call `registry_.registerObject`, does **not** add a graph
   node, and there is no `restoreFrame`. `addConnector` is the same. So a frame
   has an `ObjectId` that nothing can resolve, and `removeObject` cannot see it.
2. **Neither is persisted.** The serializer mentions neither. ADR-009 D6 records
   this deliberately — "the Origin frame is re-created fresh on load" — and the
   `PartDocument` constructor does exactly that, calling `addFrame("Origin")`.
   A frame the user moves today is lost on save.
3. **A Sketch does not reference a frame.** `SketchFrame` is a *value* embedded
   in the Sketch — a `Transform3D` and nothing more. Its own header states the
   plan M10 is now executing: *"when frames do become first-class, a Sketch can
   gain an optional supportFrameId with this transform as the fallback, and
   neither entity storage nor the (u,v) convention changes."* M10 takes that
   route rather than inventing another.
4. **`frames()` is a known-open accessor door**, recorded in review round 3
   (R1R3-M2) as inert *"and closed the sketches()/bodies()/Parameter way **the
   day a consumer appears**"*. M10 is the day a consumer appears. Closing it is
   part of this milestone, not a follow-up.

So M10 is not "expose what is already there". Everything except the two structs
has to be built.

## 4. Scope

**Required for M10 to close:**

- **ReferenceFrame as a first-class object**: registry identity, graph node,
  parent hierarchy, composed world transform, undoable edits, const-correct
  accessors
- **Sketch-on-frame**: a Sketch optionally supported by a frame; moving the
  frame moves the sketch and everything downstream, selectively
- **Connector as a first-class object**: a named role on a frame, with an
  **owner** (§18.3), reusable, and first-class whichever route created it
  (§18.1)
- **Persistence (v10)**: frames, connectors, and a sketch's support reference
- failure semantics: a cyclic parent chain, a missing support frame, a deleted
  frame with dependents

**In scope, closable as deferred only by explicit ADR:** attaching a frame to a
planar face; Mirror and Pattern (ADR-M9-006 named M10 as their successor and
listed the four registration sites they need).

Out of scope: Assembly instances and mates (M11), motion (M12), USER/TOOL
runtime integration beyond making the abstraction ready for it, connector
visibility (§18.3 — presentation, not Core), and realign-from-selected-entity
(§3.0).

**M10 must precede M11** and the roadmap says why (§35.3): the shared
ReferenceFrame is the structural advantage EP3D is being built for, and an
Assembly built on anything else would have to be redone.

## 5. Architectural Rule — the frame graph

```text
World
 └─ Frame001 (local transform)          ← registered, persisted, graph node
     ├─ Frame002 (local, relative to Frame001)
     └─ Connector "ShaftAxis" (role, on Frame001)

Sketch001.supportFrameId ──▶ Frame001
        │
        └─▶ Pad001 ──▶ ...
```

- A frame's **world transform is composed**, never stored: `world(child) =
  world(parent) ∘ local(child)`. Storing both is how two truths disagree.
- A Sketch references its support frame **by ObjectId** (ADR-M4-004), never by
  pointer or index, and keeps its embedded `SketchFrame` as the fallback for a
  sketch with no support frame. A world-XY sketch stays a *case* of the general
  path, not a shortcut around it.
- The frame → sketch edge is a **dependency-graph edge**, wired by the facade
  exactly as Parameter edges are. Moving a frame dirties its sketches, which
  dirty their features, through the existing M2 machinery. Nothing new
  schedules.
- A frame's parent edge is a graph edge too, so a parent move reaches every
  descendant without anything walking the tree at recompute time.
- Connectors carry **role and owner**, not geometry. A connector *is* a frame
  reference plus meaning.

Forbidden: a cached world transform; a sketch storing a frame pointer; frames
outside the registry; a second conversion path from (u,v) to world (ADR-M4-002
made `SketchFrame::toWorld` the single site and it stays single).

## 6. Failure semantics

- A **cyclic** parent chain is refused at creation and at load, exactly as the
  dependency graph refuses one. A cycle discovered at recompute time would be
  an unbounded walk, and M9.1 already learned what an unbounded walk costs.
- A Sketch whose support frame is **missing** fails with a diagnostic naming the
  frame — it does not silently fall back to world XY, because a sketch that
  quietly relocates to the origin is the geometric twin of the stale-result
  defect this project has fixed three times.
- **Deleting a frame that supports a sketch** follows M4's accepted precedent
  for deleting a sketch a Pad reads (recorded in `removeObject`): deletion is
  allowed, the dependent fails LOUDLY, and save refuses to write a document with
  a dangling reference. Reversing that is the owner's call, not a side effect.
- A frame whose transform is non-finite fails; it does not produce NaN geometry.

## 7. Release gates (all analytically oracled, counters not equal-values)

Fixture: `Frame001` at the origin supports `Sketch001`, a constrained 100 × 50
rectangle, padded 20 mm. **Volume is 100000 mm³ in every gate below** — that is
deliberate, so no gate can pass on volume alone.

| Gate | Proof |
|---|---|
| **A** | identity: a frame is registered, resolvable by id, and has a graph node; `removeObject` finds it |
| **B** | hierarchy: `world(child) = world(parent) ∘ local(child)`, hand-composed; a cyclic parent is refused |
| **C** | frame edits are undoable, and one edit is one step |
| **D** | **the release proof**: frame at origin → COM **(50, 25, 10)**; frame +30 Z → COM **(50, 25, 40)**; frame rotated 90° about X → COM **(50, −10, 25)**; volume 100000 throughout |
| **E** | selectivity: moving the frame re-solves nothing and re-extrudes once — solver calls unchanged, exactly one extrude |
| **F** | a parent move reaches a grandchild frame's sketch: two levels, hand-composed COM |
| **G** | connectors: registered, resolvable, carrying role and frame; a connector on a moved frame reports the moved world transform |
| **H** | v10 round-trips frames, the parent hierarchy, connectors and `supportFrameId`; a fresh load reproduces gate D's COM exactly |
| **I** | failure: missing support frame → sketch fails with a diagnostic naming it, never a silent fallback to world XY; save refuses a dangling support reference |
| **J** | full M0–M9 regression, both configs, single-process and shuffled seeds |

Mutation minimums: world transform cached instead of composed → Gate F fails;
the frame → sketch edge unwired → Gate E fails; `supportFrameId` dropped from
the serializer → Gate H fails; the missing-frame fallback made silent → Gate I
fails; the cycle check removed → Gate B fails.

## 8. Adversarial (beyond the gates)

A frame parented to itself; a two-frame cycle; a frame deleted while a sketch
uses it; a sketch pointed at a non-frame id; a connector on a deleted frame;
a frame moved inside an aborted edit session; a frame move undone across a
rollback position; a non-finite transform; a deeply nested chain (10 levels);
a sketch switched from embedded plane to frame support and back; save/load at
every one of those states; whole-suite single-process and shuffled runs.

## 9. Required ADRs

- ADR-M10-001 — a frame is a first-class object; what changes from ADR-009 D6
- ADR-M10-002 — the world transform is composed, never stored
- ADR-M10-003 — sketch support: an optional frame reference with the embedded
  plane as fallback, and why a missing frame fails rather than falls back
- ADR-M10-004 — a connector is a frame plus role and owner
- more as implementation discovers durable decisions

## 10. Slices

```text
M10.1 — DONE  ReferenceFrame first-class: identity, graph, hierarchy, undo,
              const-correct accessors; gates A-C
M10.2 — DONE  Sketch-on-frame; gates D-F
M10.3 — DONE  Connectors first-class; gate G
M10.4 — DONE  persistence v10; gates H-I
M10.5 — DONE  UI: frames and connectors in the tree, transform in the panel
M10.6 — DONE  Mirror / Pattern on the new frames (gates M/N/O, P/P2); the
              deferral was CLOSED, not repeated
M10.7 — DONE  self-validation + mutation audit (19 + 1 kernel mutation;
              docs/reviews/M10_SelfValidationReport.md)
M10.8 — OPEN  independent review + close (blocked on M9, which is blocked
              on M8; NOT authorized by the owner)
```

All implementation slices are delivered. Each later slice refined its section
before implementation, as M7, M8 and M9 did; what M10.1's revision authorized
in detail was M10.1 alone.

## 11. Definition of done

Required capabilities implemented; gates A–J pass; adversarial matrix green;
mutation-verified; v10 round-trips; M0–M9 regressions pass both configs
including single-process and shuffled seeds; ADRs recorded; self-validation
written as claims; independent review with no unresolved Critical/Major; owner
UI validation — **and M9's open items closed, which includes M8's, which
includes M7's, which includes M6's.**
