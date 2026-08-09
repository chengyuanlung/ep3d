#include "Kernel/Occt/OcctGeometryKernel.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

using namespace paramcad;

// Tolerances (ADR-M3-002; box is an exact analytic primitive, so tight
// tolerances are justified, not aspirational). Documented once here; never
// enlarged to make a failing implementation pass (spec 18).
constexpr double kLengthAbsTol = 1e-6;   // mm
constexpr double kVolumeMassRelTol = 1e-9;
constexpr double kInertiaDiagonalRelTol = 1e-9;
constexpr double kInertiaOffDiagonalAbsTol = 1e-9; // kg*m^2

void ExpectRel(double actual, double expected, double relTol) {
    const double tolerance = relTol * std::max(1.0, std::fabs(expected));
    EXPECT_NEAR(actual, expected, tolerance);
}

// --- M3-KERNEL: valid box / analytical volume / COM / no hardcoding --------

TEST(OcctGeometryKernelTest, M3_KERNEL_001_ValidBox100x50x20) {
    OcctGeometryKernel kernel;
    const ShapeResult result = kernel.createBox(BoxDefinition{100.0, 50.0, 20.0});
    ASSERT_TRUE(result) << result.message;
    EXPECT_TRUE(result.shape.isValid());
}

TEST(OcctGeometryKernelTest, M3_KERNEL_002_VolumeMatchesAnalyticalValue) {
    OcctGeometryKernel kernel;
    const ShapeResult shape = kernel.createBox(BoxDefinition{100.0, 50.0, 20.0});
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;
    ExpectRel(props.properties.volumeMm3, 100.0 * 50.0 * 20.0, kVolumeMassRelTol);
}

TEST(OcctGeometryKernelTest, M3_KERNEL_003_CornerOriginConvention_ComAtHalfDimensions) {
    OcctGeometryKernel kernel;
    const ShapeResult shape = kernel.createBox(BoxDefinition{100.0, 50.0, 20.0});
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;
    EXPECT_NEAR(props.properties.centerOfMassMm.x, 50.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, 25.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.z, 10.0, kLengthAbsTol);
}

TEST(OcctGeometryKernelTest, M3_KERNEL_004_AsymmetricBoxPreventsHardcoding) {
    // Second, distinctly asymmetric box (spec 17): a hardcoded/mocked answer
    // for the first box could not also satisfy this one.
    OcctGeometryKernel kernel;
    const double w = 37.0, h = 213.0, d = 5.5;
    const ShapeResult shape = kernel.createBox(BoxDefinition{w, h, d});
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;
    ExpectRel(props.properties.volumeMm3, w * h * d, kVolumeMassRelTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.x, w / 2.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, h / 2.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.z, d / 2.0, kLengthAbsTol);
}

// --- Negative (spec 13/17) ---------------------------------------------------

TEST(OcctGeometryKernelTest, M3_KERNEL_005_ZeroDimensionFails) {
    OcctGeometryKernel kernel;
    const ShapeResult result = kernel.createBox(BoxDefinition{0.0, 50.0, 20.0});
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, KernelError::InvalidDimension);
    EXPECT_FALSE(result.shape.isValid());
}

// kMinBoxDimensionMm exists because OCCT rejects degenerate primitives by
// throwing, which would escape the structured InvalidDimension contract of
// spec 13 (ADR-M3-009). The Core-side tests for it use the fake kernel, so
// this is the only place the threshold is checked against REAL OCCT -- the
// one that decides whether the chosen value is actually high enough.
TEST(OcctGeometryKernelTest, M3_KERNEL_010_DegenerateDimensionRejectedBeforeReachingOcct) {
    OcctGeometryKernel kernel;
    const ShapeResult justBelow =
        kernel.createBox(BoxDefinition{kMinBoxDimensionMm * 0.99, 50.0, 20.0});
    EXPECT_FALSE(justBelow);
    EXPECT_EQ(justBelow.error, KernelError::InvalidDimension)
        << "degenerate dimension surfaced as a kernel-internal error: " << justBelow.message;
    EXPECT_FALSE(justBelow.message.empty());

    // At the threshold OCCT must genuinely succeed, or the constant is too low.
    const ShapeResult atThreshold =
        kernel.createBox(BoxDefinition{kMinBoxDimensionMm, 100.0, 50.0});
    EXPECT_TRUE(atThreshold) << atThreshold.message;
    EXPECT_TRUE(atThreshold.shape.isValid());
}

TEST(OcctGeometryKernelTest, M3_KERNEL_011_LargeDimensionSucceeds) {
    // Spec 23 adversarial check: a large but reasonable dimension (1 km).
    OcctGeometryKernel kernel;
    const ShapeResult result = kernel.createBox(BoxDefinition{1.0e6, 1.0e6, 1.0e6});
    ASSERT_TRUE(result) << result.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(result.shape);
    ASSERT_TRUE(props) << props.message;
    ExpectRel(props.properties.volumeMm3, 1.0e18, kVolumeMassRelTol);
}

TEST(OcctGeometryKernelTest, M3_KERNEL_006_NegativeDimensionFails) {
    OcctGeometryKernel kernel;
    const ShapeResult result = kernel.createBox(BoxDefinition{-5.0, 50.0, 20.0});
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, KernelError::InvalidDimension);
}

TEST(OcctGeometryKernelTest, M3_KERNEL_007_NaNDimensionFails) {
    OcctGeometryKernel kernel;
    const ShapeResult result =
        kernel.createBox(BoxDefinition{std::numeric_limits<double>::quiet_NaN(), 50.0, 20.0});
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, KernelError::InvalidDimension);
}

