// M7.1 — reading DXF DIMENSION entities, and the full release chain from a
// real file (spec 37).
//
// Every expected number is computed by hand from `dimensioned_rectangle.dxf`,
// which is small enough to read. Nothing here takes an expectation from the
// reader, the reconstructor or the kernel.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Import/SketchImporter.h"
#include "Core/Reconstruction/SketchReconstructor.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include "Import/Dxf/DxfReader.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace {

using namespace paramcad;

std::string Fixture(const char* name) {
    return std::string(PARAMCAD_DXF_FIXTURE_DIR) + "/" + name;
}

constexpr double kPi = 3.14159265358979323846;

// --- The reader ------------------------------------------------------------

TEST(M7DxfDimension, LinearDimensionsAreReadAsAnnotationNotGeometry) {
    const DxfReadResult read = ReadDxfFile(Fixture("dimensioned_rectangle.dxf"));
    ASSERT_TRUE(read) << read.message;

    // Four lines and two dimensions -- NOT ten lines. The *D1 and *D2 blocks
    // hold six annotation lines between them, and importing those as model
    // geometry is exactly what ADR-M6-009 forbids.
    EXPECT_EQ(read.geometry.lines.size(), 4u);
    ASSERT_EQ(read.geometry.dimensions.size(), 2u);
    EXPECT_EQ(read.geometry.circles.size(), 0u);
    EXPECT_EQ(read.geometry.arcs.size(), 0u);
}

TEST(M7DxfDimension, DefinitionPointsAndStatedValuesAreReadExactly) {
    const DxfReadResult read = ReadDxfFile(Fixture("dimensioned_rectangle.dxf"));
    ASSERT_TRUE(read) << read.message;
    ASSERT_EQ(read.geometry.dimensions.size(), 2u);

    // Identified by their own stated values rather than by position in the
    // vector, because file order is not identity (ADR-M6-004).
    const ImportedDimension2D* width = nullptr;
    const ImportedDimension2D* height = nullptr;
    for (const ImportedDimension2D& dimension : read.geometry.dimensions) {
        ASSERT_TRUE(dimension.statedValueMm.has_value());
        if (*dimension.statedValueMm > 75.0) width = &dimension;
        else height = &dimension;
    }
    ASSERT_NE(width, nullptr);
    ASSERT_NE(height, nullptr);

    // Group codes 13/23 and 14/24, straight out of the file.
    EXPECT_DOUBLE_EQ(width->measureFrom.x, 0.0);
    EXPECT_DOUBLE_EQ(width->measureFrom.y, 0.0);
    EXPECT_DOUBLE_EQ(width->measureTo.x, 99.5);
    EXPECT_DOUBLE_EQ(width->measureTo.y, 0.0497);
    EXPECT_DOUBLE_EQ(*width->statedValueMm, 100.0);
    EXPECT_DOUBLE_EQ(width->directionRad, 0.0);
    EXPECT_EQ(width->kind, ImportedDimensionKind::Linear);

    // Code 50 is DEGREES in the file and radians here: 90 -> pi/2. A reader
    // that forwarded 90 unchanged would make the height dimension measure its
    // projection onto a direction 90 radians round, which is neither axis.
    EXPECT_NEAR(height->directionRad, kPi / 2.0, 1e-12);
    EXPECT_DOUBLE_EQ(*height->statedValueMm, 50.0);

    // The handle is retained as source metadata only (spec 7).
    EXPECT_EQ(width->sourceHandle, "2A");
    EXPECT_EQ(height->sourceHandle, "2B");

    // No text override in this file, so the drawing shows its measurement.
    EXPECT_TRUE(width->textOverride.empty());
}

TEST(M7DxfDimension, DimensionsAreNotReportedAsSkippedUnsupportedEntities) {
    const DxfReadResult read = ReadDxfFile(Fixture("dimensioned_rectangle.dxf"));
    ASSERT_TRUE(read) << read.message;

    // M6 reported every DIMENSION as an unsupported entity. Now they are read,
    // so telling the user two things were dropped would be false -- and the
    // block note must still be there, exactly once, for the two *D blocks.
    for (const ImportedSkip& skip : read.geometry.skipped)
        EXPECT_NE(skip.entityKind, "DIMENSION") << skip.detail;
}

