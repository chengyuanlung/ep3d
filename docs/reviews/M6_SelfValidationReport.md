# M6 Self-Validation Report — DXF Import to Stable Sketch Entities

**Baseline:** `a6e7078` — the accepted M5 master state.

**Commit under validation:** branch `m6-wip`, head recorded in
`M6_CompletionReport.md` when the milestone closes.

Spec 22 governs this document: nothing here is reported PASS unless it was
actually executed, and anything not executed says **NOT EXECUTED** and why.

---

## Environment

| | |
|---|---|
| OS | Windows 11 Home 10.0.26200 |
| Compiler | MSVC 19.44 (VS 2022 BuildTools 14.44.35207), C++20 |
| CMake | 4.2.3 |
| OCCT | 8.0.1 (vcpkg) |
| Qt | 6.11.1 (vcpkg) |
| Eigen | 5.0.1 (vcpkg), MPL2 |
| **DXF parser** | **libdxfrw `2025-09-25` (vcpkg), GPL-2.0-only** |

---

## Dependency and licence audit (spec 20)

| | |
|---|---|
| Library | libdxfrw |
| Version | `2025-09-25`, vcpkg port, x64-windows |
| Licence | **GPL-2.0-only** |
| Linkage | dynamic (vcpkg x64-windows triplet default) |
| Target | `ParametricCADImportDxf` **only**, `PRIVATE` |
| Translation units naming it | **one** — `src/Import/Dxf/DxfReader.cpp` |
| Crosses into Core? | **No** — measured below |
| Owner decision | **Yes.** The owner was asked and chose libdxfrw over `dime` (BSD-3-Clause) and over an own minimal reader. Recorded in ADR-M6-001. |

**The licence consequence, stated rather than buried.** GPL-2.0-only is
copyleft. Linking it — statically or dynamically — generally makes the combined
work a derivative that must be distributed under GPL-2.0-compatible terms,
including corresponding source. `ParametricCADViewer` links it, so the shipped
application inherits that. If EP3D is to be distributed as closed-source
commercial software, this dependency must be removed or replaced first. Nothing
in the build warns about it, which is why it is written in three places: the
ADR, `CMakeLists.txt` beside the `find_package`, and here.

---

## Core boundary audit (spec 5, ADR-M6-003)

| Check | Method | Result |
|---|---|---|
| Core free of DXF types | `grep -rnE "DRW_\|libdxfrw" src/Core/` excluding comments | **PASS** — zero |
| Core free of Qt / OCCT / Eigen | same scan | **PASS** — zero |
| libdxfrw symbols in `ParametricCADCore.lib` | `dumpbin /symbols` | **PASS** — **0** |
| libdxfrw symbols in `ParametricCADViewerCore.lib` | `dumpbin /symbols` | **PASS** — **0** |
| libdxfrw symbols in `ParametricCADImportDxf.lib` | `dumpbin /symbols` | 205 — the boundary is where it should be |
| Core test binary dependents | `dumpbin /dependents` on the Release exe | **PASS** — KERNEL32, MSVCP140, VCRUNTIME140(_1), UCRT only |
| Import representation is neutral | read `ImportedGeometry.h` | **PASS** — coordinates, radii, angles, unit metadata and skip records; no parser pointer, no handle, no file offset |

Core compiles, links and tests with libdxfrw absent: the import target is behind
`if(libdxfrw_FOUND)` and Core links neither it nor the library.

---

## Builds and regression

| | Result |
|---|---|
| Debug build | **PASS** — 0 errors |
| Debug tests | **539 / 539** |
| Release build | **PASS** — 0 errors |
| Release tests | **539 / 539** |
| Release actually ran Release binaries | **PASS** — 539 of 539 ctest command lines name `build/Release/...`, zero name `build/Debug/` (301 Core, 87 Integration, 53 Solver, 47 KernelOcct, 38 Import, 13 Viewer) |
| M0–M5 regression | **PASS** — every pre-M6 test still passes |

Baseline was 498 at the end of M5; **41 tests added**.

---

## Release gates (spec 16)

