// M50.1 -- the fold, as arithmetic.
//
// A broken view exists so a 3 metre bar fits on an A3 sheet. What it must
// never do is make the bar 3 metres long on the paper and 1 metre long in the
// dimension -- and the way that happens is somebody removing material from the
// projection and then remembering, in one more place, to add it back.
//
// Nothing here removes anything. The fold maps model millimetres onto paper
// and UNFOLDS back, so a length measured through the inverse is the length the
// part actually is. These tests pin the pair.

#include "Core/Drawing/BreakFold.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace paramcad;

BreakSpan Middle(double from, double to, double gap = 3.0) {
    BreakSpan span;
    span.active = true;
    span.fromMm = from;
    span.toMm = to;
    span.gapMm = gap;
    span.horizontal = true;
    return span;
}

TEST(BreakFoldTest, M50_FOLD_001_TheFoldAndTheUnfoldAreEachOthers) {
    // THE PROPERTY THE WHOLE DESIGN RESTS ON. Everything that DRAWS goes
    // through the fold; everything that MEASURES goes through the unfold. If
    // the two are not inverses, a dimension across the break reads a number
    // that is not the part -- and every other number on the drawing agrees
    // with it.
    const BreakSpan span = Middle(100.0, 500.0);
    for (const double at : {0.0, 25.0, 99.0, 100.0, 500.0, 501.0, 550.0, 600.0}) {
        const double there = FoldAlongMm(at, span);
        const double back = UnfoldAlongMm(there, span);
        EXPECT_NEAR(back, at, 1e-9)
            << "a point at " << at << " folded to " << there << " and came back as " << back;
    }
}

TEST(BreakFoldTest, M50_FOLD_002_A600LongBarStillMeasures600) {
    // THE TEST THIS MILESTONE IS FOR. A 600 mm bar broken between 100 and 500
    // draws 200 long plus the gap; the dimension across it reads 600, because
    // the number is taken through the unfold and not off the paper.
    const BreakSpan span = Middle(100.0, 500.0);
    const double leftEnd = FoldAlongMm(0.0, span);
    const double rightEnd = FoldAlongMm(600.0, span);

    // What the paper shows: 100 of bar, a 3 mm gap, then 100 more.
    EXPECT_NEAR(rightEnd - leftEnd, 203.0, 1e-9);
    // What the part is, measured back through the inverse.
    EXPECT_NEAR(UnfoldAlongMm(rightEnd, span) - UnfoldAlongMm(leftEnd, span), 600.0, 1e-9);
    EXPECT_NEAR(RemovedMm(span), 400.0, 1e-9);
}

TEST(BreakFoldTest, M50_FOLD_003_NothingBelowTheBreakMoves) {
    const BreakSpan span = Middle(100.0, 500.0);
    EXPECT_NEAR(FoldAlongMm(0.0, span), 0.0, 1e-9);
    EXPECT_NEAR(FoldAlongMm(60.0, span), 60.0, 1e-9);
    EXPECT_NEAR(FoldAlongMm(100.0, span), 100.0, 1e-9);
    // ...and everything above it moves by exactly what was taken, less the gap.
    EXPECT_NEAR(FoldAlongMm(500.0, span), 103.0, 1e-9);
    EXPECT_NEAR(FoldAlongMm(600.0, span), 203.0, 1e-9);
}

TEST(BreakFoldTest, M50_FOLD_004_TheRemovedMiddleLandsOnTheSeam) {
    // Nothing is drawn there -- the curves that crossed were trimmed at the
    // lips -- so what matters is only that it does not fly off somewhere.
    const BreakSpan span = Middle(100.0, 500.0);
    EXPECT_NEAR(FoldAlongMm(300.0, span), 100.0, 1e-9);
    EXPECT_NEAR(FoldAlongMm(499.0, span), 100.0, 1e-9);
    // AND THE FOLD NEVER GOES BACKWARDS. A mapping that did would put the far
    // end of the bar to the left of the near one and draw it inside out.
    double previous = -1e18;
    for (double at = -50.0; at <= 700.0; at += 7.0) {
        const double there = FoldAlongMm(at, span);
        EXPECT_GE(there, previous - 1e-9) << "the fold went backwards at " << at;
        previous = there;
    }
}

