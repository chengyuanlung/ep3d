# M3 Completion Report

**Milestone:** M3 — Geometry Kernel Adapter & First Parametric Solid
**Baseline:** `8245c89` (M2, APPROVE 100/100)
**Final commit:** `f583f29` on `master` (see "Commit" below)
**Date:** 2026-08-09

---

## Result

**M3 COMPLETE.** Independent review: **APPROVE, 97/100**. No Critical findings,
no unresolved Major findings. Spec §28 final gate: all 34 items pass.

---

## Implemented

- Kernel-neutral geometry boundary (`IGeometryKernel`, `KernelShape`,
  `KernelTypes`) inside `src/Core`, containing zero OCCT.
- OCCT-backed adapter (`src/Kernel/Occt`) — the only place OCCT is included or
  linked. `BRepPrimAPI_MakeBox` for construction, `BRepGProp`/`GProp_GProps` for
  mass properties.
- `BoxFeature` with Width/Height/Depth parameters participating in the M2
  dependency graph and recompute engine; kernel injected through
  `RecomputeContext::kernel`, never constructed by the feature.
- `MassPropertiesNode` producing exact Volume, Mass, COM and a 3x3 inertia
  tensor, with a single traceable mm→m conversion site.
- Transactional recompute: a failed rebuild retains the last valid shape and
  numbers while marking them unequivocally stale; recovery is deterministic.
- Schema v3 persistence of semantic state only; no runtime OCCT object,
  handle, or pointer is ever serialized. v1 and v2 files still load.
- `FakeGeometryKernel` test double, so all Core-side behaviour is testable
  without linking OCCT at all.

## Not implemented (spec §3 non-goals, unchanged)

Qt viewport, DXF, sketch solver, Pocket/Fillet/Chamfer, Assembly, Joint,
Collision, Robot, dynamics, FEA, CAM, multithreaded recompute, scripting,
plugins.

---

## OCCT integration

| | |
|---|---|
| Version | **8.0.1** (`opencascade[core,freetype]:x64-windows`) |
| Discovery | `find_package(OpenCASCADE CONFIG)` via vcpkg toolchain file |
| Configure | `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` |
| Runtime DLLs | `VCPKG_APPLOCAL_DEPS` copies 4 OCCT DLLs beside each linked binary |
| Debug/Release | Both build and pass; no configuration-specific behaviour |
| Without OCCT | Core and all Core-only tests still build; only the OCCT targets are skipped |

No developer-specific absolute path appears anywhere in `CMakeLists.txt`
(spec §16). Discovery is hardened with `NO_CMAKE_PACKAGE_REGISTRY
NO_CMAKE_SYSTEM_PACKAGE_REGISTRY` — this machine carries a stray
`export(PACKAGE OpenCASCADE)` entry pointing at an unrelated OCCT build, and the
guard was confirmed to hold (resolved `OpenCASCADE_DIR` is the vcpkg install).

---

## Files

**Added:** `src/Core/Kernel/{KernelTypes.h, KernelShape.h/.cpp, IGeometryKernel.h}`,
`src/Core/Feature/BoxFeature.h/.cpp`, `src/Core/Physics/MassPropertiesNode.h/.cpp`,
`src/Kernel/Occt/{OcctShape.h, OcctGeometryKernel.h/.cpp}`,
`tests/Fakes/FakeGeometryKernel.h`, `tests/BoxFeatureTests.cpp`,
`tests/MassPropertiesNodeTests.cpp`, `tests/SerializationV3Tests.cpp`,
`tests/M3ReviewFindingTests.cpp`,
`tests/Kernel/{OcctGeometryKernelTests.cpp, OcctRecomputeIntegrationTests.cpp}`,
`docs/reviews/{M3_SelfValidationReport.md, M3_IndependentReview.md, M3_CompletionReport.md}`.

**Modified:** `CMakeLists.txt`, `README.md`, `AGENTS.md`, `docs/Roadmap.md`,
`docs/DecisionLog.md`, `src/Core/Body/Body.h/.cpp`,
`src/Core/Document/PartDocument.h/.cpp`, `src/Core/Feature/Feature.h`,
`src/Core/Feature/PlaceholderFeature.*`, `src/Core/Material/Material.*`,
`src/Core/Physics/MassProperties.h`, `src/Core/Document/ObjectRegistry.h`,
`src/Core/Recompute/RecomputeContext.h`,
`src/Core/Serialization/PartDocumentSerializer.cpp`, `src/App/main.cpp`.

---

## ADRs

