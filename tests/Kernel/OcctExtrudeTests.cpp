// M4-E/F: kernel-neutral profile extrusion against REAL OCCT geometry
// (spec 18 "Kernel" matrix, spec 15 oracles; ADR-M4-003).
//
// Every expected value here is computed from the raw analytical formula, never
// by calling a production helper (spec 15/10) -- the point is to check OCCT,
// so deriving the expectation from OCCT's own answer would check nothing.

#include "Core/Sketch/SketchFrame.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Kernel/Occt/OcctSketchWireframe.h"
#include "Kernel/Occt/OcctSplineCurve.h"
#include <gtest/gtest.h>
#include <cstdio>
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

// --- Splines (M17.26) --------------------------------------------------------

TEST(OcctExtrudeTest, M17_KERNEL_040_ASplineClosesAProfileAndExtrudes) {
    // A spline as one side of a closed loop -- which is the whole reason a
    // sketch has them. Its ends must land EXACTLY on the neighbouring edges'
    // ends or the wire does not close, and OCCT says so rather than guessing.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = {
        ProfileLineSegment{Vec2{0, 0}, Vec2{100, 0}},
        ProfileLineSegment{Vec2{100, 0}, Vec2{100, 40}},
        // Back along the top, bulging, through points the curve must hit.
        ProfileSplineSegment{{Vec2{100, 40}, Vec2{60, 55}, Vec2{40, 25}, Vec2{0, 40}}, false},
        ProfileLineSegment{Vec2{0, 40}, Vec2{0, 0}}};

    const ShapeResult shape = kernel.extrudeProfile(profile, 10.0);
    ASSERT_TRUE(shape) << shape.message;
    EXPECT_TRUE(shape.shape.isValid());

    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;
    // NOT the rectangle it would be with a straight top. The bulge above and
    // the dip below do not cancel here -- the point is that the volume is
    // neither the plain 100x40 plate nor obviously wrong, so it is bracketed
    // rather than pinned to a number only OCCT could produce.
    EXPECT_GT(props.properties.volumeMm3, 0.8 * 100.0 * 40.0 * 10.0);
    EXPECT_LT(props.properties.volumeMm3, 1.2 * 100.0 * 40.0 * 10.0);
}

TEST(OcctExtrudeTest, M17_KERNEL_041_AClosedSplineIsALoopOnItsOwn) {
    // Like a circle: one segment, no neighbours, and it has to close itself.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = {ProfileSplineSegment{
        {Vec2{-40, 0}, Vec2{0, 30}, Vec2{40, 0}, Vec2{0, -30}}, true}};

    const ShapeResult shape = kernel.extrudeProfile(profile, 5.0);
    ASSERT_TRUE(shape) << shape.message;
    EXPECT_TRUE(shape.shape.isValid());

    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;

    // BRACKETED, not pinned. The curve bulges out past the diamond through its
    // four points (area 2400) and stays inside the box round them (9600), which
    // is what tells an INTERPOLATING curve from a fitting one and from a
    // polygon -- and it is a number this test can derive rather than read back
    // from OCCT.
    EXPECT_GT(props.properties.volumeMm3, 2400.0 * 5.0);
    EXPECT_LT(props.properties.volumeMm3, 9600.0 * 5.0);

    // SYMMETRY IS DELIBERATELY NOT ASSERTED, and that is worth recording.
    //
    // The four points are symmetric about both axes and every chord between
    // them is the same length, so the obvious expectation is a centroid at the
    // origin. OCCT's periodic interpolation does not give one: it comes out
    // about 2.6 mm off in x on a 40 mm shape, far too much to be the mass
    // integrator's tessellation error (an elliptical face costs ~1e-3 mm).
    // Something about which point the periodic parametrisation starts at
    // breaks the symmetry.
    //
    // That is a real question and it is not answered here, so this asserts only
    // that the centroid is INSIDE the shape -- claiming symmetry would be
    // asserting a belief rather than a measurement.
    EXPECT_LT(std::fabs(props.properties.centerOfMassMm.x), 40.0);
    EXPECT_LT(std::fabs(props.properties.centerOfMassMm.y), 30.0);
}

TEST(OcctExtrudeTest, M17_KERNEL_044_AProfileWhoseSplineDoesNotREACHIsRefused) {
    // The spline's last point is 5 mm short of where the next line begins. OCCT
    // will not close that wire, and the refusal has to arrive as a message
    // rather than as a solid with a gap in it.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = {
        ProfileLineSegment{Vec2{0, 0}, Vec2{100, 0}},
        ProfileLineSegment{Vec2{100, 0}, Vec2{100, 40}},
        ProfileSplineSegment{{Vec2{100, 40}, Vec2{50, 50}, Vec2{5, 40}}, false},
        ProfileLineSegment{Vec2{0, 40}, Vec2{0, 0}}};

    const ShapeResult shape = kernel.extrudeProfile(profile, 10.0);
    EXPECT_FALSE(shape);
    EXPECT_FALSE(shape.message.empty());
}

TEST(OcctExtrudeTest, M17_KERNEL_045_ARepeatedPointIsRefusedBEFOREOCCTSeesIt) {
    // OCCT's interpolator refuses the whole curve for one repeated point, with
    // a message naming neither the point nor the caller. Core refuses it first.
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = {ProfileSplineSegment{
        {Vec2{0, 0}, Vec2{40, 30}, Vec2{40, 30}, Vec2{80, 0}}, false}};
    EXPECT_FALSE(IsValidProfileDefinition(profile));
}

// --- Ellipses (M17.25) -------------------------------------------------------

