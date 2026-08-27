// M52.1 -- a sheet metal part as its own cross-section, and the two answers
// that must come out of ONE description.
//
// The folded solid and the flat blank. A program where those are separate
// descriptions is a program where they disagree -- quietly, because each is
// self-consistent on its own.
//
// AND THE ONE THAT LOOKS LIKE A BUG AND IS NOT: the folded section's AREA is
// not the flat blank's length times the thickness. They differ by exactly what
// the K factor says, because K is not a fact about the folded shape -- it is a
// statement about metal stretching. Anyone who makes them agree has set K to
// one half and thrown the material table away.

#include "Core/Feature/SheetContour.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

ContourStep Step(double flangeMm, double angleDeg, double radiusMm, bool left = true) {
    ContourStep step;
    step.flangeMm = flangeMm;
    step.bend.angleDeg = angleDeg;
    step.bend.innerRadiusMm = radiusMm;
    step.turnsLeft = left;
    return step;
}

// A channel: up, along, up. Two right-angle bends the same way.
SheetContour Channel() {
    SheetContour contour;
    contour.steps.push_back(Step(30.0, 90.0, 2.0, true));
    contour.steps.push_back(Step(60.0, 90.0, 2.0, true));
    contour.lastFlangeMm = 30.0;
    return contour;
}

// A Z: the same three lengths and the same two bends, turning OPPOSITE ways.
SheetContour Zed() {
    SheetContour contour;
    contour.steps.push_back(Step(30.0, 90.0, 2.0, true));
    contour.steps.push_back(Step(60.0, 90.0, 2.0, false));
    contour.lastFlangeMm = 30.0;
    return contour;
}

Vec2 EndOf(const ProfileSegment& segment) {
    if (const auto* line = std::get_if<ProfileLineSegment>(&segment)) return line->end;
    const auto* arc = std::get_if<ProfileArcSegment>(&segment);
    return Vec2{arc->center.x + arc->radiusMm * std::cos(arc->endAngleRad),
                arc->center.y + arc->radiusMm * std::sin(arc->endAngleRad)};
}

Vec2 StartOf(const ProfileSegment& segment) {
    if (const auto* line = std::get_if<ProfileLineSegment>(&segment)) return line->start;
    const auto* arc = std::get_if<ProfileArcSegment>(&segment);
    return Vec2{arc->center.x + arc->radiusMm * std::cos(arc->startAngleRad),
                arc->center.y + arc->radiusMm * std::sin(arc->startAngleRad)};
}

TEST(SheetContourTest, M52_CONTOUR_001_TheProfileCLOSESAndEverySegmentMeetsTheNext) {
    // A profile is a promise: it comes back to where it started, and every
    // piece begins where the last one ended. A gap of a hundredth is not a
    // small gap -- OCCT refuses the wire, and the message is about topology
    // rather than about the part.
    const ContourProfileResult profile =
        ContourProfile(Channel(), SheetMaterial::MildSteelAluminium, 2.0);
    ASSERT_TRUE(profile.ok) << profile.why;
    ASSERT_GE(profile.segments.size(), 4u);

    for (std::size_t i = 0; i + 1 < profile.segments.size(); ++i) {
        const Vec2 end = EndOf(profile.segments[i]);
        const Vec2 next = StartOf(profile.segments[i + 1]);
        EXPECT_NEAR(end.x, next.x, 1e-9) << "segment " << i << " does not meet the next";
        EXPECT_NEAR(end.y, next.y, 1e-9) << "segment " << i << " does not meet the next";
    }
    const Vec2 last = EndOf(profile.segments.back());
    const Vec2 first = StartOf(profile.segments.front());
    EXPECT_NEAR(last.x, first.x, 1e-9) << "the profile does not close";
    EXPECT_NEAR(last.y, first.y, 1e-9) << "the profile does not close";
}

TEST(SheetContourTest, M52_CONTOUR_002_TheTwoFacesShareACentreAndDifferByTheThickness) {
    // Concentric BY CONSTRUCTION rather than by two pieces of arithmetic that
    // have to agree. A bend whose faces came out at different centres is a
    // section that pinches -- and on a drawing of it, at any ordinary scale,
    // it looks like a bend.
    const double t = 2.0;
    const ContourProfileResult profile =
        ContourProfile(Channel(), SheetMaterial::MildSteelAluminium, t);
    ASSERT_TRUE(profile.ok) << profile.why;

    std::vector<const ProfileArcSegment*> arcs;
    for (const ProfileSegment& segment : profile.segments)
        if (const auto* arc = std::get_if<ProfileArcSegment>(&segment)) arcs.push_back(arc);
    // Two bends, each with a traced face and a far face.
    ASSERT_EQ(arcs.size(), 4u);

    // Each centre appears exactly twice, with radii a thickness apart.
    for (std::size_t i = 0; i < arcs.size(); ++i) {
        int partners = 0;
        for (std::size_t j = 0; j < arcs.size(); ++j) {
            if (i == j) continue;
            if (std::fabs(arcs[i]->center.x - arcs[j]->center.x) > 1e-9) continue;
            if (std::fabs(arcs[i]->center.y - arcs[j]->center.y) > 1e-9) continue;
            ++partners;
            EXPECT_NEAR(std::fabs(arcs[i]->radiusMm - arcs[j]->radiusMm), t, 1e-9)
                << "two faces of one bend are not a thickness apart";
        }
        EXPECT_EQ(partners, 1) << "a bend's arc has no matching face";
    }
}

