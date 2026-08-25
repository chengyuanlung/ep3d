// M43 -- an in-view anchor that cannot land somewhere else quietly.
//
// This is NOT a topological name and does not claim to be: a projected curve
// still does not carry which model edge it came from, so re-finding a point is
// still a search. What changed is that the search can no longer answer with
// something that is not what was asked for.
//
// Two failures are being closed, and both print a plausible number:
//
//   * A CENTRE RE-ATTACHING TO A CORNER. "Nearest snap point" has no idea
//     that a hole's centre and a plate's corner are different kinds of thing.
//     Move the bore a little and a diameter dimension can measure the
//     distance between two corners -- a real distance between two real
//     points, and not the dimension anybody put there.
//
//   * AN AMBIGUOUS CHOICE MADE SILENTLY. Two candidates of the same kind
//     within reach, and the old rule took whichever was a hair nearer. The
//     hair is noise; the choice is a guess; and the drawing carries it
//     without saying so.
//
// The fixture builds projections by hand rather than through the kernel,
// because what is under test is the RULE and not the geometry -- and a
// hand-built projection is the only way to put two candidates exactly where
// the rule has to decide between them.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

// A drawing with one view whose projection is whatever a test says it is.
struct Sheet {
    DrawingDocument document{"Plate"};
    ObjectId view = kInvalidObjectId;

    Sheet() {
        view = document.addView("Front", "parts/block.ep3d", "Block", ViewDirection::Front,
                                Vec2{150.0, 150.0})
                   .id();
    }

    void projects(std::vector<ProjectedCurve> curves) {
        ProjectedDrawing drawing;
        drawing.curves = std::move(curves);
        for (const ProjectedCurve& curve : drawing.curves) {
            if (const auto* line = std::get_if<ProjectedLine>(&curve.shape)) {
                drawing.extent.empty = false;
                drawing.extent.min = Vec2{std::min(line->a.x, line->b.x),
                                          std::min(line->a.y, line->b.y)};
                drawing.extent.max = Vec2{std::max(line->a.x, line->b.x),
                                          std::max(line->a.y, line->b.y)};
            }
        }
        document.viewForTesting(view)->setProjectionForTesting(std::move(drawing));
    }

    static ProjectedCurve line(Vec2 a, Vec2 b) {
        ProjectedCurve curve;
        curve.shape = ProjectedLine{a, b};
        return curve;
    }

    static ProjectedCurve circle(Vec2 centre, double radius) {
        ProjectedCurve curve;
        ProjectedArc arc;
        arc.centre = centre;
        arc.radius = radius;
        arc.startAngle = 0.0;
        arc.endAngle = 2.0 * kPi;
        arc.isFullCircle = true;
        curve.shape = arc;
        return curve;
    }

    // Does an anchor of this kind, put here, still find a point?
    std::optional<Vec2> resolve(Vec2 at, ViewPointRole role, double toleranceMm) {
        const DrawingDimension& dimension = document.addDimension(
            DimensionKind::Linear,
            DimensionAnchor::inView(view, at, role, toleranceMm),
            DimensionAnchor::free(Vec2{0.0, 0.0}), Vec2{10.0, 10.0});
        const DimensionMeasurement measured = document.measure(dimension);
        return measured.ok ? std::optional<Vec2>{measured.firstMm} : std::nullopt;
    }
};

