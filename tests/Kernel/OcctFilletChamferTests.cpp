// M8.3: fillet / chamfer at the kernel boundary, against real
// OCCT, with hand-computed volumes.
//
// FILLET oracle -- the Minkowski rounded box. Filleting every edge of a
// w x h x d box at radius r (with OCCT's spherical corner patches) is the
// Minkowski sum of the shrunken (w-2r)(h-2r)(d-2r) box with a radius-r ball:
//
//   V = (w-2r)(h-2r)(d-2r)                              inner box
//     + 2r[(w-2r)(h-2r)+(w-2r)(d-2r)+(h-2r)(d-2r)]      face slabs
//     + pi*r^2*[(w-2r)+(h-2r)+(d-2r)]                   quarter-cylinder edges
//     + (4/3)*pi*r^3                                    eighth-sphere corners
//
// CHAMFER oracle -- Pappus on a cylinder's two rims. A 45-degree chamfer of
// distance c removes, per rim, the solid of revolution of a right triangle
// with legs c whose centroid sits at radius (r - c/3):
//
//   V_removed_per_rim = 2*pi * (r - c/3) * c^2/2
//
// The cylinder avoids the corner-overlap arithmetic a chamfered box needs --
// its two rim circles have no corners.

#include "Kernel/Occt/OcctGeometryKernel.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cmath>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

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

PlanarProfileDefinition CircleProfile(double cx, double cy, double r) {
    PlanarProfileDefinition profile;
    profile.plane.origin = Vec3{0, 0, 0};
    profile.plane.uAxis = Vec3{1, 0, 0};
    profile.plane.vAxis = Vec3{0, 1, 0};
    profile.plane.normal = Vec3{0, 0, 1};
    profile.segments.push_back(ProfileCircleSegment{Vec2{cx, cy}, r});
    return profile;
}

double VolumeOf(OcctGeometryKernel& kernel, const KernelShape& shape) {
    const KernelMassPropertiesResult result = kernel.calculateMassProperties(shape);
    EXPECT_TRUE(result) << result.message;
    return result.properties.volumeMm3;
}

TEST(M8OcctFilletChamfer, GATE_FA_FilletedBoxMatchesTheMinkowskiOracle) {
    OcctGeometryKernel kernel;
    // 100 x 50 x 20 box as a prism, filleted r=2 on all 12 edges.
    ShapeResult box = kernel.extrudeProfile(RectangleProfile(0, 0, 100, 50), 20.0);
    ASSERT_TRUE(box) << box.message;
    ShapeResult rounded = kernel.filletEdges(box.shape, AllEdgesSelection(), 2.0);
    ASSERT_TRUE(rounded) << rounded.message;

    // Inner 96x46x16 = 70656; slabs 2*2*(96*46+96*16+46*16) = 26752;
    // edges pi*4*(96+46+16) = 632*pi; corners (4/3)*pi*8.
    const double expected = 70656.0 + 26752.0 + 632.0 * kPi + (4.0 / 3.0) * kPi * 8.0;
    EXPECT_NEAR(VolumeOf(kernel, rounded.shape), expected, expected * 1e-6);
    // The input was not consumed.
    EXPECT_NEAR(VolumeOf(kernel, box.shape), 100000.0, 1e-6);
}

TEST(M8OcctFilletChamfer, GATE_CA_ChamferedCylinderMatchesThePappusOracle) {
    OcctGeometryKernel kernel;
    // Cylinder r=20, h=40, both rim edges chamfered c=2.
    ShapeResult cylinder = kernel.extrudeProfile(CircleProfile(0, 0, 20.0), 40.0);
    ASSERT_TRUE(cylinder) << cylinder.message;
    ShapeResult chamfered = kernel.chamferEdges(cylinder.shape, AllEdgesSelection(), 2.0);
    ASSERT_TRUE(chamfered) << chamfered.message;

    // Removed per rim: 2*pi*(20 - 2/3)*(4/2) = 2*pi*(58/3)*2. Two rims.
    const double removed = 2.0 * (2.0 * kPi * (20.0 - 2.0 / 3.0) * 2.0);
    const double expected = kPi * 400.0 * 40.0 - removed;
    EXPECT_NEAR(VolumeOf(kernel, chamfered.shape), expected, expected * 1e-6);
}