TEST(BreakFoldTest, M50_FOLD_005_TheAxisIsTheSpansAndNotAGuess) {
    const BreakSpan across = Middle(100.0, 500.0);
    BreakSpan upright = Middle(100.0, 500.0);
    upright.horizontal = false;

    const Vec2 point{600.0, 600.0};
    // A break along X moves X and leaves Y; along Y it is the other way. Get
    // this backwards and the part is drawn shortened in the direction it was
    // not broken in -- which is a picture of a different part.
    EXPECT_NEAR(FoldPointMm(point, across).x, 203.0, 1e-9);
    EXPECT_NEAR(FoldPointMm(point, across).y, 600.0, 1e-9);
    EXPECT_NEAR(FoldPointMm(point, upright).x, 600.0, 1e-9);
    EXPECT_NEAR(FoldPointMm(point, upright).y, 203.0, 1e-9);

    EXPECT_NEAR(UnfoldPointMm(FoldPointMm(point, across), across).x, 600.0, 1e-9);
    EXPECT_NEAR(UnfoldPointMm(FoldPointMm(point, upright), upright).y, 600.0, 1e-9);
}

TEST(BreakFoldTest, M50_FOLD_006_AnInactiveOrUnusableBreakChangesNothing) {
    BreakSpan off;
    EXPECT_NEAR(FoldAlongMm(300.0, off), 300.0, 1e-9);
    EXPECT_NEAR(UnfoldAlongMm(300.0, off), 300.0, 1e-9);
    EXPECT_NEAR(RemovedMm(off), 0.0, 1e-9);

    // Active but backwards: it removes nothing, so it moves nothing. A fold
    // that quietly ran anyway would shift the part by a negative amount and
    // draw it LONGER than it is.
    BreakSpan backwards = Middle(500.0, 100.0);
    EXPECT_FALSE(backwards.usable());
    EXPECT_NEAR(FoldAlongMm(300.0, backwards), 300.0, 1e-9);
}

TEST(BreakFoldTest, M50_FOLD_006B_TheSeamUnFOLDSToTheNearLip) {
    // INSIDE THE GAP THE INVERSE IS NOT DEFINED -- the fold sent a whole span
    // of material onto one line, and no single answer can undo that. So the
    // contract is a CHOICE and it is written down here: a point in the gap
    // reads back as the near lip, which is the last place there was material.
    //
    // Left to fall through the arithmetic below it, a point 1.5 mm into the
    // gap comes back as 498.5 -- a confident answer about somewhere that was
    // cut away. Nothing anchors in a gap, so nothing breaks either way; what
    // is pinned is which answer this function gives, because "it does not
    // matter" is how a function ends up with two.
    const BreakSpan span = Middle(100.0, 500.0);
    EXPECT_NEAR(UnfoldAlongMm(101.5, span), 100.0, 1e-9);
    EXPECT_NEAR(UnfoldAlongMm(100.0 + 1e-6, span), 100.0, 1e-9);
    // ...and the moment the gap is past, the far half is back.
    EXPECT_NEAR(UnfoldAlongMm(103.0, span), 500.0, 1e-9);
}

TEST(BreakFoldTest, M50_FOLD_007_ABreakOffThePartIsRefused) {
    // It removes no material and draws its symbols across empty paper, which
    // reads as a part that continues off the sheet.
    EXPECT_FALSE(WhyBreakRefused(Middle(700.0, 900.0), 0.0, 600.0).empty());
    EXPECT_FALSE(WhyBreakRefused(Middle(-300.0, -100.0), 0.0, 600.0).empty());
    EXPECT_TRUE(WhyBreakRefused(Middle(100.0, 500.0), 0.0, 600.0).empty());

    // A BREAK THAT REMOVES NOTHING is not a break: it says material was taken
    // out and takes none.
    EXPECT_FALSE(WhyBreakRefused(Middle(200.0, 200.0), 0.0, 600.0).empty());
    EXPECT_FALSE(WhyBreakRefused(Middle(500.0, 100.0), 0.0, 600.0).empty());

    // ...AND ONE THAT SWALLOWS THE PART leaves two ends and no middle.
    EXPECT_FALSE(WhyBreakRefused(Middle(-10.0, 610.0), 0.0, 600.0).empty());

    // A negative gap is not a tighter drawing, it is the halves overlapping.
    EXPECT_FALSE(WhyBreakRefused(Middle(100.0, 500.0, -1.0), 0.0, 600.0).empty());

    // With nothing projected yet there is no extent to judge against, and the
    // shape rules still apply.
    EXPECT_TRUE(WhyBreakRefused(Middle(100.0, 500.0), 0.0, 0.0).empty());
    EXPECT_FALSE(WhyBreakRefused(Middle(500.0, 100.0), 0.0, 0.0).empty());
}

