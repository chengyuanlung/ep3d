# M8 — Core Feature Modeling

## 1. Mission

M7 proved a 2D drawing can become one parametric solid. M8 makes the solid the
*start* of a model instead of the end of one:

> A Body's shape becomes the ordered result of a **feature history** — Sketch,
> Pad, Pocket, Revolve, Fillet, ... — where every feature consumes the result
> of the one before it, carries stable identity and semantic input references,
> and rebuilds selectively when its inputs change.

This is roadmap §9 ("Feature List is the primary parametric history") and
roadmap §33's M8 rung. The central release proof:

> Pad a 100 × 50 rectangle to 20 mm, pocket a 20 × 30 rectangle 10 mm into it,
> and verify 94000 mm³ analytically. Change Width 100 → 120 through the normal
> editing path and verify 114000 mm³. Change the pocket depth to 20 (through)
> and verify 88000 mm³ — with counters proving the sketch did not re-solve and
> the pad did not re-extrude.

## 2. Baseline

M8 starts from `m7-wip` head — M7 functionally complete **plus** independent
review round 1's fixes (all 4 Criticals and 14 Majors closed, 690/690 in Debug
and Release, 395/395 single-process).

**Stated plainly rather than implied:** M7 is NOT yet accepted. Round 2 review
and owner UI validation are open, as are two inherited M6 items. M8 building on
this baseline is a scheduling decision; **M8 cannot close before M7 closes**,
and nothing in this spec supersedes those obligations.

M8 must preserve every accepted M0–M7 contract.

## 3. Alignment (roadmap §32)

| | |
|---|---|
| Reference behavior | Onshape Part Studio: a feature list where each feature edits the part's solid in order; editing an early feature rebuilds only affected descendants |
| EP3D intended behavior | identical semantics on the EP3D graph: feature → feature dependency edges, selective recompute, diagnostics per feature |
| Implemented before M8 | every solid feature built **from nothing**; MassProperties pointed at one feature; no feature consumed another |
| Intentional differences | no rollback bar (roadmap §16 defers it to M9); no feature preview/accept transaction (§10, M9); no generalized extrude extents (§12, later); Feature List is the existing Model Tree, not a separate panel |
| Validation | Gates below + owner manual UI validation |

## 4. Scope

The roadmap's M8 list, sequenced. **Required for M8 to close:**

- **Pocket** (remove-extrude) — the chain-founding feature
- **Revolve**
- **Fillet**, **Chamfer**
- feature failure isolation: a failed feature does not corrupt its base, and
  recovery works

**In scope, closable as deferred only by explicit ADR:** Hole, Boolean between
bodies, Mirror, Pattern, Shell.

Out of scope: Sweep/Loft/Draft/surfaces, feature rollback position, preview
transactions, Assembly, Drawings, configurations.

## 5. Architectural Rule — the feature chain

```text
Sketch001 ──▶ Pad001 ──▶ Pocket001 ──▶ Fillet001 ──▶ (Body result)
                ▲            ▲
             Length       Depth (Parameter)
```

- A consuming feature references its base by **ObjectId of the base feature**
  (semantic input ref, spec A03) — never by position in `Body::features()`,
  never by pointer, never by kernel handle.
- The base's shape is read through **`ISolidFeature`** (capability, ADR-M3-007)
  at recompute time, from the registry. A consuming feature never caches its
  base's shape.
- The dependency edge `base → consumer` is wired by the PartDocument facade at
  creation, exactly as Parameter edges are. Editing the base's inputs dirties
  the consumer through the existing M2 machinery — nothing new schedules.
- MassProperties and the viewer follow the **chain tail**: intermediate results
  are computed state, not displayed duplicates.
- Kernel operations added for M8 (`subtractShape`, later `revolveProfile`,
  `filletEdges`, ...) enter through `IGeometryKernel` only. Core still names no
  OCCT type.

Forbidden: a feature storing a copy of its base's geometry; a boolean performed
in Core; the chain order derived from vector order rather than explicit base
references.

## 6. Failure semantics

- A feature whose base is Failed or missing fails with a diagnostic naming the
  base — it does not extrude a tool into stale geometry.
- A failed consumer keeps its last valid shape byte-for-byte (ADR-M3-001/004);
  staleness travels through ComputeState, and MassProperties reports
  not-current, exactly as M5's contracts require.
- A tool profile that clears the base entirely, or misses it entirely, is a
  **legal cut** (result = empty-difference or base unchanged) — refusing would
  make ordinary modelling fail; the diagnostic layer may note it.

## 7. Release gates (all analytically oracled, counters not equal-values)

| Gate | Proof |
|---|---|
| **A** | subtract at the kernel boundary: box − box, hand-computed volume |
| **B** | Pad 100×50×20 − Pocket 20×30×10 = **94000 mm³**, mass 0.25380 kg at 2700 kg/m³ |
| **C** | Width 100→120 rebuilds pad AND pocket → **114000 mm³** |
| **D** | pocket depth 10→20 (through) → **88000 mm³**; solver calls unchanged, exactly one tool extrude, pad not rebuilt |
| **E** | failure isolation: NaN/negative/zero depth → pocket Failed, base Valid, mass not current; recovery restores 94000 |
| **F** | save → fresh load → edit Width → correct volume; v6 round-trips the chain reference semantically |
| **G** | deleting the pocket restores the pad as chain tail; deleting the BASE fails the pocket with a diagnostic, no crash |
| **H** | viewer shows the chain TAIL, not tail plus intermediates |
| **I** | full M0–M7 regression, Debug and Release, Release proven, single-process runs green |

Mutation minimums: subtract replaced by "return base" → Gate B fails; pocket
bound to wrong Parameter → Gate D fails; chain edge unwired → selective
recompute gate fails; pocket record dropped from the serializer → Gate F fails.

## 8. Adversarial (beyond the gates)

Pocket sketch missing / deleted; base id invalid; open pocket profile; pocket
sketch == pad sketch; two pockets chained (Pad→Pocket→Pocket); depth exactly at
the dimension floor and just below; disjoint pocket; repeated save/load;
whole-suite single-process runs (the M7-review net).

## 9. Required ADRs

- ADR-M8-001 — feature chain: base references, wiring, tail semantics
- ADR-M8-002 — tool direction and legal-cut policy
- ADR-M8-003 — display and mass follow the chain tail
- more as implementation discovers durable decisions

## 10. Slices

```text
M8.1 — kernel subtract + PocketFeature + chain wiring + v6 + gates A–I
M8.2 — Revolve (kernel revolveProfile + RevolveFeature)
M8.3 — Fillet / Chamfer (edge selection semantics — the identity-hard slice)
M8.4 — Hole / Mirror / Pattern / Shell or explicit deferral ADRs
M8.5 — UI: feature creation workflow in the shell
M8.6 — self-validation + mutation audit
M8.7 — independent review + close (blocked on M7 closing first)
```

Only M8.1 is authorized by this spec revision in detail; later slices refine
their sections before implementation, as M7 did.

## 11. Definition of done

Required features implemented and chained; gates A–I pass; adversarial matrix
green; mutation-verified; v6 round-trips; M0–M7 regressions pass both configs
including single-process; ADRs recorded; self-validation written as claims;
independent review with no unresolved Critical/Major; owner UI validation —
**and M7's own open items closed.**
