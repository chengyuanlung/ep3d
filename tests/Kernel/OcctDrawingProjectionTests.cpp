// M32.2 -- hidden-line removal: the half of a drawing EasyCad could not help
// with, because it has no 3D to project.
//
// These tests measure the CURVES THAT CAME BACK, not the fact that a call
// returned true. A projector that answered success with an empty drawing, or
// with everything marked visible, would pass every check that only read a
// status -- and the failure would first be noticed as a drawing that is
// missing its holes.

#include "Core/Drawing/ProjectedGeometry.h"
#include "Core/Kernel/DrawingProjection.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/SketchFrame.h"
#include "Kernel/Occt/OcctDrawingProjection.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kTwoPi = 6.283185307179586476925286766559;

// A 100 x 40 x 10 block, lying on the world XY plane.
ShapeResult Block(OcctGeometryKernel& kernel, double width = 100.0, double depth = 40.0,
                  double height = 10.0) {
    PlanarProfileDefinition profile;
    profile.plane = PlaneOfSketchFrame(SketchFrame::WorldXY());
    profile.segments = {ProfileLineSegment{Vec2{0, 0}, Vec2{width, 0}},
                        ProfileLineSegment{Vec2{width, 0}, Vec2{width, depth}},
                        ProfileLineSegment{Vec2{width, depth}, Vec2{0, depth}},
                        ProfileLineSegment{Vec2{0, depth}, Vec2{0, 0}}};
    return kernel.extrudeProfile(profile, height);
}

// A cylinder standing on the world XY plane -- the shape whose SILHOUETTE is
// not an edge of the solid at all.
ShapeResult Cylinder(OcctGeometryKernel& kernel, double radius = 20.0, double height = 30.0) {
    PlanarProfileDefinition profile;
    profile.plane = PlaneOfSketchFrame(SketchFrame::WorldXY());
    profile.segments = {ProfileCircleSegment{Vec2{0, 0}, radius}};
    return kernel.extrudeProfile(profile, height);
}

DrawingProjectionRequest LookingFrom(Vec3 towards, Vec3 up) {
    DrawingProjectionRequest request;
    request.towards = towards;
    request.up = up;
    return request;
}

std::size_t CountOf(const ProjectedDrawing& drawing, ProjectedVisibility visibility) {
    std::size_t count = 0;
    for (const ProjectedCurve& curve : drawing.curves)
        if (curve.visibility == visibility) ++count;
    return count;
}

std::size_t CountOf(const ProjectedDrawing& drawing, ProjectedEdgeKind kind) {
    std::size_t count = 0;
    for (const ProjectedCurve& curve : drawing.curves)
        if (curve.kind == kind) ++count;
    return count;
}

std::size_t CountArcs(const ProjectedDrawing& drawing) {
    std::size_t count = 0;
    for (const ProjectedCurve& curve : drawing.curves)
        if (std::holds_alternative<ProjectedArc>(curve.shape)) ++count;
    return count;
}

} // namespace

TEST(OcctDrawingProjectionTest, M32_HLR_001_ABlockSeenFromTheFrontIsARectangleTheRightSize) {
    // THE FIRST QUESTION A DRAWING HAS TO ANSWER: is the thing on the paper
    // the size of the thing in the model? Measured as the extent, in MODEL
    // millimetres -- the scale is applied later and elsewhere, which is the
    // decision this whole block rests on.
    OcctGeometryKernel kernel;
    const ShapeResult block = Block(kernel, 100.0, 40.0, 10.0);
    ASSERT_TRUE(block) << block.message;

    // Front: looking along +Y, up is +Z. So the page shows X across and Z up:
    // 100 wide and 10 tall.
    const DrawingProjectionResult projected = ProjectShapeForDrawing(
        block.shape, LookingFrom(Vec3{0, 1, 0}, Vec3{0, 0, 1}));
    ASSERT_TRUE(projected) << projected.message;

    EXPECT_NEAR(projected.drawing.extent.widthMm(), 100.0, 1e-6)
        << "the front view is not as wide as the part";
    EXPECT_NEAR(projected.drawing.extent.heightMm(), 10.0, 1e-6)
        << "the front view is not as tall as the part";
}