| ADR | Decision |
|---|---|
| M3-001 | Kernel boundary and shape ownership; transactional `currentShape_` |
| M3-002 | Units (mm / kg·m³ / kg / kg·m²), single conversion site, tolerances |
| M3-003 | Kernel service injection through `RecomputeContext` |
| M3-004 | Failed-feature retention; density policy (zero valid, negative/non-finite fail) |
| M3-005 | Schema v3; Option-A/B edge-persistence split; closes the ADR-009/012 `dynamic_cast` debt |
| M3-006 | **Retention vs currency** for derived results (post-review) |
| M3-007 | **Cached feature state is derived, never authoritative**; features are heterogeneous (post-review) |
| M3-008 | **Removal completeness and save/load symmetry** (post-review) |
| M3-009 | **Minimum accepted box dimension** (post-review) |

Declared deviation (not silent): the kernel-neutral interface lives under
`src/Core/Kernel/` rather than spec §5's suggested top-level `src/Kernel/`, so
that Core can reference it without violating CodingRule 2. `src/Kernel/Occt/` is
exactly where the spec puts it.

---

## Build and test evidence

All figures below were produced by executing the commands, in this working tree.
The independent reviewer reproduced every one of them from a build tree deleted
and recreated from empty.

| | Debug | Release |
|---|---|---|
| Clean configure + build | exit 0 | exit 0 |
| Tests | **180 / 180** | **180 / 180** |
| Failures | 0 | 0 |

**Test accounting:** 180 = 103 (M2 baseline) + 37 (M3 Core, phases 1–4)
+ 23 (OCCT-linked) + 17 (post-review regression). All 103 baseline tests still
pass; none was modified, deleted or skipped.

**Architecture boundary.** `src/Core` scan for all 9 spec-§20 patterns
(`TopoDS_`, `BRep`, `gp_`, `AIS_`, `V3d_`, `OpenCASCADE`, `QObject`, `QWidget`,
`QString`): **0 matches**, so there are no false positives to explain. Every
non-Core `#include` under `src/Core` is a standard-library header.

Binary-level confirmation via `dumpbin /dependents` — stronger than a text scan,
since it reflects what the linker actually resolved:

| Binary | OCCT (`TK*`) DLL imports |
|---|---|
| `ParametricCADCoreTests.exe` (links Core only) | **0** |
| `ParametricCADKernelOcctTests.exe` | 4 |
| `ParametricCADApp.exe` | 4 |

---

## Mandatory release gate (spec §19)

`IntegrationTest.M3_GATE_ReleaseScenario`, steps A–E verbatim, against **real
OCCT geometry** — **PASS in both Debug and Release**.

| Step | Expected | Result |
|---|---|---|
| Initial | V=100000 mm³, m=0.27 kg, COM=(50,25,10) | PASS |
| A — Width→120 | Box +1, Mass +1, V=120000, m=0.324, COM=(60,25,10) | PASS |
| B — Density→7850 | Box unchanged, Mass +1, V/COM unchanged, m=0.942, inertia changes | PASS |
| C — Width→0 | Feature failed, downstream not current, diagnostic, no crash | PASS |
| D — Width→80 | Recovered, V=80000, m=0.628 | PASS |
| E — Save/load/recompute | Equivalent geometry, material, mass properties, stable ids | PASS |

**Analytical oracles** are computed independently in the tests from raw formulas,
never via production helpers (spec §10).

One correctness question carried real risk and is worth recording as settled:
`GProp_GProps::MatrixOfInertia()` is **already about the centre of mass**, so no
parallel-axis (Huyghens) correction is needed. Under the corner-origin
convention the COM is at (50,25,10), not the origin, so an origin-relative
matrix would differ by terms of the same order as the diagonal itself — the
oracle passing at 1e-9 relative tolerance can only happen if it is COM-relative.
The reviewer independently confirmed this from OCCT source
(`GProp_GProps.cxx` subtracts the parallel-axis term) and numerically.

---

## Self-validation

`docs/reviews/M3_SelfValidationReport.md`, all 17 spec-§20 items executed and
recorded. Revision 3 is final.

**It was not reliable, and that is recorded in it.** The revision-1 self-score of
94/100 certified "no Major self-findings" for code that had five, and stated a
regression baseline that was simply wrong (140 instead of 103 — a working-state
count mistaken for the baseline).

---

## Independent review

`docs/reviews/M3_IndependentReview.md`. A genuinely separate reviewer agent was
run — this was not self-review, and no claim of independent review is made
beyond what actually happened.

| Round | Decision | Score | Findings |
|---|---|---|---|
| 1 | REQUEST CHANGES | 92/100 | 5 Major, 0 Critical |
| 2 | REQUEST CHANGES | 91/100 | 4 Majors resolved; **1 Critical introduced by the Major-2 fix** |
| 3 | **APPROVE** | **97/100** | 0 Critical, 0 Major open |

