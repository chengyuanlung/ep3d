// M42 -- item balloons.
//
// A balloon ties a part on the picture to a row in the parts list. What makes
// that hard is not drawing a circle: it is that the number in the circle and
// the number in the table are the same fact, written in two places on the same
// sheet, and a reader ORDERS FROM one of them.
//
// So the number is not stored anywhere. A balloon says which list and which
// part; the item number is the list's answer, asked for every time. Insert a
// part, the list renumbers, and every balloon renumbers with it -- because
// none of them ever held a number to go stale.
//
// The failure this file is written against is the quiet one: a balloon
// carrying a number that is still A number the list contains, just not this
// part's. Nothing looks broken and the wrong part gets ordered.

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Document/PartDocument.h"
#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/AssemblyDocumentSerializer.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"
#include "Core/Serialization/PartDocumentSerializer.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using namespace paramcad;

struct Scratch {
    std::string path;
    explicit Scratch(const char* name) {
        path = (std::filesystem::temp_directory_path() / (std::string("ep3d-bal-") + name))
                   .string();
        std::remove(path.c_str());
    }
    ~Scratch() { std::remove(path.c_str()); }
};

void WritePart(const std::string& path, const std::string& bodyName) {
    PartDocument part{"Source"};
    part.addBody(bodyName);
    ASSERT_TRUE(savePartDocumentToFile(part, path));
}

DimensionAnchor Somewhere(Vec2 at) {
    DimensionAnchor anchor;
    anchor.kind = DimensionAnchorKind::Free;
    anchor.at = at;
    return anchor;
}

// A drawing with a parts list on it, counting an assembly of a plate and four
// bolts.
struct Rig {
    Scratch bolt{"bolt.ep3d"};
    Scratch plate{"plate.ep3d"};
    Scratch rig{"rig.ep3da"};
    DrawingDocument drawing{"Assembly"};
    ObjectId table = kInvalidObjectId;

    Rig() {
        WritePart(bolt.path, "M6x20");
        WritePart(plate.path, "Plate");
        AssemblyDocument assembly{"Rig"};
        assembly.addInstance("Plate1", plate.path, "Plate");
        for (int i = 1; i <= 4; ++i)
            assembly.addInstance("Bolt" + std::to_string(i), bolt.path, "M6x20");
        std::ofstream out(rig.path, std::ios::binary);
        EXPECT_TRUE(out.good());
        EXPECT_TRUE(saveAssemblyDocument(assembly, out));
        out.close();

        table = drawing.addBomTable("Parts", rig.path, Vec2{200.0, 40.0}).id();
    }

    ObjectId balloonOn(const std::string& file, const std::string& body, Vec2 at) {
        BalloonSpec spec;
        spec.tableId = table;
        spec.sourceFile = file;
        spec.partName = body;
        return drawing.addAnnotation(spec, Somewhere(at), Vec2{at.x + 10.0, at.y + 10.0})
            .id();
    }
};

TEST(BalloonTest, M42_BAL_001_TheNumberIsTheROWSNumberAndComesFromTheList) {
    Rig rig;
    const BomContents rows = rig.drawing.countBom(*rig.drawing.findBomTable(rig.table));
    ASSERT_TRUE(rows.ok) << rows.why;
    ASSERT_EQ(rows.rows.size(), 2u);

    const ObjectId onPlate = rig.balloonOn(rig.plate.path, "Plate", Vec2{60.0, 80.0});
    const ObjectId onBolt = rig.balloonOn(rig.bolt.path, "M6x20", Vec2{90.0, 80.0});

    // Whatever the list says those rows are, the balloons say the same.
    std::string plateItem;
    std::string boltItem;
    for (const BomRow& row : rows.rows) {
        if (row.partName == "Plate") plateItem = std::to_string(row.item);
        if (row.partName == "M6x20") boltItem = std::to_string(row.item);
    }
    ASSERT_FALSE(plateItem.empty());
    ASSERT_FALSE(boltItem.empty());
    EXPECT_EQ(rig.drawing.annotationText(onPlate), plateItem);
    EXPECT_EQ(rig.drawing.annotationText(onBolt), boltItem);
    EXPECT_NE(plateItem, boltItem) << "two different parts came out as the same item";
}

