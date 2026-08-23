// M17.7 -- drawing a sketch in the part view, against a real kernel.
//
// The risk this guards is narrow and severe: a sketch on a raised or tilted
// plane drawn FLAT AT THE WORLD ORIGIN. On the 2D canvas it looks perfect --
// the canvas works in (u,v) and never asks where the plane is -- so the only
// place the mistake shows is the 3D view, and the only way to check it without
// a display is to ask the kernel where the geometry ended up.
//
// That is what BoundsOf is for: a kernel-neutral door onto "where is it", so a
// test can make the claim without naming an OCCT type (ADR-M4-004).

#include "Core/Sketch/Profile.h"
#include "Core/Sketch/SketchFrame.h"
#include "Kernel/Occt/OcctSketchWireframe.h"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

ProfilePlane WorldXY() { return PlaneOfSketchFrame(SketchFrame::WorldXY()); }

// The plane of the top face of a 20 mm box: z = 20, u = +X, v = +Y.
ProfilePlane RaisedXY(double z) {
    const std::optional<SketchFrame> frame =
        SketchFrame::FromBasis(Vec3{0, 0, z}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    EXPECT_TRUE(frame.has_value());
    return PlaneOfSketchFrame(frame.value_or(SketchFrame::WorldXY()));
}

std::vector<SketchGeometry> Rectangle(double w, double h) {
    return {SketchLine{Vec2{0, 0}, Vec2{w, 0}}, SketchLine{Vec2{w, 0}, Vec2{w, h}},
            SketchLine{Vec2{w, h}, Vec2{0, h}}, SketchLine{Vec2{0, h}, Vec2{0, 0}}};
}

void ExpectBounds(const KernelBounds& bounds, Vec3 min, Vec3 max, const char* what) {
    ASSERT_TRUE(bounds.ok) << what;
    EXPECT_NEAR(bounds.min.x, min.x, 1e-6) << what << " min.x";
    EXPECT_NEAR(bounds.min.y, min.y, 1e-6) << what << " min.y";
    EXPECT_NEAR(bounds.min.z, min.z, 1e-6) << what << " min.z";
    EXPECT_NEAR(bounds.max.x, max.x, 1e-6) << what << " max.x";
    EXPECT_NEAR(bounds.max.y, max.y, 1e-6) << what << " max.y";
    EXPECT_NEAR(bounds.max.z, max.z, 1e-6) << what << " max.z";
}

} // namespace

TEST(OcctSketchWireframeTest, M17_WIRE_001_EveryEntityBecomesAnEdgeOrAVertex) {
    std::vector<SketchGeometry> geometry = Rectangle(40.0, 30.0);
    geometry.push_back(SketchCircle{Vec2{20, 15}, 5.0});
    geometry.push_back(SketchArc{Vec2{5, 5}, 3.0, 0.0, kPi / 2.0, true});
    geometry.push_back(SketchPoint{Vec2{0, 0}});

    const SketchWireframe wireframe = BuildSketchWireframe(geometry, WorldXY());
    EXPECT_EQ(wireframe.edges, 6); // four sides, a circle, an arc
    EXPECT_EQ(wireframe.vertices, 1);
    EXPECT_EQ(wireframe.skipped, 0);
    EXPECT_TRUE(wireframe.shape.isValid());
}

TEST(OcctSketchWireframeTest, M17_WIRE_002_ASketchOnARaisedPlaneIsDrawnATThatPlane) {
    // THE test. A wireframe built in (u,v) and handed to the viewer without its
    // plane applied lands at z = 0 -- flat on the world floor, under the solid
    // it belongs to, and perfectly convincing from directly above.
    const SketchWireframe wireframe = BuildSketchWireframe(Rectangle(40.0, 30.0), RaisedXY(20.0));
    ASSERT_TRUE(wireframe.shape.isValid());
    ExpectBounds(BoundsOf(wireframe.shape), Vec3{0, 0, 20}, Vec3{40, 30, 20}, "raised rectangle");
}

