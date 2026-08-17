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
    src/Core/Feature/PadFeature.cpp
    src/Core/Feature/PocketFeature.cpp
    src/Core/Feature/RevolveFeature.cpp
    src/Core/Feature/EdgeDressFeatures.cpp
    src/Core/Feature/PlaceholderFeature.cpp
    src/Core/Kernel/KernelShape.cpp
    src/Core/Kernel/ProfileDefinition.cpp
    src/Core/Physics/MassPropertiesNode.cpp
    src/Core/Serialization/JsonValue.cpp
    src/Core/Serialization/PartDocumentSerializer.cpp
    src/Core/Import/ImportedGeometry.cpp
    src/Core/Import/SketchImporter.cpp
    src/Core/Reconstruction/ReconstructionPlan.cpp
    src/Core/Reconstruction/SketchReconstructor.cpp
    src/Core/Sketch/Profile.cpp
    src/Core/Sketch/SketchConstraint.cpp
    src/Core/Sketch/ISketchSolver.cpp
    src/Core/Sketch/SketchSolveSession.cpp
    src/Core/Sketch/Sketch.cpp
    src/Core/Sketch/SketchFrame.cpp
    src/Core/Sketch/SketchTypes.cpp
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

# --- Sketch constraint solver (M5, ADR-M5-003) ------------------------------
# Eigen is used for dense linear algebra and, critically, for the
# rank-revealing decomposition that makes DOF a measured quantity rather than
# "variables minus constraint count". It is header-only and MPL2 -- strictly
# less encumbering than the OCCT and Qt dependencies already present -- and it
# is PRIVATE to this target: ParametricCADCore gains no Eigen include path, and
# ISketchSolver, SketchSolveProblem and SketchSolveResult carry no Eigen type,
# so the replacement boundary stays exactly that interface.
find_package(Eigen3 CONFIG QUIET
    NO_CMAKE_PACKAGE_REGISTRY
    NO_CMAKE_SYSTEM_PACKAGE_REGISTRY
)

