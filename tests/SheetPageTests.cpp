// M44 -- several sheets in one drawing file.
//
// The title block has had a "Sheet" row since M35 and it could only ever say
// 1 / 1. That is the kind of half-truth a drawing carries into a workshop: a
// reader who sees 1 / 1 believes there is no second page, and acts on it.
//
// What a page is: paper, a frame, a name. What a page is NOT is a container --
// the views and symbols stay in the document's lists and each says which page
// it is on. Ownership would make "which page" impossible to get wrong, and it
// would have rewritten forty methods; the boundary check does most of the same
// work, in one place, at save and at load.
//
// So the failures this file is written against are:
//
//   * a number in the title block that is not where the page actually sits
//   * an object on a page that has gone -- on no tab, findable by nothing
//   * everything drawing on every page, which is one sheet with three
//     drawings on top of each other

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <sstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {

using namespace paramcad;

TEST(SheetPageTest, M44_PAGE_001_ADrawingHasOnePageAndCannotBeLeftWithNone) {
    DrawingDocument document{"Plate"};
    ASSERT_EQ(document.sheetPages().size(), 1u);
    const ObjectId first = document.currentSheetId();
    EXPECT_NE(first, kInvalidObjectId);
    EXPECT_EQ(document.sheetNumberOf(first), "1 / 1");

    // THE LAST PAGE CANNOT GO. A drawing with no paper is not a drawing, and
    // every accessor that asks for "the current page" would need a case that
    // cannot happen -- which is a case nobody maintains.
    EXPECT_FALSE(document.removeSheetPage(first));
    EXPECT_EQ(document.sheetPages().size(), 1u);
}

TEST(SheetPageTest, M44_PAGE_002_TheSheetRowSaysWHERETheePageSitsAndNotWhatWasStored) {
    DrawingDocument document{"Plate"};
    const ObjectId one = document.currentSheetId();
    const ObjectId two = document.addSheetPage("Details").id();
    const ObjectId three = document.addSheetPage("Wiring").id();

    EXPECT_EQ(document.sheetNumberOf(one), "1 / 3");
    EXPECT_EQ(document.sheetNumberOf(two), "2 / 3");
    EXPECT_EQ(document.sheetNumberOf(three), "3 / 3");

    // ...AND IT MOVES WHEN A PAGE GOES. Stored, "2 / 3" would still be printed
    // on a drawing that now has two sheets -- in the one place on the paper a
    // reader trusts without checking.
    ASSERT_TRUE(document.removeSheetPage(two));
    EXPECT_EQ(document.sheetNumberOf(one), "1 / 2");
    EXPECT_EQ(document.sheetNumberOf(three), "2 / 2");
    EXPECT_TRUE(document.sheetNumberOf(two).empty()) << "a page that is gone still has a number";
}

TEST(SheetPageTest, M44_PAGE_003_WhatIsDrawnLandsOnTheePageBeingLookedAt) {
    // A user who switched to sheet 2 and drew a line meant it to be on sheet 2.
    // Finding it on sheet 1 is the kind of surprise that gets blamed on the
    // drawing rather than on the tool.
    DrawingDocument document{"Plate"};
    const ObjectId one = document.currentSheetId();
    const ObjectId two = document.addSheetPage("Details").id();

    const ObjectId onFirst = document.addEntity(DrawLine{Vec2{0, 0}, Vec2{10, 0}}).id();
    ASSERT_TRUE(document.setCurrentSheet(two));
    const ObjectId onSecond = document.addEntity(DrawLine{Vec2{0, 0}, Vec2{10, 0}}).id();

    EXPECT_EQ(document.sheetOfObject(onFirst), one);
    EXPECT_EQ(document.sheetOfObject(onSecond), two);
    EXPECT_EQ(document.objectsOnSheet(one), 1u);
    EXPECT_EQ(document.objectsOnSheet(two), 1u);

    // The painter's question, and the whole of what a page means: not hidden,
    // not deleted -- on another sheet.
    EXPECT_FALSE(document.isOnCurrentSheet(document.findEntity(onFirst)->sheetId()));
    EXPECT_TRUE(document.isOnCurrentSheet(document.findEntity(onSecond)->sheetId()));
}

TEST(SheetPageTest, M44_PAGE_004_APageWithThingsOnItIsNotDeleted) {
    // Cascading would throw away work nobody asked to lose; letting the
    // objects dangle leaves them on no tab at all, findable by nothing. The
    // count is what tells the user how much there is to move first.
    DrawingDocument document{"Plate"};
    const ObjectId two = document.addSheetPage("Details").id();
    ASSERT_TRUE(document.setCurrentSheet(two));
    const ObjectId line = document.addEntity(DrawLine{Vec2{0, 0}, Vec2{10, 0}}).id();

    EXPECT_EQ(document.objectsOnSheet(two), 1u);
    EXPECT_FALSE(document.removeSheetPage(two)) << "a page with a line on it was deleted";

    // Move the line off, and the page can go.
    ASSERT_TRUE(document.setObjectSheet(line, document.sheetPages().front()->id()));
    EXPECT_EQ(document.objectsOnSheet(two), 0u);
    EXPECT_TRUE(document.removeSheetPage(two));
    EXPECT_NE(document.findEntity(line), nullptr) << "the line went with the page anyway";
}

TEST(SheetPageTest, M44_PAGE_005_EachPageHasItsOwnPaper) {
    // A general arrangement on A2 and its details on A4 is an ordinary drawing
    // set. Paper on the document rather than the page would make that one
    // choice for the whole file.
    DrawingDocument document{"Plate"};
    const ObjectId one = document.currentSheetId();
    const ObjectId two = document.addSheetPage("Details").id();

    ASSERT_TRUE(document.setCurrentSheet(two));
    ASSERT_TRUE(document.setSheetSize(SheetSize::A4));
    EXPECT_EQ(document.sheet().size(), SheetSize::A4);

    ASSERT_TRUE(document.setCurrentSheet(one));
    EXPECT_NE(document.sheet().size(), SheetSize::A4)
        << "changing one page's paper changed the other's";

    // ...and a new page starts as a copy of the one it was added from, which
    // is what a drawing set nearly always wants.
    ASSERT_TRUE(document.setCurrentSheet(two));
    const ObjectId three = document.addSheetPage("More details").id();
    ASSERT_TRUE(document.setCurrentSheet(three));
    EXPECT_EQ(document.sheet().size(), SheetSize::A4);
}

TEST(SheetPageTest, M44_PAGE_006_TheTitleBlockIsTheDRAWINGSAndOnlyItsSheetRowDiffers) {
    // One title, one drawing number, one approval -- facts about the drawing
    // and not about a page of it. A block per page would be several copies of
    // one fact, and they would drift the first time somebody corrected the
    // title on whichever page they were looking at.
    DrawingDocument document{"Plate"};
    ASSERT_TRUE(document.setTitleBlockField("Title", "Bearing bracket"));
    const ObjectId two = document.addSheetPage("Details").id();
    ASSERT_TRUE(document.setCurrentSheet(two));

    bool found = false;
    for (const TitleBlockField& field : document.titleBlock().fields())
        if (field.label == "Title") {
            found = true;
            EXPECT_EQ(field.value, "Bearing bracket")
                << "the second page has a title block of its own";
        }
    EXPECT_TRUE(found);

    // What DOES differ is the number, and it is derived.
    EXPECT_EQ(document.currentSheetNumber(), 2);
    EXPECT_EQ(document.sheetCount(), 2);
}

TEST(SheetPageTest, M44_PAGE_007_ThePagesSurviveASaveAndSoDoesWhatIsOnThem) {
    DrawingDocument document{"Plate"};
    const ObjectId one = document.currentSheetId();
    const ObjectId two = document.addSheetPage("Details").id();
    ASSERT_TRUE(document.setCurrentSheet(two));
    ASSERT_TRUE(document.setSheetSize(SheetSize::A4));
    const ObjectId line = document.addEntity(DrawLine{Vec2{0, 0}, Vec2{10, 0}}).id();

    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(document, out));
    const std::string text = out.str();
    // THE NUMBER IS NOT IN THE FILE. It is where the page sits, and a written
    // copy is the first thing to go stale when a page is inserted.
    EXPECT_EQ(text.find("2 / 2"), std::string::npos) << "a sheet number was written down";

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->sheetPages().size(), 2u);
    EXPECT_EQ(loaded.document->currentSheetId(), two) << "the page being looked at was lost";
    EXPECT_EQ(loaded.document->sheet().size(), SheetSize::A4);
    EXPECT_EQ(loaded.document->sheetOfObject(line), two)
        << "the line came back on the wrong page";
    EXPECT_EQ(loaded.document->sheetNumberOf(one), "1 / 2");

    std::ostringstream again;
    ASSERT_TRUE(saveDrawingDocument(*loaded.document, again));
    EXPECT_EQ(again.str(), text);
}

