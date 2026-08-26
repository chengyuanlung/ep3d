// M49.2 -- the detail view as a thing ON A DRAWING, and in a file.
//
// DetailClipTests pins the crop. This pins what only a document can own: that
// the letter under the detail and the circle on the parent cannot disagree,
// that sections and details draw from ONE sequence of letters, and that a
// detail is captioned so nobody measures it with the sheet's ruler.
//
// The failure behind the shared sequence: two pools of letters put a
// "SECTION A-A" and a "DETAIL A" on the same sheet. A line marked A and a
// circle marked A on the same parent view, and a reader who looks up A finds
// whichever they see first. Neither view looks wrong.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

struct Sheet {
    DrawingDocument document{"Bracket"};
    ObjectId front = kInvalidObjectId;

    Sheet() {
        front = document.addView("Front", "bracket.ep3d", "", ViewDirection::Front,
                                 Vec2{80.0, 180.0})
                    .id();
    }
};

TEST(DetailViewTest, M49_DOC_001_SectionsAndDetailsShareOneSequenceOfLetters) {
    Sheet sheet;
    const ObjectId section =
        sheet.document
            .addSectionView("Cut", sheet.front, Vec2{-30.0, 0.0}, Vec2{30.0, 0.0}, 1, 90.0)
            .id();
    const ObjectId detail =
        sheet.document
            .addDetailView("Corner", sheet.front, Vec2{20.0, 20.0}, 8.0, DrawingScale{2, 1},
                           90.0)
            .id();

    EXPECT_EQ(sheet.document.viewLetterOf(section), "A");
    // B, NOT A. Separate pools would give both an A -- a cut line and a circle
    // on the same parent, both marked A.
    EXPECT_EQ(sheet.document.viewLetterOf(detail), "B")
        << "a detail took a letter a section on the same sheet already has";

    // ...and the two are captioned differently, because they are different
    // instructions about where to look on the parent.
    EXPECT_NE(sheet.document.viewLabelText(section).find("A-A"), std::string::npos);
    EXPECT_NE(sheet.document.viewLabelText(detail).find("DETAIL B"), std::string::npos);
    EXPECT_EQ(sheet.document.viewLabelText(detail).find("B-B"), std::string::npos)
        << "a detail was captioned as a section, which sends the reader hunting for a cut line";
}

TEST(DetailViewTest, M49_DOC_002_TheCaptionAlwaysCarriesTheScale) {
    // A DETAIL EXISTS BECAUSE IT IS DRAWN AT A DIFFERENT SIZE. A caption that
    // leaves the scale out invites the reader to measure it with the sheet's
    // ruler, and every number they take off it is wrong by exactly the
    // enlargement.
    Sheet sheet;
    const ObjectId detail =
        sheet.document
            .addDetailView("Corner", sheet.front, Vec2{20.0, 20.0}, 8.0, DrawingScale{5, 1},
                           90.0)
            .id();
    const std::string label = sheet.document.viewLabelText(detail);
    EXPECT_NE(label.find("5:1"), std::string::npos) << label;
    // ...and it has its OWN scale rather than following the sheet, so a later
    // rescale of the paper cannot quietly stop it being an enlargement.
    const DrawingView* view = sheet.document.findView(detail);
    ASSERT_NE(view, nullptr);
    EXPECT_TRUE(view->hasOwnScale());
}

TEST(DetailViewTest, M49_DOC_003_ACircleOfNoSizeIsRefused) {
    Sheet sheet;
    EXPECT_THROW(sheet.document.addDetailView("Corner", sheet.front, Vec2{20.0, 20.0}, 0.0,
                                              DrawingScale{2, 1}, 90.0),
                 std::invalid_argument);
    EXPECT_THROW(sheet.document.addDetailView("Corner", sheet.front, Vec2{20.0, 20.0}, -4.0,
                                              DrawingScale{2, 1}, 90.0),
                 std::invalid_argument);
    // ...and moving an existing circle to no size is refused too, rather than
    // leaving a view that projects the whole part at five times size.
    const ObjectId detail =
        sheet.document
            .addDetailView("Corner", sheet.front, Vec2{20.0, 20.0}, 8.0, DrawingScale{2, 1},
                           90.0)
            .id();
    EXPECT_FALSE(sheet.document.setDetailFrame(detail, Vec2{5.0, 5.0}, 0.0));
    ASSERT_NE(sheet.document.findView(detail), nullptr);
    EXPECT_NEAR(sheet.document.findView(detail)->detailFrame().radiusMm, 8.0, 1e-9);
}

