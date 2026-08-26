// M49.1 -- cropping a projection to a circle.
//
// The headline is not "does it look right". Every failure below LOOKS right:
// the same shape lands on the paper and the same picture comes out of the
// plotter. What changes is whether the drafter can still dimension it.
//
//   * a hole that came back as a polygon -- a diameter dimension has nothing
//     to attach to, and a measured one reads 39.97 across the flats
//   * a trimmed rim still marked "full circle" -- draws and exports as the
//     whole ring, putting back the geometry the detail cropped away
//   * a curve dropped at the boundary -- the detail is missing exactly the
//     edge somebody zoomed in to look at

#include "Core/Drawing/DetailClip.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

ProjectedCurve Line(Vec2 a, Vec2 b) {
    ProjectedCurve curve;
    curve.shape = ProjectedLine{a, b};
    return curve;
}

ProjectedCurve Circle(Vec2 centre, double radius) {
    ProjectedArc arc;
    arc.centre = centre;
    arc.radius = radius;
    arc.startAngle = 0.0;
    arc.endAngle = 0.0;
    arc.isFullCircle = true;
    ProjectedCurve curve;
    curve.shape = arc;
    return curve;
}

TEST(DetailClipTest, M49_CLIP_001_AHoleInsideTheCircleIsSTILLAHole) {
    // THE RULE THIS FILE EXISTS FOR. Tessellating it would draw identically
    // and take the hole's identity with it -- and a diameter dimension needs
    // the identity, not the picture.
    const std::vector<ProjectedCurve> curves{Circle(Vec2{10.0, 10.0}, 4.0)};
    const std::vector<ProjectedCurve> kept =
        ClipToCircle(curves, Vec2{10.0, 10.0}, 20.0);
    ASSERT_EQ(kept.size(), 1u);
    const auto* arc = std::get_if<ProjectedArc>(&kept.front().shape);
    ASSERT_NE(arc, nullptr) << "a hole inside the detail came back as something else";
    EXPECT_TRUE(arc->isFullCircle);
    EXPECT_NEAR(arc->radius, 4.0, 1e-9);
    EXPECT_NEAR(arc->centre.x, 10.0, 1e-9);
}

TEST(DetailClipTest, M49_CLIP_002_ALineInsideIsUnchangedAndOutsideIsGone) {
    const std::vector<ProjectedCurve> curves{
        Line(Vec2{-1.0, 0.0}, Vec2{1.0, 0.0}),      // inside
        Line(Vec2{50.0, 50.0}, Vec2{60.0, 50.0})};  // nowhere near
    const std::vector<ProjectedCurve> kept = ClipToCircle(curves, Vec2{0.0, 0.0}, 5.0);
    ASSERT_EQ(kept.size(), 1u);
    const auto* line = std::get_if<ProjectedLine>(&kept.front().shape);
    ASSERT_NE(line, nullptr);
    // EXACTLY, not near enough -- a curve wholly inside comes back with the
    // numbers it went in with.
    //
    // AND THE MUTATION GATE SAYS THIS IS NOT WHY THE FAST PATH IS THERE.
    // Rebuilding the line from its own endpoints at t = 0 and t = 1 survives
    // every check here, and reading it through: it is arithmetically the same
    // line, and the ulp it could round the far end by is far below anything a
    // drawing, a DXF write or an M43 anchor tolerance can tell apart. So the
    // fast path is a COST decision, not a correctness one, and it is recorded
    // as an equivalent mutation rather than defended with a contrived number
    // whose last bit nobody would ever look at.
    //
    // What the crop must not do is change the SHAPE -- a line staying a line
    // and a hole staying a hole -- and that is what the assertions above and
    // in 001 are for.
    EXPECT_EQ(line->a.x, -1.0);
    EXPECT_EQ(line->b.x, 1.0);
    EXPECT_EQ(line->a.y, 0.0);
    EXPECT_EQ(line->b.y, 0.0);
}

