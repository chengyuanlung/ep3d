# ParametricCAD Starter

This starter repository is the architectural foundation for a Windows-first parametric mechanical CAD platform with a future path to assemblies, mechanisms, robot simulation, collision checking, mass properties, and digital-twin workflows.

## Initial technology target
- C++20
- CMake
- Visual Studio 2022 x64
- Qt 6 Widgets (UI layer, to be added after Core is stable)
- OpenCASCADE (geometry/kernel layer, to be wrapped rather than leaked into Core)
- GoogleTest
- Git

## Milestone status
- M0 — Repository and Core conventions: COMPLETE.
- M1 — Parametric Part Core (data model, generic DependencyGraph, JSON schema v1, GoogleTest): COMPLETE.
- M2 — Document Recompute Infrastructure (ObjectRegistry, PartDocument recompute façade, parameter dirty sources, schema v2 dependency persistence): COMPLETE.
- M3 — Geometry Kernel Adapter & First Parametric Solid (kernel-neutral `IGeometryKernel` in Core, OCCT-backed `Kernel/Occt` adapter, `BoxFeature`/`MassPropertiesNode` real recompute, schema v3): COMPLETE.
- M4 — Qt viewer: READY.

`MassProperties` now holds real computed Volume/Mass/COM/Inertia once a `BoxFeature` recomputes through an injected `IGeometryKernel` (see `docs/DecisionLog.md` ADR-M3-002).

OCCT 8.0.1, discovered via a vcpkg toolchain file; build with
`-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`. Without OCCT the
Core library and its 157 Core-only tests still configure and build normally --
only `ParametricCADKernelOcct` and its test target are skipped.

See `docs/Architecture.md` and `docs/Roadmap.md`.

## Multi-Agent Workflow

This starter includes an orchestration contract in `AGENTS.md` and `docs/OrchestratorGuide.md`. In an AI environment that supports sub-agents, a non-trivial task should automatically use separate Architect, Developer, Tester, and Reviewer roles. See `docs/AgentRoles.md` for responsibilities.
