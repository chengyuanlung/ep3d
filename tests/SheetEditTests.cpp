// M40 -- trim, extend, fillet, chamfer, offset, array.
//
// Every one of these tools fails the same way: it does something entirely
// plausible to the WRONG PIECE. Trim keeps the half that was meant to go;
// offset lands on the far side; fillet joins the far ends of two lines instead
// of the near ones. All of them produce a valid drawing, none of them throws,
// and the only question that catches any of it is "which piece came back".
//
// So these tests never ask whether an edit worked. They ask what it returned.

#include "Core/Drawing/SheetEdits.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

const DrawLine* AsLine(const DrawShape& shape) { return std::get_if<DrawLine>(&shape); }
const DrawArc* AsArc(const DrawShape& shape) { return std::get_if<DrawArc>(&shape); }
const DrawCircle* AsCircle(const DrawShape& shape) { return std::get_if<DrawCircle>(&shape); }

bool Near(Vec2 a, Vec2 b, double within = 1e-6) {
    return std::hypot(a.x - b.x, a.y - b.y) <= within;
}

// Does any returned line have these two ends, either way round?
bool HasLine(const SheetEditResult& result, Vec2 from, Vec2 to) {
    for (const DrawShape& shape : result.shapes) {
        const DrawLine* line = AsLine(shape);
        if (line == nullptr) continue;
        if ((Near(line->a, from) && Near(line->b, to)) ||
            (Near(line->a, to) && Near(line->b, from)))
            return true;
    }
    return false;
}

TEST(SheetEditTest, M40_TRIM_001_ThePICKEDPieceIsTheOneThatGoes) {
    // Written the other way round -- keeping what was picked -- every edit
    // comes out inside out, and the drawing still looks like a drawing.
    const DrawShape victim = DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}};
    const std::vector<DrawShape> cutters{DrawLine{Vec2{40.0, -10.0}, Vec2{40.0, 10.0}}};

    const SheetEditResult right = TrimShape(victim, cutters, Vec2{80.0, 0.0});
    ASSERT_TRUE(right.ok) << right.why;
    ASSERT_EQ(right.shapes.size(), 1u);
    EXPECT_TRUE(HasLine(right, Vec2{0.0, 0.0}, Vec2{40.0, 0.0}))
        << "the piece that was picked was kept and the other one thrown away";

    // Picking the other side keeps the other side. Said explicitly, because a
    // tool that always kept the same half would pass the check above.
    const SheetEditResult left = TrimShape(victim, cutters, Vec2{10.0, 0.0});
    ASSERT_TRUE(left.ok) << left.why;
    ASSERT_EQ(left.shapes.size(), 1u);
    EXPECT_TRUE(HasLine(left, Vec2{40.0, 0.0}, Vec2{100.0, 0.0}));
}

TEST(SheetEditTest, M40_TRIM_002_TrimmingTheMIDDLELeavesTWOLines) {
    // The reason the result is a list. A tool returning one shape has to
    // choose which half to keep, and whichever it chooses is wrong half the
    // time -- silently, because one line is exactly what a caller expects.
    const DrawShape victim = DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}};
    const std::vector<DrawShape> cutters{DrawLine{Vec2{30.0, -10.0}, Vec2{30.0, 10.0}},
                                         DrawLine{Vec2{70.0, -10.0}, Vec2{70.0, 10.0}}};

    const SheetEditResult result = TrimShape(victim, cutters, Vec2{50.0, 0.0});
    ASSERT_TRUE(result.ok) << result.why;
    ASSERT_EQ(result.shapes.size(), 2u) << "the middle was cut out but only one end came back";
    EXPECT_TRUE(HasLine(result, Vec2{0.0, 0.0}, Vec2{30.0, 0.0}));
    EXPECT_TRUE(HasLine(result, Vec2{70.0, 0.0}, Vec2{100.0, 0.0}));
    EXPECT_FALSE(HasLine(result, Vec2{30.0, 0.0}, Vec2{70.0, 0.0}));
}

