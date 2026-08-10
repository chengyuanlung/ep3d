// M6.1 — DXF import boundary and LINE (spec 25).
//
// Every expected number here is computed by hand from the fixture, which is
// itself small enough to read. That is the point of hand-written fixtures
// (spec 15): a test whose expectation came from running the importer proves
// only that the importer agrees with itself.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Import/SketchImporter.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include "Import/Dxf/DxfReader.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

// PARAMCAD_DXF_FIXTURE_DIR is set by CMake, so the tests do not depend on the
// working directory ctest happens to use.
std::string Fixture(const char* name) {
    return std::string(PARAMCAD_DXF_FIXTURE_DIR) + "/" + name;
}

const SketchLine& LineOf(const Sketch& sketch, SketchEntityId id) {
    const SketchEntity* entity = sketch.findEntity(id);
    EXPECT_NE(entity, nullptr);
    return std::get<SketchLine>(entity->geometry);
}

// --- Gate A: LINE import ------------------------------------------------------

TEST(M6DxfImport, M6_GATE_A_LineImportsWithHandComputedCoordinates) {
    const DxfReadResult read = ReadDxfFile(Fixture("line.dxf"));
    ASSERT_TRUE(read) << read.message;

    ASSERT_EQ(read.geometry.lines.size(), 1u);
    EXPECT_TRUE(read.geometry.circles.empty());
    EXPECT_TRUE(read.geometry.arcs.empty());

    // Straight from the fixture: (0,0) -> (100,50), declared in millimetres.
    const ImportedLine2D& line = read.geometry.lines.front();
    EXPECT_DOUBLE_EQ(line.start.x, 0.0);
    EXPECT_DOUBLE_EQ(line.start.y, 0.0);
    EXPECT_DOUBLE_EQ(line.end.x, 100.0);
    EXPECT_DOUBLE_EQ(line.end.y, 50.0);

    EXPECT_EQ(read.geometry.unit, ImportedLengthUnit::Millimeter);
    EXPECT_FALSE(read.geometry.unitWasDefaulted) << "the file states $INSUNITS = 4";
}

TEST(M6DxfImport, M6_IMPORT_001_ImportedEntitiesAreOrdinarySketchEntities) {
    // The claim M6 exists to make: after import there is nothing about an
    // entity that says it came from a file (ADR-M6-004).
    PartDocument document{"Imported"};
    const DxfReadResult read = ReadDxfFile(Fixture("line.dxf"));
    ASSERT_TRUE(read) << read.message;

    const SketchImportResult result =
        ImportGeometryIntoNewSketch(document, "FromDxf", read.geometry);
    ASSERT_TRUE(result) << result.message;
    EXPECT_EQ(result.importedCount, 1u);
    ASSERT_EQ(result.entityIds.size(), 1u);

    const Sketch* sketch = document.findSketch(result.sketchId);
    ASSERT_NE(sketch, nullptr) << "the imported sketch is not a document object";
    EXPECT_NE(result.entityIds.front(), kInvalidSketchEntityId);

    const SketchLine& line = LineOf(*sketch, result.entityIds.front());
    EXPECT_DOUBLE_EQ(line.start.x, 0.0);
    EXPECT_DOUBLE_EQ(line.end.x, 100.0);
    EXPECT_DOUBLE_EQ(line.end.y, 50.0);

    // It behaves like native geometry: it can be edited through the ordinary
    // facade, and the document dirties and recomputes as usual.
    EXPECT_TRUE(document.markSketchDirty(result.sketchId));
    EXPECT_TRUE(document.recompute().success);
}

// --- Gate B: CIRCLE import (M6.2) --------------------------------------------

const SketchCircle& CircleOf(const Sketch& sketch, SketchEntityId id) {
    const SketchEntity* entity = sketch.findEntity(id);
    EXPECT_NE(entity, nullptr);
    return std::get<SketchCircle>(entity->geometry);
}

TEST(M6DxfImport, M6_GATE_B_CircleImportsWithHandComputedCentreAndRadius) {
    const DxfReadResult read = ReadDxfFile(Fixture("circle.dxf"));
    ASSERT_TRUE(read) << read.message;

    ASSERT_EQ(read.geometry.circles.size(), 1u);
    EXPECT_TRUE(read.geometry.lines.empty());
    EXPECT_TRUE(read.geometry.arcs.empty()) << "a CIRCLE was imported as an ARC";

    // Straight from the fixture: centre (25,30), radius 10, in millimetres.
    // The centre is off the origin on purpose -- a reader that dropped it would
    // still get the radius right.
    const ImportedCircle2D& circle = read.geometry.circles.front();
    EXPECT_DOUBLE_EQ(circle.center.x, 25.0);
    EXPECT_DOUBLE_EQ(circle.center.y, 30.0);
    EXPECT_DOUBLE_EQ(circle.radiusMm, 10.0);
}