TEST(ViewAnchorTest, M43_ANCHOR_001_ACentreOnlyEverReFindsACentre) {
    // The bore's centre and the plate's corner are 3 mm apart. With a generous
    // tolerance the old rule would take whichever was nearer -- and after the
    // bore moves, that is the corner.
    Sheet sheet;
    sheet.projects({Sheet::line(Vec2{0.0, 0.0}, Vec2{40.0, 0.0}),
                    Sheet::line(Vec2{40.0, 0.0}, Vec2{40.0, 40.0}),
                    Sheet::circle(Vec2{37.0, 3.0}, 5.0)});

    // Anchored on the centre, asking from a point 1 mm off it: the corner at
    // (40, 0) is 3.6 mm away and the centre 1 mm, so BOTH are within a 5 mm
    // tolerance.
    const std::optional<Vec2> found = sheet.resolve(Vec2{36.0, 3.0},
                                                    ViewPointRole::Centre, 5.0);
    ASSERT_TRUE(found.has_value()) << "a centre 1 mm from where it was left was not found";
    // Sheet millimetres: the view sits at (150, 150) and the scale is 1:1.
    EXPECT_NEAR(found->x, 150.0 + 37.0, 1e-6);
    EXPECT_NEAR(found->y, 150.0 + 3.0, 1e-6);

    // ...and a CORNER anchor in the same place finds the corner, not the
    // centre, even though the centre is nearer.
    const std::optional<Vec2> corner = sheet.resolve(Vec2{36.0, 3.0},
                                                     ViewPointRole::Corner, 5.0);
    ASSERT_TRUE(corner.has_value());
    EXPECT_NEAR(corner->x, 150.0 + 40.0, 1e-6)
        << "a corner anchor re-attached to the circle's centre";
    EXPECT_NEAR(corner->y, 150.0 + 0.0, 1e-6);
}

TEST(ViewAnchorTest, M43_ANCHOR_002_ACentreWithNoCircleLeftDanglesRatherThanTakingACorner) {
    // The bore is deleted from the model. There is no centre any more, and the
    // corner two millimetres away is NOT an answer -- it is a different kind
    // of thing, and a diameter measured to it is a number nobody asked for.
    Sheet sheet;
    sheet.projects({Sheet::line(Vec2{0.0, 0.0}, Vec2{40.0, 0.0}),
                    Sheet::line(Vec2{40.0, 0.0}, Vec2{40.0, 40.0})});

    EXPECT_FALSE(sheet.resolve(Vec2{39.0, 1.0}, ViewPointRole::Centre, 5.0).has_value())
        << "a centre anchor took a corner when its circle had gone";
}

TEST(ViewAnchorTest, M43_ANCHOR_003_TwoCandidatesOfTheSameKindAreREFUSEDNotGuessedAt) {
    // Two holes, and the anchor sits almost exactly between them: 2.0 mm from
    // one and 2.2 from the other. The old rule took the 2.0 -- a tenth of a
    // millimetre deciding which hole a dimension is about, with nothing on the
    // paper saying a choice was made.
    Sheet sheet;
    sheet.projects({Sheet::circle(Vec2{10.0, 0.0}, 3.0), Sheet::circle(Vec2{14.2, 0.0}, 3.0)});

    EXPECT_FALSE(sheet.resolve(Vec2{12.0, 0.0}, ViewPointRole::Centre, 5.0).has_value())
        << "the anchor picked between two holes that are equally plausible";

    // ...and when one of them is CLEARLY nearer, the answer is clear again.
    // The rule is "at least twice as far", so 1 mm against 3 mm is decided.
    const std::optional<Vec2> decided = sheet.resolve(Vec2{11.0, 0.0},
                                                      ViewPointRole::Centre, 5.0);
    ASSERT_TRUE(decided.has_value()) << "a clear nearest candidate was refused anyway";
    EXPECT_NEAR(decided->x, 150.0 + 10.0, 1e-6);
}

TEST(ViewAnchorTest, M43_ANCHOR_004_APointThatDidNotMoveIsNeverAmbiguous) {
    // The ordinary case, and it has to stay free: a part that was not touched
    // reprojects to exactly the same points, so every anchor finds its own
    // point at distance zero. Nothing else can be "nearly as near" as zero.
    Sheet sheet;
    sheet.projects({Sheet::circle(Vec2{10.0, 0.0}, 3.0), Sheet::circle(Vec2{10.5, 0.0}, 3.0)});

    const std::optional<Vec2> exact = sheet.resolve(Vec2{10.0, 0.0},
                                                    ViewPointRole::Centre, 5.0);
    ASSERT_TRUE(exact.has_value())
        << "an anchor sitting exactly on its own point was called ambiguous";
    EXPECT_NEAR(exact->x, 150.0 + 10.0, 1e-6);

    // Half a millimetre away from one of two centres half a millimetre apart
    // IS ambiguous, and that is the same rule doing its job rather than a
    // different one.
    EXPECT_FALSE(sheet.resolve(Vec2{10.25, 0.0}, ViewPointRole::Centre, 5.0).has_value());
}