TEST(OcctGeometryKernelTest, M3_KERNEL_008_InfiniteDimensionFails) {
    OcctGeometryKernel kernel;
    const ShapeResult result =
        kernel.createBox(BoxDefinition{std::numeric_limits<double>::infinity(), 50.0, 20.0});
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, KernelError::InvalidDimension);
}

TEST(OcctGeometryKernelTest, M3_KERNEL_009_MassPropertiesOnInvalidShapeIsControlledFailure) {
    // A default-constructed (never built) KernelShape must not crash the
    // kernel; dynamic_cast on a null handle yields a controlled failure.
    OcctGeometryKernel kernel;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(KernelShape{});
    EXPECT_FALSE(props);
    EXPECT_EQ(props.error, KernelError::GeometryConstructionFailed);
}

// --- Mass (spec 17 "Mass" section) ------------------------------------------

TEST(OcctGeometryKernelTest, M3_MASS_001_DensityConversionMatchesAnalyticalFormula) {
    // 100x50x20 mm, density 2700 kg/m^3 -> 0.27 kg (spec 9 mandatory case).
    // Conversion performed independently here (never via production
    // MassPropertiesNode helpers) using the same raw formula.
    OcctGeometryKernel kernel;
    const ShapeResult shape = kernel.createBox(BoxDefinition{100.0, 50.0, 20.0});
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;

    const double volumeM3 = props.properties.volumeMm3 * 1e-9;
    const double massAt2700 = 2700.0 * volumeM3;
    const double massAt7850 = 7850.0 * volumeM3;
    ExpectRel(massAt2700, 0.27, kVolumeMassRelTol);
    ExpectRel(massAt7850, 0.785, kVolumeMassRelTol);
}

TEST(OcctGeometryKernelTest, M3_MASS_002_VolumeAndComAreDensityIndependent) {
    // The kernel never computes density-dependent quantities at all --
    // volume/COM are the same result object regardless of what density a
    // caller later multiplies in (density lives only in MassPropertiesNode).
    OcctGeometryKernel kernel;
    const ShapeResult shape = kernel.createBox(BoxDefinition{100.0, 50.0, 20.0});
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult first = kernel.calculateMassProperties(shape.shape);
    const KernelMassPropertiesResult second = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(first) << first.message;
    ASSERT_TRUE(second) << second.message;
    EXPECT_EQ(first.properties.volumeMm3, second.properties.volumeMm3);
    EXPECT_EQ(first.properties.centerOfMassMm.x, second.properties.centerOfMassMm.x);
    EXPECT_EQ(first.properties.centerOfMassMm.y, second.properties.centerOfMassMm.y);
    EXPECT_EQ(first.properties.centerOfMassMm.z, second.properties.centerOfMassMm.z);
}

// --- Inertia (spec 10/17) ----------------------------------------------------

TEST(OcctGeometryKernelTest, M3_INERTIA_001_DiagonalMatchesAnalyticalFormula) {
    // Independent oracle (spec 10): Ixx = m/12*(h^2+d^2), Iyy = m/12*(w^2+d^2),
    // Izz = m/12*(w^2+h^2), in meters -- computed here with raw formulas, not
    // via production helpers.
    const double w = 100.0, h = 50.0, d = 20.0, density = 2700.0;
    OcctGeometryKernel kernel;
    const ShapeResult shape = kernel.createBox(BoxDefinition{w, h, d});
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;

    // Expected mass is derived from the DIMENSIONS, not from the kernel's own
    // reported volume (spec 10: the oracle must be independent). Reusing the
    // measured volume here would let a wrong volume cancel itself out of the
    // inertia comparison.
    const double volumeM3 = (w * h * d) * 1e-9;
    const double massKg = density * volumeM3;
    const double wM = w * 1e-3, hM = h * 1e-3, dM = d * 1e-3;
    const double expectedIxx = massKg / 12.0 * (hM * hM + dM * dM);
    const double expectedIyy = massKg / 12.0 * (wM * wM + dM * dM);
    const double expectedIzz = massKg / 12.0 * (wM * wM + hM * hM);

    // Convert the kernel's density-independent second moment (mm^5) the same
    // way MassPropertiesNode does, but computed independently in this test.
    constexpr double kMm5ToM5 = 1e-15;
    const Matrix3& secondMoment = props.properties.secondMomentMm5;
    const double actualIxx = density * secondMoment.m[0] * kMm5ToM5;
    const double actualIyy = density * secondMoment.m[4] * kMm5ToM5;
    const double actualIzz = density * secondMoment.m[8] * kMm5ToM5;

    ExpectRel(actualIxx, expectedIxx, kInertiaDiagonalRelTol);
    ExpectRel(actualIyy, expectedIyy, kInertiaDiagonalRelTol);
    ExpectRel(actualIzz, expectedIzz, kInertiaDiagonalRelTol);
}

TEST(OcctGeometryKernelTest, M3_INERTIA_002_OffDiagonalApproximatelyZero) {
    OcctGeometryKernel kernel;
    const ShapeResult shape = kernel.createBox(BoxDefinition{100.0, 50.0, 20.0});
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;

    constexpr double density = 2700.0;
    constexpr double kMm5ToM5 = 1e-15;
    const Matrix3& secondMoment = props.properties.secondMomentMm5;
    for (int index : {1, 2, 3, 5, 6, 7}) {
        EXPECT_NEAR(density * secondMoment.m[index] * kMm5ToM5, 0.0, kInertiaOffDiagonalAbsTol)
            << "off-diagonal index " << index;
    }
}

} // namespace
