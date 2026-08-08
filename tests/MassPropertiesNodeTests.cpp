#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Fakes/FakeGeometryKernel.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

using namespace paramcad;

// Tolerances (ADR-M3-002; box is an exact analytic primitive so tight
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

struct MassFixture {
    PartDocument document{"MassDoc"};
    FakeGeometryKernel kernel;
    Material* material = nullptr;
    Parameter* width = nullptr;
    Parameter* height = nullptr;
    Parameter* depth = nullptr;
    BoxFeature* box = nullptr;

    MassFixture(double w, double h, double d, double densityKgPerM3) {
        document.setGeometryKernel(&kernel);
        material = &document.addMaterial("Mat", densityKgPerM3);
        width = &document.addParameter("Width", w, UnitType::Millimeter);
        height = &document.addParameter("Height", h, UnitType::Millimeter);
        depth = &document.addParameter("Depth", d, UnitType::Millimeter);
        Body& body = document.addBody("Body001");
        box = &document.addBoxFeature(body, "Box001", width->id(), height->id(), depth->id());
    }
};

// --- Mass (spec 17 "Mass" section, using the fake kernel's analytical box) --

TEST(MassPropertiesNodeTest, M3_FAKE_101_MassMatchesAnalyticalFormula) {
    // 100x50x20 mm, density 2700 kg/m^3 -> 0.27 kg (spec 9 mandatory case).
    MassFixture fx(100.0, 50.0, 20.0, 2700.0);
    ASSERT_TRUE(fx.document.recompute().success);

    const MassProperties& mp = fx.document.massProperties();
    EXPECT_TRUE(mp.valid);
    ExpectRel(mp.volumeMm3, 100.0 * 50.0 * 20.0, kVolumeMassRelTol);
    // Independently computed expected mass: V[m^3] * density, never via the
    // production conversion helpers.
    const double expectedVolumeM3 = (100.0 * 50.0 * 20.0) * 1e-9;
    const double expectedMassKg = 2700.0 * expectedVolumeM3;
    ExpectRel(mp.massKg, expectedMassKg, kVolumeMassRelTol);
    EXPECT_NEAR(mp.centerOfMassMm.x, 50.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.y, 25.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.z, 10.0, kLengthAbsTol);
}

TEST(MassPropertiesNodeTest, M3_FAKE_102_MassAtSecondDensity) {
    // Same box, density 7850 -> 0.785 kg.
    MassFixture fx(100.0, 50.0, 20.0, 7850.0);
    ASSERT_TRUE(fx.document.recompute().success);
    const double expectedVolumeM3 = (100.0 * 50.0 * 20.0) * 1e-9;
    ExpectRel(fx.document.massProperties().massKg, 7850.0 * expectedVolumeM3, kVolumeMassRelTol);
}

