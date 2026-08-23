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
