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

## First milestone
1. Create a PartDocument.
2. Add parameters Width, Height, Length.
3. Recompute a simple parametric solid through a kernel adapter.
4. Recalculate volume, mass, center of mass, and inertia.
5. Save/load the document.
6. Keep Core free of Qt and OpenCASCADE types.

See `docs/Architecture.md` and `docs/Roadmap.md`.

## Multi-Agent Workflow

This starter includes an orchestration contract in `AGENTS.md` and `docs/OrchestratorGuide.md`. In an AI environment that supports sub-agents, a non-trivial task should automatically use separate Architect, Developer, Tester, and Reviewer roles. See `docs/AgentRoles.md` for responsibilities.
