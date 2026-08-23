# M8 Independent Review (Rounds 1–3, Consolidated)

> Round 1 reviewed `880e6cc`; round 2 reviewed `ab8513a` (round 1's fixes);
> round 3 reviewed `c555269` (round 2's fixes). Each round's section is
> appended in order, with its own fix record.

# Round 1

**Reviewed commit:** `880e6cc` (m8-wip head at review time).
**Method:** three reviewers, disjoint partitions (spec 33's protocol, as in M7),
each in an isolated `git archive` export at `D:/Program2/EP3D/m8review/{r1,r2,r3}`
with its own configured build. Each was instructed to treat
`M8_SelfValidationReport.md` as claims, re-derive oracles independently, and
run its own mutations with delete-binaries-before-rebuild discipline.

| Reviewer | Partition | Decision | Score |
|---|---|---|---|
| R1 | feature chain semantics / geometry | **REQUEST CHANGES** | 73/100 |
| R2 | identity / persistence / transactions | **REQUEST CHANGES** | 70/100 |
| R3 | recompute / evidence quality / mutation audit | APPROVE WITH MINOR CHANGES | 88/100 |

**Round verdict: REQUEST CHANGES.** Two Criticals, both demonstrated by
execution. Fix status is tracked in the register below; the fix commit(s) are
on `m8-wip` after `880e6cc`.

**What survived attack (the honest-negative side, all three reviewers):**
every volume oracle re-derived independently and confirmed — including
GATE_CB's 320π, the item the self-report flagged as least confident (derived
two more times, by two reviewers, both matching). The 17-mutation table is
honest: R3 re-ran 10 of 17 himself, 10 CONFIRMED / 0 REFUTED, including the
MUT6 "unreachable through the engine" claim, which R3 attacked via its
strongest candidate path and confirmed. Headline numbers (737 ctest both
configs, 406/406 single-process both configs, Release command-line proof)
independently reproduced by R2 and R3. The C2 registration-order net works
(R3 reintroduced the violation: 65 tests poisoned, WholeSuite entry red,
per-test ctest green — exactly the failure the net exists to catch).
R2's 19-probe adversarial battery: 13 held, 6 failed — all six one defect
(C-R2-1). Dress-then-cut (Pocket on a Fillet base) composes correctly to
1e-6 (R1, untested capability, now proven by probe).

---

## Findings register

IDs keep each reviewer's numbering. "Demonstrated" = by execution in the
reviewer's export unless marked inspection.

### Critical

| ID | Finding | Status |
|---|---|---|
| R1-C1 | **A diamond — two features consuming the SAME base — is silently accepted**: both pockets cut the original pad, mass silently follows the last-wired consumer, the viewer shows two overlapping solids, and the file round-trips (save/load check only "earlier in same body", never "not already consumed"). Violates spec §5's chain rule with no diagnostic; M9 history would inherit it. | FIXED |
| R2-C1 | **`savePartDocument` writes files its own loader refuses — six ways, all four new M8 types** (ADR-M3-008's named worst class, fourth recurrence): `validateSaveable` misses Pocket depth-parameter and sketch, Revolve angle-parameter, sketch and axis-entity, and Fillet/Chamfer size-parameter references. Sharpest: deleting any sketch line that happens to be a revolve axis (A5) silently poisons every future save. All six demonstrated save-OK → load-refused through the public facade. | FIXED |

### Major

| ID | Finding | Status |
|---|---|---|
| R1-M1 | ADR-M8-005 and RevolveFeature.h claim the axis MAY be a profile member ("canonical first cylinder") — but the unconditional exclusion breaks the loop and the case fails. ADR asserting an untested (and untrue) capability — M7's penalized defect class. | FIXED (capability implemented + gated) |
| R1-M2 | Cross-body consumption: legal at creation, presenter's consumed-set is per body → base AND consumer both display (transient state; save already refuses). | FIXED (creation refused; presenter set doc-wide) |
| R1-M3 | Dress-feature chain semantics structurally untested: base→dress edge deleted, `consumedSolidId()`→invalid — all tests stay green (2 UNGUARDED mutations). No upstream-edit or tail-display test for a dress chain. | FIXED (GATE_FE, GATE_FF) |
| R1-M4 | Chamfer's `BRepCheck_Analyzer` unverified: analyzer deleted from `chamferAllEdges` → all green. Kernel layer duplicates the guard per verb; F6 pinned only the fillet's copy. | FIXED (shared helper + chamfer refusal test) |
| R1-M5 | Sketch→Pocket edge unguarded: deleted → all green. No test edits a pocket's sketch. | FIXED (GATE_KC) |
| R1-M6 | Spec §8 adversarial rows absent at document level (behavior correct by probe; coverage missing): same-sketch pocket, invalid base id, depth at/below floor, open pocket profile. | FIXED (tests added) |
| R2-M1 | Core suite blind to the chain edge and to the pocket half of the save-side base rule — both killed only by the OCCT-linked Integration suite; a Core-only build has zero failing test for ADR-M8-001's central invariant. | FIXED (Core twins added) |
| R2-M2 | Loader accepts a chain base that is not a solid feature (e.g. a Placeholder), then `wire*` silently discards the failed `addDependency` `GraphResult` — the exact unrepresentable state ADR-M8-001 promises. Mitigations hold (loud recompute failure, no crash, symmetric save) but acceptance and dropped edge are silent, unpinned. | FIXED (loader refuses; wire throws; both pinned) |
| R3-M1 | GATE_RC does not discriminate selectivity: under an engine degraded to global-recompute, GATE_D and GATE_FB go red but GATE_RC stays green (fixture has no constraints and no second counter-bearing node). Same class as M7 review's Gate K finding. | FIXED (GATE_RC2) |
| R3-M2 | m8-chain selftest panel rows assert only non-emptiness; hard-coding `displayedPropertyValue` to `"42"` leaves the smoke green (caught only one step removed by DXF smokes). Same pattern the same file already fixed for import counts. | FIXED (exact values) |

### Minor

| ID | Finding | Status |
|---|---|---|
| R1/R2-m | `featuresReferencingSketch` enumerates only `PadFeature`; its header contract is false for Pocket/Revolve sketches. | FIXED |
| R2-m1 / R3-m2 | F5's guard misattributed: M8_SER_202 is swap-blind (symmetric swap keeps counts 1/1); the kill is M8_SER_201's per-id `dynamic_cast` + GATE_FD. Self-report corrected; 202 strengthened to per-id type mapping. | FIXED |
| R2-m2 | Schema version is a ceiling, not a content gate (v6-stamped file with v8 records loads). Defensible, was undocumented. | DOCUMENTED |
| R2-m3 | `M5_SER_001_SchemaVersionIsFive` name rot (asserts 8). | FIXED |
| R2-m5 | Self-report's `removeObject` limitation wording wrong (conservative direction): mass is detached and invalidated, not left dangling; real limitation is no re-point to the new tail. | CORRECTED |
| R2-m6 / R3-m3 / R1 | "737/737" counts one permanently-Skipped fresh-process child; 736 execute. Same accounting nit M7 was corrected for. | CORRECTED |
| R3-m1 | Persisted-failure barrier credited to GATE_E2 (ADR-M8-004, PocketFeature comment); actually pinned only by unit-level `DependencyGraphTests.StaleFailureGates*`. | FIXED (GATE_E3 + docs corrected) |
| R1-m | `wire*` helpers discard `GraphResult` (subsumed by R2-M2). | FIXED |
| R1-m | `validateSaveable` chain walk uses concrete-type `dynamic_cast` enumeration instead of `consumedSolidId()` capability. | FIXED (capability used) |
| R1-m | GATE_CB implicitly relies on OCCT ignoring full-revolution seam edges; uncommented. | COMMENTED |
| R1-m | Empty-shape legal-cut result (volume 0, mass valid) matches ADR-M8-002 but unpinned. | PINNED |

### Harness lesson (R1 and R3, independently)

Timestamp-preserving restores (`Copy-Item`, `cp -p`) make MSBuild skip
recompiles — a **second flavor of the stale-binary hazard**: the source is
restored but the object file still carries the mutation (or vice versa). Both
reviewers hit it, both detected it (R3: two mutations with byte-identical
failure lists; R1: a probe surviving in a binary after source restore), both
re-ran affected cycles with forced-touch restores. AGENTS.md rule 2 extended.

---

## Reviewer mutation records

R1 (5): base→dress edge **UNGUARDED**; Sketch→Pocket edge **UNGUARDED**;
chamfer analyzer **UNGUARDED**; dress `consumedSolidId()`→invalid
**UNGUARDED**; BuildProfile exclusion made positional **guarded** (GATE_RB2
direct).

R2 (9): loader pocket earlier-base KILLED (M8_SER_003); loader dress
earlier-base KILLED (M8_SER_203); save-side earlier-base block KILLED
(M8_SER_204) — but pocket-only branch: Core **406/406 green**, killed only by
Integration GATE_H; axis-of-sketch check KILLED (M8_SER_102); base→pocket
edge: Core green, killed by 4 integration gates; dress-size-into-depth-slot
KILLED (M8_SER_201); restore dispatch swap KILLED by M8_SER_201 (not 202);
schema pin 8→7 KILLED by 5 tests — no pin vacuous.

R3 (10 of the author's 17 re-run): all verdicts **CONFIRMED** as recorded,
including MUT6's documented-unreachable (attacked via stale-base path, held);
plus his own: engine degraded to global recompute → GATE_D and GATE_FB red,
GATE_RC green (R3-M1); `displayedPropertyValue` hard-coded → m8-chain smoke
green (R3-M2); persisted-failure barrier deleted → integration green, 3 unit
tests red (R3-m1); registration-order violation reintroduced → WholeSuite
entry red, per-test ctest green.

---

## Fix record (author, after consolidation)

Every CONFIRMED finding fixed on `m8-wip`; the register above carries per-item
status. The shape of the fixes:

- **ADR-M8-008** (new): a consumer's base must be a SOLID feature of the
  consumer's OWN body, consumed at most once document-wide -- enforced at
  creation (`requireConsumableBase`, by capability), save (`validateSaveable`,
  now also carrying the six missing reference checks with loader-mirrored
  wording), load (solid-type + uniqueness in the chain walk; `wire*` now
  throws on a failed base-edge `GraphResult`), and display (document-wide
  consumed set).
- **Revolve axis-as-member implemented** (fallback rebuild with the axis when
  the excluded profile does not close), making ADR-M8-005's claim true;
  RevolveFeature.h and the ADR record that the sentence was false as shipped.
- **Kernel analyzer unified** into one `AnalyzedDressResult` for both dress
  verbs; the chamfer refusal has its own test.
- **24 new tests**, one per finding: Core 406->419, Integration 131->141,
  KernelOcct 59->60.
- Docs corrected in place: ADR-M8-004 (two-half guard credit), ADR-M8-006
  (kernel-duplication amendment), self-report (F5 attribution, 736-execute
  qualifier, removeObject mass wording), M5_SER_001 rename, GATE_CB seam
  comment, AGENTS.md rule 9 (timestamp-preserving-restore hazard).

**Fix-verification mutation battery** (all binaries deleted before rebuild
and asserted present; every file restored and `cmp`-verified; restores
`touch`ed against the stale-object trap):

| # | Round-1 mutation re-run | Verdict after fixes |
|---|---|---|
| V1 | base->dress chain edge deleted (R1 MUT1) | **guarded** -- GATE_FE |
| V2 | Sketch->Pocket edge deleted (R1 MUT2) | **guarded** -- GATE_KC |
| V3 | shared analyzer skipped (R1 MUT3) | **guarded** -- GATE_FC, GATE_FF, kernel refusal test |
| V4 | dress `consumedSolidId()` -> invalid (R1 MUT4) | **guarded** -- GATE_FF, M8_SER_204 |
| V5 | pocket save-side reference checks deleted (R2-C1) | **guarded** -- M8_REV_301/302 |
| V6 | revolve axis save-side check deleted (R2-C1 A5) | **guarded** -- M8_REV_313 |
| V7 | `requireConsumableBase` neutered (R1-C1) | **guarded** -- M8_REV_307/308 |
| V8 | loader uniqueness check alone deleted | **masked by design** -- the restore-path helper still refuses the same file, though with DIFFERENT wording ("by feature N" vs the loader's "by an earlier feature" -- round 3 corrected this row, and round 2's 304 word-pin exploits exactly that difference; defense in depth, stated here so it is never read as a guard) |
| V8b | BOTH diamond layers deleted | **guarded** -- M8_REV_304 (+307/308) |

---

# Round 2

**Reviewed commit:** `ab8513a`. Fresh exports at
`D:/Program2/EP3D/m8review2/{r1,r2,r3}`, same partitions, same discipline
(now including the rule-9 touch-restores both round-1 reviewers' incidents
forced).

| Reviewer | Partition | Decision | Score |
|---|---|---|---|
| R1 | chain semantics / geometry | APPROVE WITH MINOR CHANGES | 89/100 |
| R2 | identity / persistence / transactions | **REQUEST CHANGES** | 78/100 |
| R3 | evidence quality / fix-verification audit | APPROVE WITH MINOR CHANGES | 81/100 |

**Round verdict: REQUEST CHANGES** (0 Criticals). Every round-1 finding was
independently confirmed CLOSED (all probes re-run: the diamond, all six
A1–A6 symmetry gaps, GATE_RH's cylinder, the Minkowski/Pappus oracles
re-derived again). What round 2 found is the project's round-N pattern:
defects in the FIXES' enforcement machinery and evidence, not in geometry or
data. The V-battery was audited by re-execution: 8 of 9 rows exact, V8's
"masked by design" claim CONFIRMED honest, one kill-list overstated (V8b
credited 308, which did not fire).

## Round-2 findings register

| ID | Finding | Status |
|---|---|---|
| R2R2-M1 | **`bodies()` bypass**: constness stops at the `unique_ptr`, `Body::addFeature` was public → one line builds a rogue consumer behind every door (no `requireConsumableBase`, no registry entry, `removeObject` blind to it, document permanently unsavable). Same accessor hazard fixed for `sketches()` in M5, unapplied to `Body`. | FIXED — `addFeature`/`removeFeature` private, `friend PartDocument`; placeholders get a facade path (`addPlaceholderFeature`/`restorePlaceholderFeature`); the 12 direct test call sites migrated |
| R2R2-M2 / R2-R1-M1 | **Solid-type frontier drift** (found independently by two reviewers): save side decides "solid" by capability, load side by an inline name list; adding "Placeholder" to the list survived all 761 shipped tests (779 counting the reviewer's own probes) (refusal silently moved past the id-generator advance), and the next milestone's solid type would recreate save-OK→load-refused with zero signal. Box-as-base: fully supported, zero coverage. | FIXED — ONE shared `kSolidFeatureTypeNames` table feeds the chain walk AND the reserved-typename check; M8_REV_322 pins the table -- FULLY only since round 3: R3R3-M1 proved the first fixture covered half of it (consumer types never appeared as bases; dropping "Chamfer" survived everything), and the fixture now consumes every name as a base; GATE_BB pins Box-as-base end-to-end |
| R3R2-M1 | **M8_REV_308 was vacuous** for its finding: the fixture's pad was still consumed, so the throw came from the uniqueness half — a mutant with the same-body restriction deleted PASSED 308. | FIXED — 308 removes the pocket first; mutation W1 re-run: killed |
| R3R2-M2 | **GATE_E3 miscredited again**: it pins the two-layer system (engine barrier + feature base-state check), not the barrier alone — barrier-only deletion keeps all integration green. The round-1 "correction" recommitted the exact miscrediting defect it fixed. | FIXED — doc retreat in PocketFeature.cpp, EdgeDressFeatures.cpp, ADR-M8-004, and the GATE_E3 comment; barrier's only direct pins are the unit tests, stated everywhere |
| R2-m (304) | M8_REV_304's "already consumed" substring matched BOTH layers → loader deletion invisible. | FIXED — asserts the loader's exact "already consumed by an earlier feature"; W4 re-run: killed |
| R2-m / R1-m (citations) | Consumed-once diagnostics cited ADR-M8-001; the rule is ADR-M8-008. | FIXED ×3 sites |
| R3-m (V8 layer) | The V8 masking layer (restore-path refusal) was permanently untested dead code. | PINNED — M8_REV_310 calls the restore facade directly, asserts the throw and that nothing was half-restored |
| R2-m (204) | M8_SER_204's comment claimed the chamfer consumes the pad (it consumes the fillet). | FIXED |
| R3-m (309) | M8_REV_309's kill belongs to the wire-layer GraphResult check, not requireConsumableBase. | NOTED in the test |
| R1-m (equivalences) | Three defense-in-depth layers are equivalent-under-invariant and survive mutation BY DESIGN: document-wide scan in `requireConsumableBase` (vs own-body), the presenter's document-wide consumed set, and the save-side uniqueness branch (unreachable now that the bypass is closed). | RECORDED here, V8-style, so future audits read them as masked-by-design, not coverage holes |
| R1-m (diagnostic) | Interior/straddling-axis revolve refusal is loud but passes through raw OCCT text. | DEFERRED to the UI milestone (ADR-M8-002 policy) |

## Round-2 fix verification (W-battery)

Same discipline as the V-battery (binaries deleted/asserted, touch-restores,
`cmp`-verified):

| # | Mutation | Verdict |
|---|---|---|
| W1 | same-body half of `requireConsumableBase` deleted (round 2's surviving XBODY) | **guarded** — amended M8_REV_308 |
| W2 | "Box" dropped from the shared type table | **guarded** — M8_REV_322 |
| W3 | "Placeholder" added to the table (round 2's surviving MUT-3) | **guarded** — reserved-typename check now shares the table, placeholder saves refuse |
| W4 | loader uniqueness deleted (round 2's surviving MUT-4) | **guarded** — word-pinned M8_REV_304 |

The `bodies()` bypass itself is closed at compile time (private + friend);
its regression "test" is the type system.

---

# Round 3

**Reviewed commit:** `c555269`. Fresh exports at
`D:/Program2/EP3D/m8review3/{r1,r2,r3}`; the mandate was the round-2 change
set line by line plus its blast radius, with unchanged code explicitly out of
scope (each report states that scope choice).

| Reviewer | Partition | Decision | Score |
|---|---|---|---|
| R1 | chain semantics / round-2 fixes as code | **REQUEST CHANGES** | 77/100 |
| R2 | identity / persistence / round-2 serializer changes | **REQUEST CHANGES** | 84/100 |
| R3 | evidence quality / W-battery audit / the record itself | **REQUEST CHANGES** | 78/100 |

**Round verdict: REQUEST CHANGES** (0 Criticals). Round 2's fixes were
confirmed sound in what they claim — all four round-2 Majors independently
re-verified closed, the W-battery re-run row by row (W1–W4 all CONFIRMED,
W3's "5 tests" exact), the compile-time closure of the `bodies()` bypass
proven real (C2248 with a compiling control), and GATE_E3's twice-corrected
wording finally matching demonstrated reality in both directions. The round-N
pattern held anyway, in the fixes' own new code and the record's own
generalizations.

## Round-3 findings register

| ID | Finding | Status |
|---|---|---|
| R1R3-M1 / R2R3-M1 / R3R3-M2 | **All three reviewers independently: `restorePlaceholderFeature` — round 2's own new facade — was the seventh restore path and the only one without a duplicate-id guard.** Save-OK→load-refused demonstrated multiple ways (ADR-M3-008's class, fifth recurrence, introduced by a fix); sharpest variant: placeholders are unregistered, so a placeholder-held id silently defeats the six sibling guards' `registry_.contains` checks, and `removeObject` cannot see the ghost. | FIXED — the guard has BOTH halves (registry check + all-bodies feature scan, since the registry is blind to placeholders), checked before construction; M8_REV_341/342 pin both collision flavors |
| R1R3-M1 (2nd half) | `validateSaveable` had no id-uniqueness net at all — the loader has enforced document-wide id uniqueness since M2, the save side never did. | FIXED — the loader's net mirrored in `validateSaveable` (document/parameters/material/bodies/features/sketches, loader-worded); **round 4 refuted the "no current route reaches it" half of this sentence** — see R1R4-C2 and the corrected X2 row |
| R3R3-M1 | **"M8_REV_322 pins the table" was refuted by execution**: the fixture consumed only Box/Pad/Revolve as bases — dropping "Chamfer" from the table survived all 763 executing tests (a legal Pad←Chamfer←Fillet file then saved and refused to load). Four doc sites overstated the pin. | FIXED — fixture expanded so every table name is consumed as a base (9 solids: Box←Pocket←Fillet←Chamfer; Pad←Chamfer←Fillet; Revolve←Pocket); X-battery re-kills both surviving drops; all four doc sites corrected to the round-3 truth |
| R1R3-M2 | The const-stops-at-the-pointer accessor class is NOT unique to `bodies()`: `parameters().items()` (mutable `Parameter*`, public `setValue` → stale-as-current demonstrated), `material()` (shared_ptr, public `setDensity` → stale mass demonstrated), `frames()` (same shape, inert today). | FIXED for the two live doors — Parameter mutators private (friends: PartDocument, DocumentRecomputeEngine; new `setParameterExpression` facade fills the gap that had NO facade path), `material()` returns `const Material*`; `frames()`/connectors recorded as a known-open-inert door in the header |
| R2R3-m1 | The shared table conflates "legal chain base" and "reserved concrete name" — correct today, a trap the day a concrete NON-solid type ships. | DOCUMENTED (future-divergence note at the table: introduce `kConcreteTypeNames` then) |
| R1/R2-m | The consumer frontier ("Pocket"/"Fillet"/"Chamfer") was still an inline `\|\|` chain — the drift shape the solid table fixed. | FIXED — `kConsumingFeatureTypeNames` table beside the solid table; members pinned by M8_SER_003/203 + M8_REV_304 (X5 verifies) |
| R3R3-m1 | "779 tests" appears in two docs, underivable from any shipped total (761); source was 761 shipped + the reviewer's 18 probes. | CORRECTED at both sites |
| R3R3-m2 | Round-1 V8 row still claimed the masking layer refuses "with the same diagnostic" — false, and round 2's own 304 word-pin exploits the difference. | CORRECTED |
| R3R3-m3 | The barrier's unit-pin list was incomplete: `EdgeRewireAcrossFailedPrerequisite` also pins it. | CORRECTED at all three sites |
| R1-m | `friend class PartDocument` is wider than its use (reaches Body's fields; verified unused). | RECORDED as a decision in Body.h |

## Round-3 fix verification (X-battery)

Same discipline (binaries deleted/asserted, touch-restores, `cmp`-verified):

| # | Mutation | Verdict |
|---|---|---|
| X1 | placeholder restore guard deleted | **guarded** — M8_REV_341 + M8_REV_342 |
| X2 | save-side id-uniqueness net deleted | ~~**masked by design** — the facade guard refuses every current route first~~ **REFUTED BY ROUND 4 (R1R4-C2).** Two current routes reached it, both through the public facade: a placeholder-held id defeated every sibling restore guard (R1R4-C1), and the document's own id defeated all of them (R2R4-m1). The net was load-bearing while recorded as dead, and therefore untested. Both routes are now closed at the guard, which puts the net back in backstop position — but it is not being re-recorded as unreachable-by-inspection, because that inspection was wrong. |
| X3 | "Chamfer" dropped from the solid table (round 3's survivor) | **guarded** — expanded M8_REV_322 |
| X4 | "Pocket" dropped from the solid table (round 3's survivor) | **guarded** — expanded M8_REV_322 |
| X5 | "Pocket" dropped from the consumer table | **guarded** — M8_SER_003 + M8_REV_304/305 |
| X6 | direct `Parameter::setValue` outside the facade | **guarded at compile time** — C2248, binary not produced |

## Standing blocks (unchanged by all rounds)

M8 close remains blocked on: **M7 round 2 review**, **M7 owner UI
validation**, and the two inherited M6 items (M8 spec §2). Round 3's fixes
above are themselves unreviewed — the same question as after round 2, one
level up; the change set is again small (one guard, one net, one fixture
expansion, two accessor closures, doc corrections). The owner decides whether
a round 4 runs or the remaining risk is accepted and recorded.

---

# Round 4

**Reviewed commit:** `7a60b6b`. Fresh exports at
`D:/Program2/EP3D/m8review4/{r1,r2,r3}`, same protocol and discipline. The
mandate covered **two** unreviewed change sets rather than one: `9e0c399`
(M8 round-3 fixes) **and** `ebf2f16` + `7a60b6b` (M7 round-2 fixes and
follow-up), because both were work nobody but their author had read, and M8
cannot close while M7's review is open.

| Reviewer | Partition | Decision | Score |
|---|---|---|---|
| R1 | chain semantics / round-3 fixes as code | **REQUEST CHANGES** | 71/100 |
| R2 | identity / persistence / serializer | **REQUEST CHANGES** | 68/100 |
| R3 | evidence quality / M7 round-2 follow-up / the record | **REQUEST CHANGES** | 76/100 |

**Round verdict: REQUEST CHANGES -- 3 Criticals.** The round-N pattern held for
the fourth time, and twice in its sharpest form: **a fix that named a hole in
neighbouring code and did not close it**, and **a table that reproduced the
table-drift defect inside the commit that fixed table drift**.

Two findings were reached independently by two reviewers each -- the
const-accessor class and the consumer-table drift -- which is the strongest
signal this protocol produces.

## Round-4 findings register

### Critical

| ID | Finding | Status |
|---|---|---|
| R1R4-C1 | **The round-3 duplicate-id guard is one-directional.** `restorePlaceholderFeature` got a two-half check and its own comment stated the general fact -- placeholders are unregistered, so a placeholder-held id defeated the sibling guards -- but **no sibling was changed**. All ten other restore paths still checked only `registry_.contains`. Restoring a Pad onto a placeholder's id left two features carrying one ObjectId in one Body; the repair was worse than the disease -- `removeObject` resolved the Pad through the registry, unregistered it, dropped its graph node, then `Body::removeFeature` erased the FIRST match (the placeholder), leaving the Pad an unregistered, graph-less, unremovable orphan **that then saved and loaded cleanly as a healthy Pad**. Silent divergence between memory and file; ADR-M8-008's unconstructible state constructed through public calls alone. Demonstrated by execution. | FIXED |
| R2R4-C1 | **`savePartDocument` writes a `dependencies` edge its own loader refuses** -- ADR-M3-008's named worst class, **sixth recurrence**, a different route from round 3's. The writer persists any edge whose endpoints are persisted ids and whose DEPENDENT is not a feature; the loader accepts an endpoint only if it is a PARAMETER. So `addDependency(parameterId, featureId)` -- four public facade calls -- saved OK and load-refused. `validateSaveable`, extended in round 3 precisely as this net, never walked the graph at all. Demonstrated by execution. | FIXED |
| R1R4-C2 | **The X2 masked-by-design row is refuted.** Round 3 recorded the save-side id-uniqueness net as unreachable ("the facade guard refuses every current route first"). R1R4-C1 was a current route that landed on it, and R2R4-m1 found a second (the document's own id). The net was load-bearing while documented as dead -- and therefore untested. | FIXED (both routes closed at the guard; the record corrected below) |

### Major

| ID | Finding | Status |
|---|---|---|
| R1R4-M1 / R2R4-M1 | **The const-accessor class was declared closed with its largest doors open** (two reviewers, two different doors). `bodies()` then `Body::features()` stops constness at the pointer twice, so `Feature::setSuppressed`, `markDirty`, `setMaterialReference` and `clearMaterialReference` were reachable from a `const PartDocument&` -- state changed and a document made unsavable, in code that compiles, with no C2248 counterpart to round 3's X6. Separately `ObjectRegistry::find` was const but its variant held NON-const pointees, handing out mutable `Material*`, `Sketch*` and `Feature*` -- reopening two of the three doors round 3 had just shut (a density doubled through a const document, cached mass still valid). | FIXED -- Feature's mutators and `IMaterialReferencing`'s are private with `friend PartDocument`; `ObjectRegistry` grew a `ConstObjectRef` projection so the const overload yields const pointees; `Material::setDensity` private, closing the last non-const door (`addMaterial` returns `Material&`) |
| R1R4-M2 / R2R4-M2 | **`kConsumingFeatureTypeNames` pins only some of its members while its comment claims all of them** (two reviewers). "Fillet/Chamfer by M8_SER_203" -- 203 swaps Pad and FILLET; nothing swapped Fillet and CHAMFER, so dropping "Chamfer" survived every shipped test. R3R3-M1 reproduced **inside the commit that fixed R3R3-M1, in the table that commit added.** | FIXED -- `M8_SER_205` moves a Chamfer ahead of the Fillet it consumes and word-pins the chain-walk diagnostic; the comment credits each member separately and states the rule for future types |
| R2R4-M3 / R3R4-M1 | **"reaches all seven branches" is false** (two reviewers): `validateSaveable` has EIGHT `capCheck` sites and the child test reached FIVE. The document, body and material branches were reached by nothing -- deleting body and material left everything green while an over-cap Body and an over-cap Material each saved OK and then load-refused. | FIXED -- the child test drives all eight, each in its own document, with the document's own id constructed after the advance |
| R2R4-M4 | **The reconstruction fingerprint's CONTENT is unpinned, and it omitted `SketchArc::counterClockwise`** while its comment claimed bit-identical geometry. Four separate field-contribution drops survived every test. The direction flag selects WHICH ARC the entity is, and the flip is reachable (`removeEntity` then `restoreEntity` reuses the id). | FIXED -- the flag is mixed; `M7_REV4_M4` pins every field by building twins with FIXED entity ids. *The first version of that test was itself non-discriminating* -- fresh documents gave fresh ids, so every row passed on the id contribution alone and dropping the circle radius still passed. Caught by mutating the line the test claims to pin, and recorded because it is the very class the test exists to close |
| R3R4-M2 | **The `reconstructionReports_` erase path was unreachable and its comment false**: `forgetProvenanceFor` had zero callers, and replacing `pruneProvenance`'s whole body with `return;` failed nothing anywhere. A Major closed by a fix no test could see. | FIXED -- `pruneProvenance` moved into `refreshAll`, so every path that rebuilds the shell prunes; the viewer selftest removes an imported sketch and asserts its report is gone (mutation ZA4 re-run: killed). The header states which of the three paths is actually wired |
| R3R4-M3 | **The skip-diagnostic row is pinned for EXISTENCE only** -- non-emptiness, directly under a comment condemning non-emptiness. Hard-coding every skip detail to the literal "42" left all viewer smokes and all ctest entries green. Third appearance of a twice-penalised class, reintroduced by the fix written to close it. | FIXED -- the row is asserted EXACT against the report's own composed string (mutation ZC1 re-run: killed) |
| R3R4-M4 | **`SketchAlreadyReconstructed`'s public header contract is false**: round 2's fix widened the predicate to "any constraint at all" and documented that in the .cpp, leaving the header -- the thing a caller reads -- describing the old "dimensional constraints bound to Parameters" rule. | FIXED -- the header states the real predicate, its cost, and why the name is wider than it looks |
| R3R4-M5 | **The record asserts, in three documents, things `7a60b6b` made false**: `AGENTS.md` still said "Six Majors remain OPEN" and contradicted itself twenty lines apart; `M7_CompletionReport.md` still read "round 2 required", "Round 2 review: NOT EXECUTED", and "All four Criticals and all fourteen Majors are now fixed and mutation-verified" -- the exact sentence round 2 refuted. | FIXED -- all three corrected, with the refuted sentence struck through rather than deleted |

### Minor

| ID | Finding | Status |
|---|---|---|
| R1R4-m1 | `PartDocument::setSuppressed` set the GRAPH node only, so `ComputeState::Suppressed` was unobservable through the facade -- the graph said Suppressed while the feature said Dirty, and no UI could report suppression at all. | FIXED -- the facade writes both. The deeper half (M2's rule that dirtiness propagates THROUGH a suppressed node, wrong for an M8 chain) is a semantics change with an ADR attached and belongs to M9.3; recorded at the code and in `M9_SPEC.md` section 3.1 |
| R2R4-m1 | The duplicate-id guard had a THIRD blind spot: the document's own id (a `PartDocument` does not register itself). Build-OK, then save-refused for ever. | FIXED -- checked first, pinned by `M8_REV_355` |
| R1R4-m2 | `requireConsumableBase` took the LAST id match instead of the first (no `break`). | FIXED |
| R2R4-m2 / R3R4-m1 | The per-sketch entity/constraint uniqueness checks added for R2-M2 are unreachable (`restoreEntity`/`restoreConstraint` already refuse duplicates) and were recorded as a Major closed rather than as masked-by-design. | RECORDED below, V8/X2-style |
| R2R4-m3 | An edge with a non-persisted endpoint (e.g. Sketch to Parameter) is silently DROPPED at save: save OK, load OK, different document. | **OPEN -- recorded, not fixed.** It is the inverse of R2R4-C1 and the honest fix has the same shape (refuse rather than diverge), but it changes what existing documents can save, so it is named here for the owner rather than decided inside a review-fix round |
| R3R4-m2 / R3R4-m3 | "797/797" and "432/432" repeat the registered-vs-executing nit, and the current totals appeared in no shipped document. | CORRECTED -- counts are stated as registered/executing below |

## Masked by design (stated, never counted as coverage)

- The per-sketch entity and constraint uniqueness checks in `validateSaveable`
  (R2-M2's fix): unreachable through the public API, because
  `Sketch::restoreEntity` and `restoreConstraint` refuse a duplicate id first.
  Deleting both fails nothing, by design.
- `Body::removeFeature` erasing by POINTER IDENTITY rather than by id: with
  `requireUnusedId` on every restore path a duplicate-id body is
  unconstructible, so reverting to first-id-match fails nothing (round-4
  mutation F, verified UNGUARDED). Defense in depth with no reachable failure.
- The save-side id-uniqueness net is **no longer** listed here -- see R1R4-C2.
  Both routes that reached it are now closed at the guard, which puts it back in
  backstop position, but it is not recorded as unreachable-by-inspection again:
  two rounds running, an inspection-only "unreachable" claim turned out wrong.

## Round-4 fix verification (Z-battery)

Same discipline throughout: binaries deleted before each rebuild and asserted
present afterwards, plain-copy plus `touch` restores, every mutation
`cmp`-verified as landed before any verdict was believed.

| # | Mutation | Verdict |
|---|---|---|
| A | dress "Base feature" panel row deleted | **guarded** -- `m8-dress` only |
| B | revolve "Angle" panel row deleted | **guarded** -- `m8-revolve` only |
| C | the fillet never created in the m8-dress sample | **guarded** -- four assertions, including the volume oracle |
| D | revolve sample angle 2pi to pi | **guarded** -- volume oracle and Angle row |
| E | the feature-scan half of `requireUnusedId` removed | **guarded** -- M8_REV_342, 351, 352, 353 |
| F | `Body::removeFeature` back to first-id-match | **UNGUARDED -- masked by design**, stated above |
| G | "Chamfer" dropped from `kConsumingFeatureTypeNames` | **guarded** -- M8_SER_205, and by nothing else, which was the finding |
| H | `mixDouble(circle->radiusMm)` dropped from the fingerprint | **guarded** -- M7_REV4_M4 *(and UNGUARDED against that test's first version, which is why it was rewritten)* |
| H2 | the arc direction contribution dropped | **guarded** -- M7_REV4_M4 |
| H3 | the entity id contribution dropped | **guarded** -- M7_REV4_M4 |
| ZA4 | `pruneProvenance` body replaced with `return;` | **guarded** -- viewer selftest |
| ZC1 | every skip row's detail hard-coded to "42" | **guarded** -- viewer selftest |

## What round 4 confirmed sound (honest negatives)

Both prior batteries survived row-by-row re-execution by an independent
reviewer: the **X-battery 6/6 CONFIRMED** and the **Y-battery 4/4 CONFIRMED**,
every kill list matching the record test for test -- the first batteries in this
project a reviewer could not overstate. `M8_REV_322` genuinely pins all six
solid type names (reviewers dropped names the X-battery never touched and it
died every time). Both halves of the round-3 placeholder guard are real and
independently pinned. The GATE_E3 two-layer credit round 3 rewrote is exactly
right in both directions. Every geometry oracle was re-derived independently and
confirmed again (94000, 40000pi, the four-term Minkowski rounded box, the 320pi
Pappus chamfer). `DocumentRecomputeEngine`'s friendship is not abusable; the
`Parameter*` door is genuinely shut; `setParameterExpression` covers everything
the private mutators served; save and load uniqueness scopes are exactly
equivalent in both directions; Gate J's redirect is load-bearing in both
directions, proven with a negative control; `Shuffled_*` entries exist for all
five suites and discriminate; X6's compile-time closure is real; Release matches
Debug exactly.

## Totals, stated as registered / executing

Measured on the round-4 fix head, both configurations, with nothing else
running (a count taken while a second test run was in flight read one test
lower -- the two contend for the generator-limit proof file, which is a
measurement hazard, not a product defect, and is recorded so the next reader
does not chase it):

| | Debug | Release |
|---|---|---|
| ctest entries registered | **808** | **808** |
| of those, executing | **804** | **804** |
| registered-Skipped children (spawned by their parents) | 4 | 4 |
| ctest result | 808/808 pass | 808/808 pass |
| Single-process `ParametricCADCoreTests.exe` | 444 registered / **441 pass** / 3 child-Skipped | 444 registered / **441 pass** / 3 child-Skipped |

Round-4 baseline was 797 registered / 793 executing and 435 / 432, so the round
added 11 ctest entries and 9 Core tests.

## Standing blocks after round 4

Round 4's own fixes are **unreviewed**, as every round's have been. The change
set is again smaller than the last. Round 5, or accepted-and-recorded risk, is
the owner's call.

Owner UI validation remains open for **M6, M7 and M8**. M8's checklist now
exists (`M8_UI_UserValidation.md`) and covers all four required features,
because the shell gained `--sample m8-revolve` and `--sample m8-dress`; before
those, Revolve, Fillet and Chamfer were unreachable from the running
application and this validation could only ever have covered the pocket.
