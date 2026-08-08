# Windows 11 / Visual Studio 2022 Build Guide

## Prerequisites

Install Visual Studio 2022 with:
- Desktop development with C++
- MSVC x64 toolchain
- Windows 11 SDK
- CMake tools for Windows

Also install Git.

Qt and OpenCASCADE are intentionally not required for the current Core-only starter. They will be integrated in later milestones so the first build stays small and deterministic.

## Command line build

Open **x64 Native Tools Command Prompt for VS 2022** or a terminal with CMake/MSVC available:

```powershell
cd D:\Projects\ParametricCAD
cmake -S . -B build -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

## Visual Studio

Visual Studio 2022 can open the folder containing `CMakeLists.txt` directly:

1. File → Open → Folder.
2. Select the ParametricCAD folder.
3. Allow CMake configuration to finish.
4. Choose x64 Debug.
5. Build All.
6. Run `ParametricCADApp`.

## Next dependency milestone

After M1 Core stabilizes:
1. Add a dedicated `src/Kernel` target.
2. Integrate OpenCASCADE only into `Kernel`.
3. Add `src/UI` and Qt Widgets.
4. Keep `ParametricCADCore` independently buildable and testable.
