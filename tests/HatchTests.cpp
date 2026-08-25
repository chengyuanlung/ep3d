// M38.2 -- hatching.
//
// The parallel lines that say where the knife went. Two things must be right,
// and both fail invisibly:
//
//   * A HOLE MUST BE A HOLE. Hatch that runs across a bore is a section
//     drawing showing material where there is none.
//   * A VERTEX ON A SCANLINE MUST COUNT ONCE. Counted twice it flips the
//     parity and the fill inverts from that line on, which on paper looks like
//     the hatch leaking out of the part.

#include "Core/Drawing/Hatch.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace {

using namespace paramcad;

std::vector<Vec2> Rectangle(double x0, double y0, double x1, double y1) {
    return {Vec2{x0, y0}, Vec2{x1, y0}, Vec2{x1, y1}, Vec2{x0, y1}};
}

// A regular polygon, for the loops a real projection produces -- which are
// flattened curves and never axis-aligned.
std::vector<Vec2> Circle(Vec2 centre, double radius, int sides) {
    std::vector<Vec2> points;
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    for (int i = 0; i < sides; ++i) {
        const double angle = kTwoPi * static_cast<double>(i) / static_cast<double>(sides);
        points.push_back(Vec2{centre.x + radius * std::cos(angle),
                              centre.y + radius * std::sin(angle)});
    }
    return points;
}

double TotalLength(const HatchLines& lines) {
    double total = 0.0;
    for (const auto& segment : lines.segments)
        total += std::hypot(segment.second.x - segment.first.x,
                            segment.second.y - segment.first.y);
    return total;
}

// Is `point` strictly inside the segments' union along its own scanline? Used
// to ask "did any hatch land here", which is the question a hole poses.
bool AnySegmentNear(const HatchLines& lines, Vec2 point, double within) {
    for (const auto& segment : lines.segments)
        if (DistancePointToSegment(point, segment.first, segment.second) <= within)
            return true;
    return false;
}

} // namespace

TEST(HatchTest, M38_HATCH_001_ASquareIsFilledAndTheLinesStayINSIDEIt) {
    HatchRegion region;
    region.add(Rectangle(0.0, 0.0, 40.0, 40.0));
    HatchStyle style;
    style.spacingMm = 4.0;

    const HatchLines lines = HatchTheRegion(region, style);
    ASSERT_TRUE(lines.ok) << lines.why;
    EXPECT_FALSE(lines.segments.empty()) << "a 40 mm square came out with no hatch";
    // NOT ONE LINE PAST THE EDGE. Hatch outside the part is the failure a
    // reader sees as the section being wrong, and it is what an even-odd bug
    // produces.
    for (const auto& segment : lines.segments) {
        for (const Vec2 end : {segment.first, segment.second}) {
            EXPECT_GE(end.x, -1e-6) << "hatch ran outside the square";
            EXPECT_LE(end.x, 40.0 + 1e-6);
            EXPECT_GE(end.y, -1e-6);
            EXPECT_LE(end.y, 40.0 + 1e-6);
        }
    }
}

TEST(HatchTest, M38_HATCH_002_AHOLEIsNotHatched) {
    // THE ONE THAT MATTERS MOST. Hatch across a bore is a section drawing
    // showing material where there is none -- and it is exactly what the
    // even-odd rule exists to prevent.
    HatchRegion region;
    region.add(Rectangle(0.0, 0.0, 60.0, 60.0));
    region.add(Circle(Vec2{30.0, 30.0}, 12.0, 64));
    HatchStyle style;
    style.spacingMm = 2.0;

    const HatchLines lines = HatchTheRegion(region, style);
    ASSERT_TRUE(lines.ok) << lines.why;
    ASSERT_FALSE(lines.segments.empty());
    // The middle of the bore is 12 mm from its wall, so nothing may come
    // within 10 of the centre.
    EXPECT_FALSE(AnySegmentNear(lines, Vec2{30.0, 30.0}, 10.0))
        << "the hatch ran across the hole";
    // ...and the solid part IS filled, so this is not passing by hatching
    // nothing at all.
    EXPECT_TRUE(AnySegmentNear(lines, Vec2{5.0, 30.0}, 2.5))
        << "the material beside the hole was left blank";
}