TEST(SheetContourTest, M52_CONTOUR_003_AChannelAndAZAreDifferentPartsFromTheSameNumbers) {
    // THE FIELD WITH NO SAFE DEFAULT. Three lengths and two right angles
    // describe both, and a table of dimensions cannot tell them apart. Only
    // which way each bend turns can.
    const double t = 2.0;
    const SheetMaterial steel = SheetMaterial::MildSteelAluminium;
    const ContourProfileResult channel = ContourProfile(Channel(), steel, t);
    const ContourProfileResult zed = ContourProfile(Zed(), steel, t);
    ASSERT_TRUE(channel.ok) << channel.why;
    ASSERT_TRUE(zed.ok) << zed.why;

    // The flat blank is the SAME for both -- which is exactly why the blank
    // cannot be the only thing the shop is given.
    const FlatPatternResult flatChannel = ContourFlatLength(Channel(), steel, t);
    const FlatPatternResult flatZed = ContourFlatLength(Zed(), steel, t);
    ASSERT_TRUE(flatChannel.ok);
    ASSERT_TRUE(flatZed.ok);
    EXPECT_NEAR(flatChannel.lengthMm, flatZed.lengthMm, 1e-9);

    // ...and the folded sections are not. A channel comes back on itself; a Z
    // walks away.
    Vec2 channelEnd = EndOf(channel.segments.front());
    Vec2 zedEnd = EndOf(zed.segments.front());
    for (const ProfileSegment& segment : channel.segments) channelEnd = EndOf(segment);
    for (const ProfileSegment& segment : zed.segments) zedEnd = EndOf(segment);
    // Both close, so compare where the far end of the section reaches instead.
    double channelReach = 0.0;
    double zedReach = 0.0;
    for (const ProfileSegment& segment : channel.segments)
        channelReach = std::max(channelReach, std::fabs(EndOf(segment).x));
    for (const ProfileSegment& segment : zed.segments)
        zedReach = std::max(zedReach, std::fabs(EndOf(segment).x));
    EXPECT_GT(std::fabs(channelReach - zedReach), 1.0)
        << "the channel and the Z came out the same shape";
}

TEST(SheetContourTest, M52_CONTOUR_004_TheFoldedAreaIsNOTTheFlatBlanksAndThatIsTheKFactor) {
    // THE FACT THIS FILE EXISTS TO STATE.
    //
    // A bend's cross-section is an annulus segment: angle/2 * ((R+T)^2 - R^2).
    // The flat blank spends T * angle * (R + K*T) on the same bend. Those are
    // equal only when K is exactly one half.
    //
    // For every real material they differ, by angle * T^2 * (1/2 - K) per
    // bend, because K is not a property of the folded SHAPE -- it says how far
    // the metal stretched. Making them agree sets K to 0.5 and throws the
    // material table away, and nothing downstream would ever say so.
    const double t = 2.0;
    const SheetMaterial steel = SheetMaterial::MildSteelAluminium;
    const SheetContour channel = Channel();

    const FlatPatternResult flat = ContourFlatLength(channel, steel, t);
    ASSERT_TRUE(flat.ok) << flat.why;
    const double folded = FoldedSectionAreaMm2(channel, t);
    const double blank = flat.lengthMm * t;

    EXPECT_NE(folded, blank) << "the folded section and the flat blank agree, which means "
                                "the K factor has been quietly set to one half";
    // AND THE DIFFERENCE IS EXACTLY WHAT THE K FACTOR SAYS -- not merely "some
    // difference", which any bug would also satisfy.
    const double k = KFactorFor(steel, 2.0, t);
    double predicted = 0.0;
    for (const ContourStep& step : channel.steps)
        predicted += (step.bend.angleDeg * kPi / 180.0) * t * t * (0.5 - k);
    EXPECT_NEAR(folded - blank, predicted, 1e-9);

    // The folded section holds MORE than the blank, because K below a half
    // puts the neutral line inside the middle -- so the blank is shorter than
    // the metal that ends up there. That direction is the one to notice: a
    // blank cut to the folded area would be too long.
    EXPECT_GT(folded, blank);
}

