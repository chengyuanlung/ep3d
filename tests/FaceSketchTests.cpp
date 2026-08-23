// M17.5 -- sketching on a picked face, the half that needs no display.
//
// The question every test here asks is the same one: given a plane, does a
// point drawn at sketch (u,v) land where a user pointed? A frame that is
// almost right is the worst outcome available -- the sketch looks correct on
// screen, and the solid it pads comes out somewhere else.

#include "Core/Sketch/SketchFrame.h"
#include "Viewer/FaceSketch.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace {

using namespace paramcad;

constexpr double kTol = 1e-9;

void ExpectVec(Vec3 actual, Vec3 expected, const char* what) {
    EXPECT_NEAR(actual.x, expected.x, 1e-9) << what << ".x";
    EXPECT_NEAR(actual.y, expected.y, 1e-9) << what << ".y";
    EXPECT_NEAR(actual.z, expected.z, 1e-9) << what << ".z";
}

PickedFace Planar(Vec3 point, Vec3 normal) {
    PickedFace face;
    face.isFace = true;
    face.planar = true;
    face.point = point;
    face.normal = normal;
    return face;
}

} // namespace

// --- The frame maths --------------------------------------------------------

TEST(SketchFrameBasisTest, M17_FRAME_001_ABasisBecomesAFrameThatReproducesIt) {
    const std::optional<SketchFrame> frame =
        SketchFrame::FromBasis(Vec3{5, 6, 7}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    ASSERT_TRUE(frame.has_value());
    ExpectVec(frame->uAxis(), Vec3{1, 0, 0}, "u");
    ExpectVec(frame->vAxis(), Vec3{0, 1, 0}, "v");
    ExpectVec(frame->normal(), Vec3{0, 0, 1}, "n");
    // The origin is where the frame says it is, which is what toWorld(0,0) is.
    ExpectVec(frame->toWorld(Vec2{0, 0}), Vec3{5, 6, 7}, "origin");
}

TEST(SketchFrameBasisTest, M17_FRAME_002_ASkewUAxisIsSquaredUpNotTrusted) {
    // u leans 45 degrees out of the plane. The frame must still be a rotation:
    // a basis that is not orthonormal puts every sketch point off the plane.
    const std::optional<SketchFrame> frame =
        SketchFrame::FromBasis(Vec3{}, Vec3{1, 0, 1}, Vec3{0, 0, 1});
    ASSERT_TRUE(frame.has_value());
    ExpectVec(frame->uAxis(), Vec3{1, 0, 0}, "u");
    ExpectVec(frame->normal(), Vec3{0, 0, 1}, "n");
    // Orthonormal, checked as such rather than inferred from the axes above.
    const Vec3 u = frame->uAxis();
    const Vec3 v = frame->vAxis();
    const Vec3 n = frame->normal();
    EXPECT_NEAR(u.x * v.x + u.y * v.y + u.z * v.z, 0.0, kTol);
    EXPECT_NEAR(u.x * n.x + u.y * n.y + u.z * n.z, 0.0, kTol);
    EXPECT_NEAR(std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z), 1.0, kTol);
}

TEST(SketchFrameBasisTest, M17_FRAME_003_TheUpsideDownFaceIsTheOneTheMathsMustNotDropOn) {
    // A 180-degree rotation is where a single-formula quaternion conversion
    // divides by zero. The bottom face of every box is exactly that case.
    const std::optional<SketchFrame> frame =
        SketchFrame::FromBasis(Vec3{0, 0, -10}, Vec3{1, 0, 0}, Vec3{0, 0, -1});
    ASSERT_TRUE(frame.has_value());
    ExpectVec(frame->normal(), Vec3{0, 0, -1}, "n");
    ExpectVec(frame->uAxis(), Vec3{1, 0, 0}, "u");
    ExpectVec(frame->vAxis(), Vec3{0, -1, 0}, "v"); // right-handed with n = -Z
    ExpectVec(frame->toWorld(Vec2{2, 3}), Vec3{2, -3, -10}, "a point on it");
}

TEST(SketchFrameBasisTest, M17_FRAME_004_ADegenerateBasisIsREFUSED) {
    // u parallel to the normal leaves no direction in the plane. Every one of
    // these has a plausible-looking wrong answer available (world XY), and
    // returning it would put a sketch somewhere the user did not pick.
    EXPECT_FALSE(SketchFrame::FromBasis(Vec3{}, Vec3{0, 0, 1}, Vec3{0, 0, 1}).has_value());
    EXPECT_FALSE(SketchFrame::FromBasis(Vec3{}, Vec3{0, 0, 0}, Vec3{0, 0, 1}).has_value());
    EXPECT_FALSE(SketchFrame::FromBasis(Vec3{}, Vec3{1, 0, 0}, Vec3{0, 0, 0}).has_value());
    const double nan = std::nan("");
    EXPECT_FALSE(SketchFrame::FromBasis(Vec3{}, Vec3{1, 0, 0}, Vec3{nan, 0, 1}).has_value());
    EXPECT_FALSE(
        SketchFrame::FromBasis(Vec3{nan, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}).has_value());
}

// --- The pick, judged -------------------------------------------------------

