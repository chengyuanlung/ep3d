// M39.4 -- the hole table.
//
// What a shop works from when a plate has twenty holes in it. Every column is
// derived from the part, so the failure this file is written against is not a
// crash: it is a table that is perfectly formatted and describes a part that
// is not the one in the model.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Drawing/HoleTable.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/HoleFeature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

struct ScratchPart {
    std::string path;
    explicit ScratchPart(const char* name) {
        path = (std::filesystem::temp_directory_path() / (std::string("ep3d-holes-") + name))
                   .string();
        std::remove(path.c_str());
    }
    ~ScratchPart() { std::remove(path.c_str()); }
};

// A plate with holes at named places, so a test can say where a row ought to
// come out rather than reading it off the answer.
struct Plate {
    PartDocument part{"Plate"};
    Body* body = nullptr;
    PadFeature* pad = nullptr;
    // WHAT THE NEXT HOLE DRILLS INTO. A solid is consumed once (ADR-M8-008),
    // so a second hole feature is drilled into the FIRST one's result, not
    // into the pad again -- the same chain a real part has.
    ObjectId last = kInvalidObjectId;

    Plate() {
        Sketch& outline = part.addSketch("Base");
        part.addSketchEntity(outline.id(), SketchLine{Vec2{0, 0}, Vec2{100, 0}});
        part.addSketchEntity(outline.id(), SketchLine{Vec2{100, 0}, Vec2{100, 60}});
        part.addSketchEntity(outline.id(), SketchLine{Vec2{100, 60}, Vec2{0, 60}});
        part.addSketchEntity(outline.id(), SketchLine{Vec2{0, 60}, Vec2{0, 0}});
        Parameter& tall = part.addParameter("H", 10.0, UnitType::Millimeter);
        body = &part.addBody("Plate");
        pad = &part.addPadFeature(*body, "Pad", outline.id(), tall.id());
        last = pad->id();
    }

    // A hole feature drilled at `where`, through, at `diameter` unless a screw
    // says otherwise.
    HoleFeature& drill(const std::string& name, const std::vector<Vec2>& where, double diameter) {
        Sketch& marks = part.addSketch(name + " marks");
        for (const Vec2 at : where) part.addSketchEntity(marks.id(), SketchPoint{at});
        Parameter& across = part.addParameter(name + " D", diameter, UnitType::Millimeter);
        Parameter& deep = part.addParameter(name + " Z", 0.0, UnitType::Millimeter);
        HoleFeature& made =
            part.addHoleFeature(*body, name, last, marks.id(), across.id(), deep.id());
        last = made.id();
        return made;
    }

    std::string writeTo(const ScratchPart& file) {
        EXPECT_TRUE(savePartDocumentToFile(part, file.path));
        return file.path;
    }
};

TEST(HoleTableTest, M39_TABLE_001_EveryHoleInThePartGetsARowAndTheyAreMEASUREDFromTheDatum) {
    // The datum is the drawing's, not the model's: a machinist works from a
    // corner of the part, and "37.5 from the origin the modeller happened to
    // sketch on" means nothing at a machine.
    Plate plate;
    plate.drill("Fixings", {Vec2{10.0, 10.0}, Vec2{90.0, 10.0}, Vec2{10.0, 50.0}}, 6.6);
    ScratchPart file{"basic.ep3d"};

    const HoleTableContents table =
        HolesOfPart(plate.writeTo(file), ViewDirection::Top, Vec2{10.0, 10.0});
    ASSERT_TRUE(table.ok) << table.why;
    ASSERT_EQ(table.rows.size(), 3u);

    // All three are the same hole, so all three carry the SAME LETTER -- which
    // is the thing a reader wants from a tag.
    EXPECT_EQ(table.rows[0].tag, "A1");
    EXPECT_EQ(table.rows[1].tag, "A2");
    EXPECT_EQ(table.rows[2].tag, "A3");

    // Read down the page then across: the hole at y = 50 comes first, and the
    // datum has taken 10 off both axes.
    EXPECT_NEAR(table.rows[0].atMm.x, 0.0, 1e-9);
    EXPECT_NEAR(table.rows[0].atMm.y, 40.0, 1e-9);
    EXPECT_NEAR(table.rows[1].atMm.x, 0.0, 1e-9);
    EXPECT_NEAR(table.rows[1].atMm.y, 0.0, 1e-9);
    EXPECT_NEAR(table.rows[2].atMm.x, 80.0, 1e-9);
    EXPECT_NEAR(table.rows[2].atMm.y, 0.0, 1e-9);
}

