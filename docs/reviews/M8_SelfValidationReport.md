# M8 Self-Validation Report — Core Feature Modeling

> **These are CLAIMS, not facts.** Spec 33's rule carries over from M7, and M7's
> round 1 proved the point on this author specifically: two claims in the M7
> report were false ("668/668" was ctest-only; a Gate J guard was tautological),
> and of 30 guards reviewers removed, 11 had no failing test while my own nine
> mutations touched none of them. Read accordingly.

**Baseline:** `7e93067` (m7-wip after review round 1's fixes).
**Branch:** `m8-wip`, not pushed. `git log` is the authority for the head.

**Standing obligations, restated:** M7 round 2 review and M7 owner UI
validation are OPEN. Two M6 items are OPEN. **M8 cannot close before they
close** (M8 spec §2), and nothing below supersedes that.

---

## What M8 delivered

| Slice | Content |
|---|---|
| M8.1 | kernel `subtractShape`; **PocketFeature**; the feature chain (base by ObjectId via `ISolidFeature`, `consumedSolidId()`, tail display/physics); schema v6 |
| M8.2 | kernel `revolveProfile` (shared face builder); **RevolveFeature** with a `SketchEntityId` axis treated as construction geometry (`BuildProfile` exclusion overload); schema v7 |
| M8.3 | kernel `filletAllEdges`/`chamferAllEdges` (+`BRepCheck_Analyzer` on results); **FilletFeature/ChamferFeature** on one shared `EdgeDressFeature` base; schema v8 |
| M8.4 | explicit deferrals with demonstrations (ADR-M8-007): Hole shown expressible as a circle Pocket by gate; Mirror/Pattern → M9; Shell → with edge/face selection |
| M8.5 | the `m8-chain` sample: chain reachable in the running shell, tail displayed, Depth editable in the panel, smoke-tested |
| M8.6 | this report |
| M8.7 | independent review round launched; **close remains blocked on M7** |

The chain now composes four kinds: `Sketch → {Pad|Revolve} → Pocket → {Fillet|Chamfer}`,
with dress-on-dress (Fillet→Chamfer) round-tripping in v8.

---

## Builds and regression

| | Result |
|---|---|
| Debug / Release builds | 0 errors |
| ctest Debug | **808 registered / 804 executing**; 808/808 pass (4 registered-Skipped children, spawned by their parents) |
| ctest Release | **808 registered / 804 executing**; 808/808 pass (same four) |
| Single-process Core binary | **444 registered / 441 pass / 3 child-Skipped, IDENTICAL in both configs** (the check M7's review forced) |
| Release actually ran Release binaries | every command line names `build\Release\`, none name Debug |
| Whole-suite ctest entries | present and passing (the net that catches registration-order poisoning) |

Baseline at M8 start: 690. **118 tests added across M8**, of which 107 by the end
of M8.6 and 11 more in review rounds 3 and 4. Schema pins moved 5→6→7→8; each
bump forced its updates, which is those tests working.

> **These figures were 737/736 and 406/406 until round 4.** They were true when
> M8.6 was written and went stale as the review rounds added tests; three
> separate rounds have now had to correct a count in this project's record, so
> the counts here are stated as **registered / executing** and dated to the head
> they were measured on. Round 4's totals above are measured on its fix head,
> both configurations, with nothing else running -- a count taken while a second
> test run was in flight reads one lower, because the two contend for the
> generator-limit proof file.

**Forced renames, each recorded where it happened:** placeholder-type examples
"Revolve" → "Loft" (M8.2) and "Fillet" → "Sweep" (M8.3) — the third and fourth
occurrences of the pattern M7.3 started with "Radius".

---

## Oracles (all hand-computed; none read back from the code under test)

- Pocket: `100000 − 6000 = 94000`; through-cut `88000`; Width edit `114000`.
- Revolve: annulus `π(30²−10²)·50 = 40000π`; half-sweep = **exactly** half (ratio).
- Fillet: the Minkowski rounded box `70656 + 26752 + 632π + (4/3)π·8` (r=2) and
  the r=1 variant after reload.
- Chamfer: Pappus on cylinder rims `2·2π(20−2/3)·2`; on the annulus's four rims
  `320π` — a three-kind chain (Sketch→Revolve→Chamfer).
- Hole-as-pocket: `100000 − π·36·20`.
- Selectivity by **counters** throughout (solver calls, extrudes, subtracts,
  revolves, fillets) — never equal final values.

---

## Mutation record (17 total, binaries deleted and asserted present per run)

| Battery | Result |
|---|---|
| M8.1 (6) | 5 guarded; MUT6 (base-state check) **unreachable through the engine** — documented as unguarded defense-in-depth ON THE CHECK ITSELF, engine contract carried by GATE_E2's counter |
| M8.2 (5) | all guarded — after the fixture was de-coincided (below) |
| M8.3 (6) | all guarded, including F5 (restore dispatch swapping the twin types) and F6 (analyzer removed). *Corrected after round 1 (R2-m1/R3-m2)*: F5's kill was misattributed to M8_SER_202 -- 202 was swap-blind (a symmetric swap keeps the type counts 1/1) and the actual killers were M8_SER_201's per-id casts and GATE_FD. 202 now pins the id->type mapping per record. |

**Findings against my own work made during M8, kept on the record:**

1. **A volume coincidence made a gate non-discriminating** (M8.2): the correct
   annulus `π·800·40` EQUALS the wrong-axis cylinder `π·1600·20`. Mutation R2
   slipped the axis-order gate and was caught one step removed. Fixture
   changed to 20×50; R2 now fails the gate directly. *An oracle is only as
   good as the coincidences it avoids.*
2. **`IsDone()` was not a sufficient success check** (M8.3): OCCT reports done
   for a radius wider than half the slab while producing self-intersecting
   geometry. Found because the refusal test failed; fixed with
   `BRepCheck_Analyzer`; pinned by mutation F6.
3. **Gate H (tail display) failed on first run and was right to** (M8.1): only
   Valid consumers counted as consuming, so a failed pocket un-consumed its
   base and the viewer showed a healthy-looking wrong solid. Consumption is
   structural now.
4. **MUT1's first verdict was garbage** (M8.1): the mutation script itself had
   a syntax error and never edited the file — caught by reading the output,
   not the verdict; re-run for real.

---

## Known limitations

- Fillet/Chamfer are ALL-edges only; per-edge selection deferred with the
  selection architecture (ADR-M8-006).
- The fake kernel models no fillets/chamfers — v8 Core tests assert round-trip
  identity without recompute; real-geometry checks live in the OCCT suite and
  GATE_FD. Stated in both files.
- Hole/Mirror/Pattern/Shell deferred by ADR-M8-007, each with a demonstration
  or a named successor milestone.
- Feature creation dialogs deferred to M9's edit-transaction workflow; M8.5's
  sample is how the chain is reached today.
- `removeObject` on a chain member re-points nothing automatically. *Corrected
  after round 1 (R2-m5, by execution)*: the original wording here -- "leaves
  mass properties on the removed id" -- overstated the defect. Removing the
  tail DETACHES the mass source, removes the mass node from the graph, and
  invalidates `massProperties_`; nothing dangles. The real limitation is that
  mass is not re-pointed at the NEW tail until the next feature add. GATE_G
  pins the no-crash behavior; the friendlier re-point is M9 material.
- Pocket/dress features do not unit-check their mm parameters (consistent with
  Pad); only Revolve checks Radian, for the reason documented on the check.

## NOT EXECUTED

- **M8 owner UI validation** — no checklist run; the m8-chain smoke test is
  automated evidence only, and ADR-M4-016's line between the two stands.
- **M8 independent review verdicts** — round launched at M8.7; results not in
  at the time of writing. Everything above is unreviewed.
- Chained-feature performance beyond the fixtures (no profiling claimed).
- Fillet/Chamfer on imported (M6/M7) geometry — the chain accepts any
  `ISolidFeature` base, but no test drives an imported sketch through a dress
  feature.

## What I am least confident about

1. **The all-edges analyzer cost**: `BRepCheck_Analyzer` on every dress result
   is O(shape); nothing measures it on large parts.
2. **The v8 record sharing one shape for two types**: M8_SER_202 pins the
   discriminator, but a reviewer deleting subtler lines in the shared parse
   branch may find cases my mutations missed — the branch is newer than its
   siblings and had no independent eyes.
3. **GATE_CB's annulus-rim Pappus arithmetic**: four rims, two signs; I derived
   it twice and got the same 320π, but a sign error that halves one pair would
   hide inside the 1e-6 relative tolerance only if it were also compensated —
   still, this is the one oracle I did not cross-check by an independent
   method.
