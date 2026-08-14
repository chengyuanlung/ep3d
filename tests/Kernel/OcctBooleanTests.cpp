// M8 Gate A: subtractShape at the kernel boundary, against real OCCT, with
// hand-computed volumes (M8 spec 7).
//
// Tools are built with extrudeProfile rather than createBox so they can be
// PLACED -- containment, disjointness and swallowing are all statements about
// where the tool sits, and a corner-anchored box cannot express them.

#include "Kernel/Occt/OcctGeometryKernel.h"
#include <gtest/gtest.h>
#include <cmath>

namespace {

using namespace paramcad;

PlanarProfileDefinition RectangleProfile(double x0, double y0, double x1, double y1) {
    PlanarProfileDefinition profile;
    profile.plane.origin = Vec3{0, 0, 0};
    profile.plane.uAxis = Vec3{1, 0, 0};
    profile.plane.vAxis = Vec3{0, 1, 0};
    profile.plane.normal = Vec3{0, 0, 1};
    profile.segments.push_back(ProfileLineSegment{Vec2{x0, y0}, Vec2{x1, y0}});
    profile.segments.push_back(ProfileLineSegment{Vec2{x1, y0}, Vec2{x1, y1}});
    profile.segments.push_back(ProfileLineSegment{Vec2{x1, y1}, Vec2{x0, y1}});
    profile.segments.push_back(ProfileLineSegment{Vec2{x0, y1}, Vec2{x0, y0}});
    return profile;
}

double VolumeOf(OcctGeometryKernel& kernel, const KernelShape& shape) {
    const KernelMassPropertiesResult result = kernel.calculateMassProperties(shape);
    EXPECT_TRUE(result) << result.message;
    return result.properties.volumeMm3;
}

TEST(M8OcctBoolean, GATE_A_ContainedToolRemovesExactlyItsVolume) {
    OcctGeometryKernel kernel;
    ShapeResult base = kernel.extrudeProfile(RectangleProfile(0, 0, 100, 50), 20.0);
    ASSERT_TRUE(base) << base.message;
    ShapeResult tool = kernel.extrudeProfile(RectangleProfile(10, 10, 30, 40), 10.0);
    ASSERT_TRUE(tool) << tool.message;

    ShapeResult cut = kernel.subtractShape(base.shape, tool.shape);
    ASSERT_TRUE(cut) << cut.message;

    // 100*50*20 - 20*30*10 = 94000 mm^3, computed here from the profile
    // dimensions -- never read back from OCCT.
    EXPECT_NEAR(VolumeOf(kernel, cut.shape), 94000.0, 1e-6);
    // Neither operand was consumed: the base still measures 100000 (M8 spec 5,
    // "neither input is modified or invalidated").
    EXPECT_NEAR(VolumeOf(kernel, base.shape), 100000.0, 1e-6);
    EXPECT_NEAR(VolumeOf(kernel, tool.shape), 6000.0, 1e-6);
}

TEST(M8OcctBoolean, ADisjointToolLeavesTheBaseUnchanged) {
    OcctGeometryKernel kernel;
    ShapeResult base = kernel.extrudeProfile(RectangleProfile(0, 0, 100, 50), 20.0);
    ASSERT_TRUE(base) << base.message;
    ShapeResult tool = kernel.extrudeProfile(RectangleProfile(500, 500, 520, 530), 10.0);
    ASSERT_TRUE(tool) << tool.message;

    ShapeResult cut = kernel.subtractShape(base.shape, tool.shape);
    ASSERT_TRUE(cut) << cut.message;
    // LEGAL, not an error (M8 spec 6): a pocket dragged off the part is a
    // modelling state. The volume says the cut removed nothing.
    EXPECT_NEAR(VolumeOf(kernel, cut.shape), 100000.0, 1e-6);
}

TEST(M8OcctBoolean, ASwallowingToolLeavesAnEmptyResult) {
    OcctGeometryKernel kernel;
    ShapeResult base = kernel.extrudeProfile(RectangleProfile(10, 10, 30, 40), 10.0);
    ASSERT_TRUE(base) << base.message;
    ShapeResult tool = kernel.extrudeProfile(RectangleProfile(0, 0, 100, 50), 20.0);
    ASSERT_TRUE(tool) << tool.message;

    ShapeResult cut = kernel.subtractShape(base.shape, tool.shape);
    ASSERT_TRUE(cut) << cut.message;
    // Also legal: zero material left is an answer, not a failure.
    EXPECT_NEAR(VolumeOf(kernel, cut.shape), 0.0, 1e-9);
}

TEST(M8OcctBoolean, AForeignOrInvalidHandleIsRefusedNotDereferenced) {
    OcctGeometryKernel kernel;
    ShapeResult base = kernel.extrudeProfile(RectangleProfile(0, 0, 100, 50), 20.0);
    ASSERT_TRUE(base) << base.message;

    const ShapeResult nullTool = kernel.subtractShape(base.shape, KernelShape{});
    EXPECT_FALSE(nullTool);
    EXPECT_EQ(nullTool.error, KernelError::GeometryConstructionFailed);

    const ShapeResult nullBase = kernel.subtractShape(KernelShape{}, base.shape);
    EXPECT_FALSE(nullBase);
    EXPECT_EQ(nullBase.error, KernelError::GeometryConstructionFailed);
}

} // namespace
