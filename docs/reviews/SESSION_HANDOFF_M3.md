# Session Handoff — M3 In Progress, Migrating to New Machine

**Written:** 2026-08-09
**Reason:** Moving to a machine with more RAM to unblock the OpenCASCADE build.

## Quick orientation

- Repo: `D:\Program2\EP3D\ParametricCAD_Starter` (adjust drive letter on the new machine).
- GitHub: `https://github.com/chengyuanlung/ep3d`.
- `master` — clean, reviewed history through M2: `2f9768a` (init) → `34f67cf` (M1) → `8245c89` (M2, APPROVE 100/100). Nothing more needed there.
- `m3-wip` — pushed branch containing **unreviewed, in-progress M3 work** (commit `c51a14f`). This is a checkpoint, not a milestone commit — do not treat it as reviewed or complete.
- The project follows `AGENTS.md`'s multi-agent orchestration contract: Architect → Developer → Tester → Reviewer, with an independent Reviewer that must not be the same agent as the Developer. Milestone specs (`docs/M1...`, `docs/M2...`, `docs/M3_Implementation_SelfValidation_and_Evaluation.md`) are authoritative when present — read the spec in full before resuming, don't rely only on this summary.

## What's done (M0–M2, on `master`, pushed, reviewed)

- **M0/M1**: Core conventions, `PartDocument`/`Parameter`/`Body`/`Feature`, native JSON serialization (schema v1), GoogleTest wired in. See `docs/DecisionLog.md` ADR-001–009.
- **M2**: `ObjectRegistry`, `IRecomputable`/`DocumentRecomputeEngine`, `PartDocument` recompute façade, schema v2 (dependency-edge persistence). Reviewed against `docs/M2_Implementation_and_Evaluation.md`'s own scorecard — final APPROVE 100/100, M3 READY. ADR-010–012.

## M3 status: IN PROGRESS, blocked on this machine, not blocked in general

**Spec:** `docs/M3_Implementation_SelfValidation_and_Evaluation.md` — "Geometry Kernel Adapter & First Parametric Solid." Introduces OpenCASCADE (OCCT)-backed real geometry (a parametric box) while keeping `src/Core` free of OCCT. Has its own self-validation protocol, reviewer scorecard, and release-gate scenario — read it in full.

**Architect contract:** already produced in this session (an M3-ADR delta resolving all 6 decisions the spec assigns to the Architect: kernel boundary/shape ownership, units, service injection, failed-feature policy, geometry persistence, file/CMake layout). It is **not saved to a file** — it only exists in the prior conversation turn and in what the Developer already implemented from it. **The safest path on the new machine: re-run the Architect agent against the spec to regenerate the contract** (cheap, ~2 min), rather than trying to reconstruct it from memory — but first read the code already written on `m3-wip`, since it reflects the contract's decisions concretely (ADR-M3-001 through ADR-M3-005 are written into `docs/DecisionLog.md` on that branch).

### What's actually implemented on `m3-wip` (Phases 1–4 of the spec's implementation order — zero OCCT needed, fully built and tested)

All of this is real, compiles, and passes **140/140 tests in both Debug and Release** on this machine before the branch was pushed:

