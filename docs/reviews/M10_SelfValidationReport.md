# M10 Self-Validation Report — Reference Frames & Connectors

> **These are CLAIMS, not facts.** M7's report contained two false claims, M8's
> five stale counts and a miscredited guard, M9's own gates caught four defects
> in tests this author had just written, and M10 caught five more — including one
> in a kernel function that predates it. Read accordingly; read
> `docs/DecisionLog.md` ADR-M10-001..005 for the decisions rather than this
> summary.

**Baseline:** `m8-wip` head, M9 implemented through M9.7.
**Branch:** `m8-wip`, not pushed. `git log` is the authority.

**Standing obligations, restated — this is the FIFTH milestone opened while the
previous one's validation is unfinished:**

1. **Owner UI validation: M6, M7, M8, M9 and M10** — every checklist row blank
   in all five. M9's and M10's checklists now EXIST
   (`M9_UI_UserValidation.md`, `M10_UI_UserValidation.md`); writing them is the
   part an agent may do, and it is the only part that is done. Only the owner
   may fill a Result cell (ADR-M4-016).
2. **M8 review round 4's fixes are unreviewed.**
3. **M9 has had no independent review.** Neither has M10.
4. **M6.11–M6.14 have never been reviewed by anyone but their author.**

**M10 cannot close before M9, which cannot close before M8.** Nothing below
changes that, and M10 having gone further does not make any of them finished.

---

## What M10 delivered

| Slice | Content |
|---|---|
| M10.1 | `ReferenceFrame` first-class: registry, graph node, parent hierarchy with cycle refusal, composed world transform, undo deltas, const-correct accessors, private mutators |
| M10.2 | Sketch-on-frame: optional `supportFrameId`, the embedded plane as fallback, a frame→sketch graph edge, loud failure when the frame is gone |
| M10.3 | `Connector` first-class: role, owner, frame reference, world transform through its frame |
| M10.4 | **Schema v10**: frames, the parent hierarchy, connectors, and `supportFrameId` |
| M10.5 | Shell: frames and connectors nested in the model tree, local/world rows in the panel, a sketch's Support row, `--sample m10-frame` |
| M10.6 | **Mirror and Pattern** — ADR-M9-006's deferral CLOSED, with kernel verbs `mirrorShape` / `translateShape` / `fuseShapes` |
| M10.7 | this report |