TEST(MassPropertiesNodeTest, M3_FAKE_103_VolumeUnchangedByDensity) {
    MassFixture fx(100.0, 50.0, 20.0, 2700.0);
    ASSERT_TRUE(fx.document.recompute().success);
    const double volumeAt2700 = fx.document.massProperties().volumeMm3;

    ASSERT_TRUE(fx.document.setMaterialDensity(7850.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.document.massProperties().volumeMm3, volumeAt2700);
}

TEST(MassPropertiesNodeTest, M3_FAKE_104_ComUnchangedByUniformDensity) {
    MassFixture fx(100.0, 50.0, 20.0, 2700.0);
    ASSERT_TRUE(fx.document.recompute().success);
    const Vec3 comAt2700 = fx.document.massProperties().centerOfMassMm;

    ASSERT_TRUE(fx.document.setMaterialDensity(7850.0));
    ASSERT_TRUE(fx.document.recompute().success);
    const Vec3 comAt7850 = fx.document.massProperties().centerOfMassMm;
    EXPECT_EQ(comAt2700.x, comAt7850.x);
    EXPECT_EQ(comAt2700.y, comAt7850.y);
    EXPECT_EQ(comAt2700.z, comAt7850.z);
}

// --- Inertia -----------------------------------------------------------------

TEST(MassPropertiesNodeTest, M3_FAKE_105_InertiaMatchesAnalyticalFormula) {
    // Independent oracle (spec 10): Ixx = m/12*(h^2+d^2) etc, in meters, never
    // via production conversion helpers.
    const double w = 100.0, h = 50.0, d = 20.0, density = 2700.0;
    MassFixture fx(w, h, d, density);
    ASSERT_TRUE(fx.document.recompute().success);

    const double volumeM3 = (w * h * d) * 1e-9;
    const double massKg = density * volumeM3;
    const double wM = w * 1e-3, hM = h * 1e-3, dM = d * 1e-3;
    const double expectedIxx = massKg / 12.0 * (hM * hM + dM * dM);
    const double expectedIyy = massKg / 12.0 * (wM * wM + dM * dM);
    const double expectedIzz = massKg / 12.0 * (wM * wM + hM * hM);

    const Matrix3& inertia = fx.document.massProperties().inertiaTensorKgM2;
    ExpectRel(inertia.m[0], expectedIxx, kInertiaDiagonalRelTol);
    ExpectRel(inertia.m[4], expectedIyy, kInertiaDiagonalRelTol);
    ExpectRel(inertia.m[8], expectedIzz, kInertiaDiagonalRelTol);
}

TEST(MassPropertiesNodeTest, M3_FAKE_106_OffDiagonalInertiaApproximatelyZero) {
    MassFixture fx(100.0, 50.0, 20.0, 2700.0);
    ASSERT_TRUE(fx.document.recompute().success);
    const Matrix3& inertia = fx.document.massProperties().inertiaTensorKgM2;
    EXPECT_NEAR(inertia.m[1], 0.0, kInertiaOffDiagonalAbsTol);
    EXPECT_NEAR(inertia.m[2], 0.0, kInertiaOffDiagonalAbsTol);
    EXPECT_NEAR(inertia.m[3], 0.0, kInertiaOffDiagonalAbsTol);
    EXPECT_NEAR(inertia.m[5], 0.0, kInertiaOffDiagonalAbsTol);
    EXPECT_NEAR(inertia.m[6], 0.0, kInertiaOffDiagonalAbsTol);
    EXPECT_NEAR(inertia.m[7], 0.0, kInertiaOffDiagonalAbsTol);
}

TEST(MassPropertiesNodeTest, M3_FAKE_107_AsymmetricBoxPreventsHardcoding) {
    // Second, distinctly asymmetric box -- a hardcoded/mocked answer for the
    // first box could not also satisfy this one.
    const double w = 37.0, h = 213.0, d = 5.5, density = 1500.0;
    MassFixture fx(w, h, d, density);
    ASSERT_TRUE(fx.document.recompute().success);
    const MassProperties& mp = fx.document.massProperties();
    ExpectRel(mp.volumeMm3, w * h * d, kVolumeMassRelTol);
    EXPECT_NEAR(mp.centerOfMassMm.x, w / 2.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.y, h / 2.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.z, d / 2.0, kLengthAbsTol);
}

// --- Density policy (ADR-M3-005, spec 13) -----------------------------------

TEST(MassPropertiesNodeTest, M3_FAKE_108_ZeroDensityIsValid) {
    MassFixture fx(100.0, 50.0, 20.0, 0.0);
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_TRUE(report.success);
    const MassProperties& mp = fx.document.massProperties();
    EXPECT_TRUE(mp.valid);
    EXPECT_EQ(mp.massKg, 0.0);
    EXPECT_NE(mp.volumeMm3, 0.0); // geometry unaffected by mass being zero
    EXPECT_NEAR(mp.centerOfMassMm.x, 50.0, kLengthAbsTol); // COM from geometry alone
}

TEST(MassPropertiesNodeTest, M3_FAKE_109_NegativeDensityFails) {
    MassFixture fx(100.0, 50.0, 20.0, 2700.0);
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.setMaterialDensity(-1.0));
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
}

TEST(MassPropertiesNodeTest, M3_FAKE_110_NanDensityFails) {
    MassFixture fx(100.0, 50.0, 20.0, 2700.0);
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.setMaterialDensity(std::numeric_limits<double>::quiet_NaN()));
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
}

TEST(MassPropertiesNodeTest, M3_FAKE_111_InfiniteDensityFails) {
    MassFixture fx(100.0, 50.0, 20.0, 2700.0);
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.setMaterialDensity(std::numeric_limits<double>::infinity()));
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
}

TEST(MassPropertiesNodeTest, M3_FAKE_112_NoMaterialAssignedBehavesAsZeroDensity) {
    PartDocument document("Doc");
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    Parameter& width = document.addParameter("Width", 10.0, UnitType::Millimeter);
    Parameter& height = document.addParameter("Height", 10.0, UnitType::Millimeter);
    Parameter& depth = document.addParameter("Depth", 10.0, UnitType::Millimeter);
    Body& body = document.addBody("Body001");
    document.addBoxFeature(body, "Box001", width.id(), height.id(), depth.id());

    const DocumentRecomputeReport report = document.recompute();
    EXPECT_TRUE(report.success);
    EXPECT_TRUE(document.massProperties().valid);
    EXPECT_EQ(document.massProperties().massKg, 0.0);
}

} // namespace
