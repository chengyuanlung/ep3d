// M4-B/C: Sketch stable identity, entity semantics, and coordinate frame
// (spec 18 "Sketch" matrix; ADR-M4-001/002).

#include "Core/Sketch/Sketch.h"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kLengthAbsTol = 1e-9; // mm; frame math is exact trigonometry
constexpr double kPi = 3.14159265358979323846;

void ExpectVec3Near(Vec3 actual, Vec3 expected, double tol = kLengthAbsTol) {
    EXPECT_NEAR(actual.x, expected.x, tol);
    EXPECT_NEAR(actual.y, expected.y, tol);
    EXPECT_NEAR(actual.z, expected.z, tol);
}

// --- Stable identity (spec 18: survive insertion / removal / reorder) -------

TEST(SketchTest, M4_SKETCH_001_EntityIdsAreUniqueAndNonInvalid) {
    Sketch sketch{"Sketch001"};
    const SketchEntityId a = sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId b = sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    const SketchEntityId c = sketch.addCircle(Vec2{10, 10}, 5.0);

    EXPECT_NE(a, kInvalidSketchEntityId);
    EXPECT_NE(b, kInvalidSketchEntityId);
    EXPECT_NE(c, kInvalidSketchEntityId);
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);
}

TEST(SketchTest, M4_SKETCH_002_IdsSurviveInsertionOfOtherEntities) {
    Sketch sketch{"Sketch001"};
    const SketchEntityId a = sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId b = sketch.addLine(Vec2{100, 0}, Vec2{100, 50});

    for (int i = 0; i < 10; ++i)
        sketch.addPoint(Vec2{static_cast<double>(i), 0.0});

    ASSERT_NE(sketch.findEntity(a), nullptr);
    ASSERT_NE(sketch.findEntity(b), nullptr);
    EXPECT_EQ(sketch.findEntity(a)->id, a);
    EXPECT_EQ(sketch.findEntity(b)->id, b);
}

TEST(SketchTest, M4_SKETCH_003_IdsSurviveRemovalOfOtherEntities) {
    Sketch sketch{"Sketch001"};
    const SketchEntityId first = sketch.addLine(Vec2{0, 0}, Vec2{10, 0});
    const SketchEntityId middle = sketch.addLine(Vec2{10, 0}, Vec2{10, 10});
    const SketchEntityId last = sketch.addLine(Vec2{10, 10}, Vec2{0, 0});

    // Removing the FIRST stored entity is the case that would break any
    // implementation using position as identity.
    ASSERT_TRUE(sketch.removeEntity(first));

    EXPECT_EQ(sketch.findEntity(first), nullptr);
    ASSERT_NE(sketch.findEntity(middle), nullptr);
    ASSERT_NE(sketch.findEntity(last), nullptr);
    EXPECT_EQ(sketch.findEntity(middle)->id, middle);
    EXPECT_EQ(sketch.findEntity(last)->id, last);
    // The surviving entities keep their geometry, not just their ids.
    const auto* line = std::get_if<SketchLine>(&sketch.findEntity(last)->geometry);
    ASSERT_NE(line, nullptr);
    EXPECT_DOUBLE_EQ(line->start.x, 10.0);
    EXPECT_DOUBLE_EQ(line->start.y, 10.0);
}

TEST(SketchTest, M4_SKETCH_004_RemovingUnknownIdIsRejected) {
    Sketch sketch{"Sketch001"};
    const SketchEntityId a = sketch.addLine(Vec2{0, 0}, Vec2{10, 0});
    EXPECT_FALSE(sketch.removeEntity(kInvalidSketchEntityId));
    EXPECT_TRUE(sketch.removeEntity(a));
    EXPECT_FALSE(sketch.removeEntity(a)); // already gone
}

TEST(SketchTest, M4_SKETCH_005_DuplicateRestoredIdRejected) {
    Sketch sketch{"Sketch001"};
    const SketchEntityId id{4242};
    EXPECT_TRUE(sketch.restoreEntity(id, SketchLine{Vec2{0, 0}, Vec2{10, 0}}));
    EXPECT_FALSE(sketch.restoreEntity(id, SketchLine{Vec2{0, 0}, Vec2{20, 0}}))
        << "a duplicate entity id was accepted";
    EXPECT_EQ(sketch.entities().size(), 1u);
}