TEST(HoleTableTest, M39_TABLE_002_HolesOfDIFFERENTSizesGetDifferentLetters) {
    Plate plate;
    plate.drill("Fixings", {Vec2{10.0, 10.0}, Vec2{90.0, 10.0}}, 6.6);
    plate.drill("Dowels", {Vec2{50.0, 30.0}}, 4.0);
    ScratchPart file{"sizes.ep3d"};

    const HoleTableContents table =
        HolesOfPart(plate.writeTo(file), ViewDirection::Top, Vec2{0.0, 0.0});
    ASSERT_TRUE(table.ok) << table.why;
    ASSERT_EQ(table.rows.size(), 3u);
    EXPECT_EQ(table.rows[0].tag, "A1");
    EXPECT_EQ(table.rows[1].tag, "A2");
    EXPECT_EQ(table.rows[2].tag, "B1") << "a different size shared a letter with another";
    EXPECT_NEAR(table.rows[2].diameterMm, 4.0, 1e-9);
    EXPECT_NE(table.rows[0].callout, table.rows[2].callout);
}

TEST(HoleTableTest, M39_TABLE_003_TheCalloutIsTheOneTheHoleIsCUTFrom) {
    // A table that composed its own callouts would be a second reading of one
    // standard: the drawing would say M8 over a hole drilled to something
    // else, and both halves would look right on their own.
    Plate plate;
    HoleFeature& tapped = plate.drill("Tapped", {Vec2{20.0, 20.0}}, 12.0);
    HoleScrew screw;
    screw.designation = "M8";
    screw.tapped = true;
    ASSERT_TRUE(plate.part.setHoleScrew(tapped.id(), screw));
    ScratchPart file{"tapped.ep3d"};

    const HoleTableContents table =
        HolesOfPart(plate.writeTo(file), ViewDirection::Top, Vec2{0.0, 0.0});
    ASSERT_TRUE(table.ok) << table.why;
    ASSERT_EQ(table.rows.size(), 1u);

    EXPECT_NE(table.rows[0].callout.find("M8x1.25"), std::string::npos) << table.rows[0].callout;
    // AND THE DIAMETER IS THE TAP DRILL, not the 12 the parameter still says.
    // The parameter is left alone on purpose -- it is what the hole falls back
    // to if the screw is ever cleared -- so this is exactly the pair that goes
    // wrong when two places both work out a size.
    EXPECT_NEAR(table.rows[0].diameterMm, 6.8, 1e-9)
        << "the table reported the typed diameter instead of the drilled one";
}

TEST(HoleTableTest, M39_TABLE_004_APartWithNoHolesIsEMPTYAndAPartThatWillNotOpenIsREFUSED) {
    // The two must not look the same. A table that came back empty because the
    // file had gone would say "this part has no holes" -- the one wrong answer
    // that looks like a right one.
    Plate plate;
    ScratchPart file{"nothing.ep3d"};
    const HoleTableContents empty =
        HolesOfPart(plate.writeTo(file), ViewDirection::Top, Vec2{0.0, 0.0});
    EXPECT_TRUE(empty.ok) << empty.why;
    EXPECT_TRUE(empty.rows.empty());

    const HoleTableContents missing =
        HolesOfPart("D:/nowhere/at/all/plate.ep3d", ViewDirection::Top, Vec2{0.0, 0.0});
    EXPECT_FALSE(missing.ok) << "a part that could not be read was reported as having no holes";
    EXPECT_FALSE(missing.why.empty());
}