TEST(M6DxfImport, M6_CIRCLE_001_ImportedCircleIsAnOrdinarySketchCircle) {
    PartDocument document{"ImportedCircle"};
    const DxfReadResult read = ReadDxfFile(Fixture("circle.dxf"));
    ASSERT_TRUE(read) << read.message;

    const SketchImportResult result =
        ImportGeometryIntoNewSketch(document, "FromDxf", read.geometry);
    ASSERT_TRUE(result) << result.message;
    ASSERT_EQ(result.entityIds.size(), 1u);

    const Sketch* sketch = document.findSketch(result.sketchId);
    ASSERT_NE(sketch, nullptr);
    const SketchCircle& circle = CircleOf(*sketch, result.entityIds.front());
    EXPECT_DOUBLE_EQ(circle.center.x, 25.0);
    EXPECT_DOUBLE_EQ(circle.center.y, 30.0);
    EXPECT_DOUBLE_EQ(circle.radiusMm, 10.0);

    // And it survives save/load with the same id, like any native entity.
    const SketchEntityId id = result.entityIds.front();
    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(document, out));
    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch* reloaded = loaded.document->findSketch(result.sketchId);
    ASSERT_NE(reloaded, nullptr);
    ASSERT_NE(reloaded->findEntity(id), nullptr) << "the circle's id did not survive";
    EXPECT_DOUBLE_EQ(CircleOf(*reloaded, id).radiusMm, 10.0);
}

TEST(M6DxfImport, M6_UNITS_003_ACircleIsScaledInBothCentreAndRadius) {
    // Metres: centre (25000, 30000) mm, radius 10000 mm. The radius and the
    // centre are separate group codes, so scaling one and not the other is a
    // real and easy mistake -- this asserts both.
    const DxfReadResult read = ReadDxfFile(Fixture("circle_metres.dxf"));
    ASSERT_TRUE(read) << read.message;
    ASSERT_EQ(read.geometry.circles.size(), 1u);

    EXPECT_EQ(read.geometry.unit, ImportedLengthUnit::Meter);
    EXPECT_FALSE(read.geometry.unitWasDefaulted);
    EXPECT_DOUBLE_EQ(read.geometry.millimetresPerUnit, 1000.0);

    const ImportedCircle2D& circle = read.geometry.circles.front();
    EXPECT_DOUBLE_EQ(circle.center.x, 25000.0);
    EXPECT_DOUBLE_EQ(circle.center.y, 30000.0);
    EXPECT_DOUBLE_EQ(circle.radiusMm, 10000.0);
}

TEST(M6DxfImport, M6_CIRCLE_002_DegenerateRadiiAreSkippedAndTheRestSurvive) {
    // Zero and negative radii sit BETWEEN two valid circles. An importer that
    // abandoned the file at the first bad entity returns fewer than two; one
    // that silently repaired them returns more.
    const DxfReadResult read = ReadDxfFile(Fixture("circle_degenerate.dxf"));
    ASSERT_TRUE(read) << read.message;

    ASSERT_EQ(read.geometry.circles.size(), 2u) << "the valid circles did not both survive";
    EXPECT_DOUBLE_EQ(read.geometry.circles[0].radiusMm, 5.0);
    EXPECT_DOUBLE_EQ(read.geometry.circles[1].radiusMm, 7.0);

    // Both bad ones are REPORTED, and as invalid geometry rather than as an
    // unsupported kind -- CIRCLE is supported; these particular ones are not
    // usable, and spec 11 wants those told apart.
    const auto invalid = std::count_if(
        read.geometry.skipped.begin(), read.geometry.skipped.end(),
        [](const ImportedSkip& skip) {
            return skip.entityKind == "CIRCLE" &&
                   skip.reason == ImportSkipReason::InvalidGeometry;
        });
    EXPECT_EQ(invalid, 2) << "a zero or negative radius was not reported as invalid geometry";

    // And the whole file still imports, carrying only the usable geometry.
    PartDocument document{"Degenerate"};
    const SketchImportResult result =
        ImportGeometryIntoNewSketch(document, "FromDxf", read.geometry);
    ASSERT_TRUE(result) << result.message;
    EXPECT_EQ(result.importedCount, 2u);
    EXPECT_EQ(result.skippedCount, 2u);
}