TEST(SketchTest, M4_SKETCH_006_RestoredIdCannotCollideWithFreshlyGeneratedOnes) {
    // Entity ids come from the shared ObjectIdGenerator precisely so restore
    // inherits M1's collision safety (ADR-M4-001). A restored high id must
    // push the generator past itself.
    Sketch sketch{"Sketch001"};
    const SketchEntityId restored{900000};
    ASSERT_TRUE(sketch.restoreEntity(restored, SketchLine{Vec2{0, 0}, Vec2{1, 0}}));

    const SketchEntityId fresh = sketch.addLine(Vec2{0, 0}, Vec2{2, 0});
    EXPECT_GT(ToObjectId(fresh), ToObjectId(restored));
}

TEST(SketchTest, M4_SKETCH_007_RestoreAcceptsInvalidGeometryForLosslessRoundTrip) {
    // add* rejects invalid geometry, but restore must not silently drop an
    // entity from a hand-edited document -- it surfaces later as a profile
    // diagnostic instead.
    Sketch sketch{"Sketch001"};
    EXPECT_EQ(sketch.addCircle(Vec2{0, 0}, -5.0), kInvalidSketchEntityId);
    EXPECT_TRUE(sketch.entities().empty());

    EXPECT_TRUE(sketch.restoreEntity(SketchEntityId{77}, SketchCircle{Vec2{0, 0}, -5.0}));
    EXPECT_EQ(sketch.entities().size(), 1u);
}

// --- Entity semantics and validation (spec 18) -----------------------------

TEST(SketchTest, M4_SKETCH_010_ValidEntitiesAccepted) {
    Sketch sketch{"Sketch001"};
    EXPECT_NE(sketch.addPoint(Vec2{1, 2}), kInvalidSketchEntityId);
    EXPECT_NE(sketch.addLine(Vec2{0, 0}, Vec2{100, 0}), kInvalidSketchEntityId);
    EXPECT_NE(sketch.addCircle(Vec2{0, 0}, 10.0), kInvalidSketchEntityId);
    EXPECT_NE(sketch.addArc(Vec2{0, 0}, 10.0, 0.0, kPi / 2), kInvalidSketchEntityId);
}

TEST(SketchTest, M4_SKETCH_011_ZeroLengthLineRejected) {
    Sketch sketch{"Sketch001"};
    EXPECT_EQ(sketch.addLine(Vec2{5, 5}, Vec2{5, 5}), kInvalidSketchEntityId);
    // Degenerate but not exactly equal: still below tolerance.
    EXPECT_EQ(sketch.addLine(Vec2{0, 0}, Vec2{1e-12, 0}), kInvalidSketchEntityId);
}

TEST(SketchTest, M4_SKETCH_012_InvalidRadiiRejected) {
    Sketch sketch{"Sketch001"};
    EXPECT_EQ(sketch.addCircle(Vec2{0, 0}, 0.0), kInvalidSketchEntityId);
    EXPECT_EQ(sketch.addCircle(Vec2{0, 0}, -1.0), kInvalidSketchEntityId);
    EXPECT_EQ(sketch.addArc(Vec2{0, 0}, 0.0, 0.0, kPi), kInvalidSketchEntityId);
    EXPECT_EQ(sketch.addArc(Vec2{0, 0}, -3.0, 0.0, kPi), kInvalidSketchEntityId);
}

TEST(SketchTest, M4_SKETCH_013_NonFiniteValuesRejected) {
    Sketch sketch{"Sketch001"};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    EXPECT_EQ(sketch.addPoint(Vec2{nan, 0}), kInvalidSketchEntityId);
    EXPECT_EQ(sketch.addPoint(Vec2{0, inf}), kInvalidSketchEntityId);
    EXPECT_EQ(sketch.addLine(Vec2{0, 0}, Vec2{nan, 5}), kInvalidSketchEntityId);
    EXPECT_EQ(sketch.addLine(Vec2{-inf, 0}, Vec2{5, 5}), kInvalidSketchEntityId);
    EXPECT_EQ(sketch.addCircle(Vec2{0, 0}, nan), kInvalidSketchEntityId);
    EXPECT_EQ(sketch.addCircle(Vec2{0, 0}, inf), kInvalidSketchEntityId);
    EXPECT_EQ(sketch.addArc(Vec2{0, 0}, 10.0, nan, kPi), kInvalidSketchEntityId);
    EXPECT_EQ(sketch.addArc(Vec2{0, 0}, 10.0, 0.0, inf), kInvalidSketchEntityId);
}

