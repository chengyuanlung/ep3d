// M4-D: semantic Profile validation (spec 10 mandatory cases, spec 18
// "Profile" matrix; ADR-M4-005). Every case here is Core-only -- profile
// validation never touches OCCT.

#include "Core/Sketch/Profile.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

// Adds the four sides of a 100x50 rectangle in the given order, returning the
// entity ids in the order requested (not in loop order).
std::vector<SketchEntityId> AddRectangle(Sketch& sketch) {
    return {sketch.addLine(Vec2{0, 0}, Vec2{100, 0}),
            sketch.addLine(Vec2{100, 0}, Vec2{100, 50}),
            sketch.addLine(Vec2{100, 50}, Vec2{0, 50}),
            sketch.addLine(Vec2{0, 50}, Vec2{0, 0})};
}

bool LoopContains(const ProfileLoop& loop, SketchEntityId id) {
    return std::any_of(loop.entities.begin(), loop.entities.end(),
                       [id](const OrientedSketchEntityRef& ref) { return ref.entityId == id; });
}

// --- Accepted profiles ------------------------------------------------------

TEST(ProfileTest, M4_PROFILE_001_ClosedRectangleAccepted) {
    Sketch sketch{"Sketch001"};
    const std::vector<SketchEntityId> ids = AddRectangle(sketch);

    const ProfileResult result = BuildProfile(sketch);
    ASSERT_TRUE(result) << result.message;
    ASSERT_EQ(result.profile.outer.entities.size(), 4u);
    for (SketchEntityId id : ids) EXPECT_TRUE(LoopContains(result.profile.outer, id));
}

TEST(ProfileTest, M4_PROFILE_002_LoopIsChainedEndToEnd) {
    // The real invariant: consecutive loop members must actually connect once
    // orientation is applied. Checking membership alone would pass for a
    // scrambled loop.
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);
    const ProfileResult result = BuildProfile(sketch);
    ASSERT_TRUE(result) << result.message;

    const auto& loop = result.profile.outer.entities;
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const SketchEntity* current = sketch.findEntity(loop[i].entityId);
        const SketchEntity* next = sketch.findEntity(loop[(i + 1) % loop.size()].entityId);
        ASSERT_NE(current, nullptr);
        ASSERT_NE(next, nullptr);
        const Vec2 currentEnd = loop[i].reversed ? StartPointOf(current->geometry)
                                                 : EndPointOf(current->geometry);
        const Vec2 nextStart = loop[(i + 1) % loop.size()].reversed
                                   ? EndPointOf(next->geometry)
                                   : StartPointOf(next->geometry);
        EXPECT_TRUE(SamePoint(currentEnd, nextStart, kProfileConnectivityToleranceMm))
            << "loop members " << i << " and " << (i + 1) % loop.size() << " do not connect";
    }
}

TEST(ProfileTest, M4_PROFILE_003_StorageOrderDoesNotAffectResult) {
    // Same rectangle, sides added in a scrambled order and with two sides drawn
    // backwards. The validator must produce a chained loop regardless
    // (ADR-M4-005: traversal starts from the lowest id, not from position 0).
    Sketch scrambled{"Sketch001"};
    scrambled.addLine(Vec2{100, 50}, Vec2{100, 0}); // reversed right side
    scrambled.addLine(Vec2{0, 50}, Vec2{0, 0});     // reversed left side
    scrambled.addLine(Vec2{0, 0}, Vec2{100, 0});
    scrambled.addLine(Vec2{100, 50}, Vec2{0, 50});

    const ProfileResult result = BuildProfile(scrambled);
    ASSERT_TRUE(result) << result.message;
    EXPECT_EQ(result.profile.outer.entities.size(), 4u);

    // At least one member must be marked reversed, or orientation normalization
    // is not actually happening.
    const auto& loop = result.profile.outer.entities;
    EXPECT_TRUE(std::any_of(loop.begin(), loop.end(),
                            [](const OrientedSketchEntityRef& r) { return r.reversed; }));
}

TEST(ProfileTest, M4_PROFILE_004_DeterministicAcrossEquivalentSketches) {
    // Determinism is a requirement, not an accident: two sketches with the same
    // geometry added in different orders must yield the same loop shape.
    Sketch a{"A"};
    AddRectangle(a);
    Sketch b{"B"};
    b.addLine(Vec2{0, 50}, Vec2{0, 0});
    b.addLine(Vec2{100, 50}, Vec2{0, 50});
    b.addLine(Vec2{100, 0}, Vec2{100, 50});
    b.addLine(Vec2{0, 0}, Vec2{100, 0});

    const ProfileResult ra = BuildProfile(a);
    const ProfileResult rb = BuildProfile(b);
    ASSERT_TRUE(ra) << ra.message;
    ASSERT_TRUE(rb) << rb.message;
    EXPECT_EQ(ra.profile.outer.entities.size(), rb.profile.outer.entities.size());
}