TEST(M6DxfImport, M6_CIRCLE_003_AnImportedCircleDrivesDownstreamGeometry) {
    // Gate G in miniature, and the claim that matters: an imported entity is
    // real native geometry, not a picture. A Pad built on the imported circle
    // must produce the analytical volume of a cylinder.
    PartDocument document{"CircleToPad"};
    OcctGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    document.addMaterial("Aluminium", 2700.0);

    const DxfReadResult read = ReadDxfFile(Fixture("circle.dxf"));
    ASSERT_TRUE(read) << read.message;
    const SketchImportResult imported =
        ImportGeometryIntoNewSketch(document, "FromDxf", read.geometry);
    ASSERT_TRUE(imported) << imported.message;

    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", imported.sketchId, length.id());

    ASSERT_TRUE(document.recompute().success);
    EXPECT_EQ(pad.state(), ComputeState::Valid);

    // pi * 10^2 * 20 mm^3, computed by hand from the fixture's radius.
    const double expected = 3.14159265358979323846 * 100.0 * 20.0;
    EXPECT_NEAR(document.massProperties().volumeMm3, expected, 1e-4 * expected);
}

// --- Unit policy (ADR-M6-002) -------------------------------------------------

TEST(M6DxfImport, M6_UNITS_001_InchesAreConvertedExactly) {
    // Same numbers in the file as line.dxf, declared in inches. 100 in =
    // 2540 mm and 50 in = 1270 mm, because an inch is 25.4 mm exactly. If the
    // conversion were skipped this test and Gate A would BOTH pass with the
    // same expectations, which is why the two fixtures carry identical
    // coordinates and different units.
    const DxfReadResult read = ReadDxfFile(Fixture("line_inches.dxf"));
    ASSERT_TRUE(read) << read.message;
    ASSERT_EQ(read.geometry.lines.size(), 1u);

    EXPECT_EQ(read.geometry.unit, ImportedLengthUnit::Inch);
    EXPECT_FALSE(read.geometry.unitWasDefaulted);
    EXPECT_DOUBLE_EQ(read.geometry.millimetresPerUnit, 25.4);

    const ImportedLine2D& line = read.geometry.lines.front();
    EXPECT_DOUBLE_EQ(line.end.x, 2540.0);
    EXPECT_DOUBLE_EQ(line.end.y, 1270.0);
}

TEST(M6DxfImport, M6_UNITS_002_AMissingUnitIsAssumedAndSaidSo) {
    const DxfReadResult read = ReadDxfFile(Fixture("line_unitless.dxf"));
    ASSERT_TRUE(read) << read.message;
    ASSERT_EQ(read.geometry.lines.size(), 1u);

    // The documented default applies...
    EXPECT_DOUBLE_EQ(read.geometry.millimetresPerUnit, 1.0);
    EXPECT_DOUBLE_EQ(read.geometry.lines.front().end.x, 100.0);
    // ...and the fact that it was an assumption is RECORDED, which is the half
    // that stops a 25.4x error from being invisible.
    EXPECT_TRUE(read.geometry.unitWasDefaulted);

    PartDocument document{"Assumed"};
    const SketchImportResult result =
        ImportGeometryIntoNewSketch(document, "FromDxf", read.geometry);
    ASSERT_TRUE(result) << result.message;
    EXPECT_NE(result.message.find("assumed"), std::string::npos)
        << "the assumption is not visible to the user: " << result.message;
}

// --- Unsupported entities (spec 4, ADR-M6-005) --------------------------------

TEST(M6DxfImport, M6_SKIP_001_AnUnsupportedEntityIsReportedNotReinterpreted) {
    const DxfReadResult read = ReadDxfFile(Fixture("unsupported.dxf"));
    ASSERT_TRUE(read) << read.message;

    // BOTH lines arrive: the unsupported entity sits between them, so an
    // importer that stopped at the first unrecognised thing would return one.
    EXPECT_EQ(read.geometry.lines.size(), 2u);
    EXPECT_TRUE(read.geometry.circles.empty());
    EXPECT_TRUE(read.geometry.arcs.empty())
        << "an unsupported entity was reinterpreted as geometry";

    ASSERT_FALSE(read.geometry.skipped.empty());
    const auto text = std::find_if(
        read.geometry.skipped.begin(), read.geometry.skipped.end(),
        [](const ImportedSkip& skip) { return skip.entityKind == "TEXT"; });
    ASSERT_NE(text, read.geometry.skipped.end()) << "the TEXT entity was not reported";
    EXPECT_EQ(text->reason, ImportSkipReason::UnsupportedEntity);
    EXPECT_FALSE(text->detail.empty());
}

// --- Transaction policy (spec 10) ---------------------------------------------