TEST(DetailViewTest, M49_DOC_004_ADetailOfADetailIsRefusedRatherThanGuessedAt) {
    // Its circle would be in coordinates that have already been cropped once,
    // and the failure would be a view of somewhere nobody pointed at.
    Sheet sheet;
    const ObjectId detail =
        sheet.document
            .addDetailView("Corner", sheet.front, Vec2{20.0, 20.0}, 8.0, DrawingScale{2, 1},
                           90.0)
            .id();
    EXPECT_THROW(sheet.document.addDetailView("Closer", detail, Vec2{20.0, 20.0}, 2.0,
                                              DrawingScale{5, 1}, 90.0),
                 std::invalid_argument);
}

TEST(DetailViewTest, M49_DOC_005_TheCircleComesBackWithTheViewOnUndo) {
    // A DETAIL RESTORED WITHOUT ITS CIRCLE PROJECTS THE WHOLE PART, at the
    // enlarged scale -- and that looks like a view somebody put there on
    // purpose rather than like a bug.
    Sheet sheet;
    const ObjectId detail =
        sheet.document
            .addDetailView("Corner", sheet.front, Vec2{20.0, 20.0}, 8.0, DrawingScale{2, 1},
                           90.0)
            .id();
    ASSERT_TRUE(sheet.document.setDetailFrame(detail, Vec2{5.0, 6.0}, 3.0));
    EXPECT_NEAR(sheet.document.findView(detail)->detailFrame().radiusMm, 3.0, 1e-9);

    ASSERT_TRUE(sheet.document.undo());
    const DrawingView* back = sheet.document.findView(detail);
    ASSERT_NE(back, nullptr);
    EXPECT_TRUE(back->isDetail()) << "undo left the view but took its circle";
    EXPECT_NEAR(back->detailFrame().radiusMm, 8.0, 1e-9);
    EXPECT_NEAR(back->detailFrame().centreMm.x, 20.0, 1e-9);
}

TEST(DetailViewTest, M49_DOC_005B_ADetailDoesNotSitOnTopOfItsParent) {
    // THE SECTION LEARNED THIS FROM A SCREENSHOT AND THE DETAIL LEARNED IT
    // FROM THE NEXT ONE. A detail's direction IS its parent's, so the
    // six-direction alignment table compares a direction with itself, finds no
    // relationship, and leaves the enlargement exactly where the view it came
    // from is -- two pictures and two captions written over each other.
    Sheet sheet;
    const ObjectId detail =
        sheet.document
            .addDetailView("Corner", sheet.front, Vec2{30.0, 0.0}, 8.0, DrawingScale{2, 1},
                           60.0)
            .id();
    const Vec2 parentAt = sheet.document.viewPositionMm(sheet.front);
    const Vec2 detailAt = sheet.document.viewPositionMm(detail);
    EXPECT_GT(std::hypot(detailAt.x - parentAt.x, detailAt.y - parentAt.y), 1.0)
        << "the detail was placed on top of the view it was taken from";

    // ...AND IT LANDS ON THE SIDE THE CIRCLE IS ON, which is where the
    // reader's eye goes next. The parent has no projection in a unit test, so
    // its middle is the origin and the circle at +X pushes the detail +X.
    EXPECT_GT(detailAt.x, parentAt.x)
        << "the detail was placed away from the region it magnifies";
}

TEST(DetailViewTest, M49_DOC_005C_ACircleOnTheMiddleStillGetsSomewhereToSit) {
    // A circle exactly on the parent's centre has no side to be on. Straight
    // out to the right is arbitrary and is said to be in the code -- what it
    // must NOT do is stay at zero, which puts the detail back on its parent.
    Sheet sheet;
    const ObjectId detail =
        sheet.document
            .addDetailView("Middle", sheet.front, Vec2{0.0, 0.0}, 8.0, DrawingScale{2, 1},
                           60.0)
            .id();
    const Vec2 parentAt = sheet.document.viewPositionMm(sheet.front);
    const Vec2 detailAt = sheet.document.viewPositionMm(detail);
    EXPECT_GT(std::hypot(detailAt.x - parentAt.x, detailAt.y - parentAt.y), 1.0)
        << "a detail centred on its parent was left sitting on it";
}

