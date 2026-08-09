cmake_minimum_required(VERSION 3.24)
project(ParametricCAD VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PARAMCAD_BUILD_TESTS "Build ParametricCAD tests" ON)
option(PARAMCAD_BUILD_KERNEL_OCCT "Build the OpenCASCADE geometry kernel" ON)

add_library(ParametricCADCore
    src/Core/Document/CadDocument.cpp
    src/Core/Document/ObjectRegistry.cpp
    src/Core/Document/PartDocument.cpp
    src/Core/Recompute/DocumentRecomputeEngine.cpp
    src/Core/Parameter/Parameter.cpp
    src/Core/Parameter/ParameterManager.cpp
    src/Core/Material/Material.cpp
    src/Core/Physics/MassProperties.cpp
    src/Core/Reference/ReferenceFrame.cpp
    src/Core/Body/Body.cpp
    src/Core/Dependency/DependencyGraph.cpp
    src/Core/Feature/BoxFeature.cpp
    src/Core/Feature/Feature.cpp
    src/Core/Feature/PlaceholderFeature.cpp
    src/Core/Kernel/KernelShape.cpp
    src/Core/Physics/MassPropertiesNode.cpp
    src/Core/Serialization/JsonValue.cpp
    src/Core/Serialization/PartDocumentSerializer.cpp
    src/Core/Sketch/Sketch.cpp
    src/Core/Connector/Connector.cpp
)

target_include_directories(ParametricCADCore PUBLIC src)

target_compile_features(ParametricCADCore PUBLIC cxx_std_20)

add_executable(ParametricCADApp src/App/main.cpp)
target_link_libraries(ParametricCADApp PRIVATE ParametricCADCore)

# Source and execution character sets are UTF-8 (serialized documents are
# UTF-8; without this, MSVC interprets literals via the system codepage).
if(MSVC)
    target_compile_options(ParametricCADCore PRIVATE /utf-8)
    target_compile_options(ParametricCADApp PRIVATE /utf-8)
endif()

# --- OCCT-backed geometry kernel (M3, ADR-M3-001/003) -----------------------
# OCCT discovery is configurable (spec 16): never a hard-coded developer path.
# Configure with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
# so find_package(OpenCASCADE CONFIG) resolves portably; the vcpkg x64-windows
# triplet is dynamic, and VCPKG_APPLOCAL_DEPS (default ON with the vcpkg
# toolchain) copies the required OCCT DLLs next to each executable post-build.
# src/Core (ParametricCADCore) gains NO new include path or link dependency
# here -- this block only ever touches the new ParametricCADKernelOcct target.
# NO_CMAKE_(SYSTEM_)PACKAGE_REGISTRY: a stray export(PACKAGE OpenCASCADE) left
# behind by an unrelated OCCT build elsewhere on a developer machine can
# register itself in CMake's user/system package registry and get picked up
# here instead of the intended toolchain-provided install -- exactly the kind
# of accidental developer-machine-specific path spec 16 prohibits. Disabling
# the registry means discovery is driven ONLY by CMAKE_TOOLCHAIN_FILE /
# CMAKE_PREFIX_PATH / OpenCASCADE_DIR, all portable/explicit.
if(PARAMCAD_BUILD_KERNEL_OCCT)
    find_package(OpenCASCADE CONFIG QUIET
        NO_CMAKE_PACKAGE_REGISTRY
        NO_CMAKE_SYSTEM_PACKAGE_REGISTRY
    )
endif()