TEST(HoleTableTest, M39_TABLE_005_TurningTheViewMovesTheHolesTheWayTheModelTurns) {
    // The positions are on the VIEW'S page. A table that used model x and y
    // whatever the view was would put every number beside the wrong hole on a
    // front view, and each number would still be a number the part contains.
    Plate plate;
    // Off BOTH axes on purpose: a hole on the sketch's x axis has the same
    // page y from the front (its height, zero) as from the top (its y, zero),
    // so it cannot tell a working projection from one that ignores the view.
    plate.drill("One", {Vec2{30.0, 20.0}}, 6.0);
    ScratchPart file{"turned.ep3d"};
    const std::string path = plate.writeTo(file);

    const HoleTableContents top = HolesOfPart(path, ViewDirection::Top, Vec2{0.0, 0.0});
    ASSERT_TRUE(top.ok) << top.why;
    ASSERT_EQ(top.rows.size(), 1u);
    // Seen from above, the sketch's x is the page's x.
    EXPECT_NEAR(top.rows[0].atMm.x, 30.0, 1e-9);

    const HoleTableContents front = HolesOfPart(path, ViewDirection::Front, Vec2{0.0, 0.0});
    ASSERT_TRUE(front.ok) << front.why;
    ASSERT_EQ(front.rows.size(), 1u);
    // Seen from the front, the same hole is 30 across and at the height of the
    // sketch plane -- which is a different pair of numbers, not the same ones.
    EXPECT_NEAR(front.rows[0].atMm.x, 30.0, 1e-9);
    EXPECT_NEAR(front.rows[0].atMm.y, 0.0, 1e-9);
    EXPECT_NE(front.rows[0].atMm.y, top.rows[0].atMm.y)
        << "the table gave the same two numbers whichever way the part was looked at";
}

// A drawing with one view of a plate that has three holes in it.
struct Sheet {
    DrawingDocument drawing{"Plate"};
    ObjectId view = kInvalidObjectId;
    ScratchPart file{"charted.ep3d"};

    Sheet() {
        Plate plate;
        plate.drill("Fixings", {Vec2{10.0, 10.0}, Vec2{90.0, 10.0}, Vec2{10.0, 50.0}}, 6.6);
        plate.writeTo(file);
        view = drawing
                   .addView("Top", file.path, "Plate", ViewDirection::Top, Vec2{150.0, 200.0})
                   .id();
    }
};

TEST(HoleTableTest, M39_CHART_001_ATableListsTheHolesOfTheViewItBelongsTo) {
    // It belongs to a VIEW, not to a file. A table carrying its own source
    // path could name a different part from the view its tags are drawn on,
    // and every row in it would still be a correct row about some part.
    Sheet sheet;
    const HoleTable& table = sheet.drawing.addHoleTable("Holes", sheet.view,
                                                        Vec2{20.0, 90.0}, Vec2{10.0, 10.0});
    const HoleTableContents rows = sheet.drawing.holesOf(table);
    ASSERT_TRUE(rows.ok) << rows.why;
    ASSERT_EQ(rows.rows.size(), 3u);
    EXPECT_EQ(rows.rows[0].tag, "A1");
    EXPECT_NEAR(rows.rows[0].atMm.y, 40.0, 1e-9);

    // A table of a view that is not in this drawing is refused outright: an
    // empty box on the paper is not something a reader can interpret.
    EXPECT_THROW(sheet.drawing.addHoleTable("Nowhere", 999777, Vec2{0.0, 0.0}, Vec2{}),
                 std::invalid_argument);
}