TEST(HatchTest, M38_HATCH_003_AVertexONAScanlineDoesNotINVERTTheFill) {
    // A diamond has its left and right vertices exactly on one horizontal
    // line. Counted from both edges, the parity flips twice there and every
    // scanline above it fills the OUTSIDE instead of the inside.
    //
    // Hatching at zero degrees puts the scanlines horizontal, so the vertex
    // lands on one exactly -- which is the case the half-open edge test exists
    // for and the reason this test hatches flat rather than at 45.
    HatchRegion region;
    region.add({Vec2{20.0, 0.0}, Vec2{40.0, 20.0}, Vec2{20.0, 40.0}, Vec2{0.0, 20.0}});
    HatchStyle style;
    style.angleRad = 0.0;
    style.spacingMm = 5.0; // 20.0 is a multiple, so a scanline hits the vertices
    style.offsetMm = 0.0;

    const HatchLines lines = HatchTheRegion(region, style);
    ASSERT_TRUE(lines.ok) << lines.why;
    for (const auto& segment : lines.segments) {
        // Every segment must be inside the diamond: |x-20| + |y-20| <= 20.
        for (const Vec2 end : {segment.first, segment.second}) {
            const double distance = std::fabs(end.x - 20.0) + std::fabs(end.y - 20.0);
            EXPECT_LE(distance, 20.0 + 1e-6)
                << "the fill inverted at the vertex and hatched outside the diamond";
        }
    }
    // ...and the middle IS hatched, so the check above is not passing on an
    // empty result.
    EXPECT_TRUE(AnySegmentNear(lines, Vec2{20.0, 20.0}, 3.0));
}

TEST(HatchTest, M38_HATCH_004_TheSPACINGIsWhatWasAskedFor) {
    // Halving the spacing doubles the ink. A hatch whose pitch drifted with
    // the region's size would fill a small part solid and leave a large one
    // looking empty.
    HatchRegion region;
    region.add(Rectangle(0.0, 0.0, 40.0, 40.0));
    HatchStyle wide;
    wide.spacingMm = 4.0;
    HatchStyle narrow;
    narrow.spacingMm = 2.0;

    const double wideLength = TotalLength(HatchTheRegion(region, wide));
    const double narrowLength = TotalLength(HatchTheRegion(region, narrow));
    EXPECT_GT(wideLength, 0.0);
    EXPECT_NEAR(narrowLength / wideLength, 2.0, 0.15)
        << "halving the spacing did not double the hatch";
}

TEST(HatchTest, M38_HATCH_005_TheANGLEIsWhatWasAskedFor) {
    // Adjacent parts in an assembly section are told apart by their hatch
    // angle, so the angle has to be the one asked for and not a house default.
    HatchRegion region;
    region.add(Rectangle(0.0, 0.0, 40.0, 40.0));
    HatchStyle flat;
    flat.angleRad = 0.0;
    flat.spacingMm = 5.0;

    const HatchLines lines = HatchTheRegion(region, flat);
    ASSERT_TRUE(lines.ok) << lines.why;
    ASSERT_FALSE(lines.segments.empty());
    for (const auto& segment : lines.segments)
        EXPECT_NEAR(segment.first.y, segment.second.y, 1e-6)
            << "a hatch asked for at zero degrees came out sloped";

    HatchStyle upright;
    upright.angleRad = 1.5707963267948966;
    upright.spacingMm = 5.0;
    const HatchLines vertical = HatchTheRegion(region, upright);
    ASSERT_TRUE(vertical.ok) << vertical.why;
    for (const auto& segment : vertical.segments)
        EXPECT_NEAR(segment.first.x, segment.second.x, 1e-6)
            << "a hatch asked for at ninety degrees came out sloped";
}

TEST(HatchTest, M38_HATCH_006_TheOFFSETPutsTwoPartsOutOfStep) {
    // What tells one part from the next where they touch in a section. Two
    // regions hatched with the same angle and spacing but different offsets
    // must not land their lines in the same places.
    HatchRegion region;
    region.add(Rectangle(0.0, 0.0, 40.0, 40.0));
    HatchStyle first;
    first.angleRad = 0.0;
    first.spacingMm = 4.0;
    HatchStyle second = first;
    second.offsetMm = 2.0;

    const HatchLines a = HatchTheRegion(region, first);
    const HatchLines b = HatchTheRegion(region, second);
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    ASSERT_FALSE(a.segments.empty());
    ASSERT_FALSE(b.segments.empty());
    bool anyShared = false;
    for (const auto& one : a.segments)
        for (const auto& other : b.segments)
            if (std::fabs(one.first.y - other.first.y) < 1e-9) anyShared = true;
    EXPECT_FALSE(anyShared) << "two offsets put their lines in the same places";
}