if(PARAMCAD_BUILD_KERNEL_OCCT AND OpenCASCADE_FOUND)
    add_library(ParametricCADKernelOcct
        src/Kernel/Occt/OcctGeometryKernel.cpp
    )
    target_include_directories(ParametricCADKernelOcct PRIVATE ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(ParametricCADKernelOcct
        PUBLIC ParametricCADCore
        PRIVATE ${OpenCASCADE_LIBRARIES}
    )
    target_compile_features(ParametricCADKernelOcct PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(ParametricCADKernelOcct PRIVATE /utf-8)
    endif()
    message(STATUS "ParametricCADKernelOcct: OCCT ${OpenCASCADE_VERSION} found -- "
                    "OCCT-backed geometry kernel will be built.")
else()
    message(STATUS "ParametricCADKernelOcct: OCCT not found or "
                    "PARAMCAD_BUILD_KERNEL_OCCT=OFF -- skipping the OCCT-dependent "
                    "kernel and its tests. Core and Core-only tests still build normally.")
endif()

if(TARGET ParametricCADKernelOcct)
    # Only src/App/main.cpp ever constructs a concrete kernel (ADR-M3-003);
    # BoxFeature/Core never name ParametricCADKernelOcct or OcctGeometryKernel.
    target_link_libraries(ParametricCADApp PRIVATE ParametricCADKernelOcct)
    target_compile_definitions(ParametricCADApp PRIVATE PARAMCAD_HAVE_OCCT_KERNEL)
endif()

if(PARAMCAD_BUILD_TESTS)
    include(CTest)
    enable_testing()

    # Prefer an installed GoogleTest; otherwise fetch the pinned release.
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
            URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
        )
        # Match the parent project's runtime library on MSVC.
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    # NOTE ordering: tests/SerializationTests.cpp contains
    # GeneratorLimitTest.AdvancePastMaxUint64DoesNotWrapGenerator, which
    # permanently advances the shared ObjectId generator to ~2^63 for the rest
    # of the process. It must stay the LAST translation unit in this list (its
    # own registration order is the last within the file, and gtest schedules
    # suites in link/registration order in a single-process run) so no other
    # test observes a poisoned generator. Add new test files ABOVE this
    # comment, never below tests/SerializationTests.cpp.
    add_executable(ParametricCADCoreTests
        tests/CoreSmokeTests.cpp
        tests/DependencyGraphTests.cpp
        tests/ObjectRegistryTests.cpp
        tests/DocumentRecomputeTests.cpp
        tests/BoxFeatureTests.cpp
        tests/MassPropertiesNodeTests.cpp
        tests/SerializationV3Tests.cpp
        tests/M3ReviewFindingTests.cpp
        tests/SerializationTests.cpp
    )
    target_link_libraries(ParametricCADCoreTests PRIVATE ParametricCADCore GTest::gtest_main)
    if(MSVC)
        target_compile_options(ParametricCADCoreTests PRIVATE /utf-8)
    endif()

    include(GoogleTest)
    gtest_discover_tests(ParametricCADCoreTests)

    # OCCT-linked test suite (M3-KERNEL-*, Mass, Inertia, real-geometry
    # recompute integration, and the mandatory release gate). Built and
    # registered ONLY when the OCCT-backed kernel target exists -- this is
    # also a linker-verifiable architecture check: ParametricCADCoreTests
    # above links ParametricCADCore only, never ParametricCADKernelOcct, so it
    # can pull in zero OCCT symbols regardless of this block.
    if(TARGET ParametricCADKernelOcct)
        add_executable(ParametricCADKernelOcctTests
            tests/Kernel/OcctGeometryKernelTests.cpp
            tests/Kernel/OcctRecomputeIntegrationTests.cpp
        )
        target_link_libraries(ParametricCADKernelOcctTests
            PRIVATE ParametricCADCore ParametricCADKernelOcct GTest::gtest_main
        )
        if(MSVC)
            target_compile_options(ParametricCADKernelOcctTests PRIVATE /utf-8)
        endif()
        # DISCOVERY_MODE PRE_TEST is required here, unlike for
        # ParametricCADCoreTests above. gtest_discover_tests' default
        # POST_BUILD mode RUNS the freshly linked executable to enumerate its
        # tests, but this target needs the OCCT DLLs that vcpkg's
        # VCPKG_APPLOCAL_DEPS copies next to the binary in its own POST_BUILD
        # step -- on a clean build the enumeration runs before those DLLs are
        # in place and the whole build fails (MSB3073). PRE_TEST defers
        # enumeration to ctest time, when the DLLs are guaranteed present.
        # ParametricCADCoreTests links no DLL-carrying dependency, so it keeps
        # the default mode.
        gtest_discover_tests(ParametricCADKernelOcctTests DISCOVERY_MODE PRE_TEST)
    endif()
endif()