TEST(DetailClipTest, M49_CLIP_003_ALineAcrossTheBoundaryIsTrimmedToIt) {
    // Along the x axis from -20 to 20, cropped to radius 5 about the origin:
    // what survives runs from -5 to 5, and it is still a line.
    const std::vector<ProjectedCurve> curves{Line(Vec2{-20.0, 0.0}, Vec2{20.0, 0.0})};
    const std::vector<ProjectedCurve> kept = ClipToCircle(curves, Vec2{0.0, 0.0}, 5.0);
    ASSERT_EQ(kept.size(), 1u);
    const auto* line = std::get_if<ProjectedLine>(&kept.front().shape);
    ASSERT_NE(line, nullptr) << "a trimmed line stopped being a line";
    EXPECT_NEAR(line->a.x, -5.0, 1e-6);
    EXPECT_NEAR(line->b.x, 5.0, 1e-6);
}

TEST(DetailClipTest, M49_CLIP_004_ATrimmedRimIsNoLongerAFullCircle) {
    // A hole half outside the detail: what is left is an arc. Left marked as a
    // full circle it would draw and export as the whole ring -- putting back
    // the very geometry the detail cropped away, and doing it invisibly
    // because a ring is what a hole looks like.
    const std::vector<ProjectedCurve> curves{Circle(Vec2{5.0, 0.0}, 4.0)};
    const std::vector<ProjectedCurve> kept = ClipToCircle(curves, Vec2{0.0, 0.0}, 5.0);
    ASSERT_FALSE(kept.empty()) << "a hole straddling the boundary vanished entirely";
    for (const ProjectedCurve& curve : kept) {
        const auto* arc = std::get_if<ProjectedArc>(&curve.shape);
        ASSERT_NE(arc, nullptr) << "a trimmed rim stopped being an arc";
        EXPECT_FALSE(arc->isFullCircle) << "a trimmed rim still claims to be a whole circle";
        EXPECT_NEAR(arc->radius, 4.0, 1e-9);
        // Every kept angle is genuinely inside.
        const double middle = 0.5 * (arc->startAngle + arc->endAngle);
        const Vec2 at{arc->centre.x + arc->radius * std::cos(middle),
                      arc->centre.y + arc->radius * std::sin(middle)};
        EXPECT_TRUE(InsideCircle(at, Vec2{0.0, 0.0}, 5.0 + 1e-6))
            << "a kept piece of the rim is outside the detail";
    }
}

TEST(DetailClipTest, M49_CLIP_005_ARimEntirelyOutsideIsDropped) {
    const std::vector<ProjectedCurve> curves{Circle(Vec2{100.0, 0.0}, 4.0)};
    EXPECT_TRUE(ClipToCircle(curves, Vec2{0.0, 0.0}, 5.0).empty());

    // ...and a rim that ENCLOSES the detail without touching it is dropped
    // too: none of the rim is inside, even though the detail is inside the
    // rim. Getting this backwards keeps a curve that never enters the circle.
    const std::vector<ProjectedCurve> big{Circle(Vec2{0.0, 0.0}, 50.0)};
    EXPECT_TRUE(ClipToCircle(big, Vec2{0.0, 0.0}, 5.0).empty());
}

TEST(DetailClipTest, M49_CLIP_005B_ARimINSIDEButOffCentreSurvives) {
    // FOUND BY THE MUTATION GATE, and it is not a nicety -- it is the ordinary
    // case. Every "inside" test above happened to be CONCENTRIC with the
    // detail, which takes a different branch. An off-centre hole comfortably
    // inside the circle goes through the general one, where the inequality's
    // cosine comes out above 1 and acos of that is NOT a number.
    //
    // Without the guard the hole does not draw wrong. It VANISHES -- and a
    // detail view whose whole purpose was to enlarge that hole comes back
    // empty of it, with everything else present and correct.
    const std::vector<ProjectedCurve> curves{Circle(Vec2{12.0, 10.0}, 2.0)};
    const std::vector<ProjectedCurve> kept = ClipToCircle(curves, Vec2{10.0, 10.0}, 8.0);
    ASSERT_EQ(kept.size(), 1u) << "an off-centre hole inside the detail disappeared";
    const auto* arc = std::get_if<ProjectedArc>(&kept.front().shape);
    ASSERT_NE(arc, nullptr);
    EXPECT_TRUE(arc->isFullCircle) << "a whole hole came back as a piece of one";
    EXPECT_NEAR(arc->radius, 2.0, 1e-9);
    EXPECT_NEAR(arc->centre.x, 12.0, 1e-9);
}