TEST(ViewAnchorTest, M43_ANCHOR_005_ACornerSharedByTwoEdgesIsONECandidate) {
    // Every edge meeting at a corner offers that corner, so the same position
    // arrives two or three times. Counted as competition, every corner in
    // every drawing is ambiguous with itself -- which is not a theory: it is
    // what happened, and the whole suite went red on a rectangle.
    Sheet sheet;
    sheet.projects({Sheet::line(Vec2{0.0, 0.0}, Vec2{40.0, 0.0}),
                    Sheet::line(Vec2{40.0, 0.0}, Vec2{40.0, 30.0}),
                    Sheet::line(Vec2{40.0, 30.0}, Vec2{0.0, 30.0}),
                    Sheet::line(Vec2{0.0, 30.0}, Vec2{0.0, 0.0})});

    const std::optional<Vec2> found = sheet.resolve(Vec2{39.4, 0.6},
                                                    ViewPointRole::Corner, 2.0);
    ASSERT_TRUE(found.has_value()) << "a corner was ambiguous with its own twin";
    EXPECT_NEAR(found->x, 150.0 + 40.0, 1e-6);
    EXPECT_NEAR(found->y, 150.0 + 0.0, 1e-6);
}

TEST(ViewAnchorTest, M43_ANCHOR_006_AMiddleIsItsOwnKindAndNotACorner) {
    // A midpoint dimension is a real thing -- "from the centre of this edge"
    // -- and it must not re-find an end of that edge, which is 20 mm away and
    // measures something else entirely.
    Sheet sheet;
    sheet.projects({Sheet::line(Vec2{0.0, 0.0}, Vec2{40.0, 0.0})});

    const std::optional<Vec2> middle = sheet.resolve(Vec2{20.5, 0.0},
                                                     ViewPointRole::Middle, 30.0);
    ASSERT_TRUE(middle.has_value());
    EXPECT_NEAR(middle->x, 150.0 + 20.0, 1e-6) << "a middle anchor found an end";

    // With a tolerance wide enough to reach both ends, a CORNER anchor at the
    // same place would be ambiguous between them -- and says so.
    EXPECT_FALSE(sheet.resolve(Vec2{20.0, 0.0}, ViewPointRole::Corner, 30.0).has_value())
        << "an anchor exactly between two corners chose one of them";
}

TEST(ViewAnchorTest, M43_ANCHOR_007_APointThatMovedMoreThanHalfTheToleranceStillResolves) {
    // THE BUG THE FIRST VERSION SHIPPED WITH, as a test rather than a memory.
    //
    // Starting the runner-up at the tolerance makes "no rival at all" and "a
    // rival exactly at the edge of reach" the same number. Every test above
    // has its point moving less than half the tolerance, where 2 x best is
    // still under the tolerance and the mistake does not show. This one puts
    // it at 3 mm of a 5 mm tolerance, where it does.
    Sheet sheet;
    sheet.projects({Sheet::circle(Vec2{10.0, 0.0}, 3.0)});

    const std::optional<Vec2> found = sheet.resolve(Vec2{13.0, 0.0},
                                                    ViewPointRole::Centre, 5.0);
    ASSERT_TRUE(found.has_value())
        << "a lone centre 3 mm away was refused as though something else were near it";
    EXPECT_NEAR(found->x, 150.0 + 10.0, 1e-6);
}

