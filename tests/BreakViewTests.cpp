// M50.2 -- the broken view on a drawing, and THE test this milestone is for.
//
// A 600 mm bar broken between 100 and 500 draws about 200 long. The dimension
// across it has to say 600.
//
// Get that wrong and nothing looks wrong. The picture is a perfectly ordinary
// broken view, the break symbols are where they belong, the number is a
// plausible number, and every other number on the sheet agrees with it. The
// bar arrives 400 mm short.
//
// It is right here BY CONSTRUCTION rather than by a rule: nothing is removed
// from the projection, so there is no second copy of the length to keep in
// step. The break is a mapping onto paper with an inverse, and measuring goes
// through the inverse.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace paramcad;

// A 600 x 20 bar, as a projection put in by hand -- no kernel needed, and the
// numbers are round enough to read off the assertions.
ProjectedDrawing Bar() {
    ProjectedDrawing drawing;
    const Vec2 corners[4] = {Vec2{0.0, 0.0}, Vec2{600.0, 0.0}, Vec2{600.0, 20.0},
                             Vec2{0.0, 20.0}};
    for (int i = 0; i < 4; ++i) {
        ProjectedCurve curve;
        curve.shape = ProjectedLine{corners[i], corners[(i + 1) % 4]};
        drawing.curves.push_back(curve);
        GrowExtent(drawing.extent, curve);
    }
    return drawing;
}

struct Sheet {
    DrawingDocument document{"Bar"};
    ObjectId view = kInvalidObjectId;

    Sheet() {
        DrawingView& made = document.addView("Side", "bar.ep3d", "", ViewDirection::Front,
                                             Vec2{20.0, 100.0});
        made.setProjectionForTesting(Bar());
        view = made.id();
    }

    // A dimension from one end of the bar to the other, both anchors in the
    // view, on the corners the projection actually has.
    ObjectId lengthDimension() {
        DimensionAnchor left =
            DimensionAnchor::inView(view, Vec2{0.0, 0.0}, ViewPointRole::Corner);
        DimensionAnchor right =
            DimensionAnchor::inView(view, Vec2{600.0, 0.0}, ViewPointRole::Corner);
        return document
            .addDimension(DimensionKind::Linear, left, right, Vec2{100.0, 60.0})
            .id();
    }
};

TEST(BreakViewTest, M50_DOC_001_ADimensionAcrossTheBreakSTILLReadsTheWholeBar) {
    Sheet sheet;
    const ObjectId length = sheet.lengthDimension();
    const DrawingDimension* dimension = sheet.document.findDimension(length);
    ASSERT_NE(dimension, nullptr);

    const DimensionMeasurement before = sheet.document.measure(*dimension);
    ASSERT_TRUE(before.why.empty()) << before.why;
    EXPECT_NEAR(before.valueMm, 600.0, 1e-6);

    ASSERT_TRUE(sheet.document.setBreakSpan(sheet.view, 100.0, 500.0, true, 3.0));

    const DimensionMeasurement after = sheet.document.measure(*dimension);
    ASSERT_TRUE(after.why.empty()) << after.why;
    // THE WHOLE MILESTONE. The paper got shorter; the bar did not.
    EXPECT_NEAR(after.valueMm, 600.0, 1e-6)
        << "the dimension read the folded bar instead of the real one";
}