| Gate | Result | Evidence |
|---|---|---|
| **A** — LINE import against a hand-computed oracle | **PASS** | `M6_GATE_A` — (0,0)→(100,50) read from a fixture small enough to check by eye |
| **B** — CIRCLE centre and radius | **PASS** | `M6_GATE_B` — centre (25,30), radius 10; centre deliberately off the origin so dropping it would show |
| **C** — ARC centre, radius, start/end direction, orientation, measured geometrically | **PASS** | `M6_GATE_C` computes the arc's start and end POINTS from the model and compares them with hand-computed coordinates; `M6_ARC_001/003` measure the degree→radian conversion and the sweep through 0° |
| **D** — mixed file: count, kinds, unique ids, no dependence on array position | **PASS** | `M6_GATE_D_MixedFileImportsEveryKind`, `..._FileOrderIsNotIdentity` (same entities, shuffled file, geometry compared as sets), `..._RepeatedImportsOfOneFileAgree` |
| **E** — save/load identity | **PASS** | `M6_GATE_E_MixedImportSurvivesSaveLoadWithEveryId` — every id resolves after reload and keeps its kind |
| **F** — source independence | **PASS** | `M6_GATE_EF` asserts the saved document contains no `.dxf` reference at all, then reloads and checks the geometry |
| **G** — imported closed profile drives 3D | **PASS** | `M6_GATE_G` — 60×40×20 = 48000 mm³ and 0.1296 kg at 2700 kg/m³, both hand-computed; still parametric afterwards; survives save/load onto a **fresh** kernel |
| **H** — invalid/unsupported input | **PASS** | `M6_GATE_H` ×3 — malformed file diagnostics, a failed import leaving sketch count, registry size and graph node count unchanged, and NaN/∞ rejected at the importer |
| **I** — regression, Debug and Release, Release proven | **PASS** | see Builds above |

Every expected number in Gates A–C and G is computed by hand from the fixture.
No expectation was produced by running the importer.

---

## Fixtures (spec 15)

All hand-written and small enough to read. Several are built to make a passing
test mean something:

| Fixture | Deliberate design |
|---|---|
| `line.dxf` / `line_inches.dxf` | **Identical coordinates, different units.** If the unit conversion never ran, both tests would pass with the same expectations. |
| `circle.dxf` | Centre **off the origin** — a reader that dropped the centre would still get the radius right. |
| `circle_metres.dxf` | Centre and radius are separate group codes; scaling one and not the other is an easy mistake, so both are asserted. |
| `arc.dxf` | 30°/200°: not 0/90/180, sweep 170° so neither a quarter nor a half turn, centre off-origin with **negative y**. An unconverted 30 would mean 30 **radians** = 107°, a different point. |
| `arc_crossing_zero.dxf` | 350°→40°, a 50° sweep **through zero**. An importer assuming `end > start` gets it backwards; an arc that does not cross zero cannot reveal that. |
| `unsupported.dxf` | The unsupported entity sits **between** two supported ones — a trailing one could not distinguish "skipped it" from "stopped there". |
| `mixed.dxf` / `mixed_shuffled.dxf` | Same entities, different order, geometry compared as **sets**. |
| `closed_rectangle.dxf` | Sides listed **out of traversal order with two reversed**, because a real DXF has no obligation to list a loop in order. |
| `*_degenerate.dxf` | Bad entities **between** valid ones: abandoning the file returns too few, silently repairing returns too many. |

---

## Mutation verification (spec 18)

Every mutation was applied, the suite rebuilt, the failures recorded, then the
source restored **and rebuilt again** before re-verifying.

| Mutation | Tests killed |
|---|---|
| Omit the unit conversion | `M6_UNITS_001`, `M6_UNITS_003` |
| Swap ARC start/end | `M6_GATE_C`, `M6_ARC_001`, `M6_ARC_003`, `M6_ARC_004` |
| Skip one imported entity kind | 7 tests incl. `M6_GATE_D`, `M6_CIRCLE_003` |
| Break transaction rollback | 5 tests incl. both `M6_TRANSACTION_*` and `M6_GATE_H` |
| Duplicate an imported entity | 13 tests incl. `M6_GATE_A`, `M6_GATE_D`, both `M6_GATE_G` |
| Stop reporting skipped entities | `M6_SKIP_001` |

**Unguarded mutations: none.**

---

## Adversarial coverage (spec 17)

