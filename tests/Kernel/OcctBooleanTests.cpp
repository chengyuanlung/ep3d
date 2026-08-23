// M8 Gate A: subtractShape at the kernel boundary, against real OCCT, with
// hand-computed volumes (M8 spec 7).
//
// Tools are built with extrudeProfile rather than createBox so they can be
// PLACED -- containment, disjointness and swallowing are all statements about
// where the tool sits, and a corner-anchored box cannot express them.

#include "Core/Sketch/SketchFrame.h"
#include "Core/Kernel/ProfileDefinition.h"
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


TEST(OcctBooleanTest, M10_KERNEL_010_MassPropertiesOfOverlappingAndDisjointFusesAreBothExact) {
    // ADR-M10-005 changed `calculateMassProperties` to sum PER SOLID after
    // GATE_P found the compound path returning a right volume with a 2% wrong
    // centroid. This pins that, and pins the case M10's self-validation listed
    // as the thing it was least sure of: an OVERLAPPING fuse, which produces one
    // solid and whose union volume is NOT the sum of the parts.
    //
    // THE INPUTS ARE EXTRUDED PRISMS, NOT BOXES, and that is the whole reason
    // this test discriminates. The first version used `createBox` and passed
    // even with the defect restored -- a fused pair of BOXES measures correctly
    // through the old compound path. Box and extruded prism have behaved
    // differently three times in this milestone; the failing case is always the
    // prism. A test built from the convenient primitive would have pinned
    // nothing.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    ProfilePlane plane;
    const SketchFrame worldXY = SketchFrame::WorldXY();
    plane.origin = worldXY.toWorld(Vec2{0.0, 0.0});
    plane.uAxis = worldXY.uAxis();
    plane.vAxis = worldXY.vAxis();
    plane.normal = worldXY.normal();
    profile.plane = plane;
    profile.segments = {ProfileLineSegment{Vec2{0, 0}, Vec2{100, 0}},
                        ProfileLineSegment{Vec2{100, 0}, Vec2{100, 50}},
                        ProfileLineSegment{Vec2{100, 50}, Vec2{0, 50}},
                        ProfileLineSegment{Vec2{0, 50}, Vec2{0, 0}}};
    const ShapeResult prism = kernel.extrudeProfile(profile, 20.0);
    ASSERT_TRUE(prism) << prism.message;

    // OVERLAPPING: shifted 50 in X, so the union spans x 0..150.
    //   volume = 150 * 50 * 20 = 150000   (NOT 200000)
    //   COM    = (75, 25, 10)
    const ShapeResult shifted = kernel.translateShape(prism.shape, Vec3{50.0, 0.0, 0.0});
    ASSERT_TRUE(shifted) << shifted.message;
    const ShapeResult overlapping = kernel.fuseShapes(prism.shape, shifted.shape);
    ASSERT_TRUE(overlapping) << overlapping.message;
    const KernelMassPropertiesResult overlapProps =
        kernel.calculateMassProperties(overlapping.shape);
    ASSERT_TRUE(overlapProps) << overlapProps.message;
    EXPECT_NEAR(overlapProps.properties.volumeMm3, 150000.0, 1e-6);
    EXPECT_NEAR(overlapProps.properties.centerOfMassMm.x, 75.0, 1e-6);
    EXPECT_NEAR(overlapProps.properties.centerOfMassMm.y, 25.0, 1e-6);
    EXPECT_NEAR(overlapProps.properties.centerOfMassMm.z, 10.0, 1e-6);

    // DISJOINT: 200 apart -- the case that found the defect. Two lumps, and the
    // expected centroid deliberately does NOT sit on either lump's centre,
    // because that is where the old error cancelled and hid itself.
    const ShapeResult far = kernel.translateShape(prism.shape, Vec3{200.0, 0.0, 0.0});
    ASSERT_TRUE(far) << far.message;
    const ShapeResult disjoint = kernel.fuseShapes(prism.shape, far.shape);
    ASSERT_TRUE(disjoint) << disjoint.message;
    const KernelMassPropertiesResult disjointProps =
        kernel.calculateMassProperties(disjoint.shape);
    ASSERT_TRUE(disjointProps) << disjointProps.message;
    EXPECT_NEAR(disjointProps.properties.volumeMm3, 200000.0, 1e-6);
    EXPECT_NEAR(disjointProps.properties.centerOfMassMm.x, 150.0, 1e-6);
    EXPECT_NEAR(disjointProps.properties.centerOfMassMm.y, 25.0, 1e-6);
    EXPECT_NEAR(disjointProps.properties.centerOfMassMm.z, 10.0, 1e-6);
}

} // namespace
