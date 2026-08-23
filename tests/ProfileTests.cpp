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

TEST(ProfileTest, M4_PROFILE_012_SideBySideLoopsAreRejected) {
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);
    // A second, separate closed triangle far away -- BESIDE the rectangle, not
    // inside it.
    sketch.addLine(Vec2{500, 500}, Vec2{600, 500});
    sketch.addLine(Vec2{600, 500}, Vec2{550, 580});
    sketch.addLine(Vec2{550, 580}, Vec2{500, 500});

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    // NotNested since M17, not Disconnected: several loops are now ordinary --
    // a hole is made of them. What is still refused is loops that are not
    // nested, because "two solids" is a different thing from "one with a hole"
    // and neither may be guessed at.
    EXPECT_EQ(result.error, ProfileError::NotNested);
    EXPECT_NE(result.message.find("outer boundary"), std::string::npos) << result.message;
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
    // The id is ALLOCATED, never a constant. `SketchEntityId` draws from the
    // process-global `ObjectIdGenerator`, and `restoreEntity` calls
    // `AdvancePast`, so a literal like 999001 shoves the counter into that
    // neighbourhood for every test that runs after this one in the same
    // process. M4_PROFILE_022 used the constant 999010 and failed
    // intermittently under `--gtest_shuffle` for exactly that reason: once
    // these restore-path tests had pushed the counter to 999009, its own
    // `addArc`/`addLine` were HANDED 999009 and 999010, and the restore then
    // collided with the entity the test had just created. ObjectId.h's own
    // comment names this bug class; the tests were the ones reintroducing it.
    ASSERT_TRUE(sketch.restoreEntity(NextSketchEntityId(),
                                     SketchLine{Vec2{7, 7}, Vec2{7, 7}}));

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ProfileError::InvalidGeometry);
}

TEST(ProfileTest, M4_PROFILE_017_InvalidRadiusRejectedOnRestorePath) {
    Sketch sketch{"Sketch001"};
    ASSERT_TRUE(sketch.restoreEntity(NextSketchEntityId(), SketchCircle{Vec2{0, 0}, -5.0}));
    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ProfileError::InvalidGeometry);
}

TEST(ProfileTest, M4_PROFILE_018_NonFiniteCoordinatesRejectedOnRestorePath) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    Sketch withNan{"A"};
    ASSERT_TRUE(withNan.restoreEntity(NextSketchEntityId(),
                                      SketchLine{Vec2{0, 0}, Vec2{nan, 5}}));
    EXPECT_EQ(BuildProfile(withNan).error, ProfileError::InvalidGeometry);

    Sketch withInf{"B"};
    ASSERT_TRUE(withInf.restoreEntity(NextSketchEntityId(),
                                      SketchLine{Vec2{0, 0}, Vec2{inf, 5}}));
    EXPECT_EQ(BuildProfile(withInf).error, ProfileError::InvalidGeometry);
}

TEST(ProfileTest, M17_PROFILE_019_ACircleInsideARectangleIsAHOLE) {
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);
    const SketchEntityId hole = sketch.addCircle(Vec2{50, 25}, 5.0);

    // The commonest profile in mechanical CAD, and M4 refused it outright.
    const ProfileResult result = BuildProfile(sketch);
    ASSERT_TRUE(static_cast<bool>(result)) << result.message;
    EXPECT_EQ(result.profile.outer.entities.size(), 4u);
    ASSERT_EQ(result.profile.inners.size(), 1u);
    ASSERT_EQ(result.profile.inners.front().entities.size(), 1u);
    // The CIRCLE is the hole, decided by containment rather than by which was
    // drawn first or which has fewer entities.
    EXPECT_EQ(result.profile.inners.front().entities.front().entityId, hole);
}

TEST(ProfileTest, M17_PROFILE_019B_TheOUTERLoopIsTheCONTAININGOneNotTheFirst) {
    Sketch sketch{"Sketch001"};
    // The hole FIRST, so draw order and id order both point the wrong way.
    const SketchEntityId hole = sketch.addCircle(Vec2{50, 25}, 5.0);
    AddRectangle(sketch);

    const ProfileResult result = BuildProfile(sketch);
    ASSERT_TRUE(static_cast<bool>(result)) << result.message;
    EXPECT_EQ(result.profile.outer.entities.size(), 4u);
    ASSERT_EQ(result.profile.inners.size(), 1u);
    EXPECT_EQ(result.profile.inners.front().entities.front().entityId, hole);
}