TEST(SketchTest, M4_SKETCH_014_DegenerateArcSweepRejected) {
    Sketch sketch{"Sketch001"};
    // Zero sweep is neither an arc nor a circle; a full turn must be a Circle.
    EXPECT_EQ(sketch.addArc(Vec2{0, 0}, 10.0, 1.0, 1.0), kInvalidSketchEntityId);
}

TEST(SketchTest, M4_SKETCH_015_EndpointQueries) {
    // Point and Circle have no chainable endpoints; Line and Arc do.
    EXPECT_FALSE(HasEndpoints(SketchGeometry{SketchPoint{Vec2{1, 1}}}));
    EXPECT_FALSE(HasEndpoints(SketchGeometry{SketchCircle{Vec2{0, 0}, 10.0}}));
    EXPECT_TRUE(HasEndpoints(SketchGeometry{SketchLine{Vec2{0, 0}, Vec2{5, 0}}}));

    const SketchGeometry line{SketchLine{Vec2{1, 2}, Vec2{7, 9}}};
    EXPECT_DOUBLE_EQ(StartPointOf(line).x, 1.0);
    EXPECT_DOUBLE_EQ(EndPointOf(line).y, 9.0);

    // Arc endpoints are derived from centre/radius/angles, computed here with
    // the raw formula rather than by calling the production helper twice.
    const SketchGeometry arc{SketchArc{Vec2{0, 0}, 10.0, 0.0, kPi / 2, true}};
    ASSERT_TRUE(HasEndpoints(arc));
    EXPECT_NEAR(StartPointOf(arc).x, 10.0, kLengthAbsTol);
    EXPECT_NEAR(StartPointOf(arc).y, 0.0, kLengthAbsTol);
    EXPECT_NEAR(EndPointOf(arc).x, 0.0, kLengthAbsTol);
    EXPECT_NEAR(EndPointOf(arc).y, 10.0, kLengthAbsTol);
}

// --- Coordinate frame (spec 6/18: XY, translated, rotated) -----------------

TEST(SketchTest, M4_FRAME_001_WorldXyIsTheIdentityCase) {
    const SketchFrame frame = SketchFrame::WorldXY();
    ExpectVec3Near(frame.toWorld(Vec2{0, 0}), Vec3{0, 0, 0});
    ExpectVec3Near(frame.toWorld(Vec2{100, 50}), Vec3{100, 50, 0});
    ExpectVec3Near(frame.uAxis(), Vec3{1, 0, 0});
    ExpectVec3Near(frame.vAxis(), Vec3{0, 1, 0});
    ExpectVec3Near(frame.normal(), Vec3{0, 0, 1});
}

TEST(SketchTest, M4_FRAME_002_TranslatedFrameOffsetsOriginOnly) {
    const SketchFrame frame = SketchFrame::Translated(Vec3{10, 20, 30});
    ExpectVec3Near(frame.toWorld(Vec2{0, 0}), Vec3{10, 20, 30});
    ExpectVec3Near(frame.toWorld(Vec2{100, 50}), Vec3{110, 70, 30});
    // Axes are unaffected by translation.
    ExpectVec3Near(frame.normal(), Vec3{0, 0, 1});
    ExpectVec3Near(frame.uAxis(), Vec3{1, 0, 0});
}