TEST(M8OcctFilletChamfer, FilletAndChamferOfTheSameSizeDiffer) {
    OcctGeometryKernel kernel;
    ShapeResult cylinder = kernel.extrudeProfile(CircleProfile(0, 0, 20.0), 40.0);
    ASSERT_TRUE(cylinder) << cylinder.message;
    ShapeResult filleted = kernel.filletEdges(cylinder.shape, AllEdgesSelection(), 2.0);
    ShapeResult chamfered = kernel.chamferEdges(cylinder.shape, AllEdgesSelection(), 2.0);
    ASSERT_TRUE(filleted) << filleted.message;
    ASSERT_TRUE(chamfered) << chamfered.message;

    // A fillet keeps more material than the 45-degree bevel of the same size
    // (the round bulges past the flat): V_fillet > V_chamfer, both < V_base.
    // Pins that the two kernel verbs are genuinely different operations -- a
    // chamfer implemented by calling fillet passes every same-shape test.
    const double vf = VolumeOf(kernel, filleted.shape);
    const double vc = VolumeOf(kernel, chamfered.shape);
    const double vb = VolumeOf(kernel, cylinder.shape);
    EXPECT_GT(vf, vc);
    EXPECT_LT(vf, vb);
    // And by exactly the analytical margin: per rim the fillet leaves
    // (1 - pi/4)c^2 LESS removed area than the chamfer's c^2/2... computed:
    // chamfer removes area 2, fillet removes area 4 - pi (per rim section);
    // fillet removes MORE area but its centroid sits differently -- so the
    // inequality above is the load-bearing claim, and the exact values are:
    // fillet removed = 2*pi*xbar*(4-pi) per rim with xbar = (76 - 18*pi - 8/3)
    // / (4 - pi) -- asserted numerically:
    const double xbar = (76.0 - 18.0 * kPi - 8.0 / 3.0) / (4.0 - kPi);
    const double filletRemoved = 2.0 * (2.0 * kPi * xbar * (4.0 - kPi));
    EXPECT_NEAR(vb - vf, filletRemoved, filletRemoved * 1e-4);
}

TEST(M8OcctFilletChamfer, AnImpossibleSizeIsRefusedNotCorrupted) {
    OcctGeometryKernel kernel;
    ShapeResult box = kernel.extrudeProfile(RectangleProfile(0, 0, 100, 50), 20.0);
    ASSERT_TRUE(box) << box.message;

    // Radius 15 > half the 20mm thickness: the rounds collide. OCCT must fail
    // it cleanly, and the input must remain intact.
    ShapeResult refused = kernel.filletEdges(box.shape, AllEdgesSelection(), 15.0);
    EXPECT_FALSE(refused);
    EXPECT_NEAR(VolumeOf(kernel, box.shape), 100000.0, 1e-6);

    // And the plain invalid values.
    for (double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_FALSE(kernel.filletEdges(box.shape, AllEdgesSelection(), bad));
        EXPECT_FALSE(kernel.chamferEdges(box.shape, AllEdgesSelection(), bad));
    }
}

TEST(M8OcctFilletChamfer, M8_REV_331_AnImpossibleChamferDistanceIsRefusedNotCorrupted) {
    // The CHAMFER twin of the fillet refusal above -- round 1 (R1-M4) deleted
    // the chamfer's analyzer with every test green, because only the fillet's
    // copy of the geometric refusal was pinned. The kernel now runs one shared
    // post-check (AnalyzedDressResult) for both verbs, and this test is the
    // chamfer's half of its guard.
    OcctGeometryKernel kernel;
    ShapeResult box = kernel.extrudeProfile(RectangleProfile(0, 0, 100, 50), 20.0);
    ASSERT_TRUE(box) << box.message;

    // Distance 15 at 45 degrees from both faces of a 20mm slab: the bevels
    // collide. Refused cleanly, input intact.
    ShapeResult refused = kernel.chamferEdges(box.shape, AllEdgesSelection(), 15.0);
    EXPECT_FALSE(refused);
    EXPECT_NEAR(VolumeOf(kernel, box.shape), 100000.0, 1e-6);
}


} // namespace
