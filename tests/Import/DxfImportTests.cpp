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

// --- Gate C: ARC import (M6.3) ------------------------------------------------
//
// Arcs are measured GEOMETRICALLY, never by comparing the stored angle to the
// number in the file. Spec 16 says so, and the reason is concrete: reproducing
// the importer's own conversion is what let a broken Angle constraint ship in
// M5 with a green test beside it. Here the arc's start and end POINTS are
// computed from the fixture by hand and compared with the points the model
// actually holds.

constexpr double kPi = 3.14159265358979323846;
constexpr double kArcTol = 1e-9; // mm; the conversion is exact bar rounding

const SketchArc& ArcOf(const Sketch& sketch, SketchEntityId id) {
    const SketchEntity* entity = sketch.findEntity(id);
    EXPECT_NE(entity, nullptr);
    return std::get<SketchArc>(entity->geometry);
}

// Where an arc actually starts and ends, derived from what the model stores.
Vec2 PointAt(const ImportedArc2D& arc, double angleRad) {
    return Vec2{arc.center.x + arc.radiusMm * std::cos(angleRad),
                arc.center.y + arc.radiusMm * std::sin(angleRad)};
}

TEST(M6DxfImport, M6_GATE_C_ArcImportsWithGeometricallyMeasuredEndpoints) {
    const DxfReadResult read = ReadDxfFile(Fixture("arc.dxf"));
    ASSERT_TRUE(read) << read.message;

    ASSERT_EQ(read.geometry.arcs.size(), 1u);
    EXPECT_TRUE(read.geometry.lines.empty());
    EXPECT_TRUE(read.geometry.circles.empty()) << "an ARC was imported as a CIRCLE";

    const ImportedArc2D& arc = read.geometry.arcs.front();
    EXPECT_DOUBLE_EQ(arc.center.x, 10.0);
    EXPECT_DOUBLE_EQ(arc.center.y, -5.0);
    EXPECT_DOUBLE_EQ(arc.radiusMm, 12.0);

    // Hand-computed from the fixture: centre (10,-5), r 12, 30 deg and 200 deg.
    //   start = (10 + 12*cos30, -5 + 12*sin30) = (20.392304845, 1.0)
    //   end   = (10 + 12*cos200, -5 + 12*sin200) = (-1.276311, -9.104242)
    // These are the numbers a reader can check against a calculator, which is
    // the point of measuring points rather than re-deriving the angles.
    const Vec2 start = PointAt(arc, arc.startAngleRad);
    const Vec2 end = PointAt(arc, arc.endAngleRad);
    EXPECT_NEAR(start.x, 10.0 + 12.0 * std::cos(30.0 * kPi / 180.0), kArcTol);
    EXPECT_NEAR(start.y, -5.0 + 12.0 * std::sin(30.0 * kPi / 180.0), kArcTol);
    EXPECT_NEAR(end.x, 10.0 + 12.0 * std::cos(200.0 * kPi / 180.0), kArcTol);
    EXPECT_NEAR(end.y, -5.0 + 12.0 * std::sin(200.0 * kPi / 180.0), kArcTol);

    // Spelled out as literal coordinates as well, so the test does not lean
    // entirely on the same trig call the expectation above uses. On the first
    // run these literals were wrong and the trig-based expectations were right,
    // so the second check caught MY arithmetic rather than the importer's --
    // which is still the reason to have it.
    EXPECT_NEAR(start.x, 20.392304845413264, 1e-9);
    EXPECT_NEAR(start.y, 1.0, 1e-9);
    EXPECT_NEAR(end.x, -1.2763114494309011, 1e-9);
    EXPECT_NEAR(end.y, -9.104241719908025, 1e-9);
}

TEST(M6DxfImport, M6_ARC_001_DegreesAreConvertedNotPassedThrough) {
    // DXF stores codes 50/51 in DEGREES. libdxfrw's header says it hands them
    // over in radians; that claim is not taken on trust. If 30 arrived
    // unconverted it would mean 30 RADIANS = 1.867 rad = 107 deg, a completely
    // different direction -- which this asserts is NOT what happened.
    const DxfReadResult read = ReadDxfFile(Fixture("arc.dxf"));
    ASSERT_TRUE(read) << read.message;
    const ImportedArc2D& arc = read.geometry.arcs.front();

    EXPECT_NEAR(arc.startAngleRad, 30.0 * kPi / 180.0, 1e-12);
    EXPECT_NEAR(arc.endAngleRad, 200.0 * kPi / 180.0, 1e-12);
    EXPECT_LT(std::fabs(arc.startAngleRad), 2.0 * kPi)
        << "the start angle looks like raw degrees, not radians";
}