TEST(SketchTest, M4_FRAME_003_RotatedAboutXMapsSketchVToWorldZ) {
    // +90 deg about X: sketch +v -> world +z, normal -> world -y.
    const SketchFrame frame = SketchFrame::Rotated(Vec3{1, 0, 0}, kPi / 2);
    ExpectVec3Near(frame.toWorld(Vec2{0, 0}), Vec3{0, 0, 0});
    ExpectVec3Near(frame.toWorld(Vec2{100, 0}), Vec3{100, 0, 0});
    ExpectVec3Near(frame.toWorld(Vec2{0, 50}), Vec3{0, 0, 50});
    ExpectVec3Near(frame.uAxis(), Vec3{1, 0, 0});
    ExpectVec3Near(frame.vAxis(), Vec3{0, 0, 1});
    ExpectVec3Near(frame.normal(), Vec3{0, -1, 0});
}

TEST(SketchTest, M4_FRAME_004_RotatedAboutZKeepsPlaneButRotatesAxes) {
    const SketchFrame frame = SketchFrame::Rotated(Vec3{0, 0, 1}, kPi / 2);
    ExpectVec3Near(frame.toWorld(Vec2{100, 0}), Vec3{0, 100, 0});
    ExpectVec3Near(frame.toWorld(Vec2{0, 50}), Vec3{-50, 0, 0});
    ExpectVec3Near(frame.normal(), Vec3{0, 0, 1});
}

TEST(SketchTest, M4_FRAME_005_RotatedAndTranslatedComposes) {
    const SketchFrame frame = SketchFrame::Rotated(Vec3{1, 0, 0}, kPi / 2, Vec3{5, 5, 5});
    ExpectVec3Near(frame.toWorld(Vec2{0, 0}), Vec3{5, 5, 5});
    ExpectVec3Near(frame.toWorld(Vec2{0, 50}), Vec3{5, 5, 55});
    ExpectVec3Near(frame.toWorld(Vec2{10, 0}), Vec3{15, 5, 5});
}

TEST(SketchTest, M4_FRAME_006_FrameTransformPreservesDistances) {
    // A rigid frame must not scale: the same (u,v) separation maps to the same
    // world distance under any rotation/translation. This is what stops a
    // malformed quaternion from silently resizing a Pad.
    const std::vector<SketchFrame> frames{
        SketchFrame::WorldXY(), SketchFrame::Translated(Vec3{3, -7, 11}),
        SketchFrame::Rotated(Vec3{1, 0, 0}, kPi / 2),
        SketchFrame::Rotated(Vec3{0, 1, 0}, kPi / 3, Vec3{1, 2, 3}),
        SketchFrame::Rotated(Vec3{1, 1, 1}, 0.7, Vec3{-4, 0, 2})};

    for (const SketchFrame& frame : frames) {
        const Vec3 a = frame.toWorld(Vec2{0, 0});
        const Vec3 b = frame.toWorld(Vec2{100, 50});
        const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        const double worldDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double sketchDistance = std::sqrt(100.0 * 100.0 + 50.0 * 50.0);
        EXPECT_NEAR(worldDistance, sketchDistance, 1e-9);
    }
}

TEST(SketchTest, M4_FRAME_007_DegenerateRotationAxisYieldsIdentity) {
    const SketchFrame frame = SketchFrame::Rotated(Vec3{0, 0, 0}, kPi / 2, Vec3{1, 2, 3});
    ExpectVec3Near(frame.toWorld(Vec2{10, 20}), Vec3{11, 22, 3});
    ExpectVec3Near(frame.normal(), Vec3{0, 0, 1});
}

TEST(SketchTest, M4_FRAME_008_SketchCarriesItsFrame) {
    Sketch sketch{"Sketch001"};
    ExpectVec3Near(sketch.frame().toWorld(Vec2{1, 1}), Vec3{1, 1, 0});
    sketch.setFrame(SketchFrame::Translated(Vec3{0, 0, 25}));
    ExpectVec3Near(sketch.frame().toWorld(Vec2{1, 1}), Vec3{1, 1, 25});
    // Entity coordinates are sketch-local and are NOT rewritten by the move.
    const SketchEntityId id = sketch.addPoint(Vec2{1, 1});
    ASSERT_NE(sketch.findEntity(id), nullptr);
    const auto* point = std::get_if<SketchPoint>(&sketch.findEntity(id)->geometry);
    ASSERT_NE(point, nullptr);
    EXPECT_DOUBLE_EQ(point->position.x, 1.0);
    EXPECT_DOUBLE_EQ(point->position.y, 1.0);
}

} // namespace