TEST(OcctDrawingProjectionTest, M32_HLR_002_TheSAMEBlockFromAboveIsADifferentRectangle) {
    // ...or the test above proves only that some numbers came back. From the
    // top the page shows X across and Y up: 100 by 40.
    OcctGeometryKernel kernel;
    const ShapeResult block = Block(kernel, 100.0, 40.0, 10.0);
    ASSERT_TRUE(block) << block.message;

    const DrawingProjectionResult projected = ProjectShapeForDrawing(
        block.shape, LookingFrom(Vec3{0, 0, -1}, Vec3{0, 1, 0}));
    ASSERT_TRUE(projected) << projected.message;
    EXPECT_NEAR(projected.drawing.extent.widthMm(), 100.0, 1e-6);
    EXPECT_NEAR(projected.drawing.extent.heightMm(), 40.0, 1e-6)
        << "the top view is the depth of the part, not its height";
}

TEST(OcctDrawingProjectionTest, M32_HLR_003_HiddenEdgesAreFOUNDAndAreMarkedHidden) {
    // A projector that returned every edge as visible would draw a correct
    // wireframe and a wrong DRAWING -- and it would look plausible. What is
    // asked here is that the back of the block came back MARKED, because
    // "hidden" is what makes the line dashed.
    OcctGeometryKernel kernel;
    const ShapeResult block = Block(kernel);
    ASSERT_TRUE(block) << block.message;

    DrawingProjectionRequest request = LookingFrom(Vec3{0, 1, 0}, Vec3{0, 0, 1});
    request.includeHidden = true;
    const DrawingProjectionResult projected = ProjectShapeForDrawing(block.shape, request);
    ASSERT_TRUE(projected) << projected.message;

    EXPECT_GT(CountOf(projected.drawing, ProjectedVisibility::Visible), 0u);
    EXPECT_GT(CountOf(projected.drawing, ProjectedVisibility::Hidden), 0u)
        << "a solid block projected with no hidden edges at all";
}

TEST(OcctDrawingProjectionTest, M32_HLR_004_AskingForNoHiddenEdgesGivesNone) {
    // The switch is asked FOR rather than applied afterwards, because
    // hidden-line removal is the expensive operation here and computing edges
    // nobody will draw is work thrown away.
    OcctGeometryKernel kernel;
    const ShapeResult block = Block(kernel);
    ASSERT_TRUE(block) << block.message;

    DrawingProjectionRequest request = LookingFrom(Vec3{0, 1, 0}, Vec3{0, 0, 1});
    request.includeHidden = false;
    const DrawingProjectionResult projected = ProjectShapeForDrawing(block.shape, request);
    ASSERT_TRUE(projected) << projected.message;

    EXPECT_EQ(CountOf(projected.drawing, ProjectedVisibility::Hidden), 0u)
        << "hidden edges came back from a request that did not ask for them";
    EXPECT_GT(CountOf(projected.drawing, ProjectedVisibility::Visible), 0u)
        << "...and the visible ones went with them";
}

TEST(OcctDrawingProjectionTest, M32_HLR_005_ACylinderSeenDownItsAxisIsACIRCLENotAPolygon) {
    // THE REASON THIS USES THE EXACT PROJECTOR. A diameter dimension attaches
    // to a circle; a circle that arrived as a 40-segment polygon has no centre
    // and no radius to attach to, and DXF would write a polyline where every
    // other program expects a CIRCLE.
    OcctGeometryKernel kernel;
    const ShapeResult cylinder = Cylinder(kernel, 20.0, 30.0);
    ASSERT_TRUE(cylinder) << cylinder.message;

    // Down the axis: looking along -Z, so the round face is square-on.
    const DrawingProjectionResult projected = ProjectShapeForDrawing(
        cylinder.shape, LookingFrom(Vec3{0, 0, -1}, Vec3{0, 1, 0}));
    ASSERT_TRUE(projected) << projected.message;

    ASSERT_GT(CountArcs(projected.drawing), 0u)
        << "a cylinder seen down its axis came back with no circle in it";
    bool sawTheRightRadius = false;
    for (const ProjectedCurve& curve : projected.drawing.curves) {
        const auto* arc = std::get_if<ProjectedArc>(&curve.shape);
        if (arc == nullptr) continue;
        if (std::fabs(arc->radius - 20.0) < 1e-6) sawTheRightRadius = true;
    }
    EXPECT_TRUE(sawTheRightRadius) << "the circle that came back is not the cylinder's";
    EXPECT_NEAR(projected.drawing.extent.widthMm(), 40.0, 1e-6);
}