TEST(SheetContourTest, M52_CONTOUR_005_TheFlatLengthIsM51sAndNotASecondCopy) {
    // Unpacked into the lists M51 already reads, so there is one bend
    // allowance in this program. A contour that computed its own would be the
    // second place it is wrong if it is wrong -- and the two would be checked
    // by different tests, each passing.
    const double t = 2.0;
    const SheetMaterial steel = SheetMaterial::MildSteelAluminium;
    const SheetContour channel = Channel();
    const FlatPatternResult mine = ContourFlatLength(channel, steel, t);
    ASSERT_TRUE(mine.ok) << mine.why;

    std::vector<double> tangents;
    std::vector<SheetBend> bends;
    for (const ContourStep& step : channel.steps) {
        tangents.push_back(step.flangeMm);
        bends.push_back(step.bend);
    }
    tangents.push_back(channel.lastFlangeMm);
    const FlatPatternResult theirs = FlatLengthFromTangents(tangents, bends, steel, t);
    ASSERT_TRUE(theirs.ok) << theirs.why;
    EXPECT_EQ(mine.lengthMm, theirs.lengthMm);
}

TEST(SheetContourTest, M52_CONTOUR_006_TheBendRulesAreM51sToo) {
    const double t = 2.0;
    // A radius the material cracks at, refused through the same function that
    // refuses it anywhere else.
    SheetContour tight = Channel();
    tight.steps[0].bend.innerRadiusMm = 0.5;
    EXPECT_FALSE(WhyContourRefused(tight, SheetMaterial::MildSteelAluminium, t).empty());
    EXPECT_FALSE(ContourProfile(tight, SheetMaterial::MildSteelAluminium, t).ok);

    // A hem, and a bend of nothing.
    SheetContour hem = Channel();
    hem.steps[0].bend.angleDeg = 180.0;
    EXPECT_FALSE(WhyContourRefused(hem, SheetMaterial::MildSteelAluminium, t).empty());

    // A flange of no length, at either end of the chain.
    SheetContour stub = Channel();
    stub.steps[1].flangeMm = 0.0;
    EXPECT_FALSE(WhyContourRefused(stub, SheetMaterial::MildSteelAluminium, t).empty());
    SheetContour noTail = Channel();
    noTail.lastFlangeMm = 0.0;
    EXPECT_FALSE(WhyContourRefused(noTail, SheetMaterial::MildSteelAluminium, t).empty());
}

TEST(SheetContourTest, M52_CONTOUR_007_AFlatStripIsARealPart) {
    // No steps at all: a strip. It has a section and a blank, and both are the
    // same length -- there being no bend for them to differ over.
    const double t = 2.0;
    SheetContour strip;
    strip.lastFlangeMm = 100.0;
    const SheetMaterial steel = SheetMaterial::MildSteelAluminium;

    EXPECT_TRUE(WhyContourRefused(strip, steel, t).empty());
    const ContourProfileResult profile = ContourProfile(strip, steel, t);
    ASSERT_TRUE(profile.ok) << profile.why;
    // Four sides, and nothing curved.
    EXPECT_EQ(profile.segments.size(), 4u);
    for (const ProfileSegment& segment : profile.segments)
        EXPECT_EQ(std::get_if<ProfileArcSegment>(&segment), nullptr);

    const FlatPatternResult flat = ContourFlatLength(strip, steel, t);
    ASSERT_TRUE(flat.ok) << flat.why;
    EXPECT_NEAR(flat.lengthMm, 100.0, 1e-9);
    EXPECT_NEAR(FoldedSectionAreaMm2(strip, t), 200.0, 1e-9);
}

TEST(SheetContourTest, M52_CONTOUR_008_ATurnBendsTheWalkByExactlyItsAngle) {
    // Four right angles turning the same way come back to the start heading,
    // which is what makes a closed box section possible. Accumulated angles
    // that drifted a fraction per corner would leave the last flange out of
    // parallel by a degree -- visible on the part, invisible on the numbers.
    const double t = 2.0;
    SheetContour box;
    for (int i = 0; i < 4; ++i) box.steps.push_back(Step(40.0, 90.0, 2.0, true));
    box.lastFlangeMm = 40.0;

    const ContourProfileResult profile =
        ContourProfile(box, SheetMaterial::MildSteelAluminium, t);
    ASSERT_TRUE(profile.ok) << profile.why;

    // The last straight of the traced face runs parallel to the first, and
    // the other way about -- four right angles is half a turn twice.
    const auto* first = std::get_if<ProfileLineSegment>(&profile.segments.front());
    ASSERT_NE(first, nullptr);
    const ProfileLineSegment* last = nullptr;
    for (std::size_t i = 0; i < profile.segments.size(); ++i)
        if (const auto* line = std::get_if<ProfileLineSegment>(&profile.segments[i]))
            if (i <= 8) last = line;
    ASSERT_NE(last, nullptr);
    const Vec2 a{first->end.x - first->start.x, first->end.y - first->start.y};
    const Vec2 b{last->end.x - last->start.x, last->end.y - last->start.y};
    const double cross = a.x * b.y - a.y * b.x;
    EXPECT_NEAR(cross, 0.0, 1e-6) << "four right angles did not come back to the heading";
}

} // namespace