TEST(BreakViewTest, M50_DOC_002_ButThePAPERReallyDidGetShorter) {
    // The other half of the same claim, and the one that says the break is
    // doing anything at all. Without it, "the dimension still says 600" is
    // satisfied by a break that does nothing.
    Sheet sheet;
    const Vec2 rightBefore = sheet.document.viewPointToSheetMm(sheet.view, Vec2{600.0, 0.0});
    const Vec2 leftBefore = sheet.document.viewPointToSheetMm(sheet.view, Vec2{0.0, 0.0});
    EXPECT_NEAR(rightBefore.x - leftBefore.x, 600.0, 1e-6);

    ASSERT_TRUE(sheet.document.setBreakSpan(sheet.view, 100.0, 500.0, true, 3.0));
    const Vec2 rightAfter = sheet.document.viewPointToSheetMm(sheet.view, Vec2{600.0, 0.0});
    const Vec2 leftAfter = sheet.document.viewPointToSheetMm(sheet.view, Vec2{0.0, 0.0});
    EXPECT_NEAR(rightAfter.x - leftAfter.x, 203.0, 1e-6)
        << "the break removed nothing from the paper";
    // ...and the near end did not move at all.
    EXPECT_NEAR(leftAfter.x, leftBefore.x, 1e-9);
}

TEST(BreakViewTest, M50_DOC_003_TheMappingAndItsInverseAgreeOnThisDrawing) {
    // The fold is pinned as arithmetic in BreakFoldTests. What this pins is
    // that the DOCUMENT's two directions are the same mapping -- position,
    // scale and break, applied and undone in the same order. A scale applied
    // before the fold one way and after it the other would give a drawing
    // where short dimensions are right and long ones are not.
    Sheet sheet;
    ASSERT_TRUE(sheet.document.setViewScale(sheet.view, DrawingScale{1, 5}));
    ASSERT_TRUE(sheet.document.setBreakSpan(sheet.view, 100.0, 500.0, true, 3.0));
    for (const Vec2 at : {Vec2{0.0, 0.0}, Vec2{60.0, 20.0}, Vec2{600.0, 0.0},
                          Vec2{550.0, 12.5}}) {
        const Vec2 onPaper = sheet.document.viewPointToSheetMm(sheet.view, at);
        const Vec2 back = sheet.document.sheetPointToViewMm(sheet.view, onPaper);
        EXPECT_NEAR(back.x, at.x, 1e-6) << "x came back wrong from " << at.x;
        EXPECT_NEAR(back.y, at.y, 1e-6) << "y came back wrong from " << at.y;
    }
}

TEST(BreakViewTest, M50_DOC_004_ABreakOffThePartIsRefusedAgainstWhatIsDrawn) {
    Sheet sheet;
    // The bar reaches 0..600. A break past the end removes nothing and draws
    // its symbols across empty paper.
    EXPECT_FALSE(sheet.document.setBreakSpan(sheet.view, 700.0, 900.0, true, 3.0));
    EXPECT_FALSE(sheet.document.findView(sheet.view)->isBroken());
    // ...and one that swallows the whole bar leaves two ends and no middle.
    EXPECT_FALSE(sheet.document.setBreakSpan(sheet.view, -10.0, 610.0, true, 3.0));
    // A break along the WRONG AXIS is judged against that axis's reach: the
    // bar is only 20 tall, so 100..500 upright is off the part.
    EXPECT_FALSE(sheet.document.setBreakSpan(sheet.view, 100.0, 500.0, false, 3.0));
    // And the one that is over the part is taken.
    EXPECT_TRUE(sheet.document.setBreakSpan(sheet.view, 100.0, 500.0, true, 3.0));
    EXPECT_TRUE(sheet.document.findView(sheet.view)->isBroken());
}

TEST(BreakViewTest, M50_DOC_005_BreakingAndUnbreakingAreBothUndoable) {
    Sheet sheet;
    ASSERT_TRUE(sheet.document.setBreakSpan(sheet.view, 100.0, 500.0, true, 3.0));
    ASSERT_TRUE(sheet.document.findView(sheet.view)->isBroken());

    ASSERT_TRUE(sheet.document.undo());
    EXPECT_FALSE(sheet.document.findView(sheet.view)->isBroken())
        << "undo left the view broken";
    ASSERT_TRUE(sheet.document.redo());
    EXPECT_TRUE(sheet.document.findView(sheet.view)->isBroken());

    ASSERT_TRUE(sheet.document.clearBreakSpan(sheet.view));
    EXPECT_FALSE(sheet.document.findView(sheet.view)->isBroken());
    ASSERT_TRUE(sheet.document.undo());
    EXPECT_TRUE(sheet.document.findView(sheet.view)->isBroken())
        << "undoing an unbreak did not put the break back";
    EXPECT_NEAR(sheet.document.findView(sheet.view)->breakSpan().toMm, 500.0, 1e-9);
}