- `src/Core/Kernel/{KernelTypes,KernelShape,IGeometryKernel}.h(.cpp)` — kernel-neutral interface, zero OCCT.
- `tests/Fakes/FakeGeometryKernel.h` — analytical test double so Core-side logic is fully tested without OCCT.
- `Feature::typeName()` pure virtual (finally closes the old ADR-009/012 `dynamic_cast` serializer debt), `PlaceholderFeature` updated, `MassProperties.inertiaTensorKgMm2` renamed to `inertiaTensorKgM2` (was mis-named and never populated), `Material::setDensity` + restore ctor, `ObjectRegistry`'s variant extended with `Material*`.
- `BoxFeature`, `MassPropertiesNode`, `PartDocument` façade additions (`addMaterial`/`restoreMaterial`/`setMaterialDensity`, `addBoxFeature`/`restoreBoxFeature`, `setGeometryKernel`/`geometryKernel`), `RecomputeContext` gained a forward-declared `IGeometryKernel* kernel` member.
- Schema v3 serializer: BoxFeature/Material JSON records, Option-A/B edge-persistence split (feature-owning edges and the `MassPropertiesNode`'s edges are always re-derived from semantic id fields on load, never replayed from the generic `"dependencies"` array — see ADR-M3-005 in `docs/DecisionLog.md`).
- **A real bug was found and fixed during this work**: `massPropertiesNode_` was being unconditionally added as a graph node in the `PartDocument` constructor, which made *every* document's `recompute()` permanently fail (an isolated, permanently-Dirty, permanently-failing node) even with no `BoxFeature` ever created. Fixed by deferring graph-node join to the first `addBoxFeature`/`restoreBoxFeature` call.
- ADR-M3-001 through ADR-M3-005 written into `docs/DecisionLog.md`.
- `README.md`/`docs/Roadmap.md`/`AGENTS.md` updated to say M3 is IN PROGRESS (not complete — don't let a future pass claim completion prematurely).

### What's drafted but NOT yet compiled or verified (needs OCCT — Phase 5)

- `src/Kernel/Occt/{OcctShape.h, OcctGeometryKernel.h/.cpp}` — `BRepPrimAPI_MakeBox` for box creation, `BRepGProp`/`GProp_GProps` for mass properties. One fact was verified against official OCCT docs (not just assumed): `GProp_GProps::MatrixOfInertia()` already returns the inertia matrix relative to the center of mass — no parallel-axis (Huyghens') shift needed in the implementation.
- `tests/Kernel/{OcctGeometryKernelTests.cpp, OcctRecomputeIntegrationTests.cpp}` — including a drafted `M3_GATE_ReleaseScenario` implementing spec §19's mandatory release gate verbatim.
- `CMakeLists.txt` Phase 5 section: `PARAMCAD_BUILD_KERNEL_OCCT` option, `find_package(OpenCASCADE CONFIG QUIET)`, the `ParametricCADKernelOcct` target, conditional `ParametricCADApp` linking, `ParametricCADKernelOcctTests` executable guarded by `if(TARGET ParametricCADKernelOcct)`.
- **Environmental hazard already found and fixed**: configuring without care picked up a stray, unrelated, broken OCCT install left on *this* machine at `D:\Program2\Step2Dispense\OCCT_build2` via CMake's ambient package registry. Fixed with `NO_CMAKE_PACKAGE_REGISTRY NO_CMAKE_SYSTEM_PACKAGE_REGISTRY` on the `find_package` call — **on the new machine, check whether any other stray OCCT/CMake package registry entries exist and could cause the same class of confusing failure** (`find_package(OpenCASCADE CONFIG)` silently picking up the wrong install).

None of this Phase 5 code has been compiled yet — treat it as a well-informed draft, not verified working code.

## The actual blocker: OCCT install kept getting killed on this machine

- Installed via vcpkg: `git clone https://github.com/microsoft/vcpkg.git`, `.\bootstrap-vcpkg.bat`, then `vcpkg install opencascade:x64-windows`. vcpkg lives at `D:\vcpkg`.
- This machine has only **15.7GB total RAM** across 12 logical cores — too tight for OCCT's default-parallelism MSVC build.
- **4 install attempts, all killed** (background command terminated, not a clean vcpkg error):
  1. Default concurrency — killed early.
  2. `VCPKG_MAX_CONCURRENCY=3` — killed, but got to **5650/5657** object files, in the final **linking** phase of large OCCT toolkit DLLs (TKOpenGl, TKDESTL).
  3. Retry at concurrency=3 (hoping Ninja would resume near the end via the source tarball's preserved mtimes) — killed again, this time during TKCAF linking (less far than attempt 2 — inconsistent, suggesting the linking phase itself is the memory-hungry choke point, not simple compile parallelism).
  4. `VCPKG_MAX_CONCURRENCY=1` (fully serial) — was running in the background at the time of this handoff; **check its actual final status** (`D:\Program2\EP3D\vcpkg_occt_install4.log`, or the more authoritative `D:\vcpkg\buildtrees\opencascade\install-x64-windows-dbg-out.log` / `install-x64-windows-rel-out.log` which are the real build logs vcpkg writes — the `Tee-Object`-piped session logs I was reading from lag/buffer and are less reliable).
- **No Windows OOM event was found** in the System event log (`Get-WinEvent` for IDs 2004/2005/2006/41/1074/6008 in the relevant time window came back empty), so the exact kill mechanism is inferred (memory pressure during large-DLL linking), not confirmed via OS-level evidence. It could also be a resource cap in the local tool/sandbox environment rather than Windows itself — worth keeping in mind if the same symptom recurs on the new machine despite more RAM.

### On the new "big memory" machine

1. Clone/pull the repo, check out `master` for the reviewed baseline, or `m3-wip` to continue exactly where this left off (recommended — it's a superset of master plus real, tested M3 progress).
2. Install vcpkg + OCCT the same way (`git clone microsoft/vcpkg`, bootstrap, `vcpkg install opencascade:x64-windows`) — with real headroom, default concurrency should just work; no need to fight `VCPKG_MAX_CONCURRENCY` unless the same symptom recurs.
3. Configure with `-DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake`.
4. Resume at **Phase 5** of the Architect's implementation order (see `m3-wip`'s `docs/DecisionLog.md` ADR-M3-001..005 for the exact design): build `ParametricCADKernelOcct`, run/fix the drafted OCCT tests, then Phases 6–8 (static Core-boundary scan, `docs/reviews/M3_SelfValidationReport.md` per spec §20–21 with **real** evidence, independent Reviewer pass per spec §22–24, `docs/reviews/M3_CompletionReport.md` per spec §25).
5. Do not skip the independent Reviewer step or the self-validation report — the spec is explicit that both are mandatory, and this project's convention (established across M1–M2) is a genuinely separate Reviewer agent, not self-review.
6. Once M3 is reviewed and approved, merge/rebase `m3-wip` into a clean `master` commit (or have the Developer redo it as a fresh commit off current `master` — either is fine, just don't carry the "WIP" commit message/history into `master` verbatim; squash or rewrite it into a proper milestone commit like M1/M2's).

## Any other loose ends

- `docs/reviews/` directory now exists (created for this handoff file) — the spec expects `M3_SelfValidationReport.md` and `M3_CompletionReport.md` there too.
- Long-term memory (persisted outside this repo, in the assistant's own memory system) has a note about this project; if resuming in a fresh assistant session on the new machine, it should already have context via that memory, but this file is the authoritative, complete state as of the migration.