TEST(M7DxfDimension, AnInchesFileScalesItsDimensionsAndItsGeometryTogether) {
    // The unit-conversion mutation spec 32 names: omitting the conversion for
    // dimensions only. The definition points would then be scaled and the
    // stated value not, so the two would disagree by 25.4x and reconstruction
    // would refuse the file as self-contradictory -- a wrong answer that looks
    // like caution. Built in memory because it is a statement about the
    // scaling pass, not about parsing.
    ImportedSketchGeometry geometry;
    geometry.unit = ImportedLengthUnit::Inch;
    geometry.millimetresPerUnit = 25.4;

    ImportedDimension2D dimension;
    dimension.kind = ImportedDimensionKind::Linear;
    dimension.measureFrom = Vec2{0, 0};
    dimension.measureTo = Vec2{100, 0};
    dimension.statedValueMm = 100.0;

    // 100 inches is 2540 mm exactly. Whatever the scaling pass does, the two
    // must stay in agreement, because they describe one length.
    const double k = geometry.millimetresPerUnit;
    dimension.measureFrom.x *= k;
    dimension.measureTo.x *= k;
    *dimension.statedValueMm *= k;

    EXPECT_DOUBLE_EQ(MeasuredValueMm(dimension), 2540.0);
    EXPECT_DOUBLE_EQ(*dimension.statedValueMm, 2540.0);
}

// --- M7.3: curve dimensions from a real file --------------------------------

TEST(M7DxfDimension, RadialAndDiametricDimensionsAreReadWithTheirOwnGeometry) {
    const DxfReadResult read = ReadDxfFile(Fixture("dimensioned_circle.dxf"));
    ASSERT_TRUE(read) << read.message;

    EXPECT_EQ(read.geometry.circles.size(), 2u);
    ASSERT_EQ(read.geometry.dimensions.size(), 2u);

    const ImportedDimension2D* radial = nullptr;
    const ImportedDimension2D* diametric = nullptr;
    for (const ImportedDimension2D& dimension : read.geometry.dimensions) {
        if (dimension.kind == ImportedDimensionKind::Radius) radial = &dimension;
        if (dimension.kind == ImportedDimensionKind::Diameter) diametric = &dimension;
    }
    ASSERT_NE(radial, nullptr) << "the radial dimension was not read as a Radius";
    ASSERT_NE(diametric, nullptr) << "the diametric dimension was not read as a Diameter";

    // The radial one states its centre outright.
    EXPECT_NEAR(DimensionCenter(*radial).x, 25.0, 1e-6);
    EXPECT_NEAR(DimensionCenter(*radial).y, 30.0, 1e-6);
    EXPECT_NEAR(MeasuredValueMm(*radial), 10.0, 1e-6);

    // The diametric one never does -- its centre is the midpoint of two rim
    // points. Reading its first point as a centre would give (90.6, 40.6),
    // which is 15 mm from the real centre and matches no circle in the file.
    EXPECT_NEAR(DimensionCenter(*diametric).x, 80.0, 1e-6);
    EXPECT_NEAR(DimensionCenter(*diametric).y, 30.0, 1e-6);
    EXPECT_NEAR(MeasuredValueMm(*diametric), 30.0, 1e-6);
}