TEST(ProfileTest, M17_PROFILE_019C_SeveralHolesAreAllKept) {
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);
    sketch.addCircle(Vec2{25, 25}, 4.0);
    sketch.addCircle(Vec2{50, 25}, 4.0);
    sketch.addCircle(Vec2{75, 25}, 4.0);

    const ProfileResult result = BuildProfile(sketch);
    ASSERT_TRUE(static_cast<bool>(result)) << result.message;
    EXPECT_EQ(result.profile.inners.size(), 3u);
}

TEST(ProfileTest, M17_PROFILE_019D_AHoleInsideAHoleIsRefused) {
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);
    sketch.addCircle(Vec2{50, 25}, 10.0);
    sketch.addCircle(Vec2{50, 25}, 4.0); // an ISLAND inside that hole

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ProfileError::NotNested);
    // An island is a second solid REGION; building it as a plain hole would
    // quietly lose it, so it is named rather than silently dropped.
    EXPECT_NE(result.message.find("island"), std::string::npos) << result.message;
}

TEST(ProfileTest, M17_PROFILE_019E_LoopsThatCROSSAreRefused) {
    Sketch sketch{"Sketch001"};
    AddRectangle(sketch);                  // 0,0 to 100,50
    sketch.addCircle(Vec2{100, 25}, 20.0); // straddles the right-hand edge

    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    // Half in and half out is neither a hole nor a second boundary, and
    // choosing a reading would be inventing the user's intent.
    EXPECT_EQ(result.error, ProfileError::SelfIntersecting);
}

TEST(ProfileTest, M4_PROFILE_020_TwoSeparateCirclesAreRejected) {
    Sketch sketch{"Sketch001"};
    sketch.addCircle(Vec2{0, 0}, 10.0);
    sketch.addCircle(Vec2{50, 0}, 10.0);
    const ProfileResult result = BuildProfile(sketch);
    EXPECT_FALSE(result);
    // Side by side: two solids, not one with a hole.
    EXPECT_EQ(result.error, ProfileError::NotNested);
}