TEST(OcctDrawingProjectionTest, M32_HLR_006_ACylinderSeenSIDEWAYSHasASilhouette) {
    // A silhouette is NOT an edge of the solid: it exists only for this
    // direction of sight and moves when the view turns. A projector that
    // returned only real edges would draw a cylinder as two circles and no
    // sides -- which looks like a drawing until you try to dimension it.
    OcctGeometryKernel kernel;
    const ShapeResult cylinder = Cylinder(kernel, 20.0, 30.0);
    ASSERT_TRUE(cylinder) << cylinder.message;

    const DrawingProjectionResult projected = ProjectShapeForDrawing(
        cylinder.shape, LookingFrom(Vec3{0, 1, 0}, Vec3{0, 0, 1}));
    ASSERT_TRUE(projected) << projected.message;

    EXPECT_GT(CountOf(projected.drawing, ProjectedEdgeKind::Outline), 0u)
        << "a cylinder seen side-on has no silhouette, so it would draw as two circles";
    // ...and it is the right size: 40 across, 30 tall.
    EXPECT_NEAR(projected.drawing.extent.widthMm(), 40.0, 1e-6);
    EXPECT_NEAR(projected.drawing.extent.heightMm(), 30.0, 1e-6);
}

TEST(OcctDrawingProjectionTest, M32_HLR_007_AnUpThatLeansIntoTheSightLineIsREFUSED) {
    // Silently straightening it would rotate the view by an amount nobody
    // asked for. The six standard directions each carry an up that already
    // agrees with them, so a caller reaching this has made a mistake worth
    // hearing about.
    OcctGeometryKernel kernel;
    const ShapeResult block = Block(kernel);
    ASSERT_TRUE(block) << block.message;

    const DrawingProjectionResult projected = ProjectShapeForDrawing(
        block.shape, LookingFrom(Vec3{0, 1, 0}, Vec3{0, 1, 0}));
    EXPECT_FALSE(projected);
    EXPECT_NE(projected.message.find("leans into"), std::string::npos) << projected.message;
}

TEST(OcctDrawingProjectionTest, M32_HLR_008_ProjectingNothingIsARefusalNotAnEmptyDrawing) {
    // An empty result is indistinguishable from a part with nothing in it, and
    // the caller's next move -- put a dimension on it -- would then be made
    // against blank paper.
    OcctGeometryKernel kernel;
    const DrawingProjectionResult projected =
        ProjectShapeForDrawing(KernelShape{}, LookingFrom(Vec3{0, 1, 0}, Vec3{0, 0, 1}));
    EXPECT_FALSE(projected);
    EXPECT_FALSE(projected.message.empty());
}

TEST(OcctDrawingProjectionTest, M32_HLR_009_TheExtentOfAnArcIsTheArcNotItsWholeCircle) {
    // A quarter arc of a 50 mm circle is 50 wide only if it happens to cross
    // the axis. Taking the whole circle's box would make every view claim more
    // paper than it uses, which reads as "this does not fit" on a sheet where
    // it plainly does.
    ProjectedExtent extent;
    ProjectedCurve quarter;
    // Centre at the origin, radius 50, sweeping 0 to 90 degrees: it occupies
    // the first quadrant only, so 50 x 50 with its corner at the centre.
    quarter.shape = ProjectedArc{Vec2{0.0, 0.0}, 50.0, 0.0, kTwoPi / 4.0, false};
    GrowExtent(extent, quarter);
    EXPECT_NEAR(extent.min.x, 0.0, 1e-9);
    EXPECT_NEAR(extent.min.y, 0.0, 1e-9);
    EXPECT_NEAR(extent.max.x, 50.0, 1e-9);
    EXPECT_NEAR(extent.max.y, 50.0, 1e-9);

    // ...and a FULL circle does take its whole box.
    ProjectedExtent whole;
    ProjectedCurve circle;
    circle.shape = ProjectedArc{Vec2{0.0, 0.0}, 50.0, 0.0, kTwoPi, true};
    GrowExtent(whole, circle);
    EXPECT_NEAR(whole.min.x, -50.0, 1e-9);
    EXPECT_NEAR(whole.max.y, 50.0, 1e-9);
}
