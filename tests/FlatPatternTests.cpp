// M53.1 -- the blank, and the folds in the right places on it.
//
// THE FAILURE THIS IS FOR: a blank of exactly the right size with its bend
// lines three millimetres out. Everything measures correctly, the outline fits
// the material, the laser cuts it perfectly -- and the operator folds in the
// wrong place. There is nothing to look at, because the blank IS right.
//
// It cannot happen here because the running total that places each fold is the
// same running total that ends as the length. Not two sums that agree; one sum.

#include "Core/Drawing/FlatPattern.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace paramcad;

ContourStep Step(double flangeMm, double angleDeg, double radiusMm, bool left = true) {
    ContourStep step;
    step.flangeMm = flangeMm;
    step.bend.angleDeg = angleDeg;
    step.bend.innerRadiusMm = radiusMm;
    step.turnsLeft = left;
    return step;
}

SheetContour Channel() {
    SheetContour contour;
    contour.steps.push_back(Step(30.0, 90.0, 2.0, true));
    contour.steps.push_back(Step(60.0, 90.0, 2.0, true));
    contour.lastFlangeMm = 30.0;
    return contour;
}

TEST(FlatPatternTest, M53_FLAT_001_TheFoldsAndTheLengthComeFromONESum) {
    // The first fold starts at the first flange; the second starts at the
    // first flange plus the first allowance plus the second flange; and the
    // blank ends at the last fold plus the last flange. Every one of those is
    // the same running total at a different moment.
    const double t = 2.0;
    const SheetMaterial steel = SheetMaterial::MildSteelAluminium;
    const FlatPatternResultGeometry flat = FlatPatternOf(Channel(), steel, t, 100.0);
    ASSERT_TRUE(flat.ok) << flat.why;
    ASSERT_EQ(flat.bendLines.size(), 2u);

    const double k = KFactorFor(steel, 2.0, t);
    SheetBend right;
    right.angleDeg = 90.0;
    right.innerRadiusMm = 2.0;
    const double allowance = BendAllowanceMm(right, t, k);

    EXPECT_NEAR(flat.bendLines[0].fromMm, 30.0, 1e-9);
    EXPECT_NEAR(flat.bendLines[0].toMm, 30.0 + allowance, 1e-9);
    EXPECT_NEAR(flat.bendLines[1].fromMm, 30.0 + allowance + 60.0, 1e-9);
    EXPECT_NEAR(flat.bendLines[1].toMm, 30.0 + allowance + 60.0 + allowance, 1e-9);
    EXPECT_NEAR(flat.lengthMm, 30.0 + allowance + 60.0 + allowance + 30.0, 1e-9);

    // ...AND THE LENGTH IS M51's ANSWER, not a second one that happens to
    // match. If these two ever part, the blank and the folds are describing
    // different parts.
    const FlatPatternResult fromM51 = ContourFlatLength(Channel(), steel, t);
    ASSERT_TRUE(fromM51.ok) << fromM51.why;
    EXPECT_EQ(flat.lengthMm, fromM51.lengthMm);
}

TEST(FlatPatternTest, M53_FLAT_002_ABendIsABANDAndNotALine) {
    // A bend consumes its allowance -- four and a half millimetres on a right
    // angle at R2 in 2 mm plate. Drawn as one line in the middle, the operator
    // has to decide which edge of it the press meets, and that is most of a
    // millimetre on a tight radius.
    const double t = 2.0;
    const FlatPatternResultGeometry flat =
        FlatPatternOf(Channel(), SheetMaterial::MildSteelAluminium, t, 100.0);
    ASSERT_TRUE(flat.ok) << flat.why;
    for (const BendLine& bend : flat.bendLines) {
        EXPECT_GT(bend.toMm - bend.fromMm, 1.0)
            << "the bend band has no width, so it is a line and the press has nowhere to sit";
        EXPECT_NEAR(bend.middleMm(), 0.5 * (bend.fromMm + bend.toMm), 1e-12);
    }
}