TEST(BreakFoldTest, M50_FOLD_008_AZeroGapIsAllowedAndSaysSo) {
    // Butted together the two halves read as one continuous part, which is why
    // the gap defaults to something visible -- but a drafter who wants none is
    // making a drafting choice, not a mistake.
    const BreakSpan tight = Middle(100.0, 500.0, 0.0);
    EXPECT_TRUE(WhyBreakRefused(tight, 0.0, 600.0).empty());
    EXPECT_NEAR(FoldAlongMm(600.0, tight), 200.0, 1e-9);
    EXPECT_NEAR(UnfoldAlongMm(FoldAlongMm(600.0, tight), tight), 600.0, 1e-9);
}


TEST(BreakFoldTest, M50_FOLD_009_ALineAcrossTheBreakIsCutAtTheLips) {
    // FOLDING THE ENDPOINTS IS NOT ENOUGH, and this is the failure that hides:
    // a line running the whole bar has both ends outside the break, so both
    // fold cleanly and it draws as ONE STRAIGHT LINE ACROSS THE GAP. The break
    // symbols sit on top of a continuous edge and the picture says the
    // material is still there.
    const BreakSpan span = Middle(100.0, 500.0);
    ProjectedCurve edge;
    edge.shape = ProjectedLine{Vec2{0.0, 0.0}, Vec2{600.0, 0.0}};

    const std::vector<ProjectedCurve> pieces = SplitAtBreak({edge}, span);
    ASSERT_EQ(pieces.size(), 2u) << "the edge was not cut at the break";
    for (const ProjectedCurve& piece : pieces) {
        const auto* line = std::get_if<ProjectedLine>(&piece.shape);
        ASSERT_NE(line, nullptr) << "a cut edge stopped being a line";
        for (const Vec2 at : {line->a, line->b})
            EXPECT_TRUE(at.x <= 100.0 + 1e-6 || at.x >= 500.0 - 1e-6)
                << "a kept end is inside the material the break removed: " << at.x;
    }
}

TEST(BreakFoldTest, M50_FOLD_010_WhatIsWhollyInsideIsDroppedAndOutsideIsKeptWhole) {
    const BreakSpan span = Middle(100.0, 500.0);
    ProjectedCurve buried;
    buried.shape = ProjectedLine{Vec2{200.0, 0.0}, Vec2{300.0, 0.0}};
    EXPECT_TRUE(SplitAtBreak({buried}, span).empty())
        << "an edge in the middle of what was removed survived the break";

    ProjectedCurve near;
    near.shape = ProjectedLine{Vec2{10.0, 0.0}, Vec2{90.0, 0.0}};
    const std::vector<ProjectedCurve> kept = SplitAtBreak({near}, span);
    ASSERT_EQ(kept.size(), 1u);
    const auto* line = std::get_if<ProjectedLine>(&kept.front().shape);
    ASSERT_NE(line, nullptr);
    EXPECT_EQ(line->a.x, 10.0);
    EXPECT_EQ(line->b.x, 90.0);

    // AND AN UNUSABLE SPAN CHANGES NOTHING, so a caller has one path.
    //
    // FOUND BY THE MUTATION GATE, and it is not academic. With the guard gone,
    // an inactive span still has from == to == 0, and the cutter treats zero
    // as a pair of lips: every curve crossing the ORIGIN comes back in two
    // pieces, on every unbroken view in the drawing. Nothing looks wrong --
    // two collinear halves draw exactly like one line -- until something
    // counts the curves or asks which one the user picked.
    BreakSpan off;
    EXPECT_EQ(SplitAtBreak({near}, off).size(), 1u);
    ProjectedCurve overTheOrigin;
    overTheOrigin.shape = ProjectedLine{Vec2{-10.0, 0.0}, Vec2{10.0, 0.0}};
    EXPECT_EQ(SplitAtBreak({overTheOrigin}, off).size(), 1u)
        << "an unbroken view cut a curve in half at the origin";
}