TEST(BalloonTest, M42_BAL_002_TwoBalloonsOnTheSamePartCarryTheSameNumber) {
    // Four bolts, four balloons, one row. A build that numbered balloons in the
    // order they were placed would give them 1, 2, 3, 4 -- four item numbers
    // for one line in the list, every one of them a number the list contains.
    Rig rig;
    const ObjectId first = rig.balloonOn(rig.bolt.path, "M6x20", Vec2{40.0, 80.0});
    const ObjectId second = rig.balloonOn(rig.bolt.path, "M6x20", Vec2{70.0, 80.0});
    const ObjectId third = rig.balloonOn(rig.bolt.path, "M6x20", Vec2{100.0, 80.0});

    const std::string said = rig.drawing.annotationText(first);
    EXPECT_FALSE(said.empty());
    EXPECT_EQ(rig.drawing.annotationText(second), said);
    EXPECT_EQ(rig.drawing.annotationText(third), said)
        << "balloons on one part were numbered by where they were placed";
}

TEST(BalloonTest, M42_BAL_003_ThreeBalloonsFollowTheListWhenTheASSEMBLYChanges) {
    // THE WHOLE REASON THE NUMBER IS NOT STORED. Add a part to the assembly,
    // the list renumbers, and the balloons have to renumber with it -- a
    // stored number would go on pointing at a row that has moved, and it would
    // still be a number the list contains.
    Rig rig;
    const ObjectId onBolt = rig.balloonOn(rig.bolt.path, "M6x20", Vec2{60.0, 80.0});
    const std::string before = rig.drawing.annotationText(onBolt);
    EXPECT_FALSE(before.empty());

    // A washer, written into the assembly AHEAD of the bolt.
    Scratch washer{"washer.ep3d"};
    WritePart(washer.path, "M6 washer");
    {
        AssemblyDocument assembly{"Rig"};
        assembly.addInstance("Plate1", rig.plate.path, "Plate");
        assembly.addInstance("Washer1", washer.path, "M6 washer");
        for (int i = 1; i <= 4; ++i)
            assembly.addInstance("Bolt" + std::to_string(i), rig.bolt.path, "M6x20");
        std::ofstream out(rig.rig.path, std::ios::binary);
        ASSERT_TRUE(out.good());
        ASSERT_TRUE(saveAssemblyDocument(assembly, out));
    }

    const std::string after = rig.drawing.annotationText(onBolt);
    EXPECT_FALSE(after.empty());
    EXPECT_NE(after, before)
        << "the list renumbered and the balloon went on saying what it said before";

    // ...and it is still the bolt's row, which is the part that matters.
    const BomContents rows = rig.drawing.countBom(*rig.drawing.findBomTable(rig.table));
    ASSERT_TRUE(rows.ok) << rows.why;
    for (const BomRow& row : rows.rows)
        if (row.partName == "M6x20") EXPECT_EQ(after, std::to_string(row.item));
}

TEST(BalloonTest, M42_BAL_004_ABalloonOnAPartTheListDoesNotHaveSaysSo) {
    // The quiet failure: a circle with a number in it, on a part the list has
    // never heard of. Drawn blank it looks like a balloon nobody filled in;
    // drawn with a stale number it looks correct.
    Rig rig;
    BalloonSpec stranger;
    stranger.tableId = rig.table;
    stranger.sourceFile = "D:/nowhere/gasket.ep3d";
    stranger.partName = "Gasket";
    const ObjectId ghost =
        rig.drawing.addAnnotation(stranger, Somewhere(Vec2{50.0, 90.0}), Vec2{60.0, 100.0})
            .id();

    const std::string why = rig.drawing.whyAnnotationRefused(ghost);
    EXPECT_FALSE(why.empty()) << "a balloon on a part that is not in the list drew happily";
    EXPECT_NE(why.find("not in the parts list"), std::string::npos) << why;
    EXPECT_TRUE(rig.drawing.annotationText(ghost).empty());
}