TEST(SheetEditTest, M40_TRIM_003_ACutterThatDoesNotREACHTheLineCutsNothing) {
    // The cutter crosses the infinite line this one lies on, and stops short
    // of the line itself. Cutting there would put a break where the paper
    // shows nothing crossing.
    const DrawShape victim = DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}};
    const std::vector<DrawShape> shortOfIt{DrawLine{Vec2{40.0, 5.0}, Vec2{40.0, 20.0}}};
    const SheetEditResult result = TrimShape(victim, shortOfIt, Vec2{80.0, 0.0});
    EXPECT_FALSE(result.ok) << "a cutter that never touches the line still cut it";
    EXPECT_FALSE(result.why.empty());

    EXPECT_FALSE(TrimShape(victim, {}, Vec2{80.0, 0.0}).ok);
}

TEST(SheetEditTest, M40_TRIM_004_ACircleNeedsTWOCutsBeforeAnythingComesOut) {
    // Cut once, a circle is still a closed circle: there is no end to remove.
    // A tool that made an arc from a single crossing would delete half a
    // circle nobody asked it to touch.
    const DrawShape victim = DrawCircle{Vec2{0.0, 0.0}, 50.0};
    const std::vector<DrawShape> once{DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}}};
    // That line starts at the centre, so it leaves the circle once.
    EXPECT_FALSE(TrimShape(victim, once, Vec2{0.0, 50.0}).ok);

    // Straight across: two crossings, and the picked half goes.
    const std::vector<DrawShape> across{DrawLine{Vec2{-100.0, 0.0}, Vec2{100.0, 0.0}}};
    const SheetEditResult result = TrimShape(victim, across, Vec2{0.0, 50.0});
    ASSERT_TRUE(result.ok) << result.why;
    ASSERT_EQ(result.shapes.size(), 1u);
    const DrawArc* arc = AsArc(result.shapes.front());
    ASSERT_NE(arc, nullptr) << "trimming a circle did not leave an arc";
    EXPECT_NEAR(arc->radius, 50.0, 1e-9);
    // What is LEFT is the bottom half, so the arc has to pass through -90 and
    // not through +90.
    EXPECT_TRUE(AngleWithinArc(-kPi / 2.0, arc->startAngle, arc->endAngle));
    EXPECT_FALSE(AngleWithinArc(kPi / 2.0, arc->startAngle, arc->endAngle))
        << "the half that was picked is the half that came back";
}

TEST(SheetEditTest, M40_EXTEND_001_TheEndNEARESTThePickIsTheOneThatMoves) {
    // The user has already said which end by clicking near it. Picking for
    // them stretches the wrong end of a nearly symmetrical line, and the
    // result looks like a line that reached something.
    const DrawShape victim = DrawLine{Vec2{0.0, 0.0}, Vec2{50.0, 0.0}};
    const std::vector<DrawShape> boundaries{DrawLine{Vec2{80.0, -10.0}, Vec2{80.0, 10.0}},
                                            DrawLine{Vec2{-30.0, -10.0}, Vec2{-30.0, 10.0}}};

    const SheetEditResult forwards = ExtendShape(victim, boundaries, Vec2{49.0, 0.0});
    ASSERT_TRUE(forwards.ok) << forwards.why;
    ASSERT_EQ(forwards.shapes.size(), 1u);
    EXPECT_TRUE(HasLine(forwards, Vec2{0.0, 0.0}, Vec2{80.0, 0.0}))
        << "the far end moved instead of the one that was picked";

    const SheetEditResult backwards = ExtendShape(victim, boundaries, Vec2{1.0, 0.0});
    ASSERT_TRUE(backwards.ok) << backwards.why;
    EXPECT_TRUE(HasLine(backwards, Vec2{-30.0, 0.0}, Vec2{50.0, 0.0}));
}

TEST(SheetEditTest, M40_EXTEND_002_NothingAheadIsAREFUSALNotADefaultLength) {
    // A line that grew by some arbitrary amount looks exactly like one that
    // met a boundary, and the drawing carries a length nobody chose.
    const DrawShape victim = DrawLine{Vec2{0.0, 0.0}, Vec2{50.0, 0.0}};
    const std::vector<DrawShape> behind{DrawLine{Vec2{-30.0, -10.0}, Vec2{-30.0, 10.0}}};
    const SheetEditResult result = ExtendShape(victim, behind, Vec2{49.0, 0.0});
    EXPECT_FALSE(result.ok) << "a line was stretched to a boundary that is behind it";
    EXPECT_FALSE(result.why.empty());

    // A boundary BEHIND the moving end would shorten the line, which is what
    // trim is for -- so it is not a candidate.
    EXPECT_FALSE(ExtendShape(victim, {}, Vec2{49.0, 0.0}).ok);
}

