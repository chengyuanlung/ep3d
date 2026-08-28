// M55.1 -- measuring, which is dimensioning without keeping it.
//
// A measure tool that had its own idea of where a point is would answer one
// number while a dimension on THE SAME TWO POINTS answered another -- and the
// one a user trusts is whichever they looked at last. So there is one
// measurement in this program and two callers of it.
//
// The failures that would otherwise hide:
//
//   * measuring on a view at 1:2 and reporting PAPER millimetres -- 40 for a
//     part that is 80, and 40 is a perfectly ordinary number
//   * measuring across a BREAK and reporting the folded length -- M50's
//     guarantee, which has to hold for the tool as well as the dimension
//   * measuring two solids and reporting "0.0 mm apart" for a gap this
//     program cannot see at all

#include "Core/Drawing/DrawingDocument.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace {

using namespace paramcad;

// A 600 x 20 bar, put in by hand -- no kernel needed.
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

    DimensionAnchor Corner(Vec2 at) const {
        return DimensionAnchor::inView(view, at, ViewPointRole::Corner);
    }
};

TEST(MeasureToolTest, M55_MEASURE_001_MeasuringAndDimensioningAgreeBECAUSETheyAreOneThing) {
    Sheet sheet;
    const DimensionAnchor left = sheet.Corner(Vec2{0.0, 0.0});
    const DimensionAnchor right = sheet.Corner(Vec2{600.0, 0.0});

    const DimensionMeasurement measured = sheet.document.measureBetween(left, right);
    ASSERT_TRUE(measured.ok) << measured.why;
    EXPECT_NEAR(measured.valueMm, 600.0, 1e-6);

    // The SAME two points, as a dimension that stays on the paper.
    const ObjectId id =
        sheet.document.addDimension(DimensionKind::Linear, left, right, Vec2{100.0, 60.0})
            .id();
    const DrawingDimension* dimension = sheet.document.findDimension(id);
    ASSERT_NE(dimension, nullptr);
    const DimensionMeasurement kept = sheet.document.measure(*dimension);
    ASSERT_TRUE(kept.ok) << kept.why;

    // NOT "close" -- the same number, because it came out of the same
    // function. Two implementations would agree today and part the day either
    // learned something.
    EXPECT_EQ(measured.valueMm, kept.valueMm);
}

TEST(MeasureToolTest, M55_MEASURE_002_TheAnswerIsTheSizeOfThePartAndNotOfThePaper) {
    // A view at 1:2 draws a 600 bar 300 long. A measure tool that read the
    // sheet would say 300 -- a perfectly ordinary number for a part that is
    // twice it.
    Sheet sheet;
    ASSERT_TRUE(sheet.document.setViewScale(sheet.view, DrawingScale{1, 2}));
    const DimensionMeasurement measured =
        sheet.document.measureBetween(sheet.Corner(Vec2{0.0, 0.0}),
                                      sheet.Corner(Vec2{600.0, 0.0}));
    ASSERT_TRUE(measured.ok) << measured.why;
    EXPECT_NEAR(measured.valueMm, 600.0, 1e-6)
        << "the measure tool reported paper millimetres";

    // ...and the ENDS it hands back are in sheet millimetres, because that is
    // where they are drawn. Both are needed and they are different spaces,
    // which is why they are named differently.
    EXPECT_NEAR(measured.secondMm.x - measured.firstMm.x, 300.0, 1e-6);
}

TEST(MeasureToolTest, M55_MEASURE_003_MeasuringAcrossABreakReadsTheWholeBar) {
    // M50's guarantee, applied to the tool. The paper got shorter; the bar did
    // not -- and a measure tool that answered off the paper would report the
    // folded length, which every other number on the drawing would agree with.
    Sheet sheet;
    ASSERT_TRUE(sheet.document.setBreakSpan(sheet.view, 100.0, 500.0, true, 3.0));
    const DimensionMeasurement measured =
        sheet.document.measureBetween(sheet.Corner(Vec2{0.0, 0.0}),
                                      sheet.Corner(Vec2{600.0, 0.0}));
    ASSERT_TRUE(measured.ok) << measured.why;
    EXPECT_NEAR(measured.valueMm, 600.0, 1e-6)
        << "the measure tool read the folded bar instead of the real one";
    // ...while the two ends really are 203 apart on the paper.
    EXPECT_NEAR(measured.secondMm.x - measured.firstMm.x, 203.0, 1e-6);
}