TEST(HatchTest, M38_HATCH_007_NothingToHatchIsSAIDRatherThanReturnedEmpty) {
    // An empty result and a refusal look the same to a caller that does not
    // ask -- and one of them means the section is not drawn.
    HatchStyle style;
    EXPECT_FALSE(HatchTheRegion(HatchRegion{}, style).ok);
    HatchRegion line;
    line.add({Vec2{0.0, 0.0}, Vec2{10.0, 0.0}});
    EXPECT_FALSE(HatchTheRegion(line, style).ok) << "a two-point loop has no inside";

    HatchRegion square;
    square.add(Rectangle(0.0, 0.0, 10.0, 10.0));
    HatchStyle broken;
    broken.spacingMm = 0.0;
    const HatchLines refused = HatchTheRegion(square, broken);
    EXPECT_FALSE(refused.ok) << "a spacing of zero would draw infinitely many lines";
    // AND IT SAYS WHICH THING IS WRONG. A zero spacing also trips the "nothing
    // was filled" refusal further down, so refusing is not enough on its own:
    // a user told the loops enclose no area goes looking at their geometry,
    // and the answer is that they typed nought into a box.
    EXPECT_NE(refused.why.find("spacing of zero"), std::string::npos) << refused.why;
}

TEST(HatchTest, M38_HATCH_008_TwoHolesAndAHoleInsideTheMaterialBetweenThem) {
    // Even-odd handles nesting without anybody marking which loop is a hole,
    // and this is the shape that catches a rule that only handles one level.
    HatchRegion region;
    region.add(Rectangle(0.0, 0.0, 100.0, 40.0));
    region.add(Circle(Vec2{25.0, 20.0}, 8.0, 48));
    region.add(Circle(Vec2{75.0, 20.0}, 8.0, 48));
    HatchStyle style;
    style.spacingMm = 2.0;

    const HatchLines lines = HatchTheRegion(region, style);
    ASSERT_TRUE(lines.ok) << lines.why;
    EXPECT_FALSE(AnySegmentNear(lines, Vec2{25.0, 20.0}, 6.0)) << "the first hole was hatched";
    EXPECT_FALSE(AnySegmentNear(lines, Vec2{75.0, 20.0}, 6.0)) << "the second hole was hatched";
    // The material between them is filled.
    EXPECT_TRUE(AnySegmentNear(lines, Vec2{50.0, 20.0}, 2.5));
}

TEST(HatchTest, M38_HATCH_009_ALoopThatIsAFlatLineIsREFUSEDNotSilentlyEmpty) {
    // Three points on one horizontal line pass any "at least three points"
    // test and enclose nothing. Every edge is horizontal, so no scanline
    // crosses one, and the honest answer is a refusal -- the caller counts
    // these so the warning reaches the screen.
    HatchRegion flat;
    flat.add({Vec2{0.0, 5.0}, Vec2{10.0, 5.0}, Vec2{20.0, 5.0}});
    const HatchLines refused = HatchTheRegion(flat, HatchStyle{});
    EXPECT_FALSE(refused.ok) << "a flat line was reported as a hatched area";
    EXPECT_FALSE(refused.why.empty());

    // ...and so is one standing on end, which takes the other path through the
    // routine: its edges are kept, they all cross, and every span between them
    // has no width.
    HatchRegion upright;
    upright.add({Vec2{5.0, 0.0}, Vec2{5.0, 10.0}, Vec2{5.0, 20.0}});
    EXPECT_FALSE(HatchTheRegion(upright, HatchStyle{}).ok)
        << "a vertical line was reported as a hatched area";
}

TEST(HatchTest, M38_HATCH_010_AnAreaSmallerThanTheSPACINGSaysSoRatherThanComingBackEmpty) {
    // The failure this is here for is silent: a 1:10 view of a small part
    // whose cut face never meets a hatch line. The paper shows an uncut-
    // looking section and nothing in the program noticed.
    HatchRegion crumb;
    // Placed OFF the grid on purpose: the lines sit at absolute multiples of
    // the spacing, so a crumb straddling y = 0 would still catch one.
    crumb.add(Rectangle(0.1, 0.1, 0.5, 0.5));
    HatchStyle style;
    style.spacingMm = 3.0;
    // Flat, so the test is about the SPACING and not about where a 45 degree
    // rotation happens to drop the crumb relative to the grid.
    style.angleRad = 0.0;
    const HatchLines refused = HatchTheRegion(crumb, style);
    EXPECT_FALSE(refused.ok);
    EXPECT_NE(refused.why.find("spacing"), std::string::npos) << refused.why;
}