TEST(SheetEditTest, M40_FILLET_001_TheArcIsTangentAndBothLinesAreCutBackToIt) {
    // Two lines meeting at the origin, one along +X and one along +Y.
    const DrawShape first = DrawLine{Vec2{100.0, 0.0}, Vec2{0.0, 0.0}};
    const DrawShape second = DrawLine{Vec2{0.0, 0.0}, Vec2{0.0, 100.0}};

    const SheetEditResult result =
        FilletLines(first, second, Vec2{50.0, 0.0}, Vec2{0.0, 50.0}, 10.0);
    ASSERT_TRUE(result.ok) << result.why;
    ASSERT_EQ(result.shapes.size(), 3u) << "a fillet is two lines and an arc";

    // At a right angle the arc touches 10 along each line, and its centre is
    // at (10, 10) -- the one place a circle of radius 10 touches both.
    EXPECT_TRUE(HasLine(result, Vec2{100.0, 0.0}, Vec2{10.0, 0.0}));
    EXPECT_TRUE(HasLine(result, Vec2{0.0, 10.0}, Vec2{0.0, 100.0}));
    const DrawArc* arc = nullptr;
    for (const DrawShape& shape : result.shapes)
        if (const DrawArc* one = AsArc(shape)) arc = one;
    ASSERT_NE(arc, nullptr);
    EXPECT_TRUE(Near(arc->centre, Vec2{10.0, 10.0}))
        << "the arc is not tangent to both lines";
    EXPECT_NEAR(arc->radius, 10.0, 1e-9);
    // ...and it is the SHORT way round. The long way is an arc that sweeps
    // away outside the corner: tangent, the right radius, and not a fillet.
    double sweep = arc->endAngle - arc->startAngle;
    while (sweep < 0.0) sweep += 2.0 * kPi;
    EXPECT_LT(sweep, kPi) << "the fillet took the long way round";
}

TEST(SheetEditTest, M40_FILLET_002_ThePICKSSayWhichOfTheFourCornersIsMeant) {
    // Two lines that CROSS make four corners, and every one of them is a valid
    // fillet. Without the picks a tool has to guess, and it is right about a
    // quarter of the time.
    const DrawShape across = DrawLine{Vec2{-100.0, 0.0}, Vec2{100.0, 0.0}};
    const DrawShape upright = DrawLine{Vec2{0.0, -100.0}, Vec2{0.0, 100.0}};

    const SheetEditResult topRight =
        FilletLines(across, upright, Vec2{50.0, 0.0}, Vec2{0.0, 50.0}, 10.0);
    ASSERT_TRUE(topRight.ok) << topRight.why;
    const DrawArc* first = nullptr;
    for (const DrawShape& shape : topRight.shapes)
        if (const DrawArc* one = AsArc(shape)) first = one;
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(Near(first->centre, Vec2{10.0, 10.0}));

    const SheetEditResult bottomLeft =
        FilletLines(across, upright, Vec2{-50.0, 0.0}, Vec2{0.0, -50.0}, 10.0);
    ASSERT_TRUE(bottomLeft.ok) << bottomLeft.why;
    const DrawArc* second = nullptr;
    for (const DrawShape& shape : bottomLeft.shapes)
        if (const DrawArc* one = AsArc(shape)) second = one;
    ASSERT_NE(second, nullptr);
    EXPECT_TRUE(Near(second->centre, Vec2{-10.0, -10.0}))
        << "the same corner came back whichever side was picked";
}