if(Eigen3_FOUND)
    add_library(ParametricCADSolver src/Solver/GaussNewtonSketchSolver.cpp)
    target_include_directories(ParametricCADSolver PUBLIC src)
    target_link_libraries(ParametricCADSolver
        PUBLIC ParametricCADCore
        PRIVATE Eigen3::Eigen
    )
    target_compile_features(ParametricCADSolver PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(ParametricCADSolver PRIVATE /utf-8)
    endif()
    message(STATUS "ParametricCADSolver: Eigen ${Eigen3_VERSION} found -- "
                    "sketch constraint solver will be built.")
else()
    message(WARNING "ParametricCADSolver: Eigen3 not found -- the sketch solver "
                    "and its tests will be skipped. Core still builds.")
endif()

# --- DXF import (M6, ADR-M6-001/003) ----------------------------------------
# LICENCE, stated where the dependency is introduced rather than only in an ADR:
# libdxfrw is GPL-2.0-only, adopted by EXPLICIT OWNER DECISION as spec 20
# requires. GPL-2.0 is copyleft: linking it generally makes the combined work a
# derivative that must be distributed under GPL-2.0-compatible terms, including
# corresponding source. If EP3D is to ship closed-source, this dependency must
# be replaced first -- nothing in the build will warn about that.
#
# It is confined to ParametricCADImportDxf and is PRIVATE to it, so
# ParametricCADCore gains no libdxfrw include path and no link dependency, and
# the Core test binary cannot pull a symbol from it. That containment is what
# makes replacing the parser a matter of rewriting one translation unit.
find_package(libdxfrw CONFIG QUIET
    NO_CMAKE_PACKAGE_REGISTRY
    NO_CMAKE_SYSTEM_PACKAGE_REGISTRY
)

if(libdxfrw_FOUND)
    add_library(ParametricCADImportDxf src/Import/Dxf/DxfReader.cpp)
    target_include_directories(ParametricCADImportDxf PUBLIC src)
    target_link_libraries(ParametricCADImportDxf
        PUBLIC ParametricCADCore
        PRIVATE libdxfrw::libdxfrw
    )
    target_compile_features(ParametricCADImportDxf PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(ParametricCADImportDxf PRIVATE /utf-8)
    endif()
    message(STATUS "ParametricCADImportDxf: libdxfrw found (GPL-2.0-only) -- "
                    "DXF import will be built.")
else()
    message(STATUS "ParametricCADImportDxf: libdxfrw not found -- skipping DXF "
                    "import and its tests. Core and every other target still build.")
endif()

# --- Minimal Qt viewer (M4, ADR-M4-006) -------------------------------------
# Qt is a hard dependency of the VIEWER TARGET ONLY. Without Qt this block is
# skipped and Core, Kernel and every non-viewer test still configure and build
# normally -- the same conditional pattern PARAMCAD_BUILD_KERNEL_OCCT uses for
# OCCT. ParametricCADCore gains no Qt and no OCCT dependency here; the
# binary-level check (dumpbin on ParametricCADCoreTests) still holds.
option(PARAMCAD_BUILD_VIEWER "Build the Qt viewer" ON)

# The document-facing half of the viewer. Free of Qt AND of OCCT, so it is unit
# testable without a display -- but it belongs to the VIEWER layer, not to Core.
# Compiling it into ParametricCADCore (as an earlier revision did) inverted the
# layering and would have let a future Qt include reach Core without the
# binary-level boundary check noticing.
add_library(ParametricCADViewerCore
    src/Viewer/DocumentPresenter.cpp
    src/Viewer/DocumentOutline.cpp
)
target_include_directories(ParametricCADViewerCore PUBLIC src)
target_link_libraries(ParametricCADViewerCore PUBLIC ParametricCADCore)
target_compile_features(ParametricCADViewerCore PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(ParametricCADViewerCore PRIVATE /utf-8)
endif()

if(PARAMCAD_BUILD_VIEWER)
    find_package(Qt6 CONFIG QUIET COMPONENTS Core Gui Widgets
        NO_CMAKE_PACKAGE_REGISTRY
        NO_CMAKE_SYSTEM_PACKAGE_REGISTRY
    )
endif()

if(PARAMCAD_BUILD_VIEWER AND Qt6_FOUND AND TARGET ParametricCADKernelOcct)
    set(CMAKE_AUTOMOC ON)
    add_executable(ParametricCADViewer WIN32
        src/Viewer/main.cpp
        src/Viewer/MainWindow.cpp
        src/Viewer/OcctViewWidget.cpp
    )
    target_include_directories(ParametricCADViewer PRIVATE src ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(ParametricCADViewer
        PRIVATE ParametricCADCore ParametricCADViewerCore ParametricCADKernelOcct
                Qt6::Core Qt6::Gui Qt6::Widgets
                ${OpenCASCADE_LIBRARIES}
    )
    # The viewer is an APPLICATION, so it is allowed to name a concrete solver
    # backend and inject it -- the same licence main.cpp has for the OCCT
    # kernel (ADR-M3-003 / ADR-M5-003). Core still links neither.
    if(TARGET ParametricCADSolver)
        target_link_libraries(ParametricCADViewer PRIVATE ParametricCADSolver)
    endif()
    # DXF import in the shell (M6.7). The viewer is an application, so it may
    # name a concrete backend -- and it inherits libdxfrw's GPL-2.0 copyleft by
    # doing so (ADR-M6-001). Core still links neither.
    if(TARGET ParametricCADImportDxf)
        target_link_libraries(ParametricCADViewer PRIVATE ParametricCADImportDxf)
        target_compile_definitions(ParametricCADViewer PRIVATE PARAMCAD_HAVE_DXF_IMPORT)
    endif()
    target_compile_features(ParametricCADViewer PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(ParametricCADViewer PRIVATE /utf-8)
    endif()
    # Qt platform plugin deployment. Without this the viewer BUILDS AND LINKS
    # but aborts on launch with "no Qt platform plugin could be initialized" --
    # vcpkg's applocal step copies the Qt DLLs next to the binary, but the
    # platform plugin must additionally live in a platforms/ SUBDIRECTORY that
    # nothing creates automatically. Found by actually running the executable;
    # a build-and-link check cannot see it.
    if(TARGET Qt6::QWindowsIntegrationPlugin)
        add_custom_command(TARGET ParametricCADViewer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:ParametricCADViewer>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
                    "$<TARGET_FILE_DIR:ParametricCADViewer>/platforms/"
            COMMENT "Deploying the Qt platform plugin next to the viewer"
        )
    else()
        message(WARNING "Qt6::QWindowsIntegrationPlugin target not found -- the viewer "
                        "will build but may fail to start.")
    endif()

    # Launching the binary is its own check (see --selftest in main.cpp): the
    # defects it catches are invisible to unit tests and to a successful build.
    if(PARAMCAD_BUILD_TESTS)
        add_test(NAME ViewerSmokeTest COMMAND ParametricCADViewer --selftest)
        set_tests_properties(ViewerSmokeTest PROPERTIES TIMEOUT 60)
        # One smoke test PER M5 sample. A single default run would leave the
        # solved, under-constrained and conflicting paths unexercised in the
        # running program -- and those are exactly the states spec 18's UI
        # requirements are about.
        foreach(sample m5-rectangle m5-underconstrained m5-conflict m5-circle m8-chain)
            add_test(NAME ViewerSmokeTest_${sample}
                     COMMAND ParametricCADViewer --selftest --sample ${sample})
            set_tests_properties(ViewerSmokeTest_${sample} PROPERTIES TIMEOUT 60)
        endforeach()
        # A bad --sample must FAIL. Both shapes of bad: an unknown name, and a
        # MISSING value (`--sample` last, or a CI variable that expands to
        # nothing). Each fell through to the M4 sample and still printed
        # SELFTEST OK, silently downgrading an M5 gate -- and each was fixed one
        # round apart, so both are pinned here.
        add_test(NAME ViewerSmokeTest_RejectsUnknownSample
                 COMMAND ParametricCADViewer --selftest --sample no-such-sample)
        add_test(NAME ViewerSmokeTest_RejectsMissingSampleValue
                 COMMAND ParametricCADViewer --selftest --sample)
        # The --key=value form, which is the most commonly typed of the three
        # and was silently dropped entirely (the flag failed strcmp, so the M4
        # sample was built and passed). Both spellings are pinned because the
        # separate-token cases above cannot see this one.
        add_test(NAME ViewerSmokeTest_RejectsUnknownSampleEqualsForm
                 COMMAND ParametricCADViewer --selftest --sample=no-such-sample)
        add_test(NAME ViewerSmokeTest_RejectsEmptySampleEqualsForm
                 COMMAND ParametricCADViewer --selftest --sample=)
        set_tests_properties(ViewerSmokeTest_RejectsUnknownSample
                             ViewerSmokeTest_RejectsMissingSampleValue
                             ViewerSmokeTest_RejectsUnknownSampleEqualsForm
                             ViewerSmokeTest_RejectsEmptySampleEqualsForm
                             PROPERTIES WILL_FAIL TRUE TIMEOUT 60)
        # And the =form with a VALID name must still WORK, so the parser is not
        # simply rejecting everything with an '=' in it.
        add_test(NAME ViewerSmokeTest_AcceptsEqualsForm
                 COMMAND ParametricCADViewer --selftest --sample=m5-circle)
        set_tests_properties(ViewerSmokeTest_AcceptsEqualsForm PROPERTIES TIMEOUT 60)

        # M6.7: the DXF import workflow driven through MainWindow in the running
        # application -- the half of it no unit test reaches (the panel commit,
        # the recompute, the redisplay, the model-tree row and the status
        # message). Only registered when the import target exists.
        if(TARGET ParametricCADImportDxf)
            # mixed.dxf carries NO dimensions, so this exercises the
            # inference-only path: 0 from source, 0 skipped.
            add_test(NAME ViewerSmokeTest_ImportsDxf
                     COMMAND ParametricCADViewer --selftest
                             --import "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dxf/mixed.dxf"
                             --expect-from-source 0 --expect-skipped 0)
            set_tests_properties(ViewerSmokeTest_ImportsDxf PROPERTIES TIMEOUT 60)
            # ...and a DIMENSIONED file, which until independent review no
            # registered test imported at all. Without this, "From source" was
            # always "0" in CI and the skip-diagnostic row never rendered once,
            # so spec 27 steps 5-10 had no automated coverage at any layer.
            add_test(NAME ViewerSmokeTest_ImportsDimensionedDxf
                     COMMAND ParametricCADViewer --selftest
                             --import "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dxf/dimensioned_rectangle.dxf"
                             --expect-from-source 2 --expect-skipped 0)
            set_tests_properties(ViewerSmokeTest_ImportsDimensionedDxf PROPERTIES TIMEOUT 60)
            # A fixture that SKIPS. Until round 2 no registered test produced
            # one: mixed.dxf carries no dimensions and dimensioned_rectangle
            # reconstructs both, so the skip-diagnostic row never rendered once
            # in CI and deleting the loop that draws it left all 15 viewer
            # smoke tests green. This file's width dimension states 250 against
            # 99.5 mm of geometry, so it is refused and the height is kept.
            add_test(NAME ViewerSmokeTest_ImportsWithASkippedDimension
                     COMMAND ParametricCADViewer --selftest
                             --import "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dxf/dimensioned_rectangle_one_skipped.dxf"
                             --expect-from-source 1 --expect-skipped 1)
            set_tests_properties(ViewerSmokeTest_ImportsWithASkippedDimension
                                 PROPERTIES TIMEOUT 60)
            # NEGATIVE CONTROLS for the two expectation flags. Independent
            # review round 2 found both flags DECLARED and never parsed, so
            # `--expect-from-source 999` returned SELFTEST OK and the entries
            # above proved nothing but their own self-consistency. An
            # expectation that cannot fail is not an expectation; these two
            # entries are what make the ones above mean something.
            add_test(NAME ViewerSmokeTest_WrongFromSourceExpectationFails
                     COMMAND ParametricCADViewer --selftest
                             --import "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dxf/dimensioned_rectangle.dxf"
                             --expect-from-source 999 --expect-skipped 0)
            add_test(NAME ViewerSmokeTest_WrongSkippedExpectationFails
                     COMMAND ParametricCADViewer --selftest
                             --import "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dxf/dimensioned_rectangle.dxf"
                             --expect-from-source 2 --expect-skipped 777)
            # And the CLASS the two dead flags belonged to: an unrecognised
            # --flag must be refused, not stepped over.
            add_test(NAME ViewerSmokeTest_RejectsAnUnknownFlag
                     COMMAND ParametricCADViewer --selftest --sample m5-rectangle
                             --no-such-flag)
            set_tests_properties(ViewerSmokeTest_WrongFromSourceExpectationFails
                                 ViewerSmokeTest_WrongSkippedExpectationFails
                                 ViewerSmokeTest_RejectsAnUnknownFlag
                                 PROPERTIES WILL_FAIL TRUE TIMEOUT 60)
            # A failed import must FAIL the selftest, so a broken import cannot
            # pass CI by producing an empty document quietly.
            add_test(NAME ViewerSmokeTest_ReportsFailedImport
                     COMMAND ParametricCADViewer --selftest
                             --import "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dxf/nope.dxf")
            add_test(NAME ViewerSmokeTest_RejectsMissingImportPath
                     COMMAND ParametricCADViewer --selftest --import)
            set_tests_properties(ViewerSmokeTest_ReportsFailedImport
                                 ViewerSmokeTest_RejectsMissingImportPath
                                 PROPERTIES WILL_FAIL TRUE TIMEOUT 60)
        endif()
    endif()

    message(STATUS "ParametricCADViewer: Qt ${Qt6_VERSION} found -- viewer will be built.")
else()
    message(STATUS "ParametricCADViewer: Qt6 or the OCCT kernel is unavailable, or "
                    "PARAMCAD_BUILD_VIEWER=OFF -- skipping the viewer. Core, Kernel and "
                    "all non-viewer tests still build normally.")
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
        tests/SketchTests.cpp
        tests/ProfileTests.cpp
        tests/PadFeatureTests.cpp
        tests/SerializationV4Tests.cpp
        tests/ViewerPresenterTests.cpp
        tests/M4ReviewFindingTests.cpp
        tests/DocumentOutlineTests.cpp
        tests/SerializationV5Tests.cpp
        tests/ReconstructionTests.cpp
        tests/ReconstructionGeneralTests.cpp
        tests/ReconstructionCurveTests.cpp
        tests/ReconstructionProvenanceTests.cpp
        tests/ReconstructionAdversarialTests.cpp
        tests/ReconstructionReviewFindingTests.cpp
        tests/SerializationV6Tests.cpp
        tests/SerializationV7Tests.cpp
        tests/SerializationV8Tests.cpp
        # LAST, and the comment above says why. M7 added its five files BELOW
        # this line and broke the rule the comment states in capitals: three
        # M7 persistence tests then ran against a generator already advanced to
        # ~2^63 and could not save. `ctest` hid it completely, because
        # gtest_discover_tests gives every test its own process -- 668/668 there
        # and 665/668 in a single-process run of this binary. Independent review
        # found it; no ctest-based check could have.
        tests/SerializationTests.cpp
    )
    target_link_libraries(ParametricCADCoreTests
        PRIVATE ParametricCADCore ParametricCADViewerCore GTest::gtest_main)
    # The id-ceiling tests re-invoke THIS binary as a child process, so the
    # process-global generator mutation they need cannot poison anything else.
    # Generator expressions, because the path differs per configuration --
    # hard-coding one would make the Release run drive the Debug binary.
    target_compile_definitions(ParametricCADCoreTests PRIVATE
        PARAMCAD_CORE_TEST_EXE="$<TARGET_FILE:ParametricCADCoreTests>"
        PARAMCAD_CORE_SCRATCH_DIR="$<TARGET_FILE_DIR:ParametricCADCoreTests>")
    if(MSVC)
        target_compile_options(ParametricCADCoreTests PRIVATE /utf-8)
    endif()

    include(GoogleTest)
    # PRE_TEST for EVERY suite, not only the DLL-carrying ones.
    #
    # gtest_discover_tests' default POST_BUILD mode writes a single
    # CONFIG-LESS <target>_tests.cmake whose add_test lines hard-code an
    # absolute path to whichever configuration was linked last. Under a
    # multi-config generator that means `ctest -C Release` silently runs the
    # DEBUG binaries: independent review found 333 of 448 tests doing exactly
    # that, so this project's "Release tests pass" claim was, for those suites,
    # a second Debug run. A Release-only defect could not have been seen --
    # and ADR-M5-005 puts DOF on a numerical rank threshold, which is precisely
    # the kind of thing that differs between configurations.
    #
    # PRE_TEST defers enumeration to ctest time and emits per-config
    # _include-<CONFIG>.cmake files, so -C selects the binary it names.

    # WHOLE-SUITE runs, one process per binary, in ADDITION to the per-test
    # registrations below.
    #
    # gtest_discover_tests gives every test its own process, which is what makes
    # `ctest -C Release` honest about which binaries it invokes -- but it also
    # makes any state one test leaks into another INVISIBLE. M7 registered its
    # files after the test that advances the shared ObjectId generator past its
    # cap; three M7 persistence tests then ran against a poisoned generator and
    # could not save. ctest said 668/668. A direct run of the binary said
    # 665/668, in both configurations, and independent review found it.
    #
    # One extra test per binary closes that hole permanently, and costs one
    # process each.
    function(paramcad_add_whole_suite_test target)
        add_test(NAME WholeSuite_${target} COMMAND ${target})
        set_tests_properties(WholeSuite_${target} PROPERTIES TIMEOUT 600)
        # ORDER-INDEPENDENCE, as a property rather than a convention.
        # WholeSuite_* runs one order -- the one the linker produced -- and
        # round 2 measured what that was worth: shuffled, the Core suite failed
        # 71, 11 and 16 tests at seeds 1, 7 and 42, every one of them poisoned
        # by a single test that advanced the process-global id counter. That
        # test now runs in a child process, and these entries are what stop the
        # hazard coming back through some other global. Fixed seeds, so a
        # failure is reproducible rather than a coin toss in CI.
        foreach(seed 1 7 42)
            add_test(NAME Shuffled_${target}_seed${seed}
                     COMMAND ${target} --gtest_shuffle --gtest_random_seed=${seed})
            set_tests_properties(Shuffled_${target}_seed${seed} PROPERTIES TIMEOUT 600)
        endforeach()
    endfunction()

    gtest_discover_tests(ParametricCADCoreTests DISCOVERY_MODE PRE_TEST)
    paramcad_add_whole_suite_test(ParametricCADCoreTests)

    # Solver test suite (M5). Built only when the solver target exists, and it
    # links ParametricCADSolver rather than Eigen -- if a test needed Eigen
    # directly, the solver boundary would not be where ADR-M5-003 says it is.
    if(TARGET ParametricCADSolver)
        add_executable(ParametricCADSolverTests
            tests/Solver/SketchSolverTests.cpp
            tests/Solver/SketchConstraintTests.cpp
        )
        target_link_libraries(ParametricCADSolverTests
            PRIVATE ParametricCADCore ParametricCADSolver GTest::gtest_main
        )
        if(MSVC)
            target_compile_options(ParametricCADSolverTests PRIVATE /utf-8)
        endif()
        gtest_discover_tests(ParametricCADSolverTests DISCOVERY_MODE PRE_TEST)
        paramcad_add_whole_suite_test(ParametricCADSolverTests)
    endif()

    # OCCT-linked test suite (M3-KERNEL-*, Mass, Inertia, real-geometry
    # recompute integration, and the mandatory release gate). Built and
    # registered ONLY when the OCCT-backed kernel target exists -- this is
    # also a linker-verifiable architecture check: ParametricCADCoreTests
    # above links ParametricCADCore only, never ParametricCADKernelOcct, so it
    # can pull in zero OCCT symbols regardless of this block.
    if(TARGET ParametricCADKernelOcct)
        add_executable(ParametricCADKernelOcctTests
            tests/Kernel/OcctGeometryKernelTests.cpp
            tests/Kernel/OcctExtrudeTests.cpp
            tests/Kernel/OcctBooleanTests.cpp
            tests/Kernel/OcctRevolveTests.cpp
            tests/Kernel/OcctFilletChamferTests.cpp
            tests/Kernel/M4ReleaseGateTests.cpp
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
        # Since the review, every suite uses PRE_TEST -- see the note at
        # ParametricCADCoreTests. The DLL argument below is why this suite
        # needed it FIRST, not why the others may go without it.
        gtest_discover_tests(ParametricCADKernelOcctTests DISCOVERY_MODE PRE_TEST)
        paramcad_add_whole_suite_test(ParametricCADKernelOcctTests)
    endif()

    # DXF import suite (M6). Built only when the import target exists, and it
    # links ParametricCADImportDxf rather than libdxfrw -- if a test needed the
    # library directly, the import boundary would not be where ADR-M6-003 says.
    if(TARGET ParametricCADImportDxf)
        add_executable(ParametricCADImportTests
            tests/Import/DxfImportTests.cpp
            tests/Import/DxfDimensionTests.cpp)
        target_link_libraries(ParametricCADImportTests
            PRIVATE ParametricCADCore ParametricCADImportDxf GTest::gtest_main
        )
        # Fixture path baked in, so the tests do not depend on ctest's working
        # directory.
        target_compile_definitions(ParametricCADImportTests
            PRIVATE PARAMCAD_DXF_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dxf")
        # M7.7's fresh-process gate re-invokes THIS binary as a child process,
        # so it needs its own path and somewhere to write. A generator
        # expression, because the path differs per configuration -- hard-coding
        # one would make the Release run drive the Debug binary, which is the
        # same class of false evidence gtest_discover_tests' PRE_TEST mode
        # exists to prevent.
        target_compile_definitions(ParametricCADImportTests PRIVATE
            PARAMCAD_IMPORT_TEST_EXE="$<TARGET_FILE:ParametricCADImportTests>"
            PARAMCAD_M7_SCRATCH_DIR="$<TARGET_FILE_DIR:ParametricCADImportTests>")
        if(TARGET ParametricCADKernelOcct)
            target_link_libraries(ParametricCADImportTests PRIVATE ParametricCADKernelOcct)
        endif()
        # The solver too: the strongest test of "imported entities are ordinary"
        # is constraining one and letting M5's solver move it.
        if(TARGET ParametricCADSolver)
            target_link_libraries(ParametricCADImportTests PRIVATE ParametricCADSolver)
        endif()
        if(MSVC)
            target_compile_options(ParametricCADImportTests PRIVATE /utf-8)
        endif()
        gtest_discover_tests(ParametricCADImportTests DISCOVERY_MODE PRE_TEST)
        paramcad_add_whole_suite_test(ParametricCADImportTests)
    endif()

    # Full-stack integration (M5-J/K). The ONLY target linking Core + Solver +
    # KernelOcct together, because spec 13's selective-recompute requirements
    # are claims about a real solver and a real kernel in one recompute pass:
    # "PadLength changed and the sketch did not re-solve" cannot be observed
    # from either single-layer suite. Same PRE_TEST reasoning as above -- it
    # carries the OCCT DLL dependency.
    if(TARGET ParametricCADSolver AND TARGET ParametricCADKernelOcct)
        add_executable(ParametricCADIntegrationTests
            tests/Integration/M5RecomputeIntegrationTests.cpp
            tests/Integration/M5UiTests.cpp
            tests/Integration/M5ReleaseGateTests.cpp
            tests/Integration/M5AdversarialTests.cpp
            tests/Integration/M7ReleaseGateTests.cpp
            tests/Integration/M8ReleaseGateTests.cpp
        )
        # ViewerCore joins so the UI's decisions can be checked against a REAL
        # solve. Faking solver output would make those tests agree with a stub
        # rather than with the solver the user runs.
        target_link_libraries(ParametricCADIntegrationTests
            PRIVATE ParametricCADCore ParametricCADSolver ParametricCADKernelOcct
                    ParametricCADViewerCore GTest::gtest_main
        )
        if(MSVC)
            target_compile_options(ParametricCADIntegrationTests PRIVATE /utf-8)
        endif()
        gtest_discover_tests(ParametricCADIntegrationTests DISCOVERY_MODE PRE_TEST)
        paramcad_add_whole_suite_test(ParametricCADIntegrationTests)
    endif()
endif()