### What the review caught

**Round 1 — five Majors, all reachable through ordinary public API use:**
`MassProperties::valid` set once and never cleared (stale numbers reported
themselves as current, violating spec §2's Definition of Done);
`Feature::markDirty()` with zero callers anywhere in `src/` (a restored feature
claimed `Valid` while holding no shape); `removeObject` reporting success while
leaving the feature owned by its Body (every later recompute failed forever, and
the "deleted" box came back on load); `savePartDocument` accepting documents the
loader rejects (a file that saved cleanly and could never be loaded again,
potentially over the only copy of the data); and the wrong baseline above.

**Round 2 — a Critical introduced by my own fix.** The Major-2 fix called
`graph_.state()` for every cached-`Valid` feature, but only `BoxFeature` has a
graph node and `DependencyGraph::state()` asserts on unknown ids. Debug aborted
the process — including inside `loadPartDocument`, so opening a file crashed —
while Release silently rewrote persisted state, leaving the two configurations
disagreeing about document semantics. **The 174-test suite passed throughout.**
Fixed with a `hasNode` guard, framed in ADR-M3-007 as a scope rule (features are
heterogeneous; a graph-less feature's state is owned by whoever drives it) rather
than as defensive coding.

Every fix is covered by regression tests that were verified to actually fail
against the unfixed code.

### One reviewer recommendation was declined

Restricting the public non-const `PartDocument::massProperties()`: every read
through a non-const document selects that overload too, so restricting it churns
~20 call sites for no behavioural gain. Documented in the header as an M4
candidate; the reviewer accepted the reasoning and withdrew the item.

---

## Limitations and deferred work

Declared, not hidden — each is recorded in an ADR:

1. **`MassPropertiesNode` is a document singleton** hard-wired to the most
   recently added/restored `BoxFeature`. Appropriate to "first parametric
   solid"; M4+ Pad/Pocket chains need real shape-source selection (ADR-M3-005).
2. **`Feature::recompute()` (the M1 no-context, bool-returning method) is
   vestigial** on `BoxFeature`; the M3 engine path never calls it. Candidate M4
   cleanup: collapse `Feature` into `IRecomputable` (ADR-M3-004).
3. **Public non-const `massProperties()`** — see above.
4. **Layout deviation** from spec §5 — declared in ADR-M3-001.
5. **Feature heterogeneity is now a live concern.** Two defects in this milestone
   came from assuming every `Feature` behaves like a `BoxFeature`. ADR-M3-007
   states the forward rule: any code iterating `Body::features()` must say which
   kinds of feature it applies to.

Remaining Minor items are cosmetic (an identity-default `Matrix3` making an
uncomputed tensor read as `Ixx=1`) and carry no correctness impact.

---

## Commit

**`master` = `f583f29`** — "M3: geometry kernel adapter and first parametric
solid". 45 files, +5162 / −65. Squashed from `m3-wip` per the handoff plan, so
`master` keeps one clean commit per milestone in the style of M1/M2:

```
f583f29  M3: geometry kernel adapter and first parametric solid
8245c89  M2: document recompute infrastructure
34f67cf  M1: native JSON serialization (schema v1) + GoogleTest adoption
2f9768a  Initial commit: ParametricCAD starter + DependencyGraph milestone
```

The squashed result was re-verified rather than assumed equivalent: clean
configure and build from an empty tree, **180/180 in Debug and Release**, Core
boundary scan 0 matches.

`m3-wip` (`4a4f3ad`, pushed) retains the development history —
`c51a14f` (WIP, phases 1–4) → `f4916af` (handoff notes) → `69c6ab1` (M3
complete) → `4a4f3ad` (hash backfill).

One file was deliberately **not** carried into `master`:
`docs/reviews/SESSION_HANDOFF_M3.md`. It was written mid-migration and every
substantive claim in it is now false — it calls the old 15.7 GB machine "this
machine", describes the OCCT build as an unresolved blocker, and calls the
Phase 5 adapter an uncompiled draft. Accurate when written, misleading in a
clean milestone history. It remains on `m3-wip` as the historical record.
That is the only difference between the two trees.

---

## M4 readiness

**READY.**

The Core/Kernel seam holds at both source and binary level and already carries
two independent kernel implementations, which is the property M4's viewer work
depends on. The density-independent second-moment split composes across
instances without re-entering the kernel — what Assembly and Physics will need
later. The three decisions taken under review pressure (ADR-M3-006/007/008) are
general rules rather than local patches.
