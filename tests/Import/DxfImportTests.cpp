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