TEST(SheetEditTest, M40_FILLET_003_ARadiusOfZeroIsACornerAndOneTooBigIsREFUSED) {
    const DrawShape first = DrawLine{Vec2{100.0, 0.0}, Vec2{0.0, 0.0}};
    const DrawShape second = DrawLine{Vec2{0.0, 0.0}, Vec2{0.0, 40.0}};

    // Zero: bring both lines to the point, which is what every CAD system
    // does with it and what a user typing 0 means.
    const SheetEditResult corner =
        FilletLines(first, second, Vec2{50.0, 0.0}, Vec2{0.0, 20.0}, 0.0);
    ASSERT_TRUE(corner.ok) << corner.why;
    EXPECT_EQ(corner.shapes.size(), 2u) << "a radius of zero still produced an arc";
    EXPECT_TRUE(HasLine(corner, Vec2{100.0, 0.0}, Vec2{0.0, 0.0}));
    EXPECT_TRUE(HasLine(corner, Vec2{0.0, 0.0}, Vec2{0.0, 40.0}));

    // Too big to fit: the arc would eat both lines and leave itself floating
    // where a corner was -- valid geometry and not a drawing of anything.
    const SheetEditResult tooBig =
        FilletLines(first, second, Vec2{50.0, 0.0}, Vec2{0.0, 20.0}, 60.0);
    EXPECT_FALSE(tooBig.ok) << "a fillet longer than its own lines was accepted";
    EXPECT_FALSE(tooBig.why.empty());

    // Parallel lines make no corner at all.
    const DrawShape parallel = DrawLine{Vec2{0.0, 20.0}, Vec2{100.0, 20.0}};
    EXPECT_FALSE(FilletLines(first, parallel, Vec2{50.0, 0.0}, Vec2{50.0, 20.0}, 5.0).ok);
}

TEST(SheetEditTest, M40_CHAMFER_001_TheSetbackIsMeasuredAlongEACHLine) {
    const DrawShape first = DrawLine{Vec2{100.0, 0.0}, Vec2{0.0, 0.0}};
    const DrawShape second = DrawLine{Vec2{0.0, 0.0}, Vec2{0.0, 100.0}};

    const SheetEditResult result =
        ChamferLines(first, second, Vec2{50.0, 0.0}, Vec2{0.0, 50.0}, 8.0);
    ASSERT_TRUE(result.ok) << result.why;
    ASSERT_EQ(result.shapes.size(), 3u);
    EXPECT_TRUE(HasLine(result, Vec2{100.0, 0.0}, Vec2{8.0, 0.0}));
    EXPECT_TRUE(HasLine(result, Vec2{8.0, 0.0}, Vec2{0.0, 8.0})) << "the chamfer face is wrong";
    EXPECT_TRUE(HasLine(result, Vec2{0.0, 8.0}, Vec2{0.0, 100.0}));

    EXPECT_FALSE(ChamferLines(first, second, Vec2{50.0, 0.0}, Vec2{0.0, 50.0}, 0.0).ok);
    EXPECT_FALSE(ChamferLines(first, second, Vec2{50.0, 0.0}, Vec2{0.0, 50.0}, 200.0).ok);
}

TEST(SheetEditTest, M40_OFFSET_001_TheSideIsTheSideThatWasPICKED) {
    // A signed distance would work for a line and mean nothing for a circle,
    // so the side is said the same way for every shape: with a point.
    const DrawShape line = DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}};

    const SheetEditResult above = OffsetShape(line, 10.0, Vec2{50.0, 5.0});
    ASSERT_TRUE(above.ok) << above.why;
    EXPECT_TRUE(HasLine(above, Vec2{0.0, 10.0}, Vec2{100.0, 10.0}));

    const SheetEditResult below = OffsetShape(line, 10.0, Vec2{50.0, -5.0});
    ASSERT_TRUE(below.ok) << below.why;
    EXPECT_TRUE(HasLine(below, Vec2{0.0, -10.0}, Vec2{100.0, -10.0}))
        << "the copy landed on the same side whichever way it was picked";

    EXPECT_FALSE(OffsetShape(line, 0.0, Vec2{50.0, 5.0}).ok);
    EXPECT_FALSE(OffsetShape(line, -10.0, Vec2{50.0, 5.0}).ok);
}

TEST(SheetEditTest, M40_OFFSET_002_ACircleGoesTheWayThePickIsAndNeverInsideOut) {
    const DrawShape circle = DrawCircle{Vec2{0.0, 0.0}, 20.0};

    const SheetEditResult bigger = OffsetShape(circle, 5.0, Vec2{40.0, 0.0});
    ASSERT_TRUE(bigger.ok) << bigger.why;
    ASSERT_NE(AsCircle(bigger.shapes.front()), nullptr);
    EXPECT_NEAR(AsCircle(bigger.shapes.front())->radius, 25.0, 1e-9);

    const SheetEditResult smaller = OffsetShape(circle, 5.0, Vec2{2.0, 0.0});
    ASSERT_TRUE(smaller.ok) << smaller.why;
    EXPECT_NEAR(AsCircle(smaller.shapes.front())->radius, 15.0, 1e-9);

    // Inward past the centre: a bare subtraction gives a NEGATIVE radius,
    // which most code then quietly turns back into a bigger circle.
    const SheetEditResult inverted = OffsetShape(circle, 30.0, Vec2{2.0, 0.0});
    EXPECT_FALSE(inverted.ok) << "an offset past the centre came back as a larger circle";
}