TEST(FaceSketchTest, M17_FACE_001_TheTopOfABoxSketchesLikeTheWorldPlaneButRaised) {
    // The first face anybody clicks. u and v must come out as world X and Y,
    // because a sketch that reads sideways on the top of a box is a sketch
    // every subsequent dimension is confusing on.
    const FaceSketchPlan plan = PlanSketchOnFace(Planar(Vec3{30, 40, 20}, Vec3{0, 0, 1}));
    ASSERT_TRUE(plan.ok) << plan.message;
    ExpectVec(plan.frame.uAxis(), Vec3{1, 0, 0}, "u");
    ExpectVec(plan.frame.vAxis(), Vec3{0, 1, 0}, "v");
    // The ORIGIN is the part origin projected onto the plane -- (0,0,20) --
    // not the point the kernel happened to hand back, which was (30,40,20).
    ExpectVec(plan.frame.toWorld(Vec2{0, 0}), Vec3{0, 0, 20}, "origin");
    ExpectVec(plan.frame.toWorld(Vec2{10, 5}), Vec3{10, 5, 20}, "a point on it");
}

TEST(FaceSketchTest, M17_FACE_002_TheORIGINDoesNotDependOnWhereOnTheFaceItWasPicked) {
    // Two clicks on the same face, far apart. A frame that used the picked
    // point as its origin would give the same drawing two different positions,
    // and no dimension measured from the origin would mean anything.
    const FaceSketchPlan nearPick = PlanSketchOnFace(Planar(Vec3{1, 1, 20}, Vec3{0, 0, 1}));
    const FaceSketchPlan farPick = PlanSketchOnFace(Planar(Vec3{900, -700, 20}, Vec3{0, 0, 1}));
    ASSERT_TRUE(nearPick.ok);
    ASSERT_TRUE(farPick.ok);
    ExpectVec(farPick.frame.toWorld(Vec2{7, 8}), nearPick.frame.toWorld(Vec2{7, 8}),
              "same point");
}

TEST(FaceSketchTest, M17_FACE_003_OnAVerticalFaceVPointsUp) {
    // The rule that makes a side face usable: whatever the wall faces, up on
    // the sketch is up in the world.
    const Vec3 normals[] = {Vec3{0, 1, 0}, Vec3{0, -1, 0}, Vec3{1, 0, 0}, Vec3{-1, 0, 0}};
    for (Vec3 normal : normals) {
        const FaceSketchPlan plan = PlanSketchOnFace(Planar(Vec3{5, 5, 5}, normal));
        ASSERT_TRUE(plan.ok) << plan.message;
        ExpectVec(plan.frame.vAxis(), Vec3{0, 0, 1}, "v");
        ExpectVec(plan.frame.normal(), normal, "n");
    }
}

TEST(FaceSketchTest, M17_FACE_004_TheNORMALIsPreservedSoAPadLeavesTheSolid) {
    // The sketch normal is the pad direction (spec 12). Flipping it would pad
    // INTO the part the user just clicked, which looks like nothing happening.
    const FaceSketchPlan plan = PlanSketchOnFace(Planar(Vec3{0, 0, -3}, Vec3{0, 0, -1}));
    ASSERT_TRUE(plan.ok) << plan.message;
    ExpectVec(plan.frame.normal(), Vec3{0, 0, -1}, "n");
}

TEST(FaceSketchTest, M17_FACE_005_ATiltedFaceKeepsItsPlane) {
    // 45 degrees about X: n = (0, -1, 1)/sqrt(2). Every sketch point must land
    // ON that plane, which is the property being checked -- not the axes.
    const double s = 1.0 / std::sqrt(2.0);
    const Vec3 n{0.0, -s, s};
    const FaceSketchPlan plan = PlanSketchOnFace(Planar(Vec3{0, 0, 10}, n));
    ASSERT_TRUE(plan.ok) << plan.message;
    const Vec3 point = plan.frame.toWorld(Vec2{3, 4});
    const Vec3 origin = plan.frame.toWorld(Vec2{0, 0});
    const Vec3 delta{point.x - origin.x, point.y - origin.y, point.z - origin.z};
    EXPECT_NEAR(delta.x * n.x + delta.y * n.y + delta.z * n.z, 0.0, kTol);
    // And it is 5 from the origin, because (3,4) is.
    EXPECT_NEAR(std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z), 5.0, kTol);
}

TEST(FaceSketchTest, M17_FACE_006_ACurvedFaceIsRefusedInWordsAUserCanAct) {
    PickedFace cylinder;
    cylinder.isFace = true;
    cylinder.planar = false;
    cylinder.point = Vec3{1, 2, 3};
    cylinder.normal = Vec3{0, 0, 1};
    const FaceSketchPlan plan = PlanSketchOnFace(cylinder);
    EXPECT_FALSE(plan.ok);
    EXPECT_NE(plan.message.find("curved"), std::string::npos) << plan.message;
}

TEST(FaceSketchTest, M17_FACE_007_EveryRefusalSaysSomething) {
    // A silent refusal is the defect this project keeps re-learning: the user
    // presses the button, nothing happens, and there is nothing to read.
    PickedFace nothing;
    PickedFace curved = Planar(Vec3{}, Vec3{0, 0, 1});
    curved.planar = false;
    const PickedFace zeroNormal = Planar(Vec3{}, Vec3{0, 0, 0});
    const PickedFace notFinite = Planar(Vec3{}, Vec3{std::nan(""), 0, 1});

    for (const PickedFace& face : {nothing, curved, zeroNormal, notFinite}) {
        const FaceSketchPlan plan = PlanSketchOnFace(face);
        EXPECT_FALSE(plan.ok);
        EXPECT_FALSE(plan.message.empty());
    }
}

TEST(FaceSketchTest, M17_FACE_008_SuccessSaysTheSketchDoesNotFollowTheFace) {
    // The limit is part of the feature, so it is part of what the user is told
    // at the moment they use it -- not a line in a document nobody opens.
    const FaceSketchPlan plan = PlanSketchOnFace(Planar(Vec3{0, 0, 20}, Vec3{0, 0, 1}));
    ASSERT_TRUE(plan.ok);
    EXPECT_NE(plan.message.find("does not follow"), std::string::npos) << plan.message;
}
