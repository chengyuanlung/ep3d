# Recommended Codex Workflow

Use Codex as an implementation agent, not as the architecture owner.

For each module:
1. Read `docs/Architecture.md` and `docs/CodingRules.md`.
2. Implement only the requested milestone/module.
3. Do not introduce Qt or OCC into `src/Core`.
4. Add/update tests.
5. Run CMake build and tests.
6. Summarize files changed, design assumptions, and remaining TODOs.

Example task:

> Implement M0 ObjectId, Vec3, Matrix3 and Transform3D in src/Core. Follow docs/Architecture.md and docs/CodingRules.md. Add GoogleTest coverage. Do not add Qt or OpenCASCADE dependencies.
