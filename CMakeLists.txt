cmake_minimum_required(VERSION 3.24)
project(ParametricCAD VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PARAMCAD_BUILD_TESTS "Build ParametricCAD tests" ON)

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
    src/Core/Feature/Feature.cpp
    src/Core/Feature/PlaceholderFeature.cpp
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

    add_executable(ParametricCADCoreTests
        tests/CoreSmokeTests.cpp
        tests/DependencyGraphTests.cpp
        tests/ObjectRegistryTests.cpp
        tests/DocumentRecomputeTests.cpp
        tests/SerializationTests.cpp
    )
    target_link_libraries(ParametricCADCoreTests PRIVATE ParametricCADCore GTest::gtest_main)
    if(MSVC)
        target_compile_options(ParametricCADCoreTests PRIVATE /utf-8)
    endif()

    include(GoogleTest)
    gtest_discover_tests(ParametricCADCoreTests)
endif()