TEST(MeasureToolTest, M55_MEASURE_004_HorizontalAndVerticalAreDifferentQuestions) {
    // The aligned distance between two corners of a bar is not its length, and
    // a tool that only ever answered one of them would be answering a question
    // the user did not ask.
    Sheet sheet;
    const DimensionAnchor a = sheet.Corner(Vec2{0.0, 0.0});
    const DimensionAnchor b = sheet.Corner(Vec2{600.0, 20.0});

    const DimensionMeasurement aligned =
        sheet.document.measureBetween(a, b, DimensionKind::Linear, LinearDirection::Aligned);
    const DimensionMeasurement across =
        sheet.document.measureBetween(a, b, DimensionKind::Linear, LinearDirection::Horizontal);
    const DimensionMeasurement up =
        sheet.document.measureBetween(a, b, DimensionKind::Linear, LinearDirection::Vertical);
    ASSERT_TRUE(aligned.ok) << aligned.why;
    ASSERT_TRUE(across.ok);
    ASSERT_TRUE(up.ok);

    EXPECT_NEAR(across.valueMm, 600.0, 1e-6);
    EXPECT_NEAR(up.valueMm, 20.0, 1e-6);
    EXPECT_NEAR(aligned.valueMm, std::hypot(600.0, 20.0), 1e-6);
    EXPECT_GT(aligned.valueMm, across.valueMm);
}

TEST(MeasureToolTest, M55_MEASURE_004B_MeasuringFromTheSheetToAPointInAViewStillScales) {
    // FOUND BY THE MUTATION GATE: every measurement above had BOTH ends in the
    // same view, so reading the view off the first anchor alone passed all of
    // them. Measuring from a point on the paper to a point in a view is an
    // ordinary thing to do -- a clearance to the frame, a note position -- and
    // with the view taken from the wrong end it is measured at 1:1 on a sheet
    // drawn at 1:2.
    Sheet sheet;
    ASSERT_TRUE(sheet.document.setViewScale(sheet.view, DrawingScale{1, 2}));

    // The far corner of the bar, and a free point on the paper 100 sheet
    // millimetres to the right of it.
    const Vec2 corner = sheet.document.viewPointToSheetMm(sheet.view, Vec2{600.0, 0.0});
    DimensionAnchor free;
    free.kind = DimensionAnchorKind::Free;
    free.at = Vec2{corner.x + 100.0, corner.y};

    const DimensionMeasurement measured =
        sheet.document.measureBetween(free, sheet.Corner(Vec2{600.0, 0.0}));
    ASSERT_TRUE(measured.ok) << measured.why;
    // 100 mm of PAPER at 1:2 is 200 mm of part, and that is the number a
    // drafter is asking for.
    EXPECT_NEAR(measured.valueMm, 200.0, 1e-6)
        << "the scale was taken from the wrong end of the measurement";
}

TEST(MeasureToolTest, M55_MEASURE_005_APointThatIsGoneIsSaidRatherThanGuessed) {
    // A measurement to something that has moved out of reach dangles loudly,
    // exactly as a dimension does -- because it IS the same resolution.
    Sheet sheet;
    const DimensionMeasurement measured =
        sheet.document.measureBetween(sheet.Corner(Vec2{0.0, 0.0}),
                                      sheet.Corner(Vec2{9999.0, 9999.0}));
    EXPECT_FALSE(measured.ok) << "a measurement to nowhere came back with a number";
    EXPECT_FALSE(measured.why.empty());
}

TEST(MeasureToolTest, M55_MEASURE_006_AnAngleIsMeasuredAboutAPointTheUserPICKED) {
    // An angle between two points has no vertex of its own, so one is asked
    // for. Defaulting to the origin would answer about a corner nobody chose,
    // and the number would be a perfectly ordinary angle.
    Sheet sheet;
    const DimensionAnchor a = sheet.Corner(Vec2{600.0, 0.0});
    const DimensionAnchor b = sheet.Corner(Vec2{0.0, 20.0});
    const Vec2 corner = sheet.document.viewPointToSheetMm(sheet.view, Vec2{0.0, 0.0});

    const DimensionMeasurement here =
        sheet.document.measureBetween(a, b, DimensionKind::Angular, LinearDirection::Aligned,
                                      corner);
    ASSERT_TRUE(here.ok) << here.why;
    EXPECT_NEAR(here.valueMm, 90.0, 1e-6);

    // About a different point it is a different angle -- which is the point.
    //
    // NOT the far corner: on a rectangle that happens to be ninety degrees as
    // well, so the first draft of this test passed whatever the vertex did.
    // Halfway along the bottom edge is nothing like a right angle.
    const Vec2 far = sheet.document.viewPointToSheetMm(sheet.view, Vec2{300.0, 0.0});
    const DimensionMeasurement there =
        sheet.document.measureBetween(a, b, DimensionKind::Angular, LinearDirection::Aligned,
                                      far);
    ASSERT_TRUE(there.ok) << there.why;
    EXPECT_GT(std::fabs(there.valueMm - here.valueMm), 1.0);
    EXPECT_NEAR(there.valueMm, 176.19, 0.05);
}

} // namespace