TEST(FlatPatternTest, M53_FLAT_003_EachFoldSaysWhichWayAndHowFar) {
    // A flat pattern with unmarked folds is one the operator guesses at, and
    // the guess is fifty-fifty per bend. Two right angles the same way is a
    // channel; one each way is a Z; the blank is identical.
    const double t = 2.0;
    SheetContour zed;
    zed.steps.push_back(Step(30.0, 90.0, 2.0, true));
    zed.steps.push_back(Step(60.0, 90.0, 2.0, false));
    zed.lastFlangeMm = 30.0;

    const SheetMaterial steel = SheetMaterial::MildSteelAluminium;
    const FlatPatternResultGeometry channel = FlatPatternOf(Channel(), steel, t, 100.0);
    const FlatPatternResultGeometry shape = FlatPatternOf(zed, steel, t, 100.0);
    ASSERT_TRUE(channel.ok) << channel.why;
    ASSERT_TRUE(shape.ok) << shape.why;

    // THE BLANKS ARE THE SAME, exactly -- which is the reason the direction
    // has to be on the pattern at all.
    EXPECT_EQ(channel.lengthMm, shape.lengthMm);
    EXPECT_EQ(channel.bendLines[0].fromMm, shape.bendLines[0].fromMm);
    EXPECT_EQ(channel.bendLines[1].fromMm, shape.bendLines[1].fromMm);
    // ...and the second fold goes the other way.
    EXPECT_TRUE(channel.bendLines[1].turnsLeft);
    EXPECT_FALSE(shape.bendLines[1].turnsLeft);
    EXPECT_NEAR(shape.bendLines[1].angleDeg, 90.0, 1e-9);
    EXPECT_NEAR(shape.bendLines[1].innerRadiusMm, 2.0, 1e-9);
}

TEST(FlatPatternTest, M53_FLAT_003B_AFoldThatIsNotARightAngleSaysSo) {
    // FOUND BY THE MUTATION GATE: every fold in the tests above happened to be
    // ninety degrees, so an angle hard-coded to ninety passed all of them. A
    // press brake set to the wrong angle makes a part that is the right length
    // and the wrong shape.
    const double t = 2.0;
    SheetContour hopper;
    hopper.steps.push_back(Step(40.0, 30.0, 2.0, true));
    hopper.steps.push_back(Step(80.0, 120.0, 2.0, true));
    hopper.lastFlangeMm = 40.0;

    const FlatPatternResultGeometry flat =
        FlatPatternOf(hopper, SheetMaterial::MildSteelAluminium, t, 100.0);
    ASSERT_TRUE(flat.ok) << flat.why;
    ASSERT_EQ(flat.bendLines.size(), 2u);
    EXPECT_NEAR(flat.bendLines[0].angleDeg, 30.0, 1e-9);
    EXPECT_NEAR(flat.bendLines[1].angleDeg, 120.0, 1e-9);
    // ...and the shallower fold eats less metal, which is the other half of
    // the same fact: the band's width IS the allowance.
    EXPECT_LT(flat.bendLines[0].toMm - flat.bendLines[0].fromMm,
              flat.bendLines[1].toMm - flat.bendLines[1].fromMm);
}

TEST(FlatPatternTest, M53_FLAT_004_APartThisProgramWillNotFoldGetsNoBlank) {
    // A blank for a part nothing can bend would be cut, and then nothing would
    // bend it. The refusals are the fold's own, asked of the same function.
    const double t = 2.0;
    SheetContour tight = Channel();
    tight.steps[0].bend.innerRadiusMm = 0.5;   // mild steel at 2 mm needs 2
    const FlatPatternResultGeometry cracked =
        FlatPatternOf(tight, SheetMaterial::MildSteelAluminium, t, 100.0);
    EXPECT_FALSE(cracked.ok) << "a blank was handed out for a bend that cracks";
    EXPECT_NE(cracked.why.find("cracks"), std::string::npos) << cracked.why;

    // And a blank with no width is not a blank.
    EXPECT_FALSE(FlatPatternOf(Channel(), SheetMaterial::MildSteelAluminium, t, 0.0).ok);
}