TEST(HoleTableTest, M39_CHART_002_MovingTheDATUMRewritesEveryRow) {
    // Nothing is stored, so nothing has to be recomputed -- which is the point
    // of deriving them. A table that kept its own copy of the positions would
    // still be measuring from the old corner, and every number in it would
    // look exactly as plausible as before.
    Sheet sheet;
    HoleTable& table = sheet.drawing.addHoleTable("Holes", sheet.view, Vec2{20.0, 90.0},
                                                  Vec2{0.0, 0.0});
    const HoleTableContents before = sheet.drawing.holesOf(table);
    ASSERT_TRUE(before.ok) << before.why;
    ASSERT_EQ(before.rows.size(), 3u);
    EXPECT_NEAR(before.rows[0].atMm.x, 10.0, 1e-9);

    ASSERT_TRUE(sheet.drawing.setHoleTableDatum(table.id(), Vec2{10.0, 10.0}));
    const HoleTableContents after = sheet.drawing.holesOf(table);
    ASSERT_TRUE(after.ok) << after.why;
    ASSERT_EQ(after.rows.size(), 3u);
    EXPECT_NEAR(after.rows[0].atMm.x, 0.0, 1e-9)
        << "the rows were still measured from the old datum";

    // ...and an undo puts the old corner back, rows and all.
    ASSERT_TRUE(sheet.drawing.undo());
    EXPECT_NEAR(sheet.drawing.holesOf(table).rows[0].atMm.x, 10.0, 1e-9);
}

TEST(HoleTableTest, M39_CHART_003_DeletingTheViewTakesItsTableAndONEUndoBringsBothBack) {
    // A table whose view has gone has nothing to read and no page to measure
    // on -- and the drawing would then refuse to SAVE, which is a document a
    // user cannot get out of without knowing why.
    Sheet sheet;
    const ObjectId tableId =
        sheet.drawing.addHoleTable("Holes", sheet.view, Vec2{20.0, 90.0}, Vec2{10.0, 10.0})
            .id();
    ASSERT_NE(sheet.drawing.findHoleTable(tableId), nullptr);

    ASSERT_TRUE(sheet.drawing.removeObject(sheet.view));
    EXPECT_EQ(sheet.drawing.findHoleTable(tableId), nullptr) << "the table outlived its view";

    ASSERT_TRUE(sheet.drawing.undo());
    ASSERT_NE(sheet.drawing.findView(sheet.view), nullptr);
    ASSERT_NE(sheet.drawing.findHoleTable(tableId), nullptr)
        << "one undo did not bring the whole family back";
    EXPECT_NEAR(sheet.drawing.findHoleTable(tableId)->datumMm().x, 10.0, 1e-9);
}

TEST(HoleTableTest, M39_CHART_004_ATableSurvivesASaveAndStillListsTheSameHoles) {
    Sheet sheet;
    const ObjectId tableId =
        sheet.drawing.addHoleTable("Holes", sheet.view, Vec2{20.0, 90.0}, Vec2{10.0, 10.0})
            .id();

    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(sheet.drawing, out));
    const std::string text = out.str();
    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;

    const HoleTable* back = loaded.document->findHoleTable(tableId);
    ASSERT_NE(back, nullptr) << "the hole table did not come back";
    EXPECT_NEAR(back->datumMm().x, 10.0, 1e-9);
    EXPECT_NEAR(back->positionMm().y, 90.0, 1e-9);
    const HoleTableContents rows = loaded.document->holesOf(*back);
    ASSERT_TRUE(rows.ok) << rows.why;
    EXPECT_EQ(rows.rows.size(), 3u);
    EXPECT_EQ(rows.rows[0].tag, "A1");

    // ...and what the file writes, it reads: the second save is the first.
    std::ostringstream again;
    ASSERT_TRUE(saveDrawingDocument(*loaded.document, again));
    EXPECT_EQ(again.str(), text);
}

TEST(HoleTableTest, M39_CHART_005_AFileWhoseTableNamesNoViewIsREFUSED) {
    // ADR-M3-008 again: what the saver refuses, the loader refuses -- and by
    // the same rule, because a file can be written by hand or by an older
    // build.
    Sheet sheet;
    sheet.drawing.addHoleTable("Holes", sheet.view, Vec2{20.0, 90.0}, Vec2{10.0, 10.0});
    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(sheet.drawing, out));
    std::string text = out.str();

    const std::string real = "\"viewId\": \"" + std::to_string(sheet.view) + "\"";
    const std::size_t at = text.find(real);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, real.size(), "\"viewId\": \"424242\"");

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded) << "a hole table of a view that is not there was accepted";
    EXPECT_EQ(loaded.error, SerializationError::UnknownDependencyId);
}

} // namespace