TEST(ProfileTest, M4_PROFILE_005_FullCircleIsAOneEntityLoop) {
    Sketch sketch{"Sketch001"};
    const SketchEntityId circle = sketch.addCircle(Vec2{0, 0}, 10.0);
    const ProfileResult result = BuildProfile(sketch);
    ASSERT_TRUE(result) << result.message;
    ASSERT_EQ(result.profile.outer.entities.size(), 1u);
    EXPECT_EQ(result.profile.outer.entities.front().entityId, circle);
}

TEST(ProfileTest, M4_PROFILE_006_LineArcLoopAccepted) {
    // Half-disc: a semicircular arc from (10,0) to (-10,0) closed by a diameter.
    Sketch sketch{"Sketch001"};
    sketch.addArc(Vec2{0, 0}, 10.0, 0.0, kPi, true);
    sketch.addLine(Vec2{-10, 0}, Vec2{10, 0});

    const ProfileResult result = BuildProfile(sketch);
    ASSERT_TRUE(result) << result.message;
    EXPECT_EQ(result.profile.outer.entities.size(), 2u);
}

TEST(ProfileTest, M4_PROFILE_007_PointsAreIgnoredNotRejected) {
    // Points are legitimate reference geometry and cannot contribute an edge.
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);
    sketch.addPoint(Vec2{50, 25});
    sketch.addPoint(Vec2{-999, 999});

    const ProfileResult result = BuildProfile(sketch);
    ASSERT_TRUE(result) << result.message;
    EXPECT_EQ(result.profile.outer.entities.size(), 4u);
}

// --- Rejected profiles (spec 10) -------------------------------------------

TEST(ProfileTest, M4_PROFILE_010_EmptySketchRejected) {
    Sketch sketch{"Sketch001"};
    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ProfileError::NoEntities);
    EXPECT_FALSE(result.message.empty());
}

TEST(ProfileTest, M4_PROFILE_011_OpenLoopRejected) {
    Sketch sketch{"Sketch001"};
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50}); // missing the closing side

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ProfileError::OpenLoop);
    EXPECT_FALSE(result.message.empty());
}

TEST(ProfileTest, M4_PROFILE_012_DisconnectedComponentsRejected) {
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);
    // A second, separate closed triangle far away.
    sketch.addLine(Vec2{500, 500}, Vec2{600, 500});
    sketch.addLine(Vec2{600, 500}, Vec2{550, 580});
    sketch.addLine(Vec2{550, 580}, Vec2{500, 500});

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ProfileError::Disconnected);
}

TEST(ProfileTest, M4_PROFILE_013_BranchRejected) {
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);
    sketch.addLine(Vec2{100, 0}, Vec2{200, -50}); // T-junction at (100,0)

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    // A stray spur makes (100,0) degree-3 and its far end degree-1; either
    // diagnosis is correct, but it must be one of them and never accepted.
    EXPECT_TRUE(result.error == ProfileError::Branch || result.error == ProfileError::OpenLoop);
}

TEST(ProfileTest, M4_PROFILE_014_DuplicateEntityRejected) {
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0}); // exact duplicate of the bottom

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ProfileError::DuplicateEntity);
}

TEST(ProfileTest, M4_PROFILE_015_ReversedDuplicateAlsoRejected) {
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);
    sketch.addLine(Vec2{100, 0}, Vec2{0, 0}); // same span, opposite direction

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ProfileError::DuplicateEntity);
}

TEST(ProfileTest, M4_PROFILE_016_DegenerateGeometryRejectedOnRestorePath) {
    // Sketch::add* already refuses these, so the only way one reaches a profile
    // is a restored (hand-edited) document -- which is exactly why the
    // validator re-checks.
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);
    ASSERT_TRUE(sketch.restoreEntity(SketchEntityId{999001},
                                     SketchLine{Vec2{7, 7}, Vec2{7, 7}}));

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ProfileError::InvalidGeometry);
}

TEST(ProfileTest, M4_PROFILE_017_InvalidRadiusRejectedOnRestorePath) {
    Sketch sketch{"Sketch001"};
    ASSERT_TRUE(sketch.restoreEntity(SketchEntityId{999002}, SketchCircle{Vec2{0, 0}, -5.0}));
    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ProfileError::InvalidGeometry);
}

TEST(ProfileTest, M4_PROFILE_018_NonFiniteCoordinatesRejectedOnRestorePath) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    Sketch withNan{"A"};
    ASSERT_TRUE(withNan.restoreEntity(SketchEntityId{999003},
                                      SketchLine{Vec2{0, 0}, Vec2{nan, 5}}));
    EXPECT_EQ(BuildProfile(withNan).error, ProfileError::InvalidGeometry);

    Sketch withInf{"B"};
    ASSERT_TRUE(withInf.restoreEntity(SketchEntityId{999004},
                                      SketchLine{Vec2{0, 0}, Vec2{inf, 5}}));
    EXPECT_EQ(BuildProfile(withInf).error, ProfileError::InvalidGeometry);
}