TEST(ProfileTest, M17_PROFILE_020B_ACircleInsideACircleIsARing) {
    Sketch sketch{"Sketch001"};
    const SketchEntityId outer = sketch.addCircle(Vec2{0, 0}, 20.0);
    const SketchEntityId inner = sketch.addCircle(Vec2{0, 0}, 8.0);

    const ProfileResult result = BuildProfile(sketch);
    ASSERT_TRUE(static_cast<bool>(result)) << result.message;
    ASSERT_EQ(result.profile.outer.entities.size(), 1u);
    EXPECT_EQ(result.profile.outer.entities.front().entityId, outer);
    ASSERT_EQ(result.profile.inners.size(), 1u);
    EXPECT_EQ(result.profile.inners.front().entities.front().entityId, inner);
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
    // Allocated, not a constant -- see M4_PROFILE_016. This exact line used
    // the literal 999010 and was the intermittent shuffle failure.
    ASSERT_TRUE(sketch.restoreEntity(NextSketchEntityId(),
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

// --- M18: a spline's handles crossing into a profile -------------------------

TEST(ProfileTest, M18_PRO_001_AReversedSplineTurnsItsHandlesROUNDAndRenumbersThem) {
    // A profile walks its loop in one direction, so an entity drawn the other
    // way is handed over reversed. For a spline that means reversing the point
    // list -- and a handle has to reverse TWICE.
    //
    // It is renumbered, because point i becomes point n-1-i. And it is NEGATED,
    // because the direction the curve LEAVES a point going one way is the
    // direction it ARRIVES going the other. Renumbering without negating gives
    // the reversed profile a curve that bulges the wrong way at every handled
    // point -- the same shape flipped, which is not the same shape, and the
    // solid would not match the sketch it came from.
    Sketch sketch{"Sketch001"};
    // THE LINE FIRST, and that is the whole setup. Traversal starts from the
    // lowest entity id and walks end to end, so starting at the line means
    // arriving at the spline's END -- and the spline is handed over reversed.
    // Adding the spline first walks it forwards, and a test written that way
    // passes whether or not the reversal does anything at all.
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    SketchSpline geometry{{Vec2{0, 0}, Vec2{40, 40}, Vec2{100, 0}}, false};
    geometry.handles[0] = Vec2{10, 25};
    const SketchEntityId spline = sketch.addSpline(geometry.points, geometry.closed);
    ASSERT_TRUE(sketch.setEntityGeometry(spline, geometry));

    const ProfileResult result = BuildProfile(sketch);
    ASSERT_TRUE(result) << result.message;
    PlanarProfileDefinition definition;
    ASSERT_TRUE(BuildKernelProfile(sketch, result.profile, definition));

    const ProfileSplineSegment* segment = nullptr;
    for (const ProfileSegment& one : definition.segments)
        if (const auto* found = std::get_if<ProfileSplineSegment>(&one)) segment = found;
    ASSERT_NE(segment, nullptr);
    ASSERT_EQ(segment->handles.size(), 1u);

    ASSERT_GT(segment->points.front().x, segment->points.back().x)
        << "the walk did not reverse the spline, so this test proves nothing";
    // Point 0 became point 2...
    EXPECT_EQ(segment->handles.find(0), segment->handles.end());
    ASSERT_NE(segment->handles.find(2), segment->handles.end());
    // ...and its tangent turned round.
    EXPECT_DOUBLE_EQ(segment->handles.at(2).x, -10.0);
    EXPECT_DOUBLE_EQ(segment->handles.at(2).y, -25.0);
}

// --- M19: the sweep PATH walk -------------------------------------------------
//
// A path is a profile's walk with one rule dropped: it need not come back to
// where it started. Everything else it keeps, and these say so -- because the
// rules it keeps are the ones that stop a sweep guessing which of several
// spines the user meant.

TEST(ProfileTest, M19_PATH_001_AChainOfCurvesIsAPathInOrder) {
    Sketch sketch{"Sketch001"};
    const SketchEntityId first = sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    const SketchEntityId second = sketch.addLine(Vec2{50, 0}, Vec2{50, 40});

    const PathResult path = BuildPath(sketch);
    ASSERT_TRUE(path) << path.message;
    EXPECT_FALSE(path.path.closed);
    ASSERT_EQ(path.path.chain.entities.size(), 2u);
    EXPECT_EQ(path.path.chain.entities[0].entityId, first);
    EXPECT_EQ(path.path.chain.entities[1].entityId, second);
}

TEST(ProfileTest, M19_PATH_002_ARINGIsAClosedPath) {
    // A pipe round a ring is an ordinary thing to want, so a closed chain is a
    // path -- and it says it is closed rather than leaving the caller to
    // compare its two ends against a tolerance.
    Sketch sketch{"Sketch001"};
    sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    sketch.addLine(Vec2{50, 0}, Vec2{50, 50});
    sketch.addLine(Vec2{50, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});

    const PathResult path = BuildPath(sketch);
    ASSERT_TRUE(path) << path.message;
    EXPECT_TRUE(path.path.closed);
    EXPECT_EQ(path.path.chain.entities.size(), 4u);
}

TEST(ProfileTest, M19_PATH_003_ACIRCLEIsAWholePathAndCannotBeChainedTo) {
    Sketch sketch{"Sketch001"};
    sketch.addCircle(Vec2{0, 0}, 40.0);

    const PathResult alone = BuildPath(sketch);
    ASSERT_TRUE(alone) << alone.message;
    EXPECT_TRUE(alone.path.closed);

    sketch.addLine(Vec2{100, 0}, Vec2{150, 0});
    const PathResult mixed = BuildPath(sketch);
    EXPECT_FALSE(mixed);
    EXPECT_EQ(mixed.error, ProfileError::NotChainable);
}

TEST(ProfileTest, M19_PATH_004_ABRANCHIsREFUSED) {
    // Three curves meeting at a point: there are several spines here and
    // picking one would be a guess about what the user drew. A sweep that
    // guessed would follow a path the drawing does not contain.
    Sketch sketch{"Sketch001"};
    sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    sketch.addLine(Vec2{50, 0}, Vec2{50, 40});
    sketch.addLine(Vec2{50, 0}, Vec2{100, 0});

    const PathResult path = BuildPath(sketch);
    EXPECT_FALSE(path);
    EXPECT_EQ(path.error, ProfileError::Branch);
}

TEST(ProfileTest, M19_PATH_005_TWOSEPARATEOpenChainsAreREFUSEDByTheirENDCOUNT) {
    // Two chains, so FOUR ends. A path has two or none, and saying which is
    // wrong -- and how many there are -- is more use than "disconnected".
    Sketch sketch{"Sketch001"};
    sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    sketch.addLine(Vec2{200, 0}, Vec2{250, 0});

    const PathResult path = BuildPath(sketch);
    EXPECT_FALSE(path);
    EXPECT_EQ(path.error, ProfileError::OpenLoop);
    EXPECT_NE(path.message.find("has 4"), std::string::npos) << path.message;
}

TEST(ProfileTest, M19_PATH_005b_TWOSEPARATERingsAreREFUSEDByTheWALK) {
    // The case the end count CANNOT see: two closed loops have no ends at all,
    // so every junction has degree two and the incidence check is satisfied.
    // Only running out of curves mid-walk notices -- and answering with
    // whichever ring the walk happened to start in would sweep along half the
    // drawing while looking like a success.
    Sketch sketch{"Sketch001"};
    sketch.addCircle(Vec2{0, 0}, 30.0);
    sketch.addLine(Vec2{100, 0}, Vec2{150, 0});
    sketch.addLine(Vec2{150, 0}, Vec2{150, 50});
    sketch.addLine(Vec2{150, 50}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{100, 0});

    const PathResult mixed = BuildPath(sketch);
    EXPECT_FALSE(mixed) << "a circle beside a closed chain is two paths, not one";

    Sketch rings{"Sketch002"};
    rings.addLine(Vec2{0, 0}, Vec2{50, 0});
    rings.addLine(Vec2{50, 0}, Vec2{50, 50});
    rings.addLine(Vec2{50, 50}, Vec2{0, 50});
    rings.addLine(Vec2{0, 50}, Vec2{0, 0});
    rings.addLine(Vec2{200, 0}, Vec2{250, 0});
    rings.addLine(Vec2{250, 0}, Vec2{250, 50});
    rings.addLine(Vec2{250, 50}, Vec2{200, 50});
    rings.addLine(Vec2{200, 50}, Vec2{200, 0});

    const PathResult twoRings = BuildPath(rings);
    EXPECT_FALSE(twoRings);
    EXPECT_EQ(twoRings.error, ProfileError::Disconnected);
    EXPECT_NE(twoRings.message.find("more than one chain"), std::string::npos)
        << twoRings.message;
}

TEST(ProfileTest, M19_PATH_006_CONSTRUCTIONGeometryIsNotPartOfThePath) {
    // The same rule a profile follows: construction geometry is in the drawing
    // to be measured from, not to be swept along. Without this, a centreline
    // beside the spine would read as a branch and refuse the whole sweep.
    Sketch sketch{"Sketch001"};
    sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    const SketchEntityId note = sketch.addLine(Vec2{0, 20}, Vec2{50, 20});
    ASSERT_TRUE(sketch.setEntityConstruction(note, true));

    const PathResult path = BuildPath(sketch);
    ASSERT_TRUE(path) << path.message;
    EXPECT_EQ(path.path.chain.entities.size(), 1u);
}

TEST(ProfileTest, M19_PATH_007_TheWalkIsTHESAMEWhateverOrderTheCurvesWereAddedIn) {
    // Determinism, the same promise BuildProfile makes: traversal starts from
    // the lowest entity id, so the spine does not depend on the order entities
    // were drawn, removed or restored in.
    const auto walk = [](bool reversed) {
        Sketch sketch{"Sketch001"};
        if (reversed) {
            sketch.addLine(Vec2{50, 0}, Vec2{50, 40});
            sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
        } else {
            sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
            sketch.addLine(Vec2{50, 0}, Vec2{50, 40});
        }
        const PathResult path = BuildPath(sketch);
        EXPECT_TRUE(path) << path.message;
        std::vector<Vec2> ends;
        for (const OrientedSketchEntityRef& ref : path.path.chain.entities) {
            const SketchEntity* entity = sketch.findEntity(ref.entityId);
            EXPECT_NE(entity, nullptr);
            ends.push_back(ref.reversed ? StartPointOf(entity->geometry)
                                        : EndPointOf(entity->geometry));
        }
        return ends;
    };

    const std::vector<Vec2> forward = walk(false);
    const std::vector<Vec2> backward = walk(true);
    ASSERT_EQ(forward.size(), 2u);
    ASSERT_EQ(backward.size(), 2u);
    // THE SAME SPINE, RUNNING THE SAME WAY.
    //
    // Not merely the same set of curves: a sweep places its section at the
    // spine's START, so a spine that ran the other way would build a different
    // solid from the same drawing. Started from whichever end the walk met
    // first, that is exactly what happened -- the direction followed the order
    // the lines were drawn in.
    for (std::size_t i = 0; i < forward.size(); ++i)
        EXPECT_NEAR(std::hypot(forward[i].x - backward[i].x, forward[i].y - backward[i].y), 0.0,
                    1e-9)
            << "step " << i;
}