TEST(OcctSketchWireframeTest, M17_WIRE_003_ASketchOnAVerticalPlaneKeepsItsOwnAxes) {
    // The +X face of a box: u is world +Y, v is world +Z. A wireframe that
    // assumed world axes would draw this rectangle lying down instead of
    // standing up, and it would still look like a rectangle.
    const std::optional<SketchFrame> frame =
        SketchFrame::FromBasis(Vec3{40, 0, 0}, Vec3{0, 1, 0}, Vec3{1, 0, 0});
    ASSERT_TRUE(frame.has_value());

    const SketchWireframe wireframe =
        BuildSketchWireframe(Rectangle(30.0, 20.0), PlaneOfSketchFrame(*frame));
    ASSERT_TRUE(wireframe.shape.isValid());
    // 30 along u (world +Y) and 20 along v (world +Z), all at x = 40.
    ExpectBounds(BoundsOf(wireframe.shape), Vec3{40, 0, 0}, Vec3{40, 30, 20}, "vertical sketch");
}

TEST(OcctSketchWireframeTest, M17_WIRE_004_ACircleIsDrawnAtItsOwnRadiusOnTheSketchPlane) {
    const SketchWireframe wireframe =
        BuildSketchWireframe({SketchCircle{Vec2{20, 15}, 5.0}}, RaisedXY(20.0));
    ASSERT_TRUE(wireframe.shape.isValid());
    EXPECT_EQ(wireframe.edges, 1);
    ExpectBounds(BoundsOf(wireframe.shape), Vec3{15, 10, 20}, Vec3{25, 20, 20}, "circle");
}

TEST(OcctSketchWireframeTest, M17_WIRE_005_AnArcIsDrawnOnTheSideOfItsChordItWasStoredOn) {
    // A quarter arc from 0 to 90 degrees about (0,0), radius 10. Counter-
    // clockwise it sweeps the FIRST quadrant, so it spans x in [0,10] and
    // y in [0,10]. Drawn the other way round it would sweep the other three
    // quadrants and span [-10,10] in both -- the same two endpoints, a
    // completely different curve, and nothing but the bounds can tell them
    // apart.
    const SketchWireframe ccw =
        BuildSketchWireframe({SketchArc{Vec2{0, 0}, 10.0, 0.0, kPi / 2.0, true}}, WorldXY());
    ASSERT_TRUE(ccw.shape.isValid());
    ExpectBounds(BoundsOf(ccw.shape), Vec3{0, 0, 0}, Vec3{10, 10, 0}, "counter-clockwise arc");

    const SketchWireframe cw =
        BuildSketchWireframe({SketchArc{Vec2{0, 0}, 10.0, 0.0, kPi / 2.0, false}}, WorldXY());
    ASSERT_TRUE(cw.shape.isValid());
    ExpectBounds(BoundsOf(cw.shape), Vec3{-10, -10, 0}, Vec3{10, 10, 0}, "clockwise arc");
}

TEST(OcctSketchWireframeTest, M17_WIRE_006_DegenerateEntitiesAreSkippedAndCOUNTED) {
    // A zero-length line is refused by OCCT outright. Losing it silently would
    // make the picture disagree with the sketch by one entity, with nothing
    // anywhere saying so.
    const std::vector<SketchGeometry> geometry = {SketchLine{Vec2{5, 5}, Vec2{5, 5}},
                                                  SketchCircle{Vec2{0, 0}, 0.0},
                                                  SketchLine{Vec2{0, 0}, Vec2{10, 0}}};
    const SketchWireframe wireframe = BuildSketchWireframe(geometry, WorldXY());
    EXPECT_EQ(wireframe.edges, 1);
    EXPECT_EQ(wireframe.skipped, 2);
    EXPECT_TRUE(wireframe.shape.isValid()); // the one good line still draws
}

TEST(OcctSketchWireframeTest, M17_WIRE_007_AnEmptySketchYieldsNoShapeRatherThanAnEmptyOne) {
    // An empty compound displays as a presentation with nothing in it: a scene
    // object a user can select and cannot see.
    const SketchWireframe wireframe = BuildSketchWireframe({}, WorldXY());
    EXPECT_TRUE(wireframe.empty());
    EXPECT_FALSE(wireframe.shape.isValid());
    EXPECT_FALSE(BoundsOf(wireframe.shape).ok);
}

TEST(OcctSketchWireframeTest, M17_WIRE_008_BoundsOfAForeignOrEmptyHandleIsNotAnAnswerAtTheOrigin) {
    // `ok` false, not a zero-sized box at (0,0,0). The two are different
    // claims, and a caller that could not tell them apart would place an
    // absent sketch at the origin.
    const KernelBounds bounds = BoundsOf(KernelShape{});
    EXPECT_FALSE(bounds.ok);
}