TEST(FlatPatternTest, M53_FLAT_005_TheDrawingIsOrdinaryCurvesAndTheFoldsAreNotEdges) {
    // The outline and the folds are both ProjectedCurves, so dimensions,
    // in-view anchors and break views work on a flat pattern exactly as they
    // do on any other view. A flat pattern whose folds were an annotation
    // layer of its own would be a view whose most important content nothing
    // else on the drawing could measure to.
    const double t = 2.0;
    const FlatPatternResultGeometry flat =
        FlatPatternOf(Channel(), SheetMaterial::MildSteelAluminium, t, 100.0);
    ASSERT_TRUE(flat.ok) << flat.why;
    const ProjectedDrawing drawing = FlatPatternDrawing(flat);

    // Four sides, and two lines per bend band.
    EXPECT_EQ(drawing.curves.size(), 4u + flat.bendLines.size() * 2u);

    // THE OUTLINE CLOSES. Checking only the extent misses a corner that does
    // not meet: the reach is still right, and the cut path is a shape with a
    // gap in it -- which a laser follows exactly as drawn.
    for (std::size_t i = 0; i < 4; ++i) {
        const auto* here = std::get_if<ProjectedLine>(&drawing.curves[i].shape);
        const auto* next = std::get_if<ProjectedLine>(&drawing.curves[(i + 1) % 4].shape);
        ASSERT_NE(here, nullptr);
        ASSERT_NE(next, nullptr);
        EXPECT_NEAR(here->b.x, next->a.x, 1e-9) << "the cut path has a gap at corner " << i;
        EXPECT_NEAR(here->b.y, next->a.y, 1e-9) << "the cut path has a gap at corner " << i;
    }

    int sharp = 0;
    int smooth = 0;
    for (const ProjectedCurve& curve : drawing.curves) {
        EXPECT_NE(std::get_if<ProjectedLine>(&curve.shape), nullptr);
        if (curve.kind == ProjectedEdgeKind::Sharp) ++sharp;
        if (curve.kind == ProjectedEdgeKind::Smooth) ++smooth;
    }
    EXPECT_EQ(sharp, 4) << "the cut path is not four sharp edges";
    // THE FOLDS ARE NOT EDGES OF THE PART. Drawn sharp they read as a cut, and
    // a flat pattern is a thing that goes to a laser.
    EXPECT_EQ(smooth, static_cast<int>(flat.bendLines.size()) * 2);

    // The extent is the blank, which is what a sheet asks when it works out
    // whether the view fits the paper.
    EXPECT_NEAR(drawing.extent.widthMm(), flat.lengthMm, 1e-9);
    EXPECT_NEAR(drawing.extent.heightMm(), flat.widthMm, 1e-9);
}

TEST(FlatPatternTest, M53_FLAT_005B_ARefusedBlankDrawsNOTHING) {
    // A blank that was refused has no length and no width, and drawing it
    // anyway gives four lines of nothing at the origin -- an outline a reader
    // would take for a very small part rather than for an answer that was
    // never given.
    const double t = 2.0;
    SheetContour tight = Channel();
    tight.steps[0].bend.innerRadiusMm = 0.5;
    const FlatPatternResultGeometry refused =
        FlatPatternOf(tight, SheetMaterial::MildSteelAluminium, t, 100.0);
    ASSERT_FALSE(refused.ok);
    EXPECT_TRUE(FlatPatternDrawing(refused).curves.empty())
        << "a blank that was refused was drawn anyway";
}

TEST(FlatPatternTest, M53_FLAT_006_AFlatStripIsItsOwnBlank) {
    const double t = 2.0;
    SheetContour strip;
    strip.lastFlangeMm = 250.0;
    const FlatPatternResultGeometry flat =
        FlatPatternOf(strip, SheetMaterial::MildSteelAluminium, t, 40.0);
    ASSERT_TRUE(flat.ok) << flat.why;
    EXPECT_NEAR(flat.lengthMm, 250.0, 1e-9);
    EXPECT_TRUE(flat.bendLines.empty());
    EXPECT_EQ(FlatPatternDrawing(flat).curves.size(), 4u);
}

TEST(FlatPatternTest, M53_FLAT_007_TheBlankIsLongerThanTheFoldedPartIsWide) {
    // The sanity a shop applies without thinking: whatever the folds do, the
    // metal that went in is longer than any single side of what comes out.
    // A blank shorter than the sum of the flanges would mean the bends had
    // given material back.
    const double t = 2.0;
    const SheetContour channel = Channel();
    const FlatPatternResultGeometry flat =
        FlatPatternOf(channel, SheetMaterial::MildSteelAluminium, t, 100.0);
    ASSERT_TRUE(flat.ok) << flat.why;

    double flanges = channel.lastFlangeMm;
    for (const ContourStep& step : channel.steps) flanges += step.flangeMm;
    EXPECT_GT(flat.lengthMm, flanges) << "the bends gave material back";
    // ...and not by much: two right angles at R2 in 2 mm spend about nine
    // millimetres between them.
    EXPECT_LT(flat.lengthMm - flanges, 12.0);
}

} // namespace