TEST(M6DxfImport, M6_TRANSACTION_001_AnEmptyImportLeavesNoSketchBehind) {
    // Nothing importable means FAILURE, not an empty sketch the user would have
    // to discover. And a failed import must leave the document exactly as it
    // was -- no orphan sketch, no registry entry, no graph node.
    PartDocument document{"Empty"};
    const std::size_t before = document.sketches().size();

    ImportedSketchGeometry nothing;
    nothing.skipped.push_back(ImportedSkip{ImportSkipReason::UnsupportedEntity, "SPLINE",
                                           "not imported by M6"});
    const SketchImportResult result = ImportGeometryIntoNewSketch(document, "Empty", nothing);

    EXPECT_FALSE(result);
    EXPECT_FALSE(result.message.empty());
    EXPECT_EQ(document.sketches().size(), before) << "a failed import left a sketch behind";
    EXPECT_EQ(document.objectRegistry().size(), 1u)
        << "a failed import left an orphan registry entry";
}

TEST(M6DxfImport, M6_TRANSACTION_002_InvalidGeometryRollsTheWholeSketchBack) {
    // One bad entity among good ones must not leave the good half behind: the
    // user would have to work out which half arrived.
    PartDocument document{"Rollback"};
    const std::size_t before = document.sketches().size();

    ImportedSketchGeometry mixed;
    mixed.lines.push_back(ImportedLine2D{Vec2{0, 0}, Vec2{10, 0}});
    mixed.lines.push_back(ImportedLine2D{Vec2{5, 5}, Vec2{5, 5}}); // degenerate
    mixed.lines.push_back(ImportedLine2D{Vec2{0, 0}, Vec2{0, 10}});

    const SketchImportResult result = ImportGeometryIntoNewSketch(document, "Bad", mixed);
    EXPECT_FALSE(result);
    EXPECT_EQ(document.sketches().size(), before);
    EXPECT_TRUE(document.recompute().success) << "the document is not usable after a rollback";
}

// --- Gate E / F: identity and source independence -----------------------------

TEST(M6DxfImport, M6_GATE_EF_ImportSurvivesSaveAndTheFileIsNoLongerNeeded) {
    PartDocument document{"Persisted"};
    const DxfReadResult read = ReadDxfFile(Fixture("line.dxf"));
    ASSERT_TRUE(read) << read.message;
    const SketchImportResult imported =
        ImportGeometryIntoNewSketch(document, "FromDxf", read.geometry);
    ASSERT_TRUE(imported) << imported.message;

    const ObjectId sketchId = imported.sketchId;
    const SketchEntityId entityId = imported.entityIds.front();

    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(document, out));
    const std::string saved = out.str();

    // Gate F: the DXF is not referenced by the saved document at all. A path
    // in the file would make the document depend on a file the user may move.
    EXPECT_EQ(saved.find("line.dxf"), std::string::npos)
        << "the saved document names the DXF it came from";
    EXPECT_EQ(saved.find(".dxf"), std::string::npos);

    std::istringstream in(saved);
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;

    // Gate E: the SAME ids, not ids re-derived from file order.
    const Sketch* sketch = loaded.document->findSketch(sketchId);
    ASSERT_NE(sketch, nullptr) << "the imported sketch did not survive save/load";
    const SketchEntity* entity = sketch->findEntity(entityId);
    ASSERT_NE(entity, nullptr) << "the imported entity id did not survive save/load";

    const SketchLine& line = std::get<SketchLine>(entity->geometry);
    EXPECT_DOUBLE_EQ(line.start.x, 0.0);
    EXPECT_DOUBLE_EQ(line.end.x, 100.0);
    EXPECT_DOUBLE_EQ(line.end.y, 50.0);
}

// --- Diagnostics (spec 11) ----------------------------------------------------

TEST(M6DxfImport, M6_DIAG_001_AMissingFileIsDistinguishableFromABadOne) {
    const DxfReadResult missing = ReadDxfFile(Fixture("no-such-file.dxf"));
    EXPECT_FALSE(missing);
    EXPECT_EQ(missing.error, DxfReadError::FileNotFound);
    EXPECT_NE(missing.message.find("no such file"), std::string::npos);

    // A file that exists and is not DXF must report a DIFFERENT cause. "Import
    // failed" for both is what spec 11 forbids.
    const std::string junkPath =
        (std::filesystem::temp_directory_path() / "paramcad_not_a_dxf.txt").string();
    {
        std::ofstream junk(junkPath);
        junk << "this is not a DXF file\n";
    }
    const DxfReadResult malformed = ReadDxfFile(junkPath);
    EXPECT_FALSE(malformed);
    EXPECT_NE(malformed.error, DxfReadError::FileNotFound)
        << "a malformed file was reported as missing";
    EXPECT_FALSE(malformed.message.empty());
    std::filesystem::remove(junkPath);
}

} // namespace
