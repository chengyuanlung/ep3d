cmake_minimum_required(VERSION 3.24)
project(ParametricCAD VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PARAMCAD_BUILD_TESTS "Build ParametricCAD tests" ON)

add_library(ParametricCADCore
    src/Core/Document/CadDocument.cpp
    src/Core/Document/PartDocument.cpp
    src/Core/Parameter/Parameter.cpp
    src/Core/Parameter/ParameterManager.cpp
    src/Core/Material/Material.cpp
    src/Core/Physics/MassProperties.cpp
    src/Core/Reference/ReferenceFrame.cpp
    src/Core/Body/Body.cpp
    src/Core/Dependency/DependencyGraph.cpp
    src/Core/Feature/Feature.cpp
    src/Core/Sketch/Sketch.cpp
    src/Core/Connector/Connector.cpp
)

target_include_directories(ParametricCADCore PUBLIC src)

target_compile_features(ParametricCADCore PUBLIC cxx_std_20)

add_executable(ParametricCADApp src/App/main.cpp)
target_link_libraries(ParametricCADApp PRIVATE ParametricCADCore)

if(PARAMCAD_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_executable(ParametricCADCoreTests tests/CoreSmokeTests.cpp)
    target_link_libraries(ParametricCADCoreTests PRIVATE ParametricCADCore)
    add_test(NAME CoreSmokeTests COMMAND ParametricCADCoreTests)

    add_executable(ParametricCADDependencyGraphTests tests/DependencyGraphTests.cpp)
    target_link_libraries(ParametricCADDependencyGraphTests PRIVATE ParametricCADCore)
    add_test(NAME DependencyGraphTests COMMAND ParametricCADDependencyGraphTests)
endif()
