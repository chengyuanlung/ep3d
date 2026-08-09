// M4-E/F: kernel-neutral profile extrusion against REAL OCCT geometry
// (spec 18 "Kernel" matrix, spec 15 oracles; ADR-M4-003).
//
// Every expected value here is computed from the raw analytical formula, never
// by calling a production helper (spec 15/10) -- the point is to check OCCT,
// so deriving the expectation from OCCT's own answer would check nothing.

#include "Core/Sketch/SketchFrame.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;
constexpr double kLengthAbsTol = 1e-6;      // mm
constexpr double kVolumeRelTol = 1e-9;      // exact primitives
constexpr double kCurvedVolumeRelTol = 1e-6; // OCCT tessellation-free but circle math is analytic

void ExpectRel(double actual, double expected, double relTol) {
    EXPECT_NEAR(actual, expected, relTol * std::max(1.0, std::fabs(expected)));
}

// Plane description derived from a SketchFrame -- the same path PadFeature
// will use, so these tests exercise the real frame conversion rather than a
// hand-built basis.
ProfilePlane PlaneOf(const SketchFrame& frame) {
    ProfilePlane plane;
    plane.origin = frame.toWorld(Vec2{0.0, 0.0});
    plane.uAxis = frame.uAxis();
    plane.vAxis = frame.vAxis();
    plane.normal = frame.normal();
    return plane;
}

// Closed rectangle (0,0)-(w,h) as four oriented line segments.
std::vector<ProfileSegment> RectangleSegments(double w, double h) {
    return {ProfileLineSegment{Vec2{0, 0}, Vec2{w, 0}},
            ProfileLineSegment{Vec2{w, 0}, Vec2{w, h}},
            ProfileLineSegment{Vec2{w, h}, Vec2{0, h}},
            ProfileLineSegment{Vec2{0, h}, Vec2{0, 0}}};
}

// --- Rectangle oracle (spec 15) --------------------------------------------

TEST(OcctExtrudeTest, M4_KERNEL_001_RectangleProfileProducesValidSolid) {
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = RectangleSegments(100.0, 50.0);

    const ShapeResult result = kernel.extrudeProfile(profile, 20.0);
    ASSERT_TRUE(result) << result.message;
    EXPECT_TRUE(result.shape.isValid());
}

TEST(OcctExtrudeTest, M4_KERNEL_002_RectangleVolumeAndComMatchAnalyticalValues) {
    // 100 x 50 mm profile, Pad 20 mm -> 100000 mm^3, COM (50,25,10).
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = RectangleSegments(100.0, 50.0);

    const ShapeResult shape = kernel.extrudeProfile(profile, 20.0);
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;

    ExpectRel(props.properties.volumeMm3, 100.0 * 50.0 * 20.0, kVolumeRelTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.x, 50.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, 25.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.z, 10.0, kLengthAbsTol);
}

TEST(OcctExtrudeTest, M4_KERNEL_003_AsymmetricRectanglePreventsHardcoding) {
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = RectangleSegments(37.5, 11.25);

    const ShapeResult shape = kernel.extrudeProfile(profile, 6.5);
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;

    ExpectRel(props.properties.volumeMm3, 37.5 * 11.25 * 6.5, kVolumeRelTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.x, 37.5 / 2.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, 11.25 / 2.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.z, 6.5 / 2.0, kLengthAbsTol);
}