TEST(BreakViewTest, M50_DOC_006_TheSpanSurvivesTheFileAndTheLengthWithIt) {
    Sheet sheet;
    const ObjectId length = sheet.lengthDimension();
    ASSERT_TRUE(sheet.document.setBreakSpan(sheet.view, 100.0, 500.0, true, 3.0));

    std::ostringstream out;
    const SaveResult saved = saveDrawingDocument(sheet.document, out);
    ASSERT_EQ(saved.error, SerializationError::None) << saved.message;

    std::istringstream in(out.str());
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const DrawingView* back = loaded.document->findView(sheet.view);
    ASSERT_NE(back, nullptr);
    ASSERT_TRUE(back->isBroken()) << "a broken view came back showing the whole bar";
    EXPECT_NEAR(back->breakSpan().fromMm, 100.0, 1e-9);
    EXPECT_NEAR(back->breakSpan().toMm, 500.0, 1e-9);
    EXPECT_NEAR(back->breakSpan().gapMm, 3.0, 1e-9);
    EXPECT_TRUE(back->breakSpan().horizontal);
    // The dimension is still there and still dangling-free only once the view
    // is reprojected, so what is pinned here is the SPAN, which is what the
    // file is responsible for.
    EXPECT_NE(loaded.document->findDimension(length), nullptr);
}

TEST(BreakViewTest, M50_DOC_006B_TheCurvesADrawingHANDSOUTAreCutAtTheBreak) {
    // The cut is pinned as arithmetic in BreakFoldTests. What this pins is
    // that the DOCUMENT applies it -- a painter reading the raw projection
    // would draw the bar's edges straight across the gap, with the break
    // symbols sitting on top of a continuous line.
    Sheet sheet;
    const std::size_t whole = sheet.document.drawableCurves(sheet.view).size();
    EXPECT_EQ(whole, 4u);

    ASSERT_TRUE(sheet.document.setBreakSpan(sheet.view, 100.0, 500.0, true, 3.0));
    const std::vector<ProjectedCurve> cut = sheet.document.drawableCurves(sheet.view);
    // The two long edges are each in two pieces now; the two short ends are
    // untouched. Six, not four.
    EXPECT_EQ(cut.size(), 6u) << "the drawing handed out curves the break did not cut";
    for (const ProjectedCurve& curve : cut) {
        const auto* line = std::get_if<ProjectedLine>(&curve.shape);
        ASSERT_NE(line, nullptr);
        for (const Vec2 at : {line->a, line->b})
            EXPECT_TRUE(at.x <= 100.0 + 1e-6 || at.x >= 500.0 - 1e-6)
                << "a curve handed out reaches into what the break removed";
    }
}

TEST(BreakViewTest, M50_DOC_006C_TheSuggestedGapIsAboutSixMillimetresOfPAPER) {
    // The gap is a drafting artefact, not a feature of the part -- so what
    // matters is how wide it is on the PAPER. Left as a fixed model-space
    // number it is 0.6 mm at 1:10, which is a break nobody can see on exactly
    // the long parts breaks are for.
    Sheet sheet;
    ASSERT_TRUE(sheet.document.setViewScale(sheet.view, DrawingScale{1, 10}));
    const double gapModel = sheet.document.suggestedBreakGapMm(sheet.view);
    EXPECT_NEAR(gapModel * sheet.document.viewScaleFactor(sheet.view), 6.0, 1e-9)
        << "the suggested gap is not six millimetres of paper";
    EXPECT_GT(gapModel, 6.0) << "at 1:10 the model-space gap has to be bigger than the paper one";

    ASSERT_TRUE(sheet.document.setViewScale(sheet.view, DrawingScale{2, 1}));
    EXPECT_NEAR(sheet.document.suggestedBreakGapMm(sheet.view) *
                    sheet.document.viewScaleFactor(sheet.view),
                6.0, 1e-9);
}