TEST(BreakFoldTest, M50_FOLD_010B_APartialArcIsCutWithinItsOwnRange) {
    // Every arc case elsewhere is a FULL circle, whose range is a whole turn
    // -- so clamping the kept pieces to the arc's own sweep never changed
    // anything and could be deleted unnoticed. On a partial arc it hands back
    // rim the arc never had: a fillet that becomes most of a circle, drawn as
    // material that is not on the part.
    const BreakSpan span = Middle(100.0, 500.0);
    ProjectedArc quarter;
    quarter.centre = Vec2{100.0, 0.0};
    quarter.radius = 40.0;
    quarter.startAngle = 0.0;
    quarter.endAngle = 3.14159265358979323846 / 2.0;   // 0 to 90 degrees
    quarter.isFullCircle = false;
    ProjectedCurve curve;
    curve.shape = quarter;

    const std::vector<ProjectedCurve> pieces = SplitAtBreak({curve}, span);
    for (const ProjectedCurve& piece : pieces) {
        const auto* arc = std::get_if<ProjectedArc>(&piece.shape);
        ASSERT_NE(arc, nullptr);
        EXPECT_GE(arc->startAngle, -1e-6)
            << "the cut handed back rim from before the arc began";
        EXPECT_LE(arc->endAngle, 3.14159265358979323846 / 2.0 + 1e-6)
            << "the cut handed back rim from past where the arc ended";
    }
}

TEST(BreakFoldTest, M50_FOLD_011_AHoleCutByABreakIsStillAnArc) {
    // The same rule the detail crop follows: a piece of a circle is an arc,
    // not a polygon, because whatever wants to dimension it needs the arc.
    const BreakSpan span = Middle(100.0, 500.0);
    ProjectedArc hole;
    hole.centre = Vec2{100.0, 10.0};
    hole.radius = 30.0;
    hole.isFullCircle = true;
    ProjectedCurve curve;
    curve.shape = hole;

    const std::vector<ProjectedCurve> pieces = SplitAtBreak({curve}, span);
    ASSERT_FALSE(pieces.empty()) << "a hole straddling the near lip vanished";
    for (const ProjectedCurve& piece : pieces) {
        const auto* arc = std::get_if<ProjectedArc>(&piece.shape);
        ASSERT_NE(arc, nullptr) << "a cut hole stopped being an arc";
        EXPECT_FALSE(arc->isFullCircle) << "a cut rim still claims to be a whole circle";
        const double middle = 0.5 * (arc->startAngle + arc->endAngle);
        const double x = arc->centre.x + arc->radius * std::cos(middle);
        EXPECT_TRUE(x <= 100.0 + 1e-6 || x >= 500.0 - 1e-6)
            << "a kept piece of the rim is inside what the break removed: " << x;
    }

    // THE FAR LIP CUTS TOO. Every case above straddles the NEAR one, so the
    // half of the arc rule that keeps material past the far lip was never
    // asked anything -- and dropping it leaves a broken view missing every
    // curve on the far side of the break, which on a symmetrical part looks
    // like a perfectly ordinary half.
    ProjectedArc farHole;
    farHole.centre = Vec2{500.0, 10.0};
    farHole.radius = 30.0;
    farHole.isFullCircle = true;
    ProjectedCurve farCurve;
    farCurve.shape = farHole;
    const std::vector<ProjectedCurve> farPieces = SplitAtBreak({farCurve}, span);
    ASSERT_FALSE(farPieces.empty()) << "a hole straddling the far lip vanished";
    for (const ProjectedCurve& piece : farPieces) {
        const auto* arc = std::get_if<ProjectedArc>(&piece.shape);
        ASSERT_NE(arc, nullptr);
        const double middle = 0.5 * (arc->startAngle + arc->endAngle);
        const double x = arc->centre.x + arc->radius * std::cos(middle);
        EXPECT_TRUE(x <= 100.0 + 1e-6 || x >= 500.0 - 1e-6)
            << "a kept piece past the far lip is inside what the break removed: " << x;
    }

    // A hole well clear of the break comes back untouched and still a circle.
    ProjectedArc clear;
    clear.centre = Vec2{40.0, 10.0};
    clear.radius = 5.0;
    clear.isFullCircle = true;
    ProjectedCurve away;
    away.shape = clear;
    const std::vector<ProjectedCurve> whole = SplitAtBreak({away}, span);
    ASSERT_EQ(whole.size(), 1u);
    const auto* back = std::get_if<ProjectedArc>(&whole.front().shape);
    ASSERT_NE(back, nullptr);
    EXPECT_TRUE(back->isFullCircle);
}

} // namespace