TEST(ViewAnchorTest, M43_ANCHOR_008_ALoneCandidateBeyondTheToleranceIsStillOutOfReach) {
    // The tolerance is a BOUND, and it has to be one on its own -- not because
    // some rival happens to be equally far. With one circle in the view there
    // is nothing to be ambiguous with, so if the bound stops working this is
    // the only thing that notices.
    Sheet sheet;
    sheet.projects({Sheet::circle(Vec2{10.0, 0.0}, 3.0)});

    EXPECT_FALSE(sheet.resolve(Vec2{40.0, 0.0}, ViewPointRole::Centre, 5.0).has_value())
        << "an anchor 30 mm from the only centre in the view adopted it anyway";
    // ...and the same point with a tolerance wide enough DOES reach it, so the
    // check above is about the bound and not about the point being unfindable.
    EXPECT_TRUE(sheet.resolve(Vec2{40.0, 0.0}, ViewPointRole::Centre, 40.0).has_value());
}

TEST(ViewAnchorTest, M43_ANCHOR_009_TheFourRolesAreFourDifferentNames) {
    // The names go in the file. Two that shared one would make a centre and a
    // corner the same thing on the way back in, which is the silent
    // re-attachment this whole mechanism exists to stop -- arriving through
    // the loader instead of through the search.
    const ViewPointRole all[] = {ViewPointRole::Corner, ViewPointRole::Middle,
                                 ViewPointRole::Centre, ViewPointRole::CurveEnd};
    for (const ViewPointRole a : all)
        for (const ViewPointRole b : all)
            if (a != b)
                EXPECT_NE(toString(a), toString(b))
                    << "two roles are written the same way";

    // ...and each name reads back as itself.
    for (const ViewPointRole role : all) {
        ViewPointRole read = ViewPointRole::CurveEnd;
        ASSERT_TRUE(ParseViewPointRole(toString(role), read));
        EXPECT_EQ(read, role);
    }

    // A NAME THIS BUILD DOES NOT KNOW IS REFUSED, and the value it was asked
    // to fill is left alone. Defaulted to Corner, a centre written by a newer
    // build would come back as a corner and measure something else.
    ViewPointRole untouched = ViewPointRole::Centre;
    EXPECT_FALSE(ParseViewPointRole("tangent", untouched));
    EXPECT_EQ(untouched, ViewPointRole::Centre) << "a failed parse changed the value anyway";
}

TEST(ViewAnchorTest, M43_ANCHOR_010_TheRoleSurvivesASaveAndAnUnknownOneIsREFUSED) {
    Sheet sheet;
    sheet.projects({Sheet::line(Vec2{0.0, 0.0}, Vec2{40.0, 0.0}),
                    Sheet::circle(Vec2{10.0, 0.0}, 3.0)});
    const DrawingDimension& dimension = sheet.document.addDimension(
        DimensionKind::Diameter,
        DimensionAnchor::inView(sheet.view, Vec2{10.0, 0.0}, ViewPointRole::Centre, 5.0),
        DimensionAnchor::inView(sheet.view, Vec2{13.0, 0.0}, ViewPointRole::CurveEnd, 5.0),
        Vec2{150.0, 120.0});
    const ObjectId id = dimension.id();

    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(sheet.document, out));
    std::string text = out.str();
    ASSERT_NE(text.find("\"role\": \"centre\""), std::string::npos)
        << "the role was never written, so a reopened drawing forgets what kind of point "
           "the dimension was on";

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const DrawingDimension* back = loaded.document->findDimension(id);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->first().role, ViewPointRole::Centre);
    EXPECT_EQ(back->second().role, ViewPointRole::CurveEnd)
        << "both anchors came back with the same role";

    // A ROLE THIS BUILD DOES NOT KNOW IS REFUSED, not quietly turned into a
    // corner -- which would make the dimension measure to a different point
    // with nothing said.
    const std::size_t at = text.find("\"role\": \"centre\"");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, std::string("\"role\": \"centre\"").size(), "\"role\": \"tangent\"");
    std::istringstream broken(text);
    EXPECT_FALSE(loadDrawingDocument(broken))
        << "a role this build does not know was read as one it does";
}

} // namespace