| Case | Status |
|---|---|
| Zero-length LINE | **PASS** — `M6_ADV_001` |
| Zero / negative CIRCLE radius | **PASS** — `M6_CIRCLE_002` |
| Invalid ARC radius | **PASS** — `M6_ARC_005` |
| ARC crossing 0° | **PASS** — `M6_ARC_003` |
| Shuffled DXF entity order | **PASS** — `M6_GATE_D_FileOrderIsNotIdentity` |
| Very small / very large coordinates | **PASS** — `M6_ADV_002` (0.001 mm and 10 m in one file) |
| NaN / Infinity | **PASS** — `M6_GATE_H_NonFiniteValuesNeverReachTheDocument` |
| Unsupported entity between supported ones | **PASS** — `M6_SKIP_001` |
| Import failure after valid entities were parsed | **PASS** — `M6_TRANSACTION_002` |
| Save/load after import | **PASS** — `M6_GATE_E`, `M6_GATE_EF` |
| Repeated import into one document | **PASS** — `M6_MIXED_001` |
| Deleting the imported sketch, then recompute | **PASS** — `M6_MIXED_002`, `M6_ADV_005` |
| Malformed DXF | **PASS** — `M6_GATE_H_AMalformedFileFailsWithAUsefulCause` |
| **Duplicate / missing DXF handles** | **NOT EXECUTED** — nothing in M6 reads a DXF handle, by design (ADR-M6-004): identity comes from the shared id generator, so a duplicate or missing handle cannot affect correctness. There is no code path to exercise. |
| **ARC near 360°** | **NOT EXECUTED** — `arc_crossing_zero.dxf` covers the wrap; a near-full-turn arc was not built. |
| **Fresh-process load** | **NOT EXECUTED as a separate process.** Save/load is exercised in-process and onto a fresh kernel. M5 found that in-process serialization tests are structurally blind to id-generator collisions; the equivalent risk here is covered by M5's `M5_SER_015`, but no M6 test starts a second process. |

---

## UI (spec 19)

| Requirement | Status |
|---|---|
| Invoke DXF import | **PASS** — File ▸ Import DXF… (Ctrl+I) |
| Select a file | **PASS** — `QFileDialog`; the only part a test cannot drive, and the only part left in the menu slot |
| Import into a Sketch | **PASS** — `ViewerSmokeTest_ImportsDxf` drives the whole path through `MainWindow` in the running application |
| Appears in the model tree | **PASS** — the selftest counts sketch rows in the outline, so an import the tree does not list fails |
| Visible through the normal presentation path | **PASS (functional)** — the imported sketch is an ordinary document object and goes through the same refresh; **pixels NOT EXECUTED** |
| Useful failure diagnostic | **PASS** — the reader's cause is carried through; `ViewerSmokeTest_ReportsFailedImport` requires a bad path to FAIL the selftest |
| **Owner manual UI validation** | **NOT EXECUTED** — the owner's to perform (ADR-M4-016), and it must never be described as an independent agent review |

---

## Known limitations

- **Supported DXF entities: LINE, CIRCLE, ARC only.** Everything else is
  reported as skipped and never reinterpreted. POINT, RAY, XLINE, ELLIPSE,
  LWPOLYLINE, POLYLINE, SPLINE, INSERT, TRACE, 3DFACE, SOLID, TEXT, MTEXT,
  DIMENSION, LEADER, HATCH, IMAGE and VIEWPORT are recognised by kind so the
  diagnostic can name them.
- **Binary DXF is untested.** libdxfrw reads it; no fixture exercises it.
- **Blocks (`INSERT`) are skipped, not expanded.** A drawing built from blocks
  imports as nothing plus a diagnostic.
- **3D DXF is flattened by ignoring Z**, and the extrusion direction (code 210)
  is not applied. A drawing on a non-XY plane imports at the wrong orientation.
  Not detected, not reported — **the most likely source of a silently wrong
  import**, and the first thing to fix if M6 is extended.
- **DXF `$INSUNITS` values other than 0/1/2/4/5/6** map to `Unrecognized` and
  take the millimetre default with a diagnostic.
- **The owner UI validation and an independent review have not been run.**

---

## Self score

**Not claimed.** M5 produced a self-validation reporting "Critical: 0" over a
broken mandatory constraint, and four review rounds then found defects the
previous round's fixes had introduced. A number I award myself before an
independent review is worth nothing.

## Ready for review

**YES.** Gates A–I pass, mutation verification shows no unguarded fix, the
dependency and boundary audits are measured rather than asserted, and the
limitations above are listed rather than omitted.