TEST(SheetEditTest, M40_ARRAY_001_TheORIGINALIsTheFirstCopy) {
    // Left out, every caller has to add it back, and the one that forgets
    // deletes what the user arrayed.
    const std::vector<DrawShape> shapes{DrawLine{Vec2{0.0, 0.0}, Vec2{10.0, 0.0}}};
    const SheetEditResult result = RectangularArray(shapes, 3, 2, Vec2{20.0, 15.0});
    ASSERT_TRUE(result.ok) << result.why;
    EXPECT_EQ(result.shapes.size(), 6u);
    EXPECT_TRUE(HasLine(result, Vec2{0.0, 0.0}, Vec2{10.0, 0.0})) << "the original was dropped";
    EXPECT_TRUE(HasLine(result, Vec2{40.0, 15.0}, Vec2{50.0, 15.0}));

    // A pitch of nothing stacks every copy on the original -- which looks
    // exactly like one object until somebody drags it.
    EXPECT_FALSE(RectangularArray(shapes, 3, 1, Vec2{0.0, 15.0}).ok);
    EXPECT_TRUE(RectangularArray(shapes, 1, 1, Vec2{0.0, 0.0}).ok);
    EXPECT_FALSE(RectangularArray(shapes, 0, 2, Vec2{20.0, 15.0}).ok);
    EXPECT_FALSE(RectangularArray({}, 3, 2, Vec2{20.0, 15.0}).ok);
}

TEST(SheetEditTest, M40_ARRAY_002_AFullCircleDoesNotPutTwoCopiesInOnePlace) {
    // Six bolts round a flange is six, not seven with two on top of each
    // other -- and two coincident objects look like one until one is moved.
    const std::vector<DrawShape> shapes{DrawCircle{Vec2{50.0, 0.0}, 3.0}};
    const SheetEditResult ring = PolarArray(shapes, Vec2{0.0, 0.0}, 6, 2.0 * kPi, true);
    ASSERT_TRUE(ring.ok) << ring.why;
    ASSERT_EQ(ring.shapes.size(), 6u);

    // The step is a sixth of a turn, so the second copy is at 60 degrees.
    const DrawCircle* second = AsCircle(ring.shapes[1]);
    ASSERT_NE(second, nullptr);
    EXPECT_TRUE(Near(second->centre, Vec2{50.0 * std::cos(kPi / 3.0),
                                          50.0 * std::sin(kPi / 3.0)}));
    // ...and the last one is NOT back at the start.
    EXPECT_FALSE(Near(AsCircle(ring.shapes.back())->centre, Vec2{50.0, 0.0}))
        << "the last copy landed on top of the first";
}

TEST(SheetEditTest, M40_ARRAY_003_RotatingTheItemsIsADifferentAnswerFromCarryingThemRound) {
    // A ring of bolts is turned to face out; a ring of labels stays readable.
    // Both are wanted often enough that neither can be the silent default.
    const std::vector<DrawShape> shapes{DrawLine{Vec2{50.0, 0.0}, Vec2{60.0, 0.0}}};

    const SheetEditResult turned = PolarArray(shapes, Vec2{0.0, 0.0}, 4, 2.0 * kPi, true);
    ASSERT_TRUE(turned.ok) << turned.why;
    ASSERT_EQ(turned.shapes.size(), 4u);
    // A quarter turn puts the second copy pointing up the page.
    EXPECT_TRUE(HasLine(turned, Vec2{0.0, 50.0}, Vec2{0.0, 60.0}));

    const SheetEditResult carried = PolarArray(shapes, Vec2{0.0, 0.0}, 4, 2.0 * kPi, false);
    ASSERT_TRUE(carried.ok) << carried.why;
    ASSERT_EQ(carried.shapes.size(), 4u);
    // The same copy has MOVED to (0, 50) and is still lying along +X.
    EXPECT_TRUE(HasLine(carried, Vec2{0.0, 50.0}, Vec2{10.0, 50.0}))
        << "the items were turned when they were asked to be carried round";
    EXPECT_FALSE(HasLine(carried, Vec2{0.0, 50.0}, Vec2{0.0, 60.0}));

    EXPECT_FALSE(PolarArray(shapes, Vec2{0.0, 0.0}, 4, 0.0, true).ok);
    EXPECT_FALSE(PolarArray(shapes, Vec2{0.0, 0.0}, 0, kPi, true).ok);
}