TEST(BalloonTest, M42_BAL_005_ABalloonNamingNoListOrNoPartIsREFUSEDAtTheDoor) {
    Rig rig;
    BalloonSpec noList;
    noList.tableId = 987321;
    noList.sourceFile = rig.bolt.path;
    noList.partName = "M6x20";
    EXPECT_THROW(rig.drawing.addAnnotation(noList, Somewhere(Vec2{}), Vec2{}),
                 std::invalid_argument);

    BalloonSpec noPart;
    noPart.tableId = rig.table;
    EXPECT_THROW(rig.drawing.addAnnotation(noPart, Somewhere(Vec2{}), Vec2{}),
                 std::invalid_argument);
}

TEST(BalloonTest, M42_BAL_006_ABalloonReadsTheListITNamesAndNotJustAnyList) {
    // A sheet can carry two lists -- what this assembly is made of, and every
    // part however deep -- and the same bolt is a different item in each. A
    // balloon that only said "the parts list" would be right on whichever was
    // drawn first.
    Rig rig;
    const ObjectId exploded =
        rig.drawing.addBomTable("Every part", rig.rig.path, Vec2{200.0, 140.0}).id();
    ASSERT_TRUE(rig.drawing.setBomDepth(exploded, BomDepth::Exploded));

    const ObjectId onFirst = rig.balloonOn(rig.bolt.path, "M6x20", Vec2{60.0, 80.0});
    BalloonSpec second;
    second.tableId = exploded;
    second.sourceFile = rig.bolt.path;
    second.partName = "M6x20";
    const ObjectId onSecond =
        rig.drawing.addAnnotation(second, Somewhere(Vec2{90.0, 80.0}), Vec2{100.0, 90.0})
            .id();

    // Both are the bolt, and each reads its OWN list's row number.
    const BomContents topRows = rig.drawing.countBom(*rig.drawing.findBomTable(rig.table));
    const BomContents allRows = rig.drawing.countBom(*rig.drawing.findBomTable(exploded));
    ASSERT_TRUE(topRows.ok) << topRows.why;
    ASSERT_TRUE(allRows.ok) << allRows.why;
    for (const BomRow& row : topRows.rows)
        if (row.partName == "M6x20")
            EXPECT_EQ(rig.drawing.annotationText(onFirst), std::to_string(row.item));
    for (const BomRow& row : allRows.rows)
        if (row.partName == "M6x20")
            EXPECT_EQ(rig.drawing.annotationText(onSecond), std::to_string(row.item));
}

TEST(BalloonTest, M42_BAL_007_ABalloonSurvivesASaveAndStillReadsTheSameRow) {
    Rig rig;
    const ObjectId onBolt = rig.balloonOn(rig.bolt.path, "M6x20", Vec2{60.0, 80.0});
    const std::string said = rig.drawing.annotationText(onBolt);
    ASSERT_FALSE(said.empty());

    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(rig.drawing, out));
    const std::string text = out.str();
    // THE NUMBER IS NOT IN THE FILE. If it were, it would be a second copy of
    // the row's item number and the first thing to go stale.
    EXPECT_EQ(text.find("\"item\""), std::string::npos) << "a balloon wrote its number down";

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->annotationText(onBolt), said);
    std::ostringstream again;
    ASSERT_TRUE(saveDrawingDocument(*loaded.document, again));
    EXPECT_EQ(again.str(), text);
}

TEST(BalloonTest, M42_BAL_008_AFileWhoseBalloonNamesAMissingListIsREFUSED) {
    // ADR-M3-008 again, and by the same call: the saver asks the document
    // whether a symbol can be drawn, and so does the loader -- after the parts
    // lists are back, because that is what a balloon reads.
    Rig rig;
    rig.balloonOn(rig.bolt.path, "M6x20", Vec2{60.0, 80.0});
    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(rig.drawing, out));
    std::string text = out.str();

    const std::string real = "\"tableId\": \"" + std::to_string(rig.table) + "\"";
    const std::size_t at = text.find(real);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, real.size(), "\"tableId\": \"515151\"");

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded) << "a balloon reading a parts list that is not there was accepted";
    EXPECT_NE(loaded.message.find("parts list"), std::string::npos) << loaded.message;
}

} // namespace