TEST(SheetPageTest, M44_PAGE_008_AFileWhoseObjectIsOnAPageThatIsNotThereIsREFUSED) {
    // ADR-M3-008, and by the SAME call: the saver asks the document whether it
    // can be written, and so does the loader. An object on a page that is not
    // there is on no tab at all -- it cannot be found, moved or deleted, and
    // nothing on the screen says it exists.
    DrawingDocument document{"Plate"};
    const ObjectId two = document.addSheetPage("Details").id();
    ASSERT_TRUE(document.setCurrentSheet(two));
    document.addEntity(DrawLine{Vec2{0, 0}, Vec2{10, 0}});

    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(document, out));
    std::string text = out.str();

    const std::string real = "\"sheetId\": \"" + std::to_string(two) + "\"";
    const std::size_t at = text.find(real);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, real.size(), "\"sheetId\": \"747474\"");

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded) << "an object on a sheet that is not in the drawing was accepted";
    EXPECT_NE(loaded.message.find("sheet"), std::string::npos) << loaded.message;
}

TEST(SheetPageTest, M44_PAGE_009_ADrawingFromBEFOREPagesOpensAsOnePage) {
    // Every file written before M44 has its paper at the top level and no
    // sheets array. Refusing those, or opening them blank, would be a format
    // change that quietly costs somebody their drawings.
    const std::string old =
        "{\n  \"format\": \"ParametricCAD\",\n  \"schemaVersion\": 42,\n"
        "  \"documentType\": \"Drawing\",\n  \"id\": \"1\",\n  \"name\": \"Old\",\n"
        "  \"frames\": [],\n  \"connectors\": [],\n  \"linetypes\": [],\n"
        "  \"layers\": [],\n  \"views\": [],\n  \"entities\": [],\n"
        "  \"sheet\": {\n    \"size\": \"A4\",\n    \"orientation\": \"Portrait\",\n"
        "    \"scale\": \"1:2\",\n    \"projectionAngle\": \"First\"\n  }\n}\n";
    std::istringstream in(old);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->sheetPages().size(), 1u);
    EXPECT_EQ(loaded.document->sheet().size(), SheetSize::A4);
    EXPECT_EQ(loaded.document->sheet().orientation(), SheetOrientation::Portrait);
    EXPECT_EQ(loaded.document->sheet().scale().toString(), "1:2");
    EXPECT_EQ(loaded.document->sheetNumberOf(loaded.document->currentSheetId()), "1 / 1");
}

TEST(SheetPageTest, M44_PAGE_010_TwoPagesCannotShareAName) {
    // The name is what a tab says. Two the same and a user cannot tell which
    // one they are about to draw on.
    DrawingDocument document{"Plate"};
    document.addSheetPage("Details");
    EXPECT_THROW(document.addSheetPage("Details"), std::invalid_argument);
    EXPECT_THROW(document.addSheetPage(""), std::invalid_argument);
    EXPECT_EQ(document.sheetPages().size(), 2u);
}

} // namespace