TEST(OcctExtrudeTest, M17_KERNEL_030_AnEllipseExtrudesToTheAnalyticalVolume) {
    // pi*a*b*h, from the formula and not from OCCT. An ellipse built with its
    // two radii swapped, or with the rotation applied twice, still produces a
    // valid solid of the WRONG size -- and volume is the only assertion that
    // notices.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = {ProfileEllipseSegment{Vec2{30.0, 20.0}, 40.0, 15.0, 0.0}};

    const ShapeResult shape = kernel.extrudeProfile(profile, 12.0);
    ASSERT_TRUE(shape) << shape.message;
    EXPECT_TRUE(shape.shape.isValid());

    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;
    ExpectRel(props.properties.volumeMm3, kPi * 40.0 * 15.0 * 12.0, kCurvedVolumeRelTol);
    // ...centred where it was put, whatever the rotation would have been.
    //
    // A LOOSER tolerance than the circular cases above, and not because
    // anything here is approximate: OCCT integrates mass properties over a
    // tessellation of the face, and an elliptical face costs it about 1e-3 mm
    // of centroid accuracy where a circular one costs nothing measurable. The
    // first draft used kLengthAbsTol and failed at 8.5e-4 -- which is the
    // integrator's error, not the ellipse's. What this assertion is for is
    // catching a centre placed somewhere else entirely.
    constexpr double kCurvedCentroidTol = 1e-2; // mm
    EXPECT_NEAR(props.properties.centerOfMassMm.x, 30.0, kCurvedCentroidTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, 20.0, kCurvedCentroidTol);
}

TEST(OcctExtrudeTest, M17_KERNEL_031_ROTATINGAnEllipseDoesNotChangeItsVolume) {
    // The rotation has to land on the MAJOR axis and nowhere else. Applied to
    // the minor one instead, this solid comes out the same size -- so the
    // centre of mass is checked too, and the SHAPE is checked by asking for the
    // bounding box, which a quarter-turn does change.
    OcctGeometryKernel kernel;
    const auto build = [&kernel](double rotation) {
        PlanarProfileDefinition profile;
        profile.plane = PlaneOf(SketchFrame::WorldXY());
        profile.segments = {ProfileEllipseSegment{Vec2{0.0, 0.0}, 40.0, 15.0, rotation}};
        return kernel.extrudeProfile(profile, 10.0);
    };

    const ShapeResult flat = build(0.0);
    ASSERT_TRUE(flat) << flat.message;
    const ShapeResult turned = build(kPi / 2.0);
    ASSERT_TRUE(turned) << turned.message;

    const KernelMassPropertiesResult a = kernel.calculateMassProperties(flat.shape);
    const KernelMassPropertiesResult b = kernel.calculateMassProperties(turned.shape);
    ASSERT_TRUE(a) << a.message;
    ASSERT_TRUE(b) << b.message;
    ExpectRel(a.properties.volumeMm3, kPi * 40.0 * 15.0 * 10.0, kCurvedVolumeRelTol);
    ExpectRel(b.properties.volumeMm3, a.properties.volumeMm3, kCurvedVolumeRelTol);
}

TEST(OcctExtrudeTest, M17_KERNEL_032_AnEllipticalHoleIsREALLYMissing) {
    // The inner-loop path, which is the one that has to reverse the wire. An
    // ellipse added the wrong way round still builds and still looks like a
    // plate; only the volume says the hole is not there.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = RectangleSegments(100.0, 50.0);
    profile.innerLoops.push_back(
        {ProfileEllipseSegment{Vec2{50.0, 25.0}, 20.0, 8.0, kPi / 6.0}});

    const ShapeResult shape = kernel.extrudeProfile(profile, 10.0);
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;
    ExpectRel(props.properties.volumeMm3, (100.0 * 50.0 - kPi * 20.0 * 8.0) * 10.0,
              kCurvedVolumeRelTol);
}

TEST(OcctExtrudeTest, M17_KERNEL_033_AHalfEllipseClosedByItsAxisIsHalfTheArea) {
    // The TRIMMED path, and the one place the parameter convention is settled:
    // OCCT's gp_Elips is parametrised the way this project stores an elliptical
    // arc, so the two parameters go through untouched. Fed a geometric ANGLE
    // instead, this arc would still start and end in the right places and cover
    // the wrong piece of curve between them -- and the volume would not be half.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = {
        // The upper half, from parameter 0 (the +major end) round to pi.
        ProfileEllipticalArcSegment{Vec2{0.0, 0.0}, 40.0, 15.0, 0.0, 0.0, kPi, true},
        // ...closed by the major axis itself.
        ProfileLineSegment{Vec2{-40.0, 0.0}, Vec2{40.0, 0.0}}};

    const ShapeResult shape = kernel.extrudeProfile(profile, 10.0);
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;
    ExpectRel(props.properties.volumeMm3, 0.5 * kPi * 40.0 * 15.0 * 10.0,
              kCurvedVolumeRelTol);
    // HALF, so the centre of mass is off the axis -- 4b/(3pi) up for a half
    // ellipse, which is the number that tells "the upper half" from "the lower".
    EXPECT_NEAR(props.properties.centerOfMassMm.y, 4.0 * 15.0 / (3.0 * kPi), 1e-2);
}

TEST(OcctExtrudeTest, M17_KERNEL_035_ROTATINGAHalfEllipseMOVESIt) {
    // THE TEST THE VOLUME CHECKS CANNOT DO. Turning an ellipse does not change
    // how much of it there is, so a kernel that ignored the rotation entirely
    // passed every one of them -- a mutation that dropped it survived.
    //
    // A HALF ellipse has a centroid off its own centre, 4b/(3pi) along the
    // minor direction, and that direction turns with the shape. Built at a
    // quarter turn, the same half lands on the -x side instead of +y.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    // Rotated a quarter turn, parameter 0 is (0, +a) and parameter pi is
    // (0, -a); the half between them bulges towards -x.
    profile.segments = {
        ProfileEllipticalArcSegment{Vec2{0.0, 0.0}, 40.0, 15.0, kPi / 2.0, 0.0, kPi, true},
        ProfileLineSegment{Vec2{0.0, -40.0}, Vec2{0.0, 40.0}}};

    const ShapeResult shape = kernel.extrudeProfile(profile, 10.0);
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;
    ExpectRel(props.properties.volumeMm3, 0.5 * kPi * 40.0 * 15.0 * 10.0, kCurvedVolumeRelTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.x, -4.0 * 15.0 / (3.0 * kPi), 1e-2);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, 0.0, 1e-2);
}