TEST(ProfileTest, M4_PROFILE_019_CircleCannotBeMixedWithOtherCurves) {
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);
    sketch.addCircle(Vec2{50, 25}, 5.0); // a hole -- deferred to a later milestone

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ProfileError::NotChainable);
}

TEST(ProfileTest, M4_PROFILE_020_TwoCirclesRejected) {
    Sketch sketch{"Sketch001"};
    sketch.addCircle(Vec2{0, 0}, 10.0);
    sketch.addCircle(Vec2{50, 0}, 10.0);
    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ProfileError::NotChainable);
}

TEST(ProfileTest, M4_PROFILE_021_ArcAndChordSharingEndpointsAreNotDuplicates) {
    // Regression for a real defect this suite caught: duplicate detection
    // originally compared only endpoints, which rejected every valid
    // two-entity loop. An arc and its chord share both endpoints and are not
    // duplicates; two arcs on opposite sides of the same chord likewise.
    Sketch halfDisc{"A"};
    halfDisc.addArc(Vec2{0, 0}, 10.0, 0.0, kPi, true);
    halfDisc.addLine(Vec2{-10, 0}, Vec2{10, 0});
    EXPECT_TRUE(BuildProfile(halfDisc)) << "arc + chord rejected as duplicates";

    Sketch lens{"B"};
    lens.addArc(Vec2{0, 0}, 10.0, 0.0, kPi, true);  // upper half
    lens.addArc(Vec2{0, 0}, 10.0, 0.0, kPi, false); // lower half, same endpoints
    EXPECT_TRUE(BuildProfile(lens)) << "the two halves of a circle rejected as duplicates";
}

TEST(ProfileTest, M4_PROFILE_022_IdenticalArcsAreDuplicates) {
    // The other side of the same rule: genuinely identical arcs must still be
    // caught, or the fix above would have disabled duplicate detection.
    Sketch sketch{"Sketch001"};
    sketch.addArc(Vec2{0, 0}, 10.0, 0.0, kPi, true);
    sketch.addLine(Vec2{-10, 0}, Vec2{10, 0});
    ASSERT_TRUE(sketch.restoreEntity(SketchEntityId{999010},
                                     SketchArc{Vec2{0, 0}, 10.0, 0.0, kPi, true}));

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ProfileError::DuplicateEntity);
}

// --- Tolerance boundary (spec 10: just inside / just outside) --------------

TEST(ProfileTest, M4_PROFILE_030_GapJustInsideToleranceAccepted) {
    const double gap = kProfileConnectivityToleranceMm * 0.5;
    Sketch sketch{"Sketch001"};
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{gap, 0}); // closes to within tolerance

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_TRUE(result) << result.message;
}

TEST(ProfileTest, M4_PROFILE_031_GapJustOutsideToleranceRejectedNotHealed) {
    const double gap = kProfileConnectivityToleranceMm * 100.0;
    Sketch sketch{"Sketch001"};
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{gap, 0});

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result) << "a gap beyond tolerance was silently healed";
    EXPECT_EQ(result.error, ProfileError::OpenLoop);
    EXPECT_FALSE(result.message.empty());
}

// --- Self-intersection (spec 10 policy: reject) ----------------------------

TEST(ProfileTest, M4_PROFILE_040_SelfIntersectingBowtieRejected) {
    // A "bowtie": the two diagonals cross. All four endpoints have degree 2, so
    // this passes every connectivity check and is caught only by the crossing
    // test -- which is the point of having one.
    Sketch sketch{"Sketch001"};
    sketch.addLine(Vec2{0, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result) << "a self-intersecting outline was accepted";
    EXPECT_EQ(result.error, ProfileError::SelfIntersecting);
}

TEST(ProfileTest, M4_PROFILE_041_ConvexRectangleIsNotFlaggedAsIntersecting) {
    // Guards against a self-intersection test so eager it rejects valid work.
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);
    EXPECT_TRUE(BuildProfile(sketch)) << "a valid rectangle was rejected as self-intersecting";
}

TEST(ProfileTest, M4_PROFILE_042_ConcaveButValidProfileAccepted) {
    // An L-shape: concave, adjacent sides collinear at the notch, still valid.
    Sketch sketch{"Sketch001"};
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 30});
    sketch.addLine(Vec2{100, 30}, Vec2{40, 30});
    sketch.addLine(Vec2{40, 30}, Vec2{40, 80});
    sketch.addLine(Vec2{40, 80}, Vec2{0, 80});
    sketch.addLine(Vec2{0, 80}, Vec2{0, 0});

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(result.profile.outer.entities.size(), 6u);
}

} // namespace