TEST(OcctExtrudeTest, M4_KERNEL_004_ExtrudedRectangleMatchesTheEquivalentBox) {
    // Two independent construction paths must agree: a 100x50 profile extruded
    // 20 mm is the same solid as createBox(100,50,20). This cross-checks the
    // new M4 path against M3's already-reviewed one.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = RectangleSegments(100.0, 50.0);

    const ShapeResult padded = kernel.extrudeProfile(profile, 20.0);
    const ShapeResult boxed = kernel.createBox(BoxDefinition{100.0, 50.0, 20.0});
    ASSERT_TRUE(padded) << padded.message;
    ASSERT_TRUE(boxed) << boxed.message;

    const KernelMassPropertiesResult a = kernel.calculateMassProperties(padded.shape);
    const KernelMassPropertiesResult b = kernel.calculateMassProperties(boxed.shape);
    ASSERT_TRUE(a) << a.message;
    ASSERT_TRUE(b) << b.message;

    ExpectRel(a.properties.volumeMm3, b.properties.volumeMm3, kVolumeRelTol);
    EXPECT_NEAR(a.properties.centerOfMassMm.x, b.properties.centerOfMassMm.x, kLengthAbsTol);
    EXPECT_NEAR(a.properties.centerOfMassMm.y, b.properties.centerOfMassMm.y, kLengthAbsTol);
    EXPECT_NEAR(a.properties.centerOfMassMm.z, b.properties.centerOfMassMm.z, kLengthAbsTol);
    for (std::size_t i = 0; i < 9; ++i)
        ExpectRel(a.properties.secondMomentMm5.m[i], b.properties.secondMomentMm5.m[i], 1e-6);
}

// --- Circle oracle (spec 15 / gate B) --------------------------------------

TEST(OcctExtrudeTest, M4_KERNEL_010_CircleVolumeMatchesPiRSquaredH) {
    // r = 10 mm, Pad = 30 mm -> pi*100*30.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = {ProfileCircleSegment{Vec2{0, 0}, 10.0}};

    const ShapeResult shape = kernel.extrudeProfile(profile, 30.0);
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;

    ExpectRel(props.properties.volumeMm3, kPi * 10.0 * 10.0 * 30.0, kCurvedVolumeRelTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.x, 0.0, 1e-6);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, 0.0, 1e-6);
    EXPECT_NEAR(props.properties.centerOfMassMm.z, 15.0, 1e-6);
}

TEST(OcctExtrudeTest, M4_KERNEL_011_OffsetCircleComFollowsItsCentre) {
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = {ProfileCircleSegment{Vec2{25.0, -8.0}, 4.0}};

    const ShapeResult shape = kernel.extrudeProfile(profile, 12.0);
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;

    ExpectRel(props.properties.volumeMm3, kPi * 16.0 * 12.0, kCurvedVolumeRelTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.x, 25.0, 1e-6);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, -8.0, 1e-6);
    EXPECT_NEAR(props.properties.centerOfMassMm.z, 6.0, 1e-6);
}

// --- Line + arc loop -------------------------------------------------------

TEST(OcctExtrudeTest, M4_KERNEL_020_HalfDiscProfileExtrudes) {
    // Upper half-disc r=10: semicircular arc closed by its diameter.
    // Area = pi*r^2/2; COM_v = 4r/(3*pi) by the standard semicircle result.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = {ProfileArcSegment{Vec2{0, 0}, 10.0, 0.0, kPi, true},
                        ProfileLineSegment{Vec2{-10, 0}, Vec2{10, 0}}};

    const ShapeResult shape = kernel.extrudeProfile(profile, 5.0);
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;

    ExpectRel(props.properties.volumeMm3, kPi * 100.0 / 2.0 * 5.0, kCurvedVolumeRelTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.x, 0.0, 1e-6);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, 4.0 * 10.0 / (3.0 * kPi), 1e-6);
    EXPECT_NEAR(props.properties.centerOfMassMm.z, 2.5, 1e-6);
}

// --- Transformed frames (spec 18, gate D) ----------------------------------

TEST(OcctExtrudeTest, M4_KERNEL_030_TranslatedFrameKeepsVolumeAndShiftsCom) {
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::Translated(Vec3{10.0, 20.0, 30.0}));
    profile.segments = RectangleSegments(100.0, 50.0);

    const ShapeResult shape = kernel.extrudeProfile(profile, 20.0);
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;

    ExpectRel(props.properties.volumeMm3, 100000.0, kVolumeRelTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.x, 60.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, 45.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.z, 40.0, kLengthAbsTol);
}