Six ADRs: M10-001 (frames are first-class; supersedes ADR-009 D6), M10-002 (the
world transform is composed, never stored; ADR-M4-002's single site moved),
M10-003 (sketch support and why a missing frame fails), M10-004 (a connector is a
frame plus meaning), M10-005 (mass properties are summed per solid), M10-006 (a test that needs an
explicit id allocates one).

---

## Release gates (M10 spec §7)

Every gate below holds volume at **100000 mm³ deliberately**, so the centre of
mass is what discriminates. That is M8 GATE_RB2's lesson applied in advance —
and §"What the gates caught" shows it was needed.

| Gate | Proof | Result |
|---|---|---|
| **A** | a frame is registered, resolvable, a graph node, and `removeObject` finds it | PASS |
| **B** | `world(child) = world(parent) ∘ local(child)`, hand-composed; a cyclic parent is refused at the door | PASS |
| **C** | frame edits are one undo step each; undoing a creation removes registry entry and graph node, redo restores the SAME id | PASS |
| **D** | **the release proof**: frame at origin → COM (50, 25, 10); +30 Z → (50, 25, 40); rotated 90° about X → (50, −10, 25) | PASS |
| **E** | moving a frame re-extrudes once and re-solves nothing | PASS |
| **F** | a parent move reaches a grandchild's solid: two levels, hand-composed COM (50, −15, 25) | PASS |
| **G** | connectors: registered, role and frame carried, world transform follows the frame, creation on a non-frame refused, undoable | PASS |
| **H** | v10 round-trips frames, hierarchy, connectors and `supportFrameId`; exactly ONE Origin survives; a loaded document carries no history | PASS |
| **I** | a deleted support frame fails the sketch loudly — never a silent fallback to world XY — and save refuses the dangling reference | PASS |
| **J** | full M0–M9 regression, both configs — **856/856**, plus single-process and shuffled-seed runs of all five binaries in both configurations, and a **150-iteration shuffled stress of the Core binary in each configuration (300 runs, 0 anomalies)** added after ADR-M10-006 | PASS |
| **M/N/O** | Mirror doubles the material about a frame's plane and follows it when the plane moves; Pattern marches along the frame's axis driven by Parameters, refuses a fractional count, accepts a count of 1; both inherit consumed-once, suppression and round-trip | PASS |
| **P/P2** | a transform uses its frame's **world** plane and **rotated** axis — the fixture that found ADR-M10-005 | PASS |

Plus B2 (cycles), D2 (a sketch with no support frame is unaffected — the
compatibility contract), F2 (placing a sketch on a frame is undoable), H2 (a
pre-v10 file still loads and keeps its Origin).

---

## Mutation record (19)

Binaries deleted before each rebuild and asserted present after; plain-copy +
`touch` restores; every edit `cmp`-verified as landed before any verdict.

**M10.1 (6)** — world transform not composed · `Compose` does not rotate the
child offset · cycle check removed · frame not registered · the Origin records an
undo step · frame moves unrecorded. **All guarded.**

**M10.2 (5)** — the pad reads the embedded frame · `effectiveSketchFrame` ignores
the support frame · the frame→sketch edge is unwired · the effective frame uses
the LOCAL transform · placement unrecorded. **All guarded** — the local-vs-world
one only by gate F, because only a two-level fixture can tell them apart.

**Kernel (1)** — the mass-properties summation reverted to the compound path.
**Guarded by `M10_KERNEL_010`** — but only after that test was rebuilt on an
extruded prism; its first version used a box and the mutation survived.

**Test-suite (1)** — duplicate-curve detection in `BuildProfile` disabled after
the id fix, to prove the rewritten `M4_PROFILE_022` still kills what it was
written for. It does; the fix changed where the id comes from, not what the test
asserts.

**M10.3/4/6 (8)** — transform uses the local frame · mirror normal ignores
rotation · fractional pattern count accepted · the transform does not consume its
base · the frame→transform edge unwired · a connector ignores its frame ·
`supportFrameId` not persisted · duplicate Origin on load. **Six guarded
immediately; two survived** — see below.

---

## What the gates caught, and what that cost

Five defects, four of them mine and one older than this milestone.

1. **Two Origin frames on load.** The constructor makes one and the file carries
   one. Caught by the byte-identical round-trip tests.
2. **Frames written in creation order, restored assuming parents come first.**
   A re-parent puts a child before its parent, and the load failed. The comment
   claiming document order guaranteed it was wrong; restore is two passes now.
3. **A deleted support frame silently relocated the sketch to world XY** —
   `worldTransform` returns identity for an unknown id — with mass still current
   and save still succeeding. The spec forbade exactly this and the first
   implementation did it anyway.
4. **The loader recorded an undo step** while removing the surplus Origin. This
   is the **fourth** instance in two milestones of "something during construction
   or load was treated as a user edit", and all four were caught by the same
   two-line rule in ADR-M9-001: *a loaded document starts with an empty history*.
   It is the highest-yield invariant written this year.
5. **`calculateMassProperties` returns a wrong CENTRE OF MASS with an exactly
   right VOLUME** for a fused compound of disjoint solids — 2% on two prisms
   200 mm apart. **This predates M10** and would have survived indefinitely,
   because every analytic oracle in this project is a volume. See ADR-M10-005.

6. **An intermittent, order-dependent test failure that `ctest` cannot see.**
   `M4_PROFILE_022_IdenticalArcsAreDuplicates` failed about one run in a
   hundred under `--gtest_shuffle`, because it restored the *constant* id
   999010 while three sibling tests shove the process-global id counter into
   that neighbourhood. See ADR-M10-006. **This predates M10 by six
   milestones** — M4 wrote both halves — and 856/856 in both configurations,
   run many times, never showed it.

Defect 5 deserves its own note. It was found only because GATE_P's expected
centroid does **not** sit at the midpoint of the two lumps. M10's own GATE_M and
GATE_N fuse disjoint solids and passed throughout — their expected values are at
the symmetric centre, where the error cancels exactly. Writing gates whose
oracles avoid coincidences is a rule this project has stated three times, and
this is the first time its author walked into the trap while writing the gates
meant to enforce it.

The diagnosis also nearly went wrong. The first hypothesis — a mirror reverses
handedness, so the mirrored solid's shells point inward — is plausible, and two
fixes were written for it. The decisive step was a CONTROL: fusing a *translated*
copy instead of a mirrored one produced the identical wrong number, refuting the
hypothesis in a single run. Without it, a fix would have been aimed at a problem
that did not exist and reported as solved.

---

## A fifth registration site

ADR-M9-006 listed four places a new feature type must be registered. Adding
`MirrorFeature` found a **fifth**: a chain of concrete-type `dynamic_cast`s in
`validateSaveable` that decides whether a feature is "a placeholder carrying a
reserved type name". A real Mirror looked like a placeholder, and a document
containing one could not be saved.

It was not fixed by adding two more casts. The check only ever meant "is this a
`PlaceholderFeature`", so it now asks exactly that — one question that cannot
drift, replacing a list that had to grow with every feature type. That is
ADR-M3-007's rule ("ask the capability, do not enumerate") applied to the code
that enforces the other tables.

---

## Known limitations

- **No attachment to a planar FACE.** It needs stable topological references —
  the same reason ADR-M8-006 deferred per-edge fillets. Roadmap §5 lists it.
- **No realign-to-axis-entity** (roadmap §18.2): its input is a *selected*
  entity, which needs the selection architecture. Offset and rotation, which
  need no selection, are delivered.
- **Flip-primary and reorient-secondary** are expressible as transforms but have
  no command yet.
- **Connector visibility is absent by design** (§18.3, A02): presentation state
  does not enter Core.
- **Pattern is LINEAR only** — one direction, one count, one spacing. Circular
  and 2D patterns are not modelled.
- **Mirror does not detect self-intersection.** A mirror plane that cuts through
  the base produces whatever the fuse produces; nothing checks whether the result
  is what a user meant.
- **`worldTransform` and `isFeatureActive` are linear scans** and are called per
  node per pass. Correct, unmeasured, and the frame hierarchy makes them hotter
  than M9 left them.
- **Frames have no UI creation command.** They are reachable in the tree and the
  panel and through `--sample m10-frame`, but only Core and the sample create
  them — M10.5 delivered inspection, not authoring.

---

## NOT EXECUTED

- **M10 owner UI validation** — the checklist is WRITTEN
  (`docs/reviews/M10_UI_UserValidation.md`, 32 rows across five tests) and
  **not run**. Four of its rows ask for a judgement no test can make; those are
  the reason it exists.
- **M10 independent review** — not launched. Nor M9's.
- Any performance measurement.
- Frames interacting with M7 reconstruction, or with DXF import.
- A frame hierarchy deeper than two levels under mutation (gate F is two).

---

## What I am least confident about

1. ~~Whether the fix also holds for OVERLAPPING fused solids.~~ **Was on this
   list and is now tested** (`M10_KERNEL_010`): an overlap fuses to ONE solid,
   and its union volume is 150000 — *not* the sum of the parts, which is the
   arithmetic a naive "add the pieces" fix would get wrong. Both halves pass,
   and the mutation restoring the old compound path kills it.

   The first version of that test pinned NOTHING: it built its solids with
   `createBox`, and a fused pair of BOXES measures correctly even through the
   defect. Box and extruded prism have behaved differently three times in this
   milestone and the failing case was the prism every time. The test is built on
   an extruded prism for exactly that reason, stated at the test.
2. **What else the compound path was quietly wrong about.** The defect was found
   through ONE symptom — a centroid — on one shape family. Inertia
   (`secondMomentMm5`) goes through the same summation and has no analytic
   oracle anywhere in this project, so nothing would notice if it were wrong
   too.
3. **The mass-properties change touches every geometric result in the project.**
   The full suite passes in both configurations, which is evidence, not proof:
   the suite's oracles are volumes, and a volume is precisely what the old code
   already got right.
4. **How much else only shuffle can see.** ADR-M10-006's defect was invisible
   to `ctest` and visible to `--gtest_shuffle` at ~1%. Seven id literals are
   fixed; what is NOT established is that id literals were the only
   process-global state the suite shares. Anything else cached in a function
   static has the same property, and nothing enumerates those.
5. **Nobody has reviewed any of M9 or M10.** Four M8 rounds found three
   Criticals and eleven Majors in work its author believed finished, two of them
   introduced by fixes for earlier findings. M9 and M10 together are larger than
   M8 was.
