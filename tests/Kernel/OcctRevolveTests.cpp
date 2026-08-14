// M8.2 Gate R-A: revolveProfile at the kernel boundary, against real OCCT,
// with hand-computed volumes.
//
// The oracles are Pappus' theorem, exact for a profile that does not cross its
// axis: V = theta * Rbar * A, where Rbar is the centroid's distance from the
// axis and A the profile area. Every number below is computed from the profile
// dimensions in the comments, never read back from OCCT.

#include "Kernel/Occt/OcctGeometryKernel.h"
#include <gtest/gtest.h>
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

double VolumeOf(OcctGeometryKernel& kernel, const KernelShape& shape) {
    const KernelMassPropertiesResult result = kernel.calculateMassProperties(shape);
    EXPECT_TRUE(result) << result.message;
    return result.properties.volumeMm3;
}

TEST(M8OcctRevolve, GATE_RA_FullRevolutionAboutAnEdgeIsACylinder) {
    OcctGeometryKernel kernel;
    // Rectangle 0..20 x 0..40 revolved fully about the v axis at u=0 -- its own
    // left edge. Cylinder: r=20, h=40, V = pi * 400 * 40.
    ShapeResult solid = kernel.revolveProfile(RectangleProfile(0, 0, 20, 40), Vec3{0, 0, 0},
                                              Vec3{0, 1, 0}, 2.0 * kPi);
    ASSERT_TRUE(solid) << solid.message;
    EXPECT_NEAR(VolumeOf(kernel, solid.shape), kPi * 400.0 * 40.0, 1e-3);
}

TEST(M8OcctRevolve, AnOffsetProfileRevolvesToAnAnnulus) {
    OcctGeometryKernel kernel;
    // Rectangle 10..30 x 0..40 about u=0: annular cylinder,
    // V = pi*(30^2 - 10^2)*40 = pi*800*40. Pappus cross-check:
    // A=800, Rbar=20 -> 2*pi*20*800 = same.
    ShapeResult solid = kernel.revolveProfile(RectangleProfile(10, 0, 30, 40), Vec3{0, 0, 0},
                                              Vec3{0, 1, 0}, 2.0 * kPi);
    ASSERT_TRUE(solid) << solid.message;
    EXPECT_NEAR(VolumeOf(kernel, solid.shape), kPi * 800.0 * 40.0, 1e-3);
}

TEST(M8OcctRevolve, AHalfSweepIsExactlyHalfTheVolume) {
    OcctGeometryKernel kernel;
    ShapeResult full = kernel.revolveProfile(RectangleProfile(10, 0, 30, 40), Vec3{0, 0, 0},
                                             Vec3{0, 1, 0}, 2.0 * kPi);
    ShapeResult half = kernel.revolveProfile(RectangleProfile(10, 0, 30, 40), Vec3{0, 0, 0},
                                             Vec3{0, 1, 0}, kPi);
    ASSERT_TRUE(full) << full.message;
    ASSERT_TRUE(half) << half.message;

    // A RATIO, like M7's Gate E: a scale error moves both numbers and cannot
    // hide in a ratio. Also pins that the angle is actually USED -- a kernel
    // that always revolved fully would give 1.0 here.
    EXPECT_NEAR(VolumeOf(kernel, half.shape) / VolumeOf(kernel, full.shape), 0.5, 1e-9);
}

TEST(M8OcctRevolve, InvalidAnglesAndAxesAreRefused) {
    OcctGeometryKernel kernel;
    const PlanarProfileDefinition profile = RectangleProfile(10, 0, 30, 40);

    for (double bad : {0.0, -1.0, 2.0 * kPi + 0.1,
                       std::numeric_limits<double>::quiet_NaN()}) {
        const ShapeResult refused =
            kernel.revolveProfile(profile, Vec3{0, 0, 0}, Vec3{0, 1, 0}, bad);
        EXPECT_FALSE(refused) << "angle " << bad << " was accepted";
        EXPECT_EQ(refused.error, KernelError::InvalidDimension);
    }

    // Degenerate axis direction.
    const ShapeResult zeroAxis =
        kernel.revolveProfile(profile, Vec3{0, 0, 0}, Vec3{0, 0, 0}, kPi);
    EXPECT_FALSE(zeroAxis);
    EXPECT_EQ(zeroAxis.error, KernelError::InvalidDimension);
}

} // namespace