TEST(DetailClipTest, M49_CLIP_006_AConcentricRimInsideIsKeptWhole) {
    const std::vector<ProjectedCurve> curves{Circle(Vec2{0.0, 0.0}, 3.0)};
    const std::vector<ProjectedCurve> kept = ClipToCircle(curves, Vec2{0.0, 0.0}, 5.0);
    ASSERT_EQ(kept.size(), 1u);
    const auto* arc = std::get_if<ProjectedArc>(&kept.front().shape);
    ASSERT_NE(arc, nullptr);
    EXPECT_TRUE(arc->isFullCircle);
}

TEST(DetailClipTest, M49_CLIP_007_APolylineComesBackAsRunsNotAsFragments) {
    // A zigzag that leaves and re-enters. What survives is TWO runs, not one
    // curve per segment: every later question -- how many curves, which did
    // the user pick -- would otherwise answer differently for a detail than
    // for the view it was cropped from.
    ProjectedPolyline zigzag;
    zigzag.points = {Vec2{-4.0, 0.0}, Vec2{-1.0, 0.0}, Vec2{-1.0, 20.0}, Vec2{1.0, 20.0},
                     Vec2{1.0, 0.0},  Vec2{4.0, 0.0}};
    ProjectedCurve curve;
    curve.shape = zigzag;

    const std::vector<ProjectedCurve> kept = ClipToCircle({curve}, Vec2{0.0, 0.0}, 5.0);
    ASSERT_EQ(kept.size(), 2u) << "the crop fragmented a polyline";
    for (const ProjectedCurve& piece : kept) {
        const auto* points = std::get_if<ProjectedPolyline>(&piece.shape);
        ASSERT_NE(points, nullptr);
        ASSERT_GE(points->points.size(), 2u);
        for (const Vec2 at : points->points)
            EXPECT_TRUE(InsideCircle(at, Vec2{0.0, 0.0}, 5.0 + 1e-6))
                << "a kept polyline point is outside the detail";
    }
}

TEST(DetailClipTest, M49_CLIP_008_TheKindAndVisibilityRideAlong) {
    // A hidden edge cropped into a detail is still hidden. Dropped, the detail
    // draws a dashed line as solid -- which reads as material that is there.
    ProjectedCurve hidden = Line(Vec2{-20.0, 0.0}, Vec2{20.0, 0.0});
    hidden.kind = ProjectedEdgeKind::Outline;
    hidden.visibility = ProjectedVisibility::Hidden;
    const std::vector<ProjectedCurve> kept = ClipToCircle({hidden}, Vec2{0.0, 0.0}, 5.0);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept.front().kind, ProjectedEdgeKind::Outline);
    EXPECT_EQ(kept.front().visibility, ProjectedVisibility::Hidden);
}

TEST(DetailClipTest, M49_CLIP_009_ACircleOfNoSizeCropsToNothing) {
    const std::vector<ProjectedCurve> curves{Line(Vec2{-1.0, 0.0}, Vec2{1.0, 0.0})};
    EXPECT_TRUE(ClipToCircle(curves, Vec2{0.0, 0.0}, 0.0).empty());
    EXPECT_TRUE(ClipToCircle(curves, Vec2{0.0, 0.0}, -3.0).empty());
}

TEST(DetailClipTest, M49_CLIP_010_AnArcIsTrimmedAtItsOwnEndsAsWellAsTheCircles) {
    // A quarter arc that would be cropped further by the detail: the answer
    // has to respect BOTH limits. Taking only the circle's would hand back
    // rim the arc never had.
    ProjectedArc quarter;
    quarter.centre = Vec2{0.0, 0.0};
    quarter.radius = 4.0;
    quarter.startAngle = 0.0;
    quarter.endAngle = kPi / 2.0;
    quarter.isFullCircle = false;
    ProjectedCurve curve;
    curve.shape = quarter;

    // A detail centred out along +X: it keeps the part of the quarter near
    // angle 0 and drops the part near 90 degrees.
    const std::vector<ProjectedCurve> kept = ClipToCircle({curve}, Vec2{4.0, 0.0}, 2.0);
    ASSERT_FALSE(kept.empty());
    for (const ProjectedCurve& piece : kept) {
        const auto* arc = std::get_if<ProjectedArc>(&piece.shape);
        ASSERT_NE(arc, nullptr);
        EXPECT_GE(arc->startAngle, -1e-6) << "the crop handed back rim before the arc began";
        EXPECT_LE(arc->endAngle, kPi / 2.0 + 1e-6)
            << "the crop handed back rim past where the arc ended";
    }
}

} // namespace