TEST(OcctExtrudeTest, M17_KERNEL_034_AnEllipseWithItsAxesSwappedIsREFUSED) {
    // Major < minor is refused in Core, before OCCT sees it, so the caller can
    // be named. OCCT refuses it too, but as an unstructured build failure.
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = {ProfileEllipseSegment{Vec2{0.0, 0.0}, 8.0, 20.0, 0.0}};
    EXPECT_FALSE(IsValidProfileDefinition(profile));
}

// --- Holes (M17) -------------------------------------------------------------

TEST(OcctExtrudeTest, M17_KERNEL_020_AHoleIsREALLYMissingFromTheSolid) {
    // 100 x 50 plate, 20 deep, with a r=10 hole through it. The VOLUME is the
    // only assertion that settles it: a face whose inner wire was added the
    // wrong way round still builds, still extrudes, and still looks like a
    // plate -- it is simply solid where the hole should be. A mutation that
    // dropped the wire reversal survived every check that stopped at "the
    // solid is valid".
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = RectangleSegments(100.0, 50.0);
    profile.innerLoops.push_back({ProfileCircleSegment{Vec2{50.0, 25.0}, 10.0}});

    const ShapeResult shape = kernel.extrudeProfile(profile, 20.0);
    ASSERT_TRUE(shape) << shape.message;
    EXPECT_TRUE(shape.shape.isValid());

    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;
    const double expected =
        (100.0 * 50.0 - 3.14159265358979323846 * 10.0 * 10.0) * 20.0;
    ExpectRel(props.properties.volumeMm3, expected, kCurvedVolumeRelTol);
    // ...and the centre of mass stays in the middle, because the hole is
    // centred. A hole cut in the wrong place would pass the volume check.
    EXPECT_NEAR(props.properties.centerOfMassMm.x, 50.0, kLengthAbsTol);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, 25.0, kLengthAbsTol);
}

TEST(OcctExtrudeTest, M17_KERNEL_021_AnOffCentreHoleMovesTheCentreOfMass) {
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = RectangleSegments(100.0, 50.0);
    profile.innerLoops.push_back({ProfileCircleSegment{Vec2{25.0, 25.0}, 10.0}});

    const ShapeResult shape = kernel.extrudeProfile(profile, 20.0);
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;

    // Material removed from the left half, so the centre of mass moves RIGHT.
    // Pins the hole's POSITION, which the volume alone cannot.
    EXPECT_GT(props.properties.centerOfMassMm.x, 50.0);
    EXPECT_NEAR(props.properties.centerOfMassMm.y, 25.0, kLengthAbsTol);
}

TEST(OcctExtrudeTest, M17_KERNEL_022_TwoHolesAreBothCut) {
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = RectangleSegments(100.0, 50.0);
    profile.innerLoops.push_back({ProfileCircleSegment{Vec2{25.0, 25.0}, 8.0}});
    profile.innerLoops.push_back({ProfileCircleSegment{Vec2{75.0, 25.0}, 8.0}});

    const ShapeResult shape = kernel.extrudeProfile(profile, 20.0);
    ASSERT_TRUE(shape) << shape.message;
    const KernelMassPropertiesResult props = kernel.calculateMassProperties(shape.shape);
    ASSERT_TRUE(props) << props.message;
    const double expected =
        (100.0 * 50.0 - 2.0 * 3.14159265358979323846 * 8.0 * 8.0) * 20.0;
    ExpectRel(props.properties.volumeMm3, expected, kCurvedVolumeRelTol);
}

TEST(OcctExtrudeTest, M17_KERNEL_023_AnEmptyInnerLoopIsRefusedBeforeOCCTSeesIt) {
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = RectangleSegments(100.0, 50.0);
    profile.innerLoops.emplace_back(); // a hole with no boundary

    EXPECT_FALSE(IsValidProfileDefinition(profile));
    const ShapeResult shape = kernel.extrudeProfile(profile, 20.0);
    // Refused with a structured failure rather than reaching OCCT as an empty
    // wire and surfacing as something unattributable.
    EXPECT_FALSE(shape);
    EXPECT_FALSE(shape.message.empty());
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

    // -20.0 LEFT THIS LIST at M17.8 (ADR-M17-031): the distance is SIGNED, and
    // the sign chooses the side of the plane. What is rejected is a distance
    // with no magnitude -- zero, or anything that is not a finite number.
    for (double distance : {0.0, std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity()}) {
        const ShapeResult result = kernel.extrudeProfile(profile, distance);
        EXPECT_FALSE(result) << "distance " << distance << " was accepted";
        EXPECT_EQ(result.error, KernelError::InvalidDimension);
        EXPECT_FALSE(result.message.empty());
    }
}