TEST(BreakViewTest, M50_DOC_006D_AnUprightBreakSurvivesTheFileAsUpright) {
    // The axis is half of what a break MEANS. Written as always-across, a
    // reopened drawing shortens the part in the direction it was not broken
    // in -- which is a picture of a different part, drawn perfectly.
    Sheet sheet;
    // The bar is 20 tall, so an upright break has to live inside that.
    ASSERT_TRUE(sheet.document.setBreakSpan(sheet.view, 5.0, 15.0, false, 1.0));
    std::ostringstream out;
    ASSERT_EQ(saveDrawingDocument(sheet.document, out).error, SerializationError::None);

    std::istringstream in(out.str());
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const DrawingView* back = loaded.document->findView(sheet.view);
    ASSERT_NE(back, nullptr);
    ASSERT_TRUE(back->isBroken());
    EXPECT_FALSE(back->breakSpan().horizontal)
        << "an upright break came back across the part";
}

TEST(BreakViewTest, M50_DOC_006E_DeletingABrokenViewAndUndoingBringsTheBreakBack) {
    // A broken view restored without its span comes back showing the whole
    // three metres of bar -- which reads as a view somebody forgot to break
    // rather than as an edit undo lost.
    Sheet sheet;
    ASSERT_TRUE(sheet.document.setBreakSpan(sheet.view, 100.0, 500.0, true, 3.0));
    ASSERT_TRUE(sheet.document.removeObject(sheet.view));
    ASSERT_EQ(sheet.document.findView(sheet.view), nullptr);

    ASSERT_TRUE(sheet.document.undo());
    const DrawingView* back = sheet.document.findView(sheet.view);
    ASSERT_NE(back, nullptr) << "undo did not bring the view back at all";
    EXPECT_TRUE(back->isBroken()) << "the view came back showing the whole bar";
    EXPECT_NEAR(back->breakSpan().fromMm, 100.0, 1e-9);
    EXPECT_NEAR(back->breakSpan().toMm, 500.0, 1e-9);
}

TEST(BreakViewTest, M50_DOC_007_WhatTheSaverRefusesTheLoaderRefuses) {
    // ADR-M3-008. A hand-edited file whose break removes nothing draws the
    // symbols, says material was taken out, and takes none.
    Sheet sheet;
    ASSERT_TRUE(sheet.document.setBreakSpan(sheet.view, 100.0, 500.0, true, 3.0));
    std::ostringstream out;
    ASSERT_EQ(saveDrawingDocument(sheet.document, out).error, SerializationError::None);
    std::string text = out.str();

    const std::string::size_type at = text.find("\"toMm\": 500");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, std::string("\"toMm\": 500").size(), "\"toMm\": 100");

    std::istringstream in(text);
    EXPECT_FALSE(loadDrawingDocument(in)) << "a break that removes nothing loaded anyway";
}

TEST(BreakViewTest, M50_DOC_008_ASectionCanBeBrokenToo) {
    // A BREAK IS NOT A KIND OF VIEW. It is a way of putting a long part on a
    // short sheet, so it belongs to any view -- and a section of a long
    // extrusion is exactly the case that wants one.
    Sheet sheet;
    DrawingView& section = sheet.document.addSectionView("Cut", sheet.view, Vec2{50.0, -10.0},
                                                         Vec2{50.0, 30.0}, 1, 90.0);
    section.setProjectionForTesting(Bar());
    ASSERT_TRUE(sheet.document.setBreakSpan(section.id(), 100.0, 500.0, true, 3.0));
    EXPECT_TRUE(section.isBroken());
    EXPECT_TRUE(section.isSection()) << "breaking a section stopped it being one";
}

} // namespace