TEST(SheetEditTest, M40_EXTEND_003_ABoundaryTheLineAlreadyCROSSESIsNotSomethingToReachTo) {
    // A boundary between the two ends would SHORTEN the line, which is what
    // trim is for. Taken as a candidate, extend quietly pulls the end backwards
    // -- and a line that got shorter when the user asked for longer still looks
    // like a line.
    const DrawShape victim = DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}};
    const std::vector<DrawShape> through{DrawLine{Vec2{40.0, -10.0}, Vec2{40.0, 10.0}}};
    EXPECT_FALSE(ExtendShape(victim, through, Vec2{99.0, 0.0}).ok)
        << "extend reached back to a boundary the line already crosses";

    // ...and with something genuinely ahead as well, it reaches THAT one and
    // not the near crossing.
    const std::vector<DrawShape> both{DrawLine{Vec2{40.0, -10.0}, Vec2{40.0, 10.0}},
                                      DrawLine{Vec2{150.0, -10.0}, Vec2{150.0, 10.0}}};
    const SheetEditResult result = ExtendShape(victim, both, Vec2{99.0, 0.0});
    ASSERT_TRUE(result.ok) << result.why;
    EXPECT_TRUE(HasLine(result, Vec2{0.0, 0.0}, Vec2{150.0, 0.0}))
        << "extend stopped at a boundary behind the end it was moving";
}

TEST(SheetEditTest, M40_FILLET_004_TheSetbackFollowsTheANGLEAndNotJustTheRadius) {
    // AT A RIGHT ANGLE THE SETBACK EQUALS THE RADIUS, and that coincidence
    // hides the arithmetic: every test above uses 90 degrees, so a build that
    // forgot the angle entirely passes all of them. A sharper corner is where
    // the two part company -- the arc has to sit further back to stay tangent.
    //
    // 60 degrees between the kept directions: half is 30, tan 30 is 0.5774, so
    // a radius of 10 touches 17.32 along each line.
    const double sixty = kPi / 3.0;
    const DrawShape first = DrawLine{Vec2{100.0, 0.0}, Vec2{0.0, 0.0}};
    const DrawShape second =
        DrawLine{Vec2{0.0, 0.0}, Vec2{100.0 * std::cos(sixty), 100.0 * std::sin(sixty)}};

    const SheetEditResult result =
        FilletLines(first, second, Vec2{50.0, 0.0},
                    Vec2{50.0 * std::cos(sixty), 50.0 * std::sin(sixty)}, 10.0);
    ASSERT_TRUE(result.ok) << result.why;
    ASSERT_EQ(result.shapes.size(), 3u);

    const double setback = 10.0 / std::tan(sixty / 2.0);
    EXPECT_NEAR(setback, 17.3205, 1e-3) << "the test's own arithmetic";
    EXPECT_TRUE(HasLine(result, Vec2{100.0, 0.0}, Vec2{setback, 0.0}))
        << "the first line was not cut back to where the arc meets it";

    // ...and the arc really is tangent: its centre is `radius` away from both
    // lines. Measured rather than compared to a constant, because that is the
    // property a fillet has and a wrong setback breaks.
    const DrawArc* arc = nullptr;
    for (const DrawShape& shape : result.shapes)
        if (const DrawArc* one = AsArc(shape)) arc = one;
    ASSERT_NE(arc, nullptr);
    EXPECT_NEAR(std::fabs(arc->centre.y), 10.0, 1e-6) << "the arc is not tangent to the first line";
    const double toSecond =
        std::fabs(arc->centre.x * std::sin(sixty) - arc->centre.y * std::cos(sixty));
    EXPECT_NEAR(toSecond, 10.0, 1e-6) << "the arc is not tangent to the second line";
}

} // namespace