TEST(OcctExtrudeTest, M4_KERNEL_031_RotatedFrameKeepsLocalDimensionsAndOrientsCorrectly) {
    // Rotate the sketch plane +90 deg about X: sketch +v -> world +z, and the
    // extrusion normal -> world -y. Local dimensions and volume must not change
    // -- this is what catches a world-XY hardcode (spec 22).
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::Rotated(Vec3{1, 0, 0}, kPi / 2));
    profile.segments = RectangleSegments(100.0, 50.0);

    const ShapeResult shape = kernel.extrudeProfile(profile, 20.0);
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;

    ExpectRel(props.properties.volumeMm3, 100000.0, kVolumeRelTol);
    // Centroid of the cross-section is (50,25) in sketch coords; under this
    // rotation that maps to world (50, 0, 25), and the extrusion carries the
    // solid 10 mm along -y.
    EXPECT_NEAR(props.properties.centerOfMassMm.x, 50.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, -10.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.z, 25.0, kLengthAbsTol);
}

TEST(OcctExtrudeTest, M4_KERNEL_032_RotatedAndTranslatedFrameComposes) {
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::Rotated(Vec3{0, 0, 1}, kPi / 2, Vec3{5, 5, 5}));
    profile.segments = RectangleSegments(100.0, 50.0);

    const ShapeResult shape = kernel.extrudeProfile(profile, 20.0);
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;

    ExpectRel(props.properties.volumeMm3, 100000.0, kVolumeRelTol);
    // +90 about Z sends sketch (50,25) to world (-25,50), then translate by
    // (5,5,5); the normal stays +z so the solid rises 10 mm.
    EXPECT_NEAR(props.properties.centerOfMassMm.x, -20.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, 55.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.z, 15.0, kLengthAbsTol);
}

// --- Negative cases --------------------------------------------------------

TEST(OcctExtrudeTest, M4_KERNEL_040_EmptyProfileRejected) {
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    const ShapeResult result = kernel.extrudeProfile(profile, 20.0);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, KernelError::InvalidDimension);
}

TEST(OcctExtrudeTest, M4_KERNEL_041_InvalidDistancesRejected) {
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = RectangleSegments(100.0, 50.0);

    for (double distance : {0.0, -20.0, std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity()}) {
        const ShapeResult result = kernel.extrudeProfile(profile, distance);
        EXPECT_FALSE(result) << "distance " << distance << " was accepted";
        EXPECT_EQ(result.error, KernelError::InvalidDimension);
        EXPECT_FALSE(result.message.empty());
    }
}

TEST(OcctExtrudeTest, M4_KERNEL_042_DegeneratePlaneRejected) {
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.plane.normal = Vec3{0, 0, 0}; // degenerate basis
    profile.segments = RectangleSegments(100.0, 50.0);

    const ShapeResult result = kernel.extrudeProfile(profile, 20.0);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, KernelError::InvalidDimension);
}

TEST(OcctExtrudeTest, M4_KERNEL_043_OpenProfileCannotProduceASolid) {
    // Core rejects open loops before the kernel is ever called (ADR-M4-005),
    // so this checks the kernel's own backstop: handed an unclosed wire it must
    // fail in a structured way rather than build something wrong or crash.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = {ProfileLineSegment{Vec2{0, 0}, Vec2{100, 0}},
                        ProfileLineSegment{Vec2{100, 0}, Vec2{100, 50}},
                        ProfileLineSegment{Vec2{100, 50}, Vec2{0, 50}}};

    const ShapeResult result = kernel.extrudeProfile(profile, 20.0);
    EXPECT_FALSE(result) << "an open profile produced a solid";
    EXPECT_FALSE(result.message.empty());
}

TEST(OcctExtrudeTest, M4_KERNEL_044_NonFiniteSegmentRejected) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = {ProfileLineSegment{Vec2{0, 0}, Vec2{nan, 0}},
                        ProfileLineSegment{Vec2{nan, 0}, Vec2{0, 0}}};

    const ShapeResult result = kernel.extrudeProfile(profile, 20.0);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, KernelError::InvalidDimension);
}

} // namespace