TEST(DetailViewTest, M49_DOC_005D_DeletingADetailAndUndoingBringsTheCircleBack) {
    // A DETAIL RESTORED WITHOUT ITS CIRCLE PROJECTS THE WHOLE PART at the
    // enlarged scale -- which looks like a view somebody put there on purpose
    // rather than like something undo got wrong.
    Sheet sheet;
    const ObjectId detail =
        sheet.document
            .addDetailView("Corner", sheet.front, Vec2{20.0, 25.0}, 8.0, DrawingScale{2, 1},
                           60.0)
            .id();
    ASSERT_TRUE(sheet.document.removeObject(detail));
    ASSERT_EQ(sheet.document.findView(detail), nullptr);

    ASSERT_TRUE(sheet.document.undo());
    const DrawingView* back = sheet.document.findView(detail);
    ASSERT_NE(back, nullptr) << "undo did not bring the detail back at all";
    EXPECT_TRUE(back->isDetail()) << "the detail came back as a view of the whole part";
    EXPECT_NEAR(back->detailFrame().radiusMm, 8.0, 1e-9);
    EXPECT_NEAR(back->detailFrame().centreMm.x, 20.0, 1e-9);
    EXPECT_NEAR(back->detailFrame().centreMm.y, 25.0, 1e-9);
}

TEST(DetailViewTest, M49_DOC_006_TheCircleSurvivesTheFile) {
    Sheet sheet;
    const ObjectId detail =
        sheet.document
            .addDetailView("Corner", sheet.front, Vec2{20.0, 25.0}, 8.0, DrawingScale{2, 1},
                           90.0)
            .id();
    std::ostringstream out;
    const SaveResult saved = saveDrawingDocument(sheet.document, out);
    ASSERT_EQ(saved.error, SerializationError::None) << saved.message;

    std::istringstream in(out.str());
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const DrawingView* back = loaded.document->findView(detail);
    ASSERT_NE(back, nullptr);
    ASSERT_TRUE(back->isDetail()) << "a detail came back as an ordinary view of the part";
    EXPECT_NEAR(back->detailFrame().centreMm.x, 20.0, 1e-9);
    EXPECT_NEAR(back->detailFrame().centreMm.y, 25.0, 1e-9);
    EXPECT_NEAR(back->detailFrame().radiusMm, 8.0, 1e-9);
    EXPECT_EQ(loaded.document->viewLabelText(detail),
              sheet.document.viewLabelText(detail));
}

TEST(DetailViewTest, M49_DOC_007_WhatTheSaverRefusesTheLoaderRefuses) {
    // ADR-M3-008. A hand-edited file whose detail circle has no size would
    // open as a view of the whole part at five times size.
    Sheet sheet;
    sheet.document.addDetailView("Corner", sheet.front, Vec2{20.0, 25.0}, 8.0,
                                 DrawingScale{2, 1}, 90.0);
    std::ostringstream out;
    ASSERT_EQ(saveDrawingDocument(sheet.document, out).error, SerializationError::None);
    std::string text = out.str();

    const std::string::size_type at = text.find("\"radiusMm\": 8");
    ASSERT_NE(at, std::string::npos) << text.substr(0, 400);
    text.replace(at, std::string("\"radiusMm\": 8").size(), "\"radiusMm\": 0");

    std::istringstream in(text);
    EXPECT_FALSE(loadDrawingDocument(in)) << "a detail circle of no size loaded anyway";
}

TEST(DetailViewTest, M49_DOC_008_AViewCannotBeBothASectionAndADetail) {
    // The recompute would cut and crop with two ideas of what its camera is,
    // and the picture would be of neither.
    Sheet sheet;
    sheet.document.addDetailView("Corner", sheet.front, Vec2{20.0, 25.0}, 8.0,
                                 DrawingScale{2, 1}, 90.0);
    std::ostringstream out;
    ASSERT_EQ(saveDrawingDocument(sheet.document, out).error, SerializationError::None);
    std::string text = out.str();

    const std::string::size_type at = text.find("\"detail\": {");
    ASSERT_NE(at, std::string::npos);
    text.insert(at, "\"section\": { \"fromXMm\": 0, \"fromYMm\": 0, \"toXMm\": 10, "
                    "\"toYMm\": 0, \"arrowSide\": 1 }, ");

    std::istringstream in(text);
    EXPECT_FALSE(loadDrawingDocument(in)) << "a view that is both a section and a detail loaded";
}

} // namespace