TEST(M6DxfImport, M6_ARC_002_StartAndEndAreDistinguishable) {
    // The mutation spec 18 names explicitly: swapping start and end must break
    // Gate C. That is only true if the two angles produce DIFFERENT points, so
    // this asserts the fixture is actually capable of detecting the swap --
    // a fixture with a symmetric sweep would silently disarm that mutation.
    const DxfReadResult read = ReadDxfFile(Fixture("arc.dxf"));
    ASSERT_TRUE(read) << read.message;
    const ImportedArc2D& arc = read.geometry.arcs.front();

    const Vec2 start = PointAt(arc, arc.startAngleRad);
    const Vec2 end = PointAt(arc, arc.endAngleRad);
    EXPECT_GT(std::hypot(end.x - start.x, end.y - start.y), 1.0)
        << "start and end coincide, so a swap could not be detected";
    EXPECT_NE(arc.startAngleRad, arc.endAngleRad);
}

TEST(M6DxfImport, M6_ARC_003_AnArcCrossingZeroKeepsItsShortSweep) {
    // start 350, end 40. DXF arcs always sweep counter-clockwise from start to
    // end, so this is a 50 degree sweep THROUGH zero -- not 310 degrees the
    // other way. An importer that assumed end > start gets it backwards, and
    // an arc that does not cross zero cannot reveal that.
    const DxfReadResult read = ReadDxfFile(Fixture("arc_crossing_zero.dxf"));
    ASSERT_TRUE(read) << read.message;
    ASSERT_EQ(read.geometry.arcs.size(), 1u);
    const ImportedArc2D& arc = read.geometry.arcs.front();

    EXPECT_NEAR(arc.startAngleRad, 350.0 * kPi / 180.0, 1e-12);
    EXPECT_NEAR(arc.endAngleRad, 40.0 * kPi / 180.0, 1e-12);

    // The counter-clockwise sweep from start to end, measured rather than read.
    double sweep = arc.endAngleRad - arc.startAngleRad;
    while (sweep <= 0.0) sweep += 2.0 * kPi;
    EXPECT_NEAR(sweep, 50.0 * kPi / 180.0, 1e-12)
        << "the sweep is " << sweep * 180.0 / kPi << " deg, not 50";
}

TEST(M6DxfImport, M6_ARC_004_ImportedArcIsAnOrdinarySketchArcAndSurvivesReload) {
    PartDocument document{"ImportedArc"};
    const DxfReadResult read = ReadDxfFile(Fixture("arc.dxf"));
    ASSERT_TRUE(read) << read.message;
    const SketchImportResult result =
        ImportGeometryIntoNewSketch(document, "FromDxf", read.geometry);
    ASSERT_TRUE(result) << result.message;
    ASSERT_EQ(result.entityIds.size(), 1u);
    const SketchEntityId id = result.entityIds.front();

    const Sketch* sketch = document.findSketch(result.sketchId);
    ASSERT_NE(sketch, nullptr);
    const SketchArc& arc = ArcOf(*sketch, id);
    EXPECT_DOUBLE_EQ(arc.center.x, 10.0);
    EXPECT_DOUBLE_EQ(arc.center.y, -5.0);
    EXPECT_DOUBLE_EQ(arc.radiusMm, 12.0);
    EXPECT_TRUE(arc.counterClockwise) << "DXF arcs sweep counter-clockwise";

    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(document, out));
    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch* reloaded = loaded.document->findSketch(result.sketchId);
    ASSERT_NE(reloaded, nullptr);
    ASSERT_NE(reloaded->findEntity(id), nullptr) << "the arc's id did not survive";

    // Measured again after the round trip, from the reloaded model.
    const SketchArc& after = ArcOf(*reloaded, id);
    EXPECT_NEAR(after.center.x + after.radiusMm * std::cos(after.startAngleRad),
                20.392304845413264, 1e-9);
    EXPECT_NEAR(after.center.y + after.radiusMm * std::sin(after.startAngleRad), 1.0, 1e-9);
}

TEST(M6DxfImport, M6_ARC_005_ADegenerateArcIsSkippedAndTheRestSurvive) {
    const DxfReadResult read = ReadDxfFile(Fixture("arc_degenerate.dxf"));
    ASSERT_TRUE(read) << read.message;

    ASSERT_EQ(read.geometry.arcs.size(), 2u) << "the valid arcs did not both survive";
    EXPECT_DOUBLE_EQ(read.geometry.arcs[0].radiusMm, 4.0);
    EXPECT_DOUBLE_EQ(read.geometry.arcs[1].radiusMm, 6.0);

    const auto invalid = std::count_if(
        read.geometry.skipped.begin(), read.geometry.skipped.end(),
        [](const ImportedSkip& skip) {
            return skip.entityKind == "ARC" &&
                   skip.reason == ImportSkipReason::InvalidGeometry;
        });
    EXPECT_EQ(invalid, 1);
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