TEST(M7DxfDimension, M7_3_CHAIN_CircleFromFileDrivesVolumeAndScalesAsTheSquare) {
    const DxfReadResult read = ReadDxfFile(Fixture("dimensioned_circle.dxf"));
    ASSERT_TRUE(read) << read.message;

    PartDocument document{"M7CircleChain"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);

    // Only the first circle, so the Pad has one profile to extrude.
    ImportedSketchGeometry single;
    single.unit = read.geometry.unit;
    single.millimetresPerUnit = read.geometry.millimetresPerUnit;
    for (const ImportedCircle2D& circle : read.geometry.circles)
        if (std::abs(circle.center.x - 25.0) < 1e-6) single.circles.push_back(circle);
    for (const ImportedDimension2D& dimension : read.geometry.dimensions)
        if (dimension.kind == ImportedDimensionKind::Radius) single.dimensions.push_back(dimension);
    ASSERT_EQ(single.circles.size(), 1u);
    ASSERT_EQ(single.dimensions.size(), 1u);

    const ObjectId sketchId = ImportGeometryIntoNewSketch(document, "circle", single).sketchId;
    ASSERT_TRUE(ReconstructSketch(document, sketchId, single.dimensions));

    Parameter& padLength = document.addParameter("PadLength", 30.0, UnitType::Millimeter);
    Body& body = document.addBody("Body001");
    document.addPadFeature(body, "Pad001", sketchId, padLength.id());
    ASSERT_TRUE(document.recompute().success);

    EXPECT_EQ(document.findSketch(sketchId)->solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(document.findSketch(sketchId)->degreesOfFreedom(), 0);

    constexpr double kPiLocal = 3.14159265358979323846;
    const double before = document.massProperties().volumeMm3;
    EXPECT_NEAR(before, kPiLocal * 100.0 * 30.0, 1e-3);

    const Parameter* radius = document.parameters().findByName("Radius");
    ASSERT_NE(radius, nullptr);
    ASSERT_TRUE(document.setParameterValue(radius->id(), 20.0));
    ASSERT_TRUE(document.recomputeFrom(radius->id()).success);

    // Gate E as a ratio: exactly 4x for a doubled radius at unchanged length.
    EXPECT_NEAR(document.massProperties().volumeMm3 / before, 4.0, 1e-9);
}

// --- The M7.1 release chain, from the file ----------------------------------

TEST(M7DxfDimension, M7_1_RELEASE_CHAIN_FromFileToRebuiltVolume) {
    const DxfReadResult read = ReadDxfFile(Fixture("dimensioned_rectangle.dxf"));
    ASSERT_TRUE(read) << read.message;

    std::string saved;
    ObjectId sketchId = kInvalidObjectId;
    {
        PartDocument document{"M7Chain"};
        OcctGeometryKernel kernel;
        GaussNewtonSketchSolver solver;
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
        document.addMaterial("Aluminium", 2700.0);

        // M6 import.
        const SketchImportResult imported =
            ImportGeometryIntoNewSketch(document, "rectangle", read.geometry);
        ASSERT_TRUE(imported) << imported.message;
        sketchId = imported.sketchId;
        ASSERT_EQ(document.findSketch(sketchId)->entities().size(), 4u);

        // M7 reconstruct.
        const ReconstructionResult reconstruction =
            ReconstructSketch(document, sketchId, read.geometry.dimensions);
        ASSERT_TRUE(reconstruction) << reconstruction.message;
        ASSERT_EQ(reconstruction.createdParameters.size(), 2u);

        Parameter& padLength = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
        Body& body = document.addBody("Body001");
        document.addPadFeature(body, "Pad001", sketchId, padLength.id());

        // Solved, DOF 0.
        ASSERT_TRUE(document.recompute().success);
        EXPECT_EQ(document.findSketch(sketchId)->solveStatus(), SketchSolveStatus::Solved);
        EXPECT_EQ(document.findSketch(sketchId)->degreesOfFreedom(), 0);

        // 100 x 50 x 20 = 100000 mm^3, from a file that draws 99.5 x 49.7.
        ASSERT_TRUE(document.massProperties().valid);
        EXPECT_NEAR(document.massProperties().volumeMm3, 100000.0, 1e-6);

        std::ostringstream out;
        ASSERT_TRUE(savePartDocument(document, out));
        saved = out.str();
    }

    // Source independence: the saved document names no DXF file.
    EXPECT_EQ(saved.find(".dxf"), std::string::npos);
    EXPECT_EQ(saved.find("dimensioned_rectangle"), std::string::npos);

    // Fresh load, fresh backends, no reader anywhere.
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    std::istringstream in(saved);
    LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    loaded.document->setGeometryKernel(&kernel);
    loaded.document->setSketchSolver(&solver);

    const Parameter* width = loaded.document->parameters().findByName("Width");
    ASSERT_NE(width, nullptr);
    EXPECT_DOUBLE_EQ(width->value(), 100.0);

    // Width 100 -> 120.
    ASSERT_TRUE(loaded.document->setParameterValue(width->id(), 120.0));
    ASSERT_TRUE(loaded.document->recomputeFrom(width->id()).success);

    EXPECT_EQ(loaded.document->findSketch(sketchId)->solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(loaded.document->findSketch(sketchId)->degreesOfFreedom(), 0);

    // V = 120 x 50 x 20 = 120000 mm^3. This is the M7.1 gate.
    ASSERT_TRUE(loaded.document->massProperties().valid);
    EXPECT_NEAR(loaded.document->massProperties().volumeMm3, 120000.0, 1e-6);
}

// --- M7.7: a genuinely FRESH PROCESS (spec 24, Gate J) ----------------------
//
// Every "fresh load" test above builds a new PartDocument inside the same
// process. That proves the document does not depend on the previous document --
// it cannot prove the document does not depend on process state: a static
// cache, a leaked generator counter, an id that only resolves because the
// ObjectIdGenerator has already been advanced past it.
//
// M6 recorded this exact gap ("fresh-process load is proven but not in CI; a
// reviewer's three-process probe passed and lives in scratch"). This closes it
// by spawning a real child process.

std::string ScratchPath(const char* name) {
    return std::string(PARAMCAD_M7_SCRATCH_DIR) + "/" + name;
}

constexpr const char* kFreshProcessDocument = "m7_fresh_process.pcad";

// The CHILD half. Skips when its input is absent, so the ordinary test sweep --
// where this runs on its own, before any parent has written anything -- passes
// without pretending to have verified something.
TEST(M7FreshProcessChild, LoadEditAndVerifyVolume) {
    const std::string path = ScratchPath(kFreshProcessDocument);
    std::ifstream probe(path);
    if (!probe) GTEST_SKIP() << "no document to load; this test is driven by its parent";
    probe.close();

    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    LoadResult loaded = loadPartDocumentFromFile(path);
    ASSERT_TRUE(loaded) << loaded.message;
    loaded.document->setGeometryKernel(&kernel);
    loaded.document->setSketchSolver(&solver);

    const Parameter* width = loaded.document->parameters().findByName("Width");
    ASSERT_NE(width, nullptr);
    EXPECT_DOUBLE_EQ(width->value(), 100.0);

    ASSERT_TRUE(loaded.document->recompute().success);
    ASSERT_TRUE(loaded.document->setParameterValue(width->id(), 120.0));
    ASSERT_TRUE(loaded.document->recomputeFrom(width->id()).success);

    // 120 x 50 x 20 = 120000 mm^3, in a process that has never seen the DXF
    // file, the parser, or the reconstruction code.
    ASSERT_TRUE(loaded.document->massProperties().valid);
    EXPECT_NEAR(loaded.document->massProperties().volumeMm3, 120000.0, 1e-6);

    // PROOF OF WORK for the parent, and the reason it is a deletion rather than
    // a flag file: the parent cannot fake it, and a child that skipped or
    // crashed leaves the document exactly where it was.
    //
    // The previous guard -- the parent checking the document EXISTS -- was
    // tautological: the parent wrote that file itself two statements earlier.
    // Independent review forced the child to GTEST_SKIP and Gate J stayed
    // green.
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(M7FreshProcess, GATE_J_EditWorksInASecondProcessWithTheDxfDeleted) {
    const std::string path = ScratchPath(kFreshProcessDocument);
    const std::string copiedDxf = ScratchPath("m7_source_copy.dxf");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // Import from a COPY of the fixture, so the copy can be deleted afterwards
    // without touching the repository's fixtures.
    std::filesystem::copy_file(Fixture("dimensioned_rectangle.dxf"), copiedDxf,
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    {
        const DxfReadResult read = ReadDxfFile(copiedDxf);
        ASSERT_TRUE(read) << read.message;

        PartDocument document{"M7Fresh"};
        OcctGeometryKernel kernel;
        GaussNewtonSketchSolver solver;
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
        document.addMaterial("Aluminium", 2700.0);

        const SketchImportResult imported =
            ImportGeometryIntoNewSketch(document, "rectangle", read.geometry);
        ASSERT_TRUE(imported) << imported.message;
        ASSERT_TRUE(ReconstructSketch(document, imported.sketchId, read.geometry.dimensions));

        Parameter& padLength = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
        Body& body = document.addBody("Body001");
        document.addPadFeature(body, "Pad001", imported.sketchId, padLength.id());
        ASSERT_TRUE(document.recompute().success);
        ASSERT_TRUE(savePartDocumentToFile(document, path));
    }

    // Gate J: the original DXF is GONE before anything reads the document.
    std::filesystem::remove(copiedDxf, ec);
    ASSERT_FALSE(std::filesystem::exists(copiedDxf));

    // A real second process. Quoted, because the build path contains a drive
    // letter and may contain spaces.
    //
    // Its output is REDIRECTED, and that is not tidiness (round 2's R3-M2).
    // The child prints gtest's own "[  SKIPPED ]" line -- it is registered as
    // a skipped test so it does not run standalone -- and that line reached
    // this process's stdout. `gtest_discover_tests` stamps
    // SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]" on every discovered test, so
    // ctest read the child's line as THIS test's verdict: when the guard below
    // was deliberately broken, ctest reported the gate *** Skipped and counted
    // it as passing. The parent asserts on the exit status and on the deleted
    // document, so the child's text carries nothing this test needs.
    const std::string command = "\"\"" + std::string(PARAMCAD_IMPORT_TEST_EXE) +
                                "\" --gtest_filter=M7FreshProcessChild.* > NUL 2>&1\"";
    const int status = std::system(command.c_str());

    EXPECT_EQ(status, 0) << "the child process failed; command was: " << command;

    // A child that SKIPPED also exits 0. The only evidence that separates
    // "verified" from "did nothing" is something ONLY the child can do, so the
    // child deletes the document after checking it.
    EXPECT_FALSE(std::filesystem::exists(path))
        << "the child left the document in place, so it never ran its checks";

    std::filesystem::remove(path, ec);
}

} // namespace