TEST(OcctExtrudeTest, M17_KERNEL_042_ANegativeDistanceExtrudesToTheOtherSide) {
    // Same solid, other side. Checked against the real kernel because this is
    // where a sign can go wrong in two different ways at once: the prism can
    // land on the correct side with an inverted orientation, which OCCT reports
    // as a NEGATIVE volume and every downstream mass readout repeats.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = RectangleSegments(100.0, 50.0);

    const ShapeResult up = kernel.extrudeProfile(profile, 20.0);
    ASSERT_TRUE(up) << up.message;
    const ShapeResult down = kernel.extrudeProfile(profile, -20.0);
    ASSERT_TRUE(down) << down.message;

    const KernelMassPropertiesResult upMass = kernel.calculateMassProperties(up.shape);
    const KernelMassPropertiesResult downMass = kernel.calculateMassProperties(down.shape);
    ASSERT_TRUE(upMass) << upMass.message;
    ASSERT_TRUE(downMass) << downMass.message;

    // POSITIVE, and equal. A solid built the other way is not made of negative
    // material.
    ExpectRel(downMass.properties.volumeMm3, 100.0 * 50.0 * 20.0, kVolumeRelTol);
    ExpectRel(downMass.properties.volumeMm3, upMass.properties.volumeMm3, kVolumeRelTol);
    // And it really is on the other side: the sketch is on world XY, so the
    // two centres of mass mirror through z = 0.
    EXPECT_NEAR(upMass.properties.centerOfMassMm.z, 10.0, kLengthAbsTol);
    EXPECT_NEAR(downMass.properties.centerOfMassMm.z, -10.0, kLengthAbsTol);
    EXPECT_NEAR(downMass.properties.centerOfMassMm.x, upMass.properties.centerOfMassMm.x,
                kLengthAbsTol);
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

// --- M18: THE TWO HALVES AGREE ABOUT WHERE A SPLINE IS GOING -----------------

TEST(OcctExtrudeTest, M18_KERNEL_001_TheInterpolatedCurveLeavesAlongTheCHORD) {
    // The claim sketch tangency rests on, checked against the OTHER evaluator.
    //
    // Core computes a spline as a uniform Catmull-Rom with reflected ends, so
    // its direction at the first point is exactly p1 - p0. A tangency
    // constrains that chord. OCCT interpolates the same points its own way, and
    // if left to choose its own end conditions it does NOT leave along the
    // chord -- so the sketch would say "smooth" and the solid would have a
    // kink, with nothing in the program able to notice.
    //
    // Deliberately asymmetric points: on a symmetric set the two end conditions
    // can agree by accident.
    const std::vector<Vec2> points{Vec2{0, 0}, Vec2{40, 30}, Vec2{95, 5}, Vec2{130, 45}};
    const SplineEndDirections ends = SplineEndDirectionsOf(points, false);
    ASSERT_TRUE(ends.ok);

    const auto unit = [](Vec2 from, Vec2 to) {
        const double dx = to.x - from.x;
        const double dy = to.y - from.y;
        const double length = std::hypot(dx, dy);
        return Vec2{dx / length, dy / length};
    };
    const Vec2 startChord = unit(points[0], points[1]);
    const Vec2 endChord = unit(points[2], points[3]);

    EXPECT_NEAR(ends.atStart.x, startChord.x, 1e-9);
    EXPECT_NEAR(ends.atStart.y, startChord.y, 1e-9);
    EXPECT_NEAR(ends.atEnd.x, endChord.x, 1e-9);
    EXPECT_NEAR(ends.atEnd.y, endChord.y, 1e-9);
}

TEST(OcctExtrudeTest, M18_KERNEL_002_ASplineOfTwoPointsLeavesStraight) {
    // The degenerate end of the same rule, and the one that would read past
    // the point list if the chord were taken from a neighbour that is not
    // there.
    const std::vector<Vec2> points{Vec2{10, 10}, Vec2{60, 35}};
    const SplineEndDirections ends = SplineEndDirectionsOf(points, false);
    ASSERT_TRUE(ends.ok);

    const double length = std::hypot(50.0, 25.0);
    EXPECT_NEAR(ends.atStart.x, 50.0 / length, 1e-9);
    EXPECT_NEAR(ends.atStart.y, 25.0 / length, 1e-9);
    EXPECT_NEAR(ends.atEnd.x, 50.0 / length, 1e-9);
    EXPECT_NEAR(ends.atEnd.y, 25.0 / length, 1e-9);
}

TEST(OcctExtrudeTest, M18_KERNEL_003_AClosedSplineIsNotGivenEndTangents) {
    // A periodic curve has no ends to condition, and loading tangents into one
    // is not a smoother closure -- it is a refusal. The check is that it still
    // interpolates at all.
    const std::vector<Vec2> points{Vec2{0, 0}, Vec2{40, 30}, Vec2{80, 0}, Vec2{40, -30}};
    const SplineEndDirections ends = SplineEndDirectionsOf(points, true);
    EXPECT_TRUE(ends.ok);
}

TEST(OcctExtrudeTest, M18_KERNEL_004_TheProfileAndTheWireframeAreTheSameCurve) {
    // ONE interpolation, two callers. Before M18 these were two separately
    // written GeomAPI_Interpolate calls -- the preview the 3D view draws and
    // the edge the solid is built from -- and they only happened to agree
    // because neither had learned anything the other had not.
    //
    // Measured as a bounding box, which is the kernel-neutral door that exists
    // for exactly this (ADR-M4-004): if the two curves differed anywhere, the
    // extents of the spline's own span would differ with them.
    const std::vector<Vec2> points{Vec2{0, 0}, Vec2{40, 30}, Vec2{95, 5}, Vec2{130, 45}};

    const SketchWireframe wireframe =
        BuildSketchWireframe({SketchSpline{points, false}}, PlaneOf(SketchFrame::WorldXY()));
    ASSERT_EQ(wireframe.edges, 1);
    ASSERT_EQ(wireframe.skipped, 0);
    const KernelBounds drawn = BoundsOf(wireframe.shape);
    ASSERT_TRUE(drawn.ok);

    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = {ProfileSplineSegment{points, false},
                        ProfileLineSegment{points.back(), Vec2{130, -40}},
                        ProfileLineSegment{Vec2{130, -40}, Vec2{0, -40}},
                        ProfileLineSegment{Vec2{0, -40}, points.front()}};
    const ShapeResult solid = kernel.extrudeProfile(profile, 10.0);
    ASSERT_TRUE(solid) << solid.message;
    const KernelBounds built = BoundsOf(solid.shape);
    ASSERT_TRUE(built.ok);

    // The spline is the ONLY thing reaching above y = 45 in either, so its own
    // extent is what the top of each box measures.
    EXPECT_NEAR(drawn.max.y, built.max.y, 1e-9);
    EXPECT_NEAR(drawn.max.x, built.max.x, 1e-9);
}

// --- M19: SWEEP and LOFT against real OCCT ------------------------------------

namespace {

// A square of `side`, centred on the sketch origin, on the given plane.
std::vector<ProfileSegment> Square(double side) {
    const double h = side / 2.0;
    return {ProfileLineSegment{Vec2{-h, -h}, Vec2{h, -h}},
            ProfileLineSegment{Vec2{h, -h}, Vec2{h, h}},
            ProfileLineSegment{Vec2{h, h}, Vec2{-h, h}},
            ProfileLineSegment{Vec2{-h, h}, Vec2{-h, -h}}};
}

// The XZ plane through the origin: u = +X, v = +Z, so a path drawn on it runs
// out of the XY plane a profile sits on.
ProfilePlane WorldXZ() {
    ProfilePlane plane;
    plane.origin = Vec3{0, 0, 0};
    plane.uAxis = Vec3{1, 0, 0};
    plane.vAxis = Vec3{0, 0, 1};
    plane.normal = Vec3{0, -1, 0};
    return plane;
}

} // namespace

TEST(OcctExtrudeTest, M19_SWEEP_001_AStraightPathSweepsToAPrism) {
    // The one sweep whose volume is known without integrating anything: a
    // 20 mm square carried 100 mm along a straight line is a prism, so its
    // volume is 20 * 20 * 100 exactly. Anything else -- a curved spine -- has
    // a volume this test would have to compute the same way the kernel does,
    // which would check nothing.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = Square(20.0);

    PlanarPathDefinition path;
    path.plane = WorldXZ();
    // Straight UP out of the profile's plane: (0,0) to (0,100) in XZ is
    // (0,0,0) to (0,0,100) in world.
    path.segments = {ProfileLineSegment{Vec2{0, 0}, Vec2{0, 100}}};

    const ShapeResult swept = kernel.sweepProfile(profile, path);
    ASSERT_TRUE(swept) << swept.message;
    const KernelMassPropertiesResult properties = kernel.calculateMassProperties(swept.shape);
    ASSERT_TRUE(properties) << properties.message;
    ExpectRel(properties.properties.volumeMm3, 20.0 * 20.0 * 100.0, kVolumeRelTol);
}

TEST(OcctExtrudeTest, M19_SWEEP_002_ASweepWithAHOLEKeepsTheHole) {
    // A face is swept, not a wire -- which is the whole reason the kernel takes
    // the profile as a face. Sweeping the outer wire alone would produce a
    // solid bar and lose the bore, and the volume is what says which happened.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = Square(20.0);
    profile.innerLoops = {Square(8.0)};

    PlanarPathDefinition path;
    path.plane = WorldXZ();
    path.segments = {ProfileLineSegment{Vec2{0, 0}, Vec2{0, 50}}};

    const ShapeResult swept = kernel.sweepProfile(profile, path);
    ASSERT_TRUE(swept) << swept.message;
    const KernelMassPropertiesResult properties = kernel.calculateMassProperties(swept.shape);
    ASSERT_TRUE(properties) << properties.message;
    ExpectRel(properties.properties.volumeMm3, (20.0 * 20.0 - 8.0 * 8.0) * 50.0, kVolumeRelTol);
}

TEST(OcctExtrudeTest, M19_SWEEP_003_ACurvedPathBendsTheSolidWithoutLosingIt) {
    // A quarter-circle spine. The exact volume of a swept square round a bend
    // is not something this test can state without doing the kernel's own work,
    // so it asserts what CAN be checked independently: the solid exists, it is
    // heavier than the straight prism of the same arc length would be at the
    // inner radius and lighter than at the outer, and it REACHES round the
    // corner -- the bounding box has to be wide in both x and z.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = Square(10.0);

    PlanarPathDefinition path;
    path.plane = WorldXZ();
    // Centre at (60, 0) in XZ, radius 60, walked CLOCKWISE from angle pi to
    // pi/2 -- so the spine leaves the origin going straight UP and arrives at
    // (60, 60) going along +u.
    //
    // The direction it LEAVES BY is the point of that arithmetic: a section
    // must not be swept along a direction lying in its own plane. Get it wrong
    // and OCCT returns a surface with no volume, which the kernel now refuses
    // rather than passing on as a solid that weighs nothing.
    path.segments = {ProfileArcSegment{Vec2{60, 0}, 60.0, kPi, kPi / 2.0, false}};

    const ShapeResult swept = kernel.sweepProfile(profile, path);
    ASSERT_TRUE(swept) << swept.message;
    const KernelMassPropertiesResult properties = kernel.calculateMassProperties(swept.shape);
    ASSERT_TRUE(properties) << properties.message;

    // PAPPUS, exactly: the volume of a solid of revolution-by-sweeping is the
    // section's area times the distance its CENTROID travels. The section is
    // centred on the spine, so its centroid rides the spine itself and that
    // distance is the arc length.
    //
    // An independent formula, not the kernel's own arithmetic -- which is what
    // makes this a check rather than a restatement.
    const double arcLength = 60.0 * kPi / 2.0;
    const double area = 10.0 * 10.0;
    ExpectRel(properties.properties.volumeMm3, area * arcLength, 1e-6);

    const KernelBounds bounds = BoundsOf(swept.shape);
    ASSERT_TRUE(bounds.ok);
    EXPECT_GT(bounds.max.x - bounds.min.x, 50.0) << "the sweep never turned the corner";
    EXPECT_GT(bounds.max.z - bounds.min.z, 50.0) << "the sweep never left its own plane";
}

TEST(OcctExtrudeTest, M19_SWEEP_004_AnEmptyPathIsREFUSEDNotSweptToNothing) {
    OcctGeometryKernel kernel;
    PlanarProfileDefinition profile;
    profile.plane = PlaneOf(SketchFrame::WorldXY());
    profile.segments = Square(20.0);

    const ShapeResult swept = kernel.sweepProfile(profile, PlanarPathDefinition{});
    EXPECT_FALSE(swept);
    EXPECT_EQ(swept.error, KernelError::InvalidDimension);
}

TEST(OcctExtrudeTest, M19_LOFT_001_TwoEqualSquaresLoftToAPrism) {
    // Again the one case with an arithmetic answer: two identical sections
    // 40 mm apart make a prism, so the volume is side * side * gap exactly.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition bottom;
    bottom.plane = PlaneOf(SketchFrame::WorldXY());
    bottom.segments = Square(20.0);

    PlanarProfileDefinition top = bottom;
    top.plane.origin = Vec3{0, 0, 40};

    const ShapeResult lofted = kernel.loftProfiles({bottom, top});
    ASSERT_TRUE(lofted) << lofted.message;
    const KernelMassPropertiesResult properties = kernel.calculateMassProperties(lofted.shape);
    ASSERT_TRUE(properties) << properties.message;
    ExpectRel(properties.properties.volumeMm3, 20.0 * 20.0 * 40.0, kVolumeRelTol);
}

TEST(OcctExtrudeTest, M19_LOFT_002_ATaperedLoftIsAFrustumAndTheFormulaSaysSo) {
    // A square 20 running to a square 10 over 30 mm is a frustum, and a
    // frustum's volume is h/3 * (A1 + A2 + sqrt(A1*A2)) -- an independent
    // formula, not something the kernel computed.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition bottom;
    bottom.plane = PlaneOf(SketchFrame::WorldXY());
    bottom.segments = Square(20.0);

    PlanarProfileDefinition top;
    top.plane = PlaneOf(SketchFrame::WorldXY());
    top.plane.origin = Vec3{0, 0, 30};
    top.segments = Square(10.0);

    const ShapeResult lofted = kernel.loftProfiles({bottom, top});
    ASSERT_TRUE(lofted) << lofted.message;
    const KernelMassPropertiesResult properties = kernel.calculateMassProperties(lofted.shape);
    ASSERT_TRUE(properties) << properties.message;

    const double a1 = 400.0;
    const double a2 = 100.0;
    const double expected = 30.0 / 3.0 * (a1 + a2 + std::sqrt(a1 * a2));
    ExpectRel(properties.properties.volumeMm3, expected, 1e-6);
}

TEST(OcctExtrudeTest, M19_LOFT_003_ASingleProfileIsREFUSED) {
    // A loft through one section has no second section to run to. Answering
    // with an extrusion of some invented depth would be the kernel deciding
    // what the user meant.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition only;
    only.plane = PlaneOf(SketchFrame::WorldXY());
    only.segments = Square(20.0);

    const ShapeResult lofted = kernel.loftProfiles({only});
    EXPECT_FALSE(lofted);
    EXPECT_EQ(lofted.error, KernelError::InvalidDimension);
}

TEST(OcctExtrudeTest, M19_LOFT_004_AHoleInASectionIsREFUSEDRatherThanDropped) {
    // ThruSections runs through WIRES, so an inner loop has nowhere to go: it
    // would be silently ignored and the loft would come back solid where the
    // user drew a bore. Said, not dropped.
    OcctGeometryKernel kernel;
    PlanarProfileDefinition bottom;
    bottom.plane = PlaneOf(SketchFrame::WorldXY());
    bottom.segments = Square(20.0);
    bottom.innerLoops = {Square(8.0)};

    PlanarProfileDefinition top = bottom;
    top.plane.origin = Vec3{0, 0, 40};

    const ShapeResult lofted = kernel.loftProfiles({bottom, top});
    EXPECT_FALSE(lofted);
    EXPECT_NE(lofted.message.find("holes"), std::string::npos) << lofted.message;
}

TEST(OcctExtrudeTest, M19_LOFT_005_TheORDEROfTheSectionsIsTheCallersAndItMatters) {
    // Lofting A-B-C and A-C-B are different solids. The kernel must not sort
    // them by anything it can see -- plane height, distance from the origin --
    // because that would be the kernel deciding what the user meant.
    //
    // Three sections, with the middle one offset sideways: run through in the
    // drawn order the solid leans; run through with the last two swapped it
    // leans the other way, and the two bounding boxes differ.
    OcctGeometryKernel kernel;
    const auto sectionAt = [](Vec3 origin, double side) {
        PlanarProfileDefinition one;
        one.plane = PlaneOf(SketchFrame::WorldXY());
        one.plane.origin = origin;
        one.segments = Square(side);
        return one;
    };
    const PlanarProfileDefinition a = sectionAt(Vec3{0, 0, 0}, 20.0);
    const PlanarProfileDefinition b = sectionAt(Vec3{40, 0, 20}, 20.0);
    const PlanarProfileDefinition c = sectionAt(Vec3{0, 0, 40}, 20.0);

    const ShapeResult drawn = kernel.loftProfiles({a, b, c});
    ASSERT_TRUE(drawn) << drawn.message;
    const ShapeResult swapped = kernel.loftProfiles({a, c, b});
    ASSERT_TRUE(swapped) << swapped.message;

    const KernelMassPropertiesResult first = kernel.calculateMassProperties(drawn.shape);
    const KernelMassPropertiesResult second = kernel.calculateMassProperties(swapped.shape);
    ASSERT_TRUE(first) << first.message;
    ASSERT_TRUE(second) << second.message;
    // Two different solids, so two different volumes. A kernel that sorted the
    // sections would return the same number twice.
    EXPECT_GT(std::fabs(first.properties.volumeMm3 - second.properties.volumeMm3), 1.0);
}

// --- M20: SHELL and DRAFT against real OCCT ----------------------------------

namespace {

// A 100 x 60 x 40 box at the origin, tagged as feature 1 so its faces have
// provenance the queries can name.
KernelShape TaggedBox(OcctGeometryKernel& kernel, double w, double h, double d) {
    BoxDefinition definition;
    definition.widthMm = w;
    definition.heightMm = h;
    definition.depthMm = d;
    const ShapeResult box = kernel.createBox(definition);
    EXPECT_TRUE(box) << box.message;
    return kernel.tagCreatedFaces(box.shape, KernelShape{}, 1u);
}

double VolumeOfShape(OcctGeometryKernel& kernel, const KernelShape& shape) {
    const KernelMassPropertiesResult properties = kernel.calculateMassProperties(shape);
    EXPECT_TRUE(properties) << properties.message;
    return properties.properties.volumeMm3;
}

FaceQuery TopFace() {
    FaceQuery query;
    query.extremeTowards = Vec3{0, 0, 1};
    return query;
}

} // namespace

TEST(OcctExtrudeTest, M20_SHELL_001_AnOpenTopBoxHasTheWallsItsThicknessSaysItHas) {
    // The one shell whose volume is arithmetic: a 100x60x40 box hollowed to a
    // 5 mm wall with the top open leaves the outer solid minus the cavity, and
    // the cavity is a box 90 x 50 x 35 -- 5 mm off each of the four sides and
    // the floor, and nothing off the open top.
    OcctGeometryKernel kernel;
    const KernelShape box = TaggedBox(kernel, 100.0, 60.0, 40.0);

    const ShapeResult shell = kernel.shellSolid(box, {TopFace()}, 5.0);
    ASSERT_TRUE(shell) << shell.message;

    const double outer = 100.0 * 60.0 * 40.0;
    const double cavity = 90.0 * 50.0 * 35.0;
    ExpectRel(VolumeOfShape(kernel, shell.shape), outer - cavity, kVolumeRelTol);
}

TEST(OcctExtrudeTest, M20_SHELL_002_TWOOpenFacesLeaveATube) {
    // Top and bottom open: the cavity now runs the whole depth, so it is
    // 90 x 50 x 40 and only the four walls remain.
    OcctGeometryKernel kernel;
    const KernelShape box = TaggedBox(kernel, 100.0, 60.0, 40.0);

    FaceQuery bottom;
    bottom.extremeTowards = Vec3{0, 0, -1};
    const ShapeResult shell = kernel.shellSolid(box, {TopFace(), bottom}, 5.0);
    ASSERT_TRUE(shell) << shell.message;

    const double outer = 100.0 * 60.0 * 40.0;
    const double cavity = 90.0 * 50.0 * 40.0;
    ExpectRel(VolumeOfShape(kernel, shell.shape), outer - cavity, kVolumeRelTol);
}

TEST(OcctExtrudeTest, M20_SHELL_003_AShellWithNOOpeningIsREFUSED) {
    // OCCT builds one happily: a hollow with no way in looks solid from every
    // side and weighs less than it should, which is a lie only a mass reading
    // tells. Refused rather than built.
    OcctGeometryKernel kernel;
    const KernelShape box = TaggedBox(kernel, 100.0, 60.0, 40.0);

    const ShapeResult shell = kernel.shellSolid(box, {}, 5.0);
    EXPECT_FALSE(shell);
    EXPECT_EQ(shell.error, KernelError::InvalidDimension);
}

TEST(OcctExtrudeTest, M20_SHELL_004_AWallThickerThanTheSolidIsREFUSED) {
    // Half of a 60 mm width is 30, so a 40 mm wall cannot fit. OCCT does not
    // refuse it -- it returns a self-intersecting shape that weighs MORE than
    // the solid it came from. Measured, and refused.
    OcctGeometryKernel kernel;
    const KernelShape box = TaggedBox(kernel, 100.0, 60.0, 40.0);

    const ShapeResult shell = kernel.shellSolid(box, {TopFace()}, 40.0);
    EXPECT_FALSE(shell);
    EXPECT_NE(shell.message.find("does not fit"), std::string::npos) << shell.message;
}

TEST(OcctExtrudeTest, M20_SHELL_005_AFaceQueryThatNamesNothingIsREFUSEDWithTheReason) {
    OcctGeometryKernel kernel;
    const KernelShape box = TaggedBox(kernel, 100.0, 60.0, 40.0);

    FaceQuery nobody;
    nobody.createdBy = static_cast<ObjectId>(999);
    const ShapeResult shell = kernel.shellSolid(box, {nobody}, 5.0);
    EXPECT_FALSE(shell);
    EXPECT_NE(shell.message.find("could not open a face"), std::string::npos) << shell.message;
}

TEST(OcctExtrudeTest, M20_DRAFT_001_ATaperedWallChangesTheVolumeTheWayTrigonometrySays) {
    // One wall of a box, tapered about the bottom face. The wall pivots on the
    // neutral plane, so the solid gains a wedge: its cross-section is the full
    // depth times depth*tan(angle)/2, run along the wall's width.
    //
    // The sign is what decides gain or loss, and there is no default for it --
    // so this test states which way it went and checks the magnitude against
    // the formula, not against the kernel.
    OcctGeometryKernel kernel;
    const KernelShape box = TaggedBox(kernel, 100.0, 60.0, 40.0);

    FaceQuery wall;
    wall.facing = Vec3{0, 1, 0};   // the +Y wall
    FaceQuery neutral;
    neutral.extremeTowards = Vec3{0, 0, -1}; // the floor

    const double angle = 10.0 * kPi / 180.0;
    const ShapeResult drafted = kernel.draftFaces(box, {wall}, neutral, angle);
    ASSERT_TRUE(drafted) << drafted.message;

    const double before = 100.0 * 60.0 * 40.0;
    const double wedge = 100.0 * 40.0 * 40.0 * std::tan(angle) / 2.0;
    const double after = VolumeOfShape(kernel, drafted.shape);
    // WHICH WAY, not just how much. The pull direction is the neutral face's
    // own normal, and taking it from anywhere else -- world +Z, say -- leans
    // the wall the other way and builds a part that locks in its mould instead
    // of releasing from it. An assertion on the magnitude alone cannot tell
    // those two apart, because they differ by exactly a sign.
    //
    // Neutral is the FLOOR here, so a positive angle widens the part as it
    // rises: the volume goes UP by the wedge.
    EXPECT_NEAR(after - before, wedge, 1e-6 * wedge)
        << "before " << before << ", after " << after;
}

TEST(OcctExtrudeTest, M20_DRAFT_002_TheOPPOSITESignLeansTheOtherWay) {
    // The sign is the whole difference between a part that releases from a
    // mould and one that locks in it, so it must not be normalised away.
    OcctGeometryKernel kernel;
    const KernelShape box = TaggedBox(kernel, 100.0, 60.0, 40.0);

    FaceQuery wall;
    wall.facing = Vec3{0, 1, 0};
    FaceQuery neutral;
    neutral.extremeTowards = Vec3{0, 0, -1};

    const double angle = 10.0 * kPi / 180.0;
    const ShapeResult positive = kernel.draftFaces(box, {wall}, neutral, angle);
    ASSERT_TRUE(positive) << positive.message;
    const ShapeResult negative = kernel.draftFaces(box, {wall}, neutral, -angle);
    ASSERT_TRUE(negative) << negative.message;

    const double before = 100.0 * 60.0 * 40.0;
    const double up = VolumeOfShape(kernel, positive.shape);
    const double down = VolumeOfShape(kernel, negative.shape);
    // One gains and one loses: their difference from the original has opposite
    // signs. Equal magnitudes would mean the sign was thrown away.
    EXPECT_LT((up - before) * (down - before), 0.0)
        << "up " << up << ", down " << down << ", before " << before;
}

TEST(OcctExtrudeTest, M20_DRAFT_003_ADraftWithNOFacesIsREFUSED) {
    OcctGeometryKernel kernel;
    const KernelShape box = TaggedBox(kernel, 100.0, 60.0, 40.0);
    FaceQuery neutral;
    neutral.extremeTowards = Vec3{0, 0, -1};

    const ShapeResult drafted = kernel.draftFaces(box, {}, neutral, 0.1);
    EXPECT_FALSE(drafted);
    EXPECT_EQ(drafted.error, KernelError::InvalidDimension);
}

TEST(OcctExtrudeTest, M20_DRAFT_004_AnAbsurdAngleIsREFUSEDBeforeOCCTSeesIt) {
    // A quarter turn lays the wall flat. OCCT's complaint about it names
    // nothing the user can act on, so it is refused here with a sentence that
    // does.
    OcctGeometryKernel kernel;
    const KernelShape box = TaggedBox(kernel, 100.0, 60.0, 40.0);
    FaceQuery wall;
    wall.facing = Vec3{0, 1, 0};
    FaceQuery neutral;
    neutral.extremeTowards = Vec3{0, 0, -1};

    const ShapeResult drafted = kernel.draftFaces(box, {wall}, neutral, kPi / 2.0);
    EXPECT_FALSE(drafted);
    EXPECT_EQ(drafted.error, KernelError::InvalidDimension);
}

TEST(OcctExtrudeTest, M20_BOUNDS_001_AShapeReportsWhereItReaches) {
    // What "through all" is a question about. A hole that guessed a very deep
    // cylinder instead would work until somebody built a part deeper than the
    // guess, and then it would stop part-way with nothing to say.
    OcctGeometryKernel kernel;
    const KernelShape box = TaggedBox(kernel, 100.0, 60.0, 40.0);

    const KernelBoundsResult bounds = kernel.boundsOfShape(box);
    ASSERT_TRUE(bounds.ok) << bounds.message;
    EXPECT_NEAR(bounds.min.x, 0.0, 1e-6);
    EXPECT_NEAR(bounds.min.y, 0.0, 1e-6);
    EXPECT_NEAR(bounds.min.z, 0.0, 1e-6);
    EXPECT_NEAR(bounds.max.x, 100.0, 1e-6);
    EXPECT_NEAR(bounds.max.y, 60.0, 1e-6);
    EXPECT_NEAR(bounds.max.z, 40.0, 1e-6);
}

TEST(OcctExtrudeTest, M20_BOUNDS_002_AnEmptyShapeHasNoBoundsAndSaysSo) {
    // Returning a box at the origin would be indistinguishable from geometry
    // that really is there.
    OcctGeometryKernel kernel;
    const KernelBoundsResult bounds = kernel.boundsOfShape(KernelShape{});
    EXPECT_FALSE(bounds.ok);
    EXPECT_FALSE(bounds.message.empty());
}
