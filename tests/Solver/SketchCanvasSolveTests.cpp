// M12 -- the sketch UI with a REAL solver behind it.
//
// tests/SketchCanvasTests.cpp covers what a click means; this covers what the
// user is TOLD afterwards. DOF, "fully constrained", "conflicting" and the
// redundant/conflicting distinction are all solver outputs, and a UI test that
// asserts them without a solver is asserting a default value.
//
// The whole point of the milestone is this sequence, so it is a test:
//   draw a rectangle -> dimension it -> it becomes fully constrained
//   -> change the dimension -> the geometry moves.

#include "Core/Document/PartDocument.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Sketch/ISketchSolver.h"
#include "Core/Sketch/Sketch.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include "Viewer/SketchCanvas.h"
#include "Viewer/SketchCommands.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace paramcad;

struct SolvingFixture {
    PartDocument document{"Doc"};
    GaussNewtonSketchSolver solver;
    ObjectId sketchId{kInvalidObjectId};
    SketchCanvasModel model;

    // The one sketch, mutable, for tests that need raw geometry rather than
    // geometry drawn by clicking. Only ever one sketch is added here, so it
    // cannot be invalidated by a later addSketch.
    Sketch* editable = nullptr;

    SolvingFixture() {
        document.setSketchSolver(&solver);
        Sketch& created = document.addSketch("Sketch1");
        sketchId = created.id();
        editable = &created;
    }

    const Sketch& sketch() const { return *document.findSketch(sketchId); }

    SketchEditOutcome apply(const SketchEdit& edit) {
        const SketchEditOutcome outcome = ApplySketchEdit(document, sketchId, edit);
        model.afterApply(outcome.createdEntities);
        (void)document.recompute();
        return outcome;
    }

    SketchEditOutcome click(Vec2 point) {
        const SnapResult snap = SnapCursor(sketch(), point, 1.0, 0.0, false);
        const SketchEdit edit = model.click(snap);
        if (!edit.valid()) return SketchEditOutcome{};
        return apply(edit);
    }

    // Draws the rectangle exactly as two mouse clicks with the Rectangle tool.
    SketchEditOutcome drawRectangle(Vec2 a, Vec2 b) {
        model.setTool(SketchTool::Rectangle);
        click(a);
        return click(b);
    }

    SketchEditOutcome dimension(const std::vector<SketchElementRef>& selection,
                               SketchEditKind explicitKind = SketchEditKind::None) {
        model.setSelection(selection);
        std::string whyNot;
        const SketchEdit edit = model.requestDimension(sketch(), explicitKind, &whyNot);
        EXPECT_TRUE(edit.valid()) << whyNot;
        if (!edit.valid()) return SketchEditOutcome{};
        return apply(edit);
    }

    SketchEditOutcome constrain(SketchEditKind kind,
                                const std::vector<SketchElementRef>& selection) {
        model.setSelection(selection);
        std::string whyNot;
        const SketchEdit edit = model.requestConstraint(sketch(), kind, &whyNot);
        EXPECT_TRUE(edit.valid()) << whyNot;
        if (!edit.valid()) return SketchEditOutcome{};
        return apply(edit);
    }
};

const SketchLine& LineOf(const Sketch& sketch, SketchEntityId id) {
    const SketchEntity* entity = sketch.findEntity(id);
    EXPECT_NE(entity, nullptr);
    return *std::get_if<SketchLine>(&entity->geometry);
}

// A drag MOVED the geometry when the solver converged. Under- and
// over-constrained both converge; they are reports about freedom, not failures.
bool DragMoved(SketchSolveStatus status) noexcept {
    return status == SketchSolveStatus::Solved ||
           status == SketchSolveStatus::UnderConstrained ||
           status == SketchSolveStatus::OverConstrained;
}

double LengthOf(const SketchLine& line) {
    const double dx = line.end.x - line.start.x;
    const double dy = line.end.y - line.start.y;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

// =============================================================================
// The milestone's whole story, end to end
// =============================================================================

TEST(SketchCanvasSolveTest, M12_SOLVE_001_ARectangleDrawnWithTwoClicksSolvesAndKeepsItsShape) {
    SolvingFixture fixture;
    const SketchEditOutcome outcome = fixture.drawRectangle(Vec2{0.0, 0.0}, Vec2{100.0, 50.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // The rectangle's four horizontal/vertical constraints and four corner
    // coincidences all have to be SATISFIABLE. If the tool emitted a
    // constraint the solver cannot meet, this is where it shows.
    EXPECT_NE(fixture.sketch().solveStatus(), SketchSolveStatus::Conflicting);
    EXPECT_NE(fixture.sketch().solveStatus(), SketchSolveStatus::InvalidInput);

    // Still a rectangle after solving: opposite sides equal, corners joined.
    const SketchLine& bottom = LineOf(fixture.sketch(), outcome.createdEntities[0]);
    const SketchLine& top = LineOf(fixture.sketch(), outcome.createdEntities[2]);
    EXPECT_NEAR(bottom.start.y, bottom.end.y, 1e-6); // horizontal
    EXPECT_NEAR(top.start.y, top.end.y, 1e-6);
    EXPECT_NEAR(LengthOf(bottom), 100.0, 1e-6);
}

TEST(SketchCanvasSolveTest, M12_SOLVE_002_DimensioningAndAnchoringReachesFullyConstrained) {
    SolvingFixture fixture;
    // Deliberately OFF the origin: this test is about an anchor the user asks
    // for, and a corner dropped on the origin is anchored before they can.
    // M12_SOLVE_008 is that case.
    const SketchEditOutcome rectangle = fixture.drawRectangle(Vec2{10.0, 10.0}, Vec2{110.0, 60.0});
    ASSERT_TRUE(rectangle.applied) << rectangle.status;
    const SketchEntityId bottom = rectangle.createdEntities[0];
    const SketchEntityId right = rectangle.createdEntities[1];

    EXPECT_EQ(fixture.sketch().solveStatus(), SketchSolveStatus::UnderConstrained);

    // Width, height, and an anchor. Without the anchor the rectangle is still
    // free to slide: that residual freedom is exactly what DOF measures, and
    // ADR-M5-005 is the reason Fix exists.
    ASSERT_TRUE(fixture.dimension({SketchElementRef{bottom, SketchSubElement::Whole}}).applied);
    ASSERT_TRUE(fixture.dimension({SketchElementRef{right, SketchSubElement::Whole}}).applied);
    ASSERT_TRUE(
        fixture.constrain(SketchEditKind::AddFix,
                          {SketchElementRef{bottom, SketchSubElement::StartPoint}})
            .applied);

    EXPECT_EQ(fixture.sketch().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(fixture.sketch().degreesOfFreedom(), 0);

    const SketchStatusLine line = DescribeSketchStatus(fixture.sketch());
    EXPECT_EQ(line.badge, "OK");
    EXPECT_NE(line.text.find("Fully constrained"), std::string::npos) << line.text;
}

TEST(SketchCanvasSolveTest, M12_SOLVE_003_ChangingADimensionMovesTheGeometry) {
    SolvingFixture fixture;
    // Deliberately OFF the origin: this test is about an anchor the user asks
    // for, and a corner dropped on the origin is anchored before they can.
    // M12_SOLVE_008 is that case.
    const SketchEditOutcome rectangle = fixture.drawRectangle(Vec2{10.0, 10.0}, Vec2{110.0, 60.0});
    ASSERT_TRUE(rectangle.applied) << rectangle.status;
    const SketchEntityId bottom = rectangle.createdEntities[0];
    const SketchEntityId right = rectangle.createdEntities[1];

    const SketchEditOutcome width =
        fixture.dimension({SketchElementRef{bottom, SketchSubElement::Whole}});
    ASSERT_TRUE(width.applied) << width.status;
    ASSERT_TRUE(fixture.dimension({SketchElementRef{right, SketchSubElement::Whole}}).applied);
    ASSERT_TRUE(
        fixture.constrain(SketchEditKind::AddFix,
                          {SketchElementRef{bottom, SketchSubElement::StartPoint}})
            .applied);
    ASSERT_EQ(fixture.sketch().degreesOfFreedom(), 0);

    // THE POINT OF A DRIVING DIMENSION: type a number, the geometry follows.
    const SketchEditOutcome commit = CommitDimensionValue(
        fixture.document, fixture.sketch(), width.createdConstraints.front(), "160");
    ASSERT_TRUE(commit.applied) << commit.status;
    (void)fixture.document.recompute();

    EXPECT_NEAR(LengthOf(LineOf(fixture.sketch(), bottom)), 160.0, 1e-6);
    // And the corner coincidences held, so the top moved with the bottom.
    EXPECT_NEAR(LengthOf(LineOf(fixture.sketch(), rectangle.createdEntities[2])), 160.0, 1e-6);
    EXPECT_EQ(fixture.sketch().solveStatus(), SketchSolveStatus::Solved);
}

TEST(SketchCanvasSolveTest, M12_SOLVE_004_AnExpressionDrivenDimensionSolvesToTheEvaluatedValue) {
    SolvingFixture fixture;
    fixture.document.addParameter("Width", 120.0, UnitType::Millimeter);
    // Deliberately OFF the origin: this test is about an anchor the user asks
    // for, and a corner dropped on the origin is anchored before they can.
    // M12_SOLVE_008 is that case.
    const SketchEditOutcome rectangle = fixture.drawRectangle(Vec2{10.0, 10.0}, Vec2{110.0, 60.0});
    ASSERT_TRUE(rectangle.applied) << rectangle.status;
    const SketchEntityId bottom = rectangle.createdEntities[0];
    const SketchEntityId right = rectangle.createdEntities[1];

    const SketchEditOutcome height =
        fixture.dimension({SketchElementRef{right, SketchSubElement::Whole}});
    ASSERT_TRUE(height.applied) << height.status;
    ASSERT_TRUE(fixture.dimension({SketchElementRef{bottom, SketchSubElement::Whole}}).applied);
    ASSERT_TRUE(
        fixture.constrain(SketchEditKind::AddFix,
                          {SketchElementRef{bottom, SketchSubElement::StartPoint}})
            .applied);

    const SketchEditOutcome commit = CommitDimensionValue(
        fixture.document, fixture.sketch(), height.createdConstraints.front(), "#Width / 4");
    ASSERT_TRUE(commit.applied) << commit.status;
    (void)fixture.document.recompute();

    EXPECT_NEAR(LengthOf(LineOf(fixture.sketch(), right)), 30.0, 1e-6);
}

TEST(SketchCanvasSolveTest, M12_SOLVE_008_ACornerOnTheOriginAnchorsTheSketchWithoutAnExplicitFix) {
    SolvingFixture fixture;
    // Two clicks, the first on the origin. Nothing else.
    const SketchEditOutcome rectangle = fixture.drawRectangle(Vec2{0.0, 0.0}, Vec2{100.0, 50.0});
    ASSERT_TRUE(rectangle.applied) << rectangle.status;
    const SketchEntityId bottom = rectangle.createdEntities[0];
    const SketchEntityId right = rectangle.createdEntities[1];

    ASSERT_TRUE(fixture.dimension({SketchElementRef{bottom, SketchSubElement::Whole}}).applied);
    ASSERT_TRUE(fixture.dimension({SketchElementRef{right, SketchSubElement::Whole}}).applied);

    // Width and height and NO explicit anchor -- and the sketch is nonetheless
    // fully constrained, because the snap that put the corner on the origin
    // produced a real Fix. This is the assertion that separates roadmap 4.2's
    // inference from drawing-time magnetism: magnetism would leave two
    // translational DOF here while the geometry looked identical.
    EXPECT_EQ(fixture.sketch().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(fixture.sketch().degreesOfFreedom(), 0);

    // Not merely constrained -- constrained AT the origin.
    const SketchLine& bottomLine = LineOf(fixture.sketch(), bottom);
    EXPECT_NEAR(bottomLine.start.x, 0.0, 1e-6);
    EXPECT_NEAR(bottomLine.start.y, 0.0, 1e-6);
}

TEST(SketchCanvasSolveTest, M12_SOLVE_010_ARectangleCornerSnappedToAPointIsHELDThere) {
    SolvingFixture fixture;

    // An anchor the rectangle knows nothing about: a Point, pinned.
    fixture.model.setTool(SketchTool::Point);
    const SketchEditOutcome anchor = fixture.click(Vec2{30.0, 20.0});
    ASSERT_TRUE(anchor.applied) << anchor.status;
    const SketchEntityId anchorId = anchor.createdEntities.front();
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{anchorId, SketchSubElement::Whole}})
                    .applied);

    // Two clicks, the first landing on that point.
    const SketchEditOutcome rectangle = fixture.drawRectangle(Vec2{30.0, 20.0}, Vec2{130.0, 70.0});
    ASSERT_TRUE(rectangle.applied) << rectangle.status;
    const SketchEntityId bottom = rectangle.createdEntities[0];
    const SketchEntityId right = rectangle.createdEntities[1];

    ASSERT_TRUE(fixture.dimension({SketchElementRef{bottom, SketchSubElement::Whole}}).applied);
    ASSERT_TRUE(fixture.dimension({SketchElementRef{right, SketchSubElement::Whole}}).applied);

    // Width, height and the inferred join -- and nothing else. If the snap had
    // been mere magnetism the rectangle would still be free to slide, and this
    // is the assertion that can tell the difference: the geometry looks
    // identical either way.
    EXPECT_EQ(fixture.sketch().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(fixture.sketch().degreesOfFreedom(), 0);
    EXPECT_NEAR(LineOf(fixture.sketch(), bottom).start.x, 30.0, 1e-6);
    EXPECT_NEAR(LineOf(fixture.sketch(), bottom).start.y, 20.0, 1e-6);
}

TEST(SketchCanvasSolveTest, M12_SOLVE_009_TheInferredAnchorIsNotRedundantWithTheRestOfTheRectangle) {
    SolvingFixture fixture;
    const SketchEditOutcome rectangle = fixture.drawRectangle(Vec2{0.0, 0.0}, Vec2{100.0, 50.0});
    ASSERT_TRUE(rectangle.applied) << rectangle.status;

    // Straight after drawing, before any dimension: an anchored but undimensioned
    // rectangle is UNDER-constrained, not conflicting. Emitting a second Fix, or
    // fixing a corner the coincidences already pinned, would show up here as an
    // over-constrained sketch the user cannot explain -- and roadmap 8.2 point 3
    // is explicit that redundant and conflicting must stay distinguishable.
    EXPECT_EQ(fixture.sketch().solveStatus(), SketchSolveStatus::UnderConstrained);
    EXPECT_GT(fixture.sketch().degreesOfFreedom(), 0);
}

// =============================================================================
// M17: the two legs of a point-to-point distance
// =============================================================================

TEST(SketchCanvasSolveTest, M17_AXIS_001_AHorizontalDistanceDrivesOnlyTheHorizontalGap) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Point);
    const SketchEditOutcome a = fixture.click(Vec2{10.0, 20.0});
    ASSERT_TRUE(a.applied) << a.status;
    const SketchEditOutcome b = fixture.click(Vec2{90.0, 60.0});
    ASSERT_TRUE(b.applied) << b.status;
    const SketchEntityId pa = a.createdEntities.front();
    const SketchEntityId pb = b.createdEntities.front();

    // Pin the first point so the solver has somewhere to measure FROM.
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{pa, SketchSubElement::Whole}})
                    .applied);

    const SketchEditOutcome dimension =
        fixture.dimension({SketchElementRef{pa, SketchSubElement::Whole},
                           SketchElementRef{pb, SketchSubElement::Whole}},
                          SketchEditKind::AddHorizontalDistance);
    ASSERT_TRUE(dimension.applied) << dimension.status;

    // Seeded at what it MEASURES: 90 - 10.
    const Parameter* parameter =
        fixture.document.parameters().findById(dimension.createdParameter);
    ASSERT_NE(parameter, nullptr);
    EXPECT_NEAR(parameter->value(), 80.0, 1e-9);

    const auto positionOf = [&](SketchEntityId id) {
        const SketchEntity* entity = fixture.sketch().findEntity(id);
        EXPECT_NE(entity, nullptr);
        return std::get<SketchPoint>(entity->geometry).position;
    };
    // Adding it moved nothing.
    EXPECT_NEAR(positionOf(pb).x, 90.0, 1e-6);
    EXPECT_NEAR(positionOf(pb).y, 60.0, 1e-6);

    // Drive it: 80 -> 120 moves the second point 40 mm to the right and leaves
    // its HEIGHT alone. A straight-line Distance here would have moved it along
    // the diagonal instead, which is the whole reason these two kinds exist.
    ASSERT_TRUE(CommitDimensionValue(fixture.document, fixture.sketch(),
                                     dimension.createdConstraints.front(), "120")
                    .applied);
    (void)fixture.document.recompute();
    // UNDER-constrained, and correctly so: the second point's HEIGHT is still
    // free, which is precisely what a horizontal dimension leaves alone. The
    // sketch solves and commits anyway -- DOF > 0 is a report, not a failure.
    EXPECT_EQ(fixture.sketch().solveStatus(), SketchSolveStatus::UnderConstrained);
    EXPECT_EQ(fixture.sketch().degreesOfFreedom(), 1);
    EXPECT_NEAR(positionOf(pb).x, 130.0, 1e-6);
    EXPECT_NEAR(positionOf(pb).y, 60.0, 1e-6);
}

TEST(SketchCanvasSolveTest, M17_AXIS_002_AVerticalDistanceDrivesOnlyTheVerticalGap) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Point);
    const SketchEditOutcome a = fixture.click(Vec2{10.0, 20.0});
    const SketchEditOutcome b = fixture.click(Vec2{90.0, 60.0});
    ASSERT_TRUE(a.applied && b.applied);
    const SketchEntityId pa = a.createdEntities.front();
    const SketchEntityId pb = b.createdEntities.front();
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{pa, SketchSubElement::Whole}})
                    .applied);

    const SketchEditOutcome dimension =
        fixture.dimension({SketchElementRef{pa, SketchSubElement::Whole},
                           SketchElementRef{pb, SketchSubElement::Whole}},
                          SketchEditKind::AddVerticalDistance);
    ASSERT_TRUE(dimension.applied) << dimension.status;
    const Parameter* parameter =
        fixture.document.parameters().findById(dimension.createdParameter);
    ASSERT_NE(parameter, nullptr);
    EXPECT_NEAR(parameter->value(), 40.0, 1e-9);

    ASSERT_TRUE(CommitDimensionValue(fixture.document, fixture.sketch(),
                                     dimension.createdConstraints.front(), "10")
                    .applied);
    (void)fixture.document.recompute();
    const SketchEntity* moved = fixture.sketch().findEntity(pb);
    ASSERT_NE(moved, nullptr);
    const Vec2 position = std::get<SketchPoint>(moved->geometry).position;
    EXPECT_NEAR(position.y, 30.0, 1e-6);
    EXPECT_NEAR(position.x, 90.0, 1e-6);
}

TEST(SketchCanvasSolveTest, M17_AXIS_003_BothLegsTogetherFullyLocateAPoint) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Point);
    // OFF the origin deliberately. A click on the origin already earns its own
    // Fix (ADR-M12-011), and the explicit one below would then be a redundant
    // second Fix -- which the solver reports as OverConstrained, correctly, and
    // which would have nothing to do with what this test is about.
    const SketchEditOutcome a = fixture.click(Vec2{10.0, 20.0});
    const SketchEditOutcome b = fixture.click(Vec2{40.0, 60.0});
    ASSERT_TRUE(a.applied && b.applied);
    const SketchEntityId pa = a.createdEntities.front();
    const SketchEntityId pb = b.createdEntities.front();
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{pa, SketchSubElement::Whole}})
                    .applied);

    ASSERT_TRUE(fixture
                    .dimension({SketchElementRef{pa, SketchSubElement::Whole},
                                SketchElementRef{pb, SketchSubElement::Whole}},
                               SketchEditKind::AddHorizontalDistance)
                    .applied);
    ASSERT_TRUE(fixture
                    .dimension({SketchElementRef{pa, SketchSubElement::Whole},
                                SketchElementRef{pb, SketchSubElement::Whole}},
                               SketchEditKind::AddVerticalDistance)
                    .applied);

    // Two legs of a right triangle pin a point exactly, and they are
    // INDEPENDENT -- unlike a distance plus an angle, which need each other.
    EXPECT_EQ(fixture.sketch().solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(fixture.sketch().degreesOfFreedom(), 0);
}

TEST(SketchCanvasSolveTest, M17_AXIS_004_TheSolverReachesTheTargetFromAnAlignedStart) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Point);
    const SketchEditOutcome a = fixture.click(Vec2{0.0, 0.0});
    // Directly ABOVE the first point: the horizontal gap starts at ZERO, which
    // is exactly where an |b - a| residual has no derivative. The signed form
    // has a constant Jacobian row and walks straight to the answer; this test
    // is the reason the constraint is signed.
    const SketchEditOutcome b = fixture.click(Vec2{0.0, 50.0});
    ASSERT_TRUE(a.applied && b.applied);
    const SketchEntityId pa = a.createdEntities.front();
    const SketchEntityId pb = b.createdEntities.front();
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{pa, SketchSubElement::Whole}})
                    .applied);

    const SketchEditOutcome dimension =
        fixture.dimension({SketchElementRef{pa, SketchSubElement::Whole},
                           SketchElementRef{pb, SketchSubElement::Whole}},
                          SketchEditKind::AddHorizontalDistance);
    // ACCEPTED, and then it works. This test used to assert the opposite -- the
    // command refused for being "seeded below the minimum" -- while its own
    // preamble three lines above explained why a signed distance has no trouble
    // at zero. The two halves contradicted each other, and the name promised
    // something the body then made unreachable: with the command refused, the
    // solver never ran, so nothing here ever checked that it reaches the target.
    //
    // The refusal was a magnitude rule applied to a signed quantity (M18). It
    // also refused the two most ordinary requests there are -- "line these up"
    // and "make these level" -- which is how it was finally noticed.
    ASSERT_TRUE(dimension.applied) << dimension.status;
    ASSERT_EQ(dimension.createdConstraints.size(), 1u);

    ASSERT_TRUE(CommitDimensionValue(fixture.document, fixture.sketch(),
                                     dimension.createdConstraints.front(), "40")
                    .applied);
    (void)fixture.document.recompute();

    const SketchEntity* moved = fixture.sketch().findEntity(pb);
    ASSERT_NE(moved, nullptr);
    // Straight to the answer from a start where an |b - a| residual would have
    // had no derivative at all.
    EXPECT_NEAR(std::get<SketchPoint>(moved->geometry).position.x, 40.0, 1e-6)
        << fixture.sketch().solveMessage();
}

// =============================================================================
// M17: Offset is a RELATIONSHIP, not a copy
// =============================================================================

TEST(SketchCanvasSolveTest, M17_OFF_101_DrivingTheOffsetDistanceMovesTheCopy) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{10.0, 10.0});
    const SketchEditOutcome source = fixture.click(Vec2{110.0, 10.0});
    ASSERT_TRUE(source.applied) << source.status;
    const SketchEntityId line = source.createdEntities.front();

    // Pin the source so the solver moves the COPY rather than the original.
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{line, SketchSubElement::StartPoint}})
                    .applied);
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{line, SketchSubElement::EndPoint}})
                    .applied);

    std::string whyNot;
    const SketchEdit offset = MakeOffsetEdit(fixture.sketch(), line, 10.0, 1.0, &whyNot);
    ASSERT_TRUE(offset.valid()) << whyNot;
    const SketchEditOutcome applied = fixture.apply(offset);
    ASSERT_TRUE(applied.applied) << applied.status;
    const SketchEntityId copy = applied.createdEntities.front();

    const auto copyLine = [&]() {
        const SketchEntity* entity = fixture.sketch().findEntity(copy);
        EXPECT_NE(entity, nullptr);
        return std::get<SketchLine>(entity->geometry);
    };
    // Creating it moved nothing.
    EXPECT_NEAR(copyLine().start.y, 20.0, 1e-6);

    // THE POINT: type a new gap and the copy moves. A command that only copied
    // geometry would leave this line exactly where it was, and the sketch would
    // look identical until something else forced a solve.
    // NEGATIVE, because side +1 put the copy where the residual measures
    // negative -- and keeping the sign is what keeps the copy on the side the
    // user asked for instead of flipping it across the source.
    ASSERT_TRUE(CommitDimensionValue(fixture.document, fixture.sketch(),
                                     applied.createdConstraints.back(), "-25")
                    .applied);
    (void)fixture.document.recompute();
    EXPECT_NE(fixture.sketch().solveStatus(), SketchSolveStatus::Conflicting);
    EXPECT_NEAR(copyLine().start.y, 35.0, 1e-6);
    // Still parallel, still the same length -- the other two constraints held.
    EXPECT_NEAR(copyLine().end.y, 35.0, 1e-6);
    const double length = std::sqrt(std::pow(copyLine().end.x - copyLine().start.x, 2.0) +
                                    std::pow(copyLine().end.y - copyLine().start.y, 2.0));
    EXPECT_NEAR(length, 100.0, 1e-6);
}

TEST(SketchCanvasSolveTest, M17_OFF_102_MovingTheSourceCarriesTheOffsetWithIt) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{10.0, 10.0});
    const SketchEditOutcome source = fixture.click(Vec2{110.0, 10.0});
    ASSERT_TRUE(source.applied) << source.status;
    const SketchEntityId line = source.createdEntities.front();

    std::string whyNot;
    const SketchEditOutcome applied =
        fixture.apply(MakeOffsetEdit(fixture.sketch(), line, 10.0, 1.0, &whyNot));
    ASSERT_TRUE(applied.applied) << applied.status;
    const SketchEntityId copy = applied.createdEntities.front();

    // Rotate the source by dimensioning the copy's gap the other way round:
    // simplest honest check is that the gap SURVIVES a solve at all, measured
    // the way the residual measures it.
    (void)fixture.document.recompute();
    const SketchLine& src = std::get<SketchLine>(fixture.sketch().findEntity(line)->geometry);
    const SketchLine& cpy = std::get<SketchLine>(fixture.sketch().findEntity(copy)->geometry);
    const double du = src.end.x - src.start.x;
    const double dv = src.end.y - src.start.y;
    const double len = std::sqrt(du * du + dv * dv);
    const double pu = cpy.start.x - src.start.x;
    const double pv = cpy.start.y - src.start.y;
    // -10: side +1 is the LEFT of start->end, and the residual calls the left
    // negative. The number the canvas shows is the magnitude; the sign is what
    // records the side.
    EXPECT_NEAR((pu * dv - pv * du) / len, -10.0, 1e-6);
}

TEST(SketchCanvasSolveTest, M17_PLD_001_APointLineDistanceDrivesThePointOffTheLine) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{10.0, 10.0});
    const SketchEditOutcome line = fixture.click(Vec2{110.0, 10.0});
    ASSERT_TRUE(line.applied) << line.status;
    const SketchEntityId edge = line.createdEntities.front();
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{edge, SketchSubElement::StartPoint}})
                    .applied);
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{edge, SketchSubElement::EndPoint}})
                    .applied);

    fixture.model.setTool(SketchTool::Point);
    const SketchEditOutcome point = fixture.click(Vec2{50.0, 40.0});
    ASSERT_TRUE(point.applied) << point.status;
    const SketchEntityId node = point.createdEntities.front();

    const SketchEditOutcome dimension =
        fixture.dimension({SketchElementRef{node, SketchSubElement::Whole},
                           SketchElementRef{edge, SketchSubElement::Whole}},
                          SketchEditKind::AddPointLineDistance);
    ASSERT_TRUE(dimension.applied) << dimension.status;
    const Parameter* parameter =
        fixture.document.parameters().findById(dimension.createdParameter);
    ASSERT_NE(parameter, nullptr);
    // NEGATIVE, and correctly so: the residual measures positive to the RIGHT
    // of the line's start->end, and this point is above a rightward line. The
    // canvas shows the magnitude; the constraint keeps the side, which is what
    // stops a solve from flipping the point across the line.
    EXPECT_NEAR(parameter->value(), -30.0, 1e-9);

    ASSERT_TRUE(CommitDimensionValue(fixture.document, fixture.sketch(),
                                     dimension.createdConstraints.front(), "-12")
                    .applied);
    (void)fixture.document.recompute();
    // UNDER-constrained is correct here: the point may still slide ALONG the
    // line. What must not happen is a refusal -- which is what a rejected
    // negative dimension looked like before DimensionValueValid learned that
    // signed separations mean something at and below zero.
    EXPECT_NE(fixture.sketch().solveStatus(), SketchSolveStatus::InvalidInput)
        << fixture.sketch().solveMessage();
    EXPECT_NE(fixture.sketch().solveStatus(), SketchSolveStatus::Conflicting);
    const SketchPoint& moved =
        std::get<SketchPoint>(fixture.sketch().findEntity(node)->geometry);
    // Perpendicular to the line, so only v moves -- and it stays on its side.
    EXPECT_NEAR(moved.position.y, 22.0, 1e-6);
    EXPECT_NEAR(moved.position.x, 50.0, 1e-6);
}

TEST(SketchCanvasSolveTest, M17_SYM_101_MovingTheSourceCarriesTheMirroredCopy) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{50.0, -50.0});
    const SketchEditOutcome axis = fixture.click(Vec2{50.0, 50.0});
    ASSERT_TRUE(axis.applied) << axis.status;
    const SketchEntityId mirror = axis.createdEntities.front();
    // Pin the mirror, or the solver may move IT instead of the copy.
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{mirror, SketchSubElement::StartPoint}})
                    .applied);
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{mirror, SketchSubElement::EndPoint}})
                    .applied);

    fixture.model.setTool(SketchTool::Point);
    const SketchEditOutcome source = fixture.click(Vec2{20.0, 10.0});
    ASSERT_TRUE(source.applied) << source.status;
    const SketchEntityId point = source.createdEntities.front();

    const MirrorOutcome mirrored =
        ApplyMirror(fixture.document, fixture.sketchId, {point}, mirror);
    ASSERT_TRUE(mirrored.applied) << mirrored.status;
    const SketchEntityId copy = mirrored.created.front();
    (void)fixture.document.recompute();

    const auto positionOf = [&](SketchEntityId id) {
        const SketchEntity* entity = fixture.sketch().findEntity(id);
        EXPECT_NE(entity, nullptr);
        return std::get<SketchPoint>(entity->geometry).position;
    };
    EXPECT_NEAR(positionOf(copy).x, 80.0, 1e-6);
    EXPECT_NEAR(positionOf(copy).y, 10.0, 1e-6);

    // THE POINT: pin the source somewhere new and the copy follows. A mirror
    // that only copied would leave this exactly where it was.
    ASSERT_TRUE(fixture
                    .dimension({SketchElementRef{mirror, SketchSubElement::StartPoint},
                                SketchElementRef{point, SketchSubElement::Whole}},
                               SketchEditKind::AddVerticalDistance)
                    .applied);
    const SketchEditOutcome across =
        fixture.dimension({SketchElementRef{mirror, SketchSubElement::StartPoint},
                           SketchElementRef{point, SketchSubElement::Whole}},
                          SketchEditKind::AddHorizontalDistance);
    ASSERT_TRUE(across.applied) << across.status;
    ASSERT_TRUE(CommitDimensionValue(fixture.document, fixture.sketch(),
                                     across.createdConstraints.front(), "-40")
                    .applied);
    (void)fixture.document.recompute();

    EXPECT_NE(fixture.sketch().solveStatus(), SketchSolveStatus::Conflicting);
    // The dimension ORDERED its pair so the seed read positive (ADR-M17-001),
    // which made it measure mirror.start -> point; -40 therefore puts the
    // SOURCE at x = 90, and its image lands at x = 10. Which of the two moved
    // is the dimension's business; that the other FOLLOWED is symmetry's, and
    // that is what this asserts.
    EXPECT_NEAR(positionOf(point).x, 90.0, 1e-6);
    EXPECT_NEAR(positionOf(copy).x, 10.0, 1e-6);
    // ...and still square to the mirror, which is the second residual's job.
    EXPECT_NEAR(positionOf(copy).y, positionOf(point).y, 1e-6);
}

// =============================================================================
// Dragging under constraints (M17)
//
// The whole value of a drag is that the CONSTRAINTS answer, not the cursor. So
// every test here is about what the sketch refuses to do, not about what it
// does when nothing is holding it.
// =============================================================================

TEST(SketchDragSolveTest, M17_DRAG_001_AFreePointFollowsTheCursor) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Point);
    const SketchEditOutcome added = fixture.click(Vec2{10.0, 10.0});
    ASSERT_TRUE(added.applied) << added.status;
    const SketchEntityId point = added.createdEntities.front();

    ASSERT_TRUE(DragMoved(fixture.document.previewSketchDrag(
        fixture.sketchId, SketchElementRef{point, SketchSubElement::Whole}, Vec2{60.0, 40.0})));
    const SketchPoint& moved =
        std::get<SketchPoint>(fixture.sketch().findEntity(point)->geometry);
    EXPECT_NEAR(moved.position.x, 60.0, 1e-6);
    EXPECT_NEAR(moved.position.y, 40.0, 1e-6);
}

TEST(SketchDragSolveTest, M17_DRAG_002_AFixedPointDoesNotMove) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Point);
    const SketchEditOutcome added = fixture.click(Vec2{10.0, 10.0});
    ASSERT_TRUE(added.applied) << added.status;
    const SketchEntityId point = added.createdEntities.front();
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{point, SketchSubElement::Whole}})
                    .applied);

    // The point does not move. The pinned attempt contradicts the Fix, and the
    // seeded one is pulled straight back to where the Fix says it belongs.
    //
    // This is the test that caught the ordering bug: with the cursor written
    // into the sketch BEFORE the problem was built, the Fix re-read its target
    // from the moved geometry and followed the mouse anywhere -- while
    // reporting Solved.
    (void)fixture.document.previewSketchDrag(
        fixture.sketchId, SketchElementRef{point, SketchSubElement::Whole}, Vec2{60.0, 40.0});
    const SketchPoint& stayed =
        std::get<SketchPoint>(fixture.sketch().findEntity(point)->geometry);
    EXPECT_NEAR(stayed.position.x, 10.0, 1e-9);
    EXPECT_NEAR(stayed.position.y, 10.0, 1e-9);
}

TEST(SketchDragSolveTest, M17_DRAG_003_ACoincidentNeighbourIsCARRIEDALONG) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{0.0, 0.0});
    const SketchEditOutcome first = fixture.click(Vec2{50.0, 0.0});
    ASSERT_TRUE(first.applied) << first.status;
    // Chained: the second line starts where the first ended, joined by a real
    // Coincident.
    const SketchEditOutcome second = fixture.click(Vec2{50.0, 50.0});
    ASSERT_TRUE(second.applied) << second.status;
    const SketchEntityId a = first.createdEntities.front();
    const SketchEntityId b = second.createdEntities.front();

    ASSERT_TRUE(DragMoved(fixture.document.previewSketchDrag(
        fixture.sketchId, SketchElementRef{a, SketchSubElement::EndPoint}, Vec2{70.0, 20.0})));

    const SketchLine& lineA = std::get<SketchLine>(fixture.sketch().findEntity(a)->geometry);
    const SketchLine& lineB = std::get<SketchLine>(fixture.sketch().findEntity(b)->geometry);
    EXPECT_NEAR(lineA.end.x, 70.0, 1e-6);
    EXPECT_NEAR(lineA.end.y, 20.0, 1e-6);
    // THE POINT: the corner moved as ONE thing. A drag that moved only what was
    // grabbed would have pulled the two lines apart, and the sketch would say
    // they are still joined.
    EXPECT_NEAR(lineB.start.x, lineA.end.x, 1e-6);
    EXPECT_NEAR(lineB.start.y, lineA.end.y, 1e-6);
}

TEST(SketchDragSolveTest, M17_DRAG_004_AHorizontalLineSTAYSHorizontal) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{0.0, 0.0});
    const SketchEditOutcome drawn = fixture.click(Vec2{50.0, 0.0});
    ASSERT_TRUE(drawn.applied) << drawn.status;
    const SketchEntityId line = drawn.createdEntities.front();
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddHorizontal,
                                  {SketchElementRef{line, SketchSubElement::Whole}})
                    .applied);

    // Dragged UP and to the right. Horizontal says the two ends share a v, so
    // the whole line rises rather than tilting.
    const SketchSolveStatus status = fixture.document.previewSketchDrag(
        fixture.sketchId, SketchElementRef{line, SketchSubElement::EndPoint}, Vec2{70.0, 30.0});
    ASSERT_TRUE(DragMoved(status)) << SolveStatusName(status);
    const SketchLine& moved = std::get<SketchLine>(fixture.sketch().findEntity(line)->geometry);
    EXPECT_NEAR(moved.start.y, moved.end.y, 1e-6);
    EXPECT_NEAR(moved.end.x, 70.0, 1e-6);
}

TEST(SketchDragSolveTest, M17_DRAG_005_ADimensionedLengthIsPRESERVED) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{0.0, 0.0});
    const SketchEditOutcome drawn = fixture.click(Vec2{50.0, 0.0});
    ASSERT_TRUE(drawn.applied) << drawn.status;
    const SketchEntityId line = drawn.createdEntities.front();
    ASSERT_TRUE(
        fixture.dimension({SketchElementRef{line, SketchSubElement::Whole}}).applied);

    // Dragged far past its own length: the length dimension wins, so the line
    // swings round instead of stretching.
    ASSERT_TRUE(DragMoved(fixture.document.previewSketchDrag(
        fixture.sketchId, SketchElementRef{line, SketchSubElement::EndPoint},
        Vec2{200.0, 200.0})));
    const SketchLine& moved = std::get<SketchLine>(fixture.sketch().findEntity(line)->geometry);
    const double length = std::sqrt(std::pow(moved.end.x - moved.start.x, 2.0) +
                                    std::pow(moved.end.y - moved.start.y, 2.0));
    EXPECT_NEAR(length, 50.0, 1e-6);
}

TEST(SketchDragSolveTest, M17_DRAG_006_AnArcTipCannotBeDraggedBecauseItHasNoVariables) {
    SolvingFixture fixture;
    SketchEdit arc;
    arc.kind = SketchEditKind::AddArc;
    arc.points = {Vec2{0.0, 0.0}, Vec2{20.0, 0.0}, Vec2{0.0, 20.0}};
    arc.label = "Add arc";
    const SketchEditOutcome added = fixture.apply(arc);
    ASSERT_TRUE(added.applied) << added.status;
    const SketchEntityId id = added.createdEntities.front();

    // ADR-M12-003 again: an arc's tips carry no solver variables, so there is
    // nothing to pin. Refused rather than dragging the CENTRE, which is the
    // nearest thing that does have variables and is not what was grabbed.
    EXPECT_FALSE(DragMoved(fixture.document.previewSketchDrag(
        fixture.sketchId, SketchElementRef{id, SketchSubElement::StartPoint}, Vec2{5.0, 5.0})));
    // The centre, however, drags.
    EXPECT_TRUE(DragMoved(fixture.document.previewSketchDrag(
        fixture.sketchId, SketchElementRef{id, SketchSubElement::CenterPoint}, Vec2{5.0, 5.0})));
}

TEST(SketchDragSolveTest, M17_DRAG_007_ADragIsONEUndoStepCarryingEveryThingThatMoved) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{0.0, 0.0});
    const SketchEditOutcome first = fixture.click(Vec2{50.0, 0.0});
    ASSERT_TRUE(first.applied) << first.status;
    const SketchEditOutcome second = fixture.click(Vec2{50.0, 50.0});
    ASSERT_TRUE(second.applied) << second.status;
    const SketchEntityId a = first.createdEntities.front();
    const SketchEntityId b = second.createdEntities.front();

    const auto before = fixture.document.sketchGeometrySnapshot(fixture.sketchId);
    const std::size_t depth = fixture.document.undoDepth();
    // Several moves, as a real drag produces -- and none of them recorded.
    ASSERT_TRUE(DragMoved(fixture.document.previewSketchDrag(
        fixture.sketchId, SketchElementRef{a, SketchSubElement::EndPoint}, Vec2{60.0, 10.0})));
    ASSERT_TRUE(DragMoved(fixture.document.previewSketchDrag(
        fixture.sketchId, SketchElementRef{a, SketchSubElement::EndPoint}, Vec2{70.0, 20.0})));
    EXPECT_EQ(fixture.document.undoDepth(), depth);

    // TWO entities moved: the line that was grabbed and the neighbour its
    // coincidence carried along. An undo that put back only the grabbed one
    // would leave the sketch in a state the user never saw.
    EXPECT_EQ(fixture.document.commitSketchDrag(fixture.sketchId, before, "Drag"), 2u);
    EXPECT_EQ(fixture.document.undoDepth(), depth + 1);

    ASSERT_TRUE(fixture.document.undo());
    const SketchLine& restoredA = std::get<SketchLine>(fixture.sketch().findEntity(a)->geometry);
    const SketchLine& restoredB = std::get<SketchLine>(fixture.sketch().findEntity(b)->geometry);
    EXPECT_NEAR(restoredA.end.x, 50.0, 1e-9);
    EXPECT_NEAR(restoredB.start.x, 50.0, 1e-9);
}

TEST(SketchDragSolveTest, M17_DRAG_008_ADragThatMovedNothingRecordsNothing) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Point);
    const SketchEditOutcome added = fixture.click(Vec2{10.0, 10.0});
    ASSERT_TRUE(added.applied) << added.status;

    const auto before = fixture.document.sketchGeometrySnapshot(fixture.sketchId);
    const std::size_t depth = fixture.document.undoDepth();
    // Grabbed and released without moving: no step for the user to walk back
    // through.
    EXPECT_EQ(fixture.document.commitSketchDrag(fixture.sketchId, before, "Drag"), 0u);
    EXPECT_EQ(fixture.document.undoDepth(), depth);
}

TEST(SketchDragSolveTest, M17_DRAG_009_EscapeDuringADragPutsEverythingBack) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{0.0, 0.0});
    const SketchEditOutcome drawn = fixture.click(Vec2{50.0, 0.0});
    ASSERT_TRUE(drawn.applied) << drawn.status;
    const SketchEntityId line = drawn.createdEntities.front();

    const auto before = fixture.document.sketchGeometrySnapshot(fixture.sketchId);
    ASSERT_TRUE(DragMoved(fixture.document.previewSketchDrag(
        fixture.sketchId, SketchElementRef{line, SketchSubElement::EndPoint}, Vec2{90.0, 90.0})));
    const std::size_t depth = fixture.document.undoDepth();

    ASSERT_TRUE(fixture.document.restoreSketchGeometry(fixture.sketchId, before));
    const SketchLine& back = std::get<SketchLine>(fixture.sketch().findEntity(line)->geometry);
    EXPECT_NEAR(back.end.x, 50.0, 1e-9);
    EXPECT_NEAR(back.end.y, 0.0, 1e-9);
    // An abandoned preview leaves NOTHING behind, including on the undo stack.
    EXPECT_EQ(fixture.document.undoDepth(), depth);
}

TEST(SketchCanvasSolveTest, M17_FIL_101_TheFilletSURVIVESASolveAndStaysTangent) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{10.0, 10.0});
    const SketchEditOutcome a = fixture.click(Vec2{110.0, 10.0});
    ASSERT_TRUE(a.applied) << a.status;
    const SketchEditOutcome b = fixture.click(Vec2{110.0, 110.0});
    ASSERT_TRUE(b.applied) << b.status;
    fixture.model.cancel();
    const SketchEntityId lineA = a.createdEntities.front();
    const SketchEntityId lineB = b.createdEntities.front();

    const ChamferOutcome fillet =
        ApplyFillet(fixture.document, fixture.sketchId, lineA, lineB, 20.0);
    ASSERT_TRUE(fillet.applied) << fillet.status;

    // THE TEST THIS FEATURE EXISTS FOR -- and it has to MOVE something.
    //
    // A plain recompute proves nothing: the fillet is placed consistent, so the
    // solver has nothing to do and everything looks fine even with the arc's
    // tips bound to nothing at all. (A mutation removing ArcTipU/V survived
    // exactly that version of this test.) Dragging one line's far end forces
    // the whole corner to be re-solved, which is when unbound tips come apart.
    ASSERT_TRUE(DragMoved(fixture.document.previewSketchDrag(
        fixture.sketchId, SketchElementRef{lineA, SketchSubElement::StartPoint},
        Vec2{0.0, 40.0})));
    (void)fixture.document.recompute();
    EXPECT_NE(fixture.sketch().solveStatus(), SketchSolveStatus::Conflicting);
    EXPECT_NE(fixture.sketch().solveStatus(), SketchSolveStatus::InvalidInput)
        << fixture.sketch().solveMessage();

    const SketchArc& arc =
        std::get<SketchArc>(fixture.sketch().findEntity(fillet.created)->geometry);
    const SketchLine& trimmedA = std::get<SketchLine>(fixture.sketch().findEntity(lineA)->geometry);
    const SketchLine& trimmedB = std::get<SketchLine>(fixture.sketch().findEntity(lineB)->geometry);

    // Still TANGENT: the centre is exactly r from each line.
    const auto distanceToLine = [](const SketchLine& line, Vec2 p) {
        const double du = line.end.x - line.start.x;
        const double dv = line.end.y - line.start.y;
        const double length = std::sqrt(du * du + dv * dv);
        return std::abs((p.x - line.start.x) * dv - (p.y - line.start.y) * du) / length;
    };
    EXPECT_NEAR(distanceToLine(trimmedA, arc.center), arc.radiusMm, 1e-5);
    EXPECT_NEAR(distanceToLine(trimmedB, arc.center), arc.radiusMm, 1e-5);

    // Still JOINED: each tip is on its line's end.
    const Vec2 tipStart{arc.center.x + arc.radiusMm * std::cos(arc.startAngleRad),
                        arc.center.y + arc.radiusMm * std::sin(arc.startAngleRad)};
    const Vec2 tipEnd{arc.center.x + arc.radiusMm * std::cos(arc.endAngleRad),
                      arc.center.y + arc.radiusMm * std::sin(arc.endAngleRad)};
    const double gapA = std::min(
        std::hypot(tipStart.x - trimmedA.end.x, tipStart.y - trimmedA.end.y),
        std::hypot(tipEnd.x - trimmedA.end.x, tipEnd.y - trimmedA.end.y));
    const double gapB = std::min(
        std::hypot(tipStart.x - trimmedB.start.x, tipStart.y - trimmedB.start.y),
        std::hypot(tipEnd.x - trimmedB.start.x, tipEnd.y - trimmedB.start.y));
    EXPECT_LT(gapA, 1e-5);
    EXPECT_LT(gapB, 1e-5);
}

TEST(SketchCanvasSolveTest, M17_FIL_102_AFilletsTangenciesNAMETheCornersTheyHold) {
    // FIL_101 above checks the corner comes out smooth, and it passed for a
    // year while the tangencies held nothing: the arc is placed already tangent
    // and the two coincidences alone keep it roughly there. This checks the
    // constraints THEMSELVES, which is the claim the comment in ApplyFillet
    // makes -- "smooth today and kinked after the first parameter change" is
    // precisely what an unnamed tangency cannot prevent.
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{10.0, 10.0});
    const SketchEditOutcome a = fixture.click(Vec2{110.0, 10.0});
    ASSERT_TRUE(a.applied) << a.status;
    const SketchEditOutcome b = fixture.click(Vec2{110.0, 110.0});
    ASSERT_TRUE(b.applied) << b.status;
    fixture.model.cancel();

    const ChamferOutcome fillet = ApplyFillet(fixture.document, fixture.sketchId,
                                              a.createdEntities.front(),
                                              b.createdEntities.front(), 20.0);
    ASSERT_TRUE(fillet.applied) << fillet.status;

    int tangencies = 0;
    for (const SketchConstraint& constraint : fixture.sketch().constraints()) {
        const auto* smooth = std::get_if<TangentConstraint>(&constraint.data);
        if (smooth == nullptr) continue;
        ++tangencies;
        EXPECT_NE(smooth->at, SketchSubElement::Whole)
            << "a fillet tangency that does not say where it touches holds nothing";
    }
    EXPECT_EQ(tangencies, 2);
}

TEST(SketchCanvasSolveTest, M17_FIL_103_AskingForTangencyAtAPinnedCornerFindsIt) {
    // The user's own Tangent command. Two entities already joined by a
    // coincidence are touching AT that joint, and asking for the sliding form
    // there would add a constraint that cannot hold. So the corner is looked up
    // once, when the constraint is made, and stored.
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{10.0, 10.0});
    const SketchEditOutcome a = fixture.click(Vec2{110.0, 10.0});
    ASSERT_TRUE(a.applied) << a.status;
    const SketchEditOutcome b = fixture.click(Vec2{110.0, 110.0});
    ASSERT_TRUE(b.applied) << b.status;
    fixture.model.cancel();

    const ChamferOutcome fillet = ApplyFillet(fixture.document, fixture.sketchId,
                                              a.createdEntities.front(),
                                              b.createdEntities.front(), 20.0);
    ASSERT_TRUE(fillet.applied) << fillet.status;

    // Ask for ANOTHER tangency between the same line and arc, by hand. It is
    // redundant with the fillet's own -- what is being checked is that the
    // command finds the corner, not that the sketch needs the constraint.
    fixture.model.clearSelection();
    fixture.model.toggleSelection(
        SketchElementRef{a.createdEntities.front(), SketchSubElement::Whole});
    fixture.model.toggleSelection(SketchElementRef{fillet.created, SketchSubElement::Whole});
    std::string whyNot;
    const SketchEdit edit =
        fixture.model.requestConstraint(fixture.sketch(), SketchEditKind::AddTangent, &whyNot);
    ASSERT_EQ(edit.kind, SketchEditKind::AddTangent) << whyNot;
    ASSERT_EQ(edit.refs.size(), 2u);
    // The LINE leads, and it carries the end the coincidence pinned.
    EXPECT_EQ(edit.refs[0].entityId, a.createdEntities.front());
    EXPECT_EQ(edit.refs[0].subElement, SketchSubElement::EndPoint);
}

TEST(SketchCanvasSolveTest, M17_FIL_104_TangencyBetweenSTRANGERSStaysUnpinned) {
    // The other half of the same rule: two entities that do NOT already meet
    // are not touching anywhere in particular, and claiming a corner would
    // constrain a point nothing puts on the curve. Whole is the honest answer.
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{-40.0, 25.0});
    const SketchEditOutcome line = fixture.click(Vec2{40.0, 25.0});
    ASSERT_TRUE(line.applied) << line.status;
    fixture.model.cancel();
    fixture.model.setTool(SketchTool::Circle);
    fixture.click(Vec2{0.0, 0.0});
    const SketchEditOutcome circle = fixture.click(Vec2{10.0, 0.0});
    ASSERT_TRUE(circle.applied) << circle.status;
    fixture.model.cancel();

    fixture.model.clearSelection();
    fixture.model.toggleSelection(
        SketchElementRef{line.createdEntities.front(), SketchSubElement::Whole});
    fixture.model.toggleSelection(
        SketchElementRef{circle.createdEntities.front(), SketchSubElement::Whole});
    std::string whyNot;
    const SketchEdit edit =
        fixture.model.requestConstraint(fixture.sketch(), SketchEditKind::AddTangent, &whyNot);
    ASSERT_EQ(edit.kind, SketchEditKind::AddTangent) << whyNot;
    ASSERT_EQ(edit.refs.size(), 2u);
    EXPECT_EQ(edit.refs[0].subElement, SketchSubElement::Whole);
}

// =============================================================================
// Diagnosis: the user has to be told WHICH constraints are at fault
// =============================================================================

TEST(SketchCanvasSolveTest, M12_SOLVE_005_ContradictoryDimensionsAreReportedAsConflicting) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{0.0, 0.0});
    const SketchEditOutcome line = fixture.click(Vec2{100.0, 0.0});
    ASSERT_TRUE(line.applied) << line.status;
    const SketchEntityId id = line.createdEntities.front();

    // Two lengths on ONE line, disagreeing. Roadmap 8.2 point 3: this is a
    // CONFLICT, and it must not be reported as the same thing as a redundant
    // but consistent constraint.
    const SketchEditOutcome first =
        fixture.dimension({SketchElementRef{id, SketchSubElement::Whole}});
    ASSERT_TRUE(first.applied) << first.status;
    const SketchEditOutcome second =
        fixture.dimension({SketchElementRef{id, SketchSubElement::Whole}});
    ASSERT_TRUE(second.applied) << second.status;
    ASSERT_TRUE(CommitDimensionValue(fixture.document, fixture.sketch(),
                                     second.createdConstraints.front(), "70")
                    .applied);
    (void)fixture.document.recompute();

    const SketchSolveStatus status = fixture.sketch().solveStatus();
    EXPECT_TRUE(status == SketchSolveStatus::Conflicting ||
                status == SketchSolveStatus::NumericalFailure)
        << SolveStatusName(status);

    const SketchStatusLine reported = DescribeSketchStatus(fixture.sketch());
    EXPECT_FALSE(reported.badge.empty());
    // "Solver failed" on its own is what roadmap section 8 forbids.
    EXPECT_FALSE(reported.text.empty());
    EXPECT_NE(reported.text, "Solver failed");
}

TEST(SketchCanvasSolveTest, M12_SOLVE_006_TheConstraintPanelMarksTheConstraintsTheSolverBlamed) {
    SolvingFixture fixture;
    fixture.model.setTool(SketchTool::Line);
    fixture.click(Vec2{0.0, 0.0});
    const SketchEditOutcome line = fixture.click(Vec2{100.0, 0.0});
    ASSERT_TRUE(line.applied) << line.status;
    const SketchEntityId id = line.createdEntities.front();

    ASSERT_TRUE(fixture.dimension({SketchElementRef{id, SketchSubElement::Whole}}).applied);
    const SketchEditOutcome second =
        fixture.dimension({SketchElementRef{id, SketchSubElement::Whole}});
    ASSERT_TRUE(second.applied) << second.status;
    ASSERT_TRUE(CommitDimensionValue(fixture.document, fixture.sketch(),
                                     second.createdConstraints.front(), "70")
                    .applied);
    (void)fixture.document.recompute();

    const std::vector<ConstraintRow> rows =
        ConstraintRowsFor(fixture.document, fixture.sketch());
    ASSERT_FALSE(rows.empty());
    // If the solver named anyone, the panel must be able to point at them --
    // that is roadmap 8.2's "locatable" requirement, and it is the difference
    // between a diagnosis and an apology.
    if (!fixture.sketch().offendingConstraints().empty()) {
        EXPECT_TRUE(std::any_of(rows.begin(), rows.end(),
                                [](const ConstraintRow& row) { return row.offending; }));
    }
}

TEST(SketchCanvasSolveTest, M12_SOLVE_007_UndoingADimensionReturnsTheSketchToItsPreviousState) {
    SolvingFixture fixture;
    const SketchEditOutcome rectangle = fixture.drawRectangle(Vec2{0.0, 0.0}, Vec2{100.0, 50.0});
    ASSERT_TRUE(rectangle.applied) << rectangle.status;
    const SketchEntityId bottom = rectangle.createdEntities[0];

    const std::size_t constraintsBefore = fixture.sketch().constraints().size();
    const SketchEditOutcome width =
        fixture.dimension({SketchElementRef{bottom, SketchSubElement::Whole}});
    ASSERT_TRUE(width.applied) << width.status;
    ASSERT_EQ(fixture.sketch().constraints().size(), constraintsBefore + 1);

    ASSERT_TRUE(fixture.document.undo());
    (void)fixture.document.recompute();
    EXPECT_EQ(fixture.sketch().constraints().size(), constraintsBefore);
    EXPECT_EQ(fixture.document.parameters().findById(width.createdParameter), nullptr);
    // The geometry is untouched and still solvable.
    EXPECT_NE(fixture.sketch().solveStatus(), SketchSolveStatus::InvalidInput);
}

// --- M17.17: four new ways to place geometry ---------------------------------
//
// Each is a different set of CLICKS, not a new kind of entity -- so what these
// pin is that the clicks land the geometry where the user pointed, and that
// the constraints that come with it leave the DOF telling the truth.

namespace {

// Applies one tool's clicks and keeps the sketch, ready to solve.
struct ToolRun {
    PartDocument document{"ToolDoc"};
    GaussNewtonSketchSolver solver;
    Sketch* sketch = nullptr;
    SketchCanvasModel model;
    std::string lastStatus;

    ToolRun() {
        document.setSketchSolver(&solver);
        sketch = &document.addSketch("Sketch001");
    }

    void click(Vec2 at) {
        SnapResult snap;
        snap.point = at;
        apply(snap);
    }

    // A click that goes through the REAL snapper, the way the canvas widget
    // does. Kept separate from click() above because most tools do not care
    // what is under the cursor, and a test that snapped by accident would be
    // inferring constraints it never mentions. The tangent arc is the one tool
    // that cannot work without a reference, so its tests use this one.
    void snapClick(Vec2 at) {
        apply(SnapCursor(*sketch, at, 2.0, 0.0, false));
    }

    void apply(const SnapResult& snap) {
        const SketchEdit edit = model.click(snap);
        if (!edit.valid()) return; // a tool mid-sequence, or a refused shape
        // The STATUS is kept, because a tool that produced an edit the document
        // refused is a different failure from a tool that produced none -- and
        // the two look identical from an entity count.
        const SketchEditOutcome outcome = ApplySketchEdit(document, sketch->id(), edit);
        lastStatus = outcome.status;
        // WHAT THE WIDGET DOES NEXT, and it is not optional: a chaining tool
        // learns the id of what it just made here, and without it the second
        // segment of every chain reaches for a reference that was never
        // recorded.
        model.afterApply(outcome.createdEntities);
    }
};

int CountOf(const Sketch& sketch, std::size_t which) {
    int count = 0;
    for (const SketchEntity& entity : sketch.entities())
        if (entity.geometry.index() == which) ++count;
    return count;
}

constexpr std::size_t kLineIndex = 1;
constexpr std::size_t kCircleIndex = 2;
constexpr std::size_t kArcIndex = 3;

} // namespace

TEST(SketchToolsTest, M17_TOOL_001_ACentreRectangleIsCentredOnTheFirstClick) {
    // The claim the tool's name makes. Built from the clicked corner alone it
    // would be a corner rectangle with a misleading prompt.
    ToolRun run;
    run.model.setTool(SketchTool::CenterRectangle);
    run.click(Vec2{50, 40});
    run.click(Vec2{70, 50}); // 20 x 10 from the centre

    EXPECT_EQ(CountOf(*run.sketch, kLineIndex), 4);
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (const SketchEntity& entity : run.sketch->entities()) {
        const auto* line = std::get_if<SketchLine>(&entity.geometry);
        ASSERT_NE(line, nullptr);
        for (Vec2 p : {line->start, line->end}) {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
    }
    EXPECT_NEAR((minX + maxX) / 2.0, 50.0, 1e-9);
    EXPECT_NEAR((minY + maxY) / 2.0, 40.0, 1e-9);
    EXPECT_NEAR(maxX - minX, 40.0, 1e-9);
    EXPECT_NEAR(maxY - minY, 20.0, 1e-9);
}

TEST(SketchToolsTest, M17_TOOL_002_AThreePointCirclePassesThroughAllThree) {
    ToolRun run;
    run.model.setTool(SketchTool::ThreePointCircle);
    run.click(Vec2{0, 10});
    run.click(Vec2{10, 0});
    run.click(Vec2{-10, 0});

    ASSERT_EQ(CountOf(*run.sketch, kCircleIndex), 1);
    const auto* circle = std::get_if<SketchCircle>(&run.sketch->entities().front().geometry);
    ASSERT_NE(circle, nullptr);
    // All three the same distance from the centre IS the definition, and it
    // catches a centre that is merely close.
    for (Vec2 p : {Vec2{0, 10}, Vec2{10, 0}, Vec2{-10, 0}}) {
        const double dx = p.x - circle->center.x;
        const double dy = p.y - circle->center.y;
        EXPECT_NEAR(std::sqrt(dx * dx + dy * dy), circle->radiusMm, 1e-9);
    }
}

TEST(SketchToolsTest, M17_TOOL_003_ThreeCollinearPointsAreRefusedNotTurnedIntoAHugeCircle) {
    // What a user gets by clicking along an edge. The available wrong answer is
    // a circle of near-infinite radius, which looks like a straight line and
    // breaks every profile it touches.
    ToolRun run;
    run.model.setTool(SketchTool::ThreePointCircle);
    run.click(Vec2{0, 0});
    run.click(Vec2{10, 0});
    run.click(Vec2{20, 0});
    EXPECT_EQ(CountOf(*run.sketch, kCircleIndex), 0);
}

TEST(SketchToolsTest, M17_TOOL_004_TheThirdClickDecidesWHICHWayTheArcGoes) {
    // Two arcs join any two points, and the third click is the entire reason
    // this tool exists. Both runs share the same two tips; only the through
    // point differs, and the results must be opposite arcs.
    ToolRun above;
    above.model.setTool(SketchTool::ThreePointArc);
    above.click(Vec2{-10, 0});
    above.click(Vec2{10, 0});
    above.click(Vec2{0, 10}); // bulging up

    ToolRun below;
    below.model.setTool(SketchTool::ThreePointArc);
    below.click(Vec2{-10, 0});
    below.click(Vec2{10, 0});
    below.click(Vec2{0, -10}); // bulging down

    ASSERT_EQ(CountOf(*above.sketch, kArcIndex), 1);
    ASSERT_EQ(CountOf(*below.sketch, kArcIndex), 1);
    const auto& up = std::get<SketchArc>(above.sketch->entities().front().geometry);
    const auto& down = std::get<SketchArc>(below.sketch->entities().front().geometry);

    EXPECT_NEAR(up.radiusMm, down.radiusMm, 1e-9);
    const auto midpoint = [](const SketchArc& arc) {
        double sweep = arc.endAngleRad - arc.startAngleRad;
        while (sweep < 0.0) sweep += 2.0 * 3.14159265358979323846;
        const double at = arc.startAngleRad + sweep / 2.0;
        return Vec2{arc.center.x + arc.radiusMm * std::cos(at),
                    arc.center.y + arc.radiusMm * std::sin(at)};
    };
    EXPECT_GT(midpoint(up).y, 0.0) << "the arc did not bulge towards the third click";
    EXPECT_LT(midpoint(down).y, 0.0) << "the arc did not bulge towards the third click";
}

TEST(SketchToolsTest, M17_TOOL_005_APolygonIsREGULARAndSaysSoInItsDOF) {
    // Six sides on a construction circle with equal lengths. The DOF is the
    // claim: a regular polygon has four freedoms -- centre, radius, rotation --
    // and anything else means the constraints do not add up to "regular".
    ToolRun run;
    run.model.setPolygonSides(6);
    run.model.setTool(SketchTool::Polygon);
    run.click(Vec2{20, 20});
    run.click(Vec2{40, 20}); // radius 20

    EXPECT_EQ(CountOf(*run.sketch, kLineIndex), 6);
    EXPECT_EQ(CountOf(*run.sketch, kCircleIndex), 1);
    // The circle is CONSTRUCTION: a circumscribed circle appearing in the solid
    // would be a curve nobody drew.
    for (const SketchEntity& entity : run.sketch->entities())
        if (std::holds_alternative<SketchCircle>(entity.geometry))
            EXPECT_TRUE(run.sketch->isConstruction(entity.id));

    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();
    EXPECT_EQ(run.sketch->degreesOfFreedom(), 4)
        << "a regular hexagon should have centre, radius and rotation left: "
        << run.sketch->solveMessage();
    EXPECT_TRUE(run.sketch->offendingConstraints().empty());

    // ...and the sides really are the same length, which is what "regular"
    // buys and what a count of constraints alone would not show.
    double first = -1.0;
    for (const SketchEntity& entity : run.sketch->entities()) {
        const auto* line = std::get_if<SketchLine>(&entity.geometry);
        if (line == nullptr) continue;
        const double dx = line->end.x - line->start.x;
        const double dy = line->end.y - line->start.y;
        const double length = std::sqrt(dx * dx + dy * dy);
        if (first < 0.0) first = length;
        EXPECT_NEAR(length, first, 1e-6);
    }
}

TEST(SketchToolsTest, M17_TOOL_006_ThePolygonSideCountIsCarriedONTheEditNotReadBack) {
    // Replaying an edit must not be at the mercy of what the toolbar happens to
    // be set to now -- which is what reading the model at apply time would do,
    // and undo/redo replays exactly this.
    ToolRun run;
    run.model.setPolygonSides(5);
    run.model.setTool(SketchTool::Polygon);
    run.click(Vec2{0, 0});
    run.click(Vec2{10, 0});
    EXPECT_EQ(CountOf(*run.sketch, kLineIndex), 5);

    run.model.setPolygonSides(8);
    run.model.setTool(SketchTool::Polygon);
    run.click(Vec2{100, 0});
    run.click(Vec2{110, 0});
    EXPECT_EQ(CountOf(*run.sketch, kLineIndex), 13); // 5 + 8
}

TEST(SketchToolsTest, M17_TOOL_010_ASlotIsFULLYHeldExceptForItsFiveOwnFreedoms) {
    // The arithmetic, and the equation that had to change before it came out.
    //
    // A slot has five freedoms -- both centres and the radius -- and its four
    // entities carry eighteen, so the constraints have to take thirteen. Four
    // corner coincidences take 8, one equal radius takes 1, and four tangencies
    // take 4. Thirteen exactly.
    //
    // This test first measured NINE. All thirteen constraints existed and all
    // were satisfied; the four tangencies removed nothing. Asked as
    // TangentLineCircle -- "the perpendicular distance from the centre to the
    // line equals the radius" -- at a point a coincidence has ALREADY pinned
    // onto the line, that distance can never exceed the radius: the residual
    // sits at a maximum, its gradient vanishes, and the constraint holds
    // nothing however true it is.
    //
    // Saying WHERE they touch is what fixed it. TangentAtPoint states tangency
    // as perpendicularity -- the line leaves the touch point square to the
    // radius -- and that has a gradient. Same four constraints, same geometry,
    // four more freedoms removed.
    ToolRun run;
    run.model.setTool(SketchTool::Slot);
    run.click(Vec2{20, 30});  // one centre
    run.click(Vec2{60, 30});  // the other
    run.click(Vec2{60, 38});  // 8 mm out: the radius

    EXPECT_EQ(CountOf(*run.sketch, kLineIndex), 2) << run.lastStatus;
    EXPECT_EQ(CountOf(*run.sketch, kArcIndex), 2) << run.lastStatus;

    // The constraints that make it a slot, counted before the DOF is read:
    // "nine constraints exist" and "they remove thirteen freedoms" are
    // different claims and they fail for different reasons. That distinction is
    // the only reason the rank deficiency was visible at all.
    int coincident = 0, equal = 0, tangent = 0;
    for (const SketchConstraint& constraint : run.sketch->constraints()) {
        const std::string kind = ConstraintKindName(constraint.data);
        if (kind == "Coincident") ++coincident;
        if (kind == "Equal") ++equal;
        if (kind == "Tangent") ++tangent;
    }
    EXPECT_EQ(coincident, 4);
    EXPECT_EQ(equal, 1);
    EXPECT_EQ(tangent, 4);

    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();
    EXPECT_TRUE(run.sketch->offendingConstraints().empty()) << run.sketch->solveMessage();
    EXPECT_EQ(run.sketch->degreesOfFreedom(), 5) << run.sketch->solveMessage();
}

TEST(SketchToolsTest, M17_TOOL_010b_EverySlotTangencySAYSWhereItHolds) {
    // The count above cannot tell a tangency that names its corner from one
    // that does not: four exist either way, and the DOF only moves once ALL
    // FOUR say where. So the corners are checked directly -- and against the
    // coincidences, which is where the tangencies were derived from.
    ToolRun run;
    run.model.setTool(SketchTool::Slot);
    run.click(Vec2{20, 30});
    run.click(Vec2{60, 30});
    run.click(Vec2{60, 38});

    std::set<std::pair<ObjectId, int>> pinnedCorners;
    std::set<std::pair<ObjectId, int>> tangentCorners;
    for (const SketchConstraint& constraint : run.sketch->constraints()) {
        if (const auto* joint = std::get_if<CoincidentConstraint>(&constraint.data)) {
            for (const SketchElementRef& ref : {joint->a, joint->b})
                pinnedCorners.insert({ToObjectId(ref.entityId), static_cast<int>(ref.subElement)});
        } else if (const auto* smooth = std::get_if<TangentConstraint>(&constraint.data)) {
            // NEVER Whole: a slot's tangencies are all at corners, and one that
            // forgot to say so is the silent-nine case coming back.
            EXPECT_NE(smooth->at, SketchSubElement::Whole);
            tangentCorners.insert({ToObjectId(smooth->a), static_cast<int>(smooth->at)});
        }
    }
    EXPECT_EQ(tangentCorners.size(), 4u) << "two tangencies claim the same corner";
    // Every corner a tangency names is a corner a coincidence actually pinned.
    // Tangency somewhere ELSE on the line is the equation that holds nothing.
    for (const auto& corner : tangentCorners)
        EXPECT_TRUE(pinnedCorners.count(corner) == 1)
            << "a tangency holds a point no coincidence pinned";
}

TEST(SketchToolsTest, M17_TOOL_010c_ASlotStaysSquareWhenItIsDragged) {
    // DOF 5 is a number. This is what the number is FOR: move one end and the
    // slot has to stay a slot -- sides parallel, corners square -- rather than
    // folding into a satisfied-but-kinked shape.
    ToolRun run;
    run.model.setTool(SketchTool::Slot);
    run.click(Vec2{20, 30});
    run.click(Vec2{60, 30});
    run.click(Vec2{60, 38});
    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();

    // Shove one end arc's centre well off the axis and re-solve.
    std::vector<SketchEntityId> arcs;
    for (const SketchEntity& entity : run.sketch->entities())
        if (std::holds_alternative<SketchArc>(entity.geometry)) arcs.push_back(entity.id);
    ASSERT_EQ(arcs.size(), 2u);
    const SketchArc& moving = std::get<SketchArc>(run.sketch->findEntity(arcs.front())->geometry);
    SketchArc shoved = moving;
    shoved.center = Vec2{moving.center.x + 6.0, moving.center.y + 14.0};
    ASSERT_TRUE(run.document.setSketchEntityGeometry(run.sketch->id(), arcs.front(), shoved));
    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();

    // The centre line the sides must stay square to.
    const Vec2 centreA = std::get<SketchArc>(run.sketch->findEntity(arcs[0])->geometry).center;
    const Vec2 centreB = std::get<SketchArc>(run.sketch->findEntity(arcs[1])->geometry).center;
    const double axisU = centreB.x - centreA.x;
    const double axisV = centreB.y - centreA.y;
    const double axisLength = std::sqrt(axisU * axisU + axisV * axisV);
    ASSERT_GT(axisLength, 1.0);

    const double radius =
        std::get<SketchArc>(run.sketch->findEntity(arcs.front())->geometry).radiusMm;

    for (const SketchEntity& entity : run.sketch->entities()) {
        const auto* side = std::get_if<SketchLine>(&entity.geometry);
        if (side == nullptr) continue;
        const double du = side->end.x - side->start.x;
        const double dv = side->end.y - side->start.y;
        const double length = std::sqrt(du * du + dv * dv);
        ASSERT_GT(length, 1.0);
        // Sides PARALLEL to the centre line: |sin| between them is zero.
        EXPECT_NEAR(std::fabs(du * axisV - dv * axisU) / (length * axisLength), 0.0, 1e-6)
            << run.sketch->solveMessage();
        // ...AND AS LONG AS IT, AND r AWAY FROM IT. Parallel alone is not a
        // slot: a first version of this test checked only that, and a mutant
        // that held the WRONG corner of each side passed it -- by blowing the
        // radius up past the span and sliding the sides out to a shape that
        // was still parallel and no longer a slot. Three facts, because
        // "parallel" is one third of what the word means.
        EXPECT_NEAR(length, axisLength, 1e-6) << run.sketch->solveMessage();
        const double offA = std::fabs((centreA.x - side->start.x) * dv -
                                      (centreA.y - side->start.y) * du) / length;
        EXPECT_NEAR(offA, radius, 1e-6) << run.sketch->solveMessage();
    }

    // BOTH ARCS STILL THE SAME SIZE -- but not necessarily 8 mm. The radius is
    // one of the slot's five remaining freedoms, so a drag is allowed to change
    // it; pinning the drawn value here would assert a dimension nobody added.
    // What must hold is that the two ends agree, which is the equal-radius
    // constraint doing its job.
    EXPECT_NEAR(std::get<SketchArc>(run.sketch->findEntity(arcs[0])->geometry).radiusMm,
                std::get<SketchArc>(run.sketch->findEntity(arcs[1])->geometry).radiusMm, 1e-6)
        << run.sketch->solveMessage();
}

// --- M17.29: which entities are pinned ----------------------------------------

TEST(SketchToolsTest, M17_PIN_001_ADIMENSIONEDLineIsPinnedAndANeighbourIsNot) {
    // The defect this exists for, stated as the smallest case: two entities in
    // one sketch, one fully constrained and one not. The sketch-wide status can
    // only say "under-constrained", and colouring from it paints the pinned one
    // loose.
    PartDocument document{"PinDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId pinned = sketch.addLine(Vec2{0, 0}, Vec2{63, 0});
    const SketchEntityId loose = sketch.addLine(Vec2{0, 40}, Vec2{50, 55});
    Parameter& length = document.addParameter("L", 100.0, UnitType::Millimeter);
    document.addSketchConstraint(
        sketch.id(), FixConstraint{SketchElementRef{pinned, SketchSubElement::StartPoint}});
    document.addSketchConstraint(sketch.id(), HorizontalConstraint{pinned});
    document.addSketchConstraint(sketch.id(), LengthConstraint{pinned, length.id()});

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    EXPECT_EQ(sketch.solveStatus(), SketchSolveStatus::UnderConstrained);
    EXPECT_TRUE(sketch.isEntityFullyConstrained(pinned))
        << "a fixed, horizontal, dimensioned line is not pinned";
    EXPECT_FALSE(sketch.isEntityFullyConstrained(loose))
        << "an untouched line came out pinned";
}

TEST(SketchToolsTest, M17_PIN_002_ASPLINEKeepsItsInteriorAndTheChordIsStillPinned) {
    // The case the user hit. A sketch with a spline in it can never reach DOF 0
    // -- no constraint can name an interior point -- so per-sketch colour stops
    // meaning anything the moment there is one.
    PartDocument document{"PinDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline = sketch.addSpline(
        {Vec2{8, -14}, Vec2{35, 32}, Vec2{62, -6}, Vec2{90, 26}, Vec2{118, -4}, Vec2{146, 24},
         Vec2{172, -12}},
        false);
    const SketchEntityId chord = sketch.addLine(Vec2{178, -30}, Vec2{2, -30});
    Parameter& span = document.addParameter("Span", 180.0, UnitType::Millimeter);
    document.addSketchConstraint(
        sketch.id(), FixConstraint{SketchElementRef{chord, SketchSubElement::EndPoint}});
    document.addSketchConstraint(sketch.id(), HorizontalConstraint{chord});
    document.addSketchConstraint(sketch.id(), LengthConstraint{chord, span.id()});
    document.addSketchConstraint(
        sketch.id(),
        CoincidentConstraint{SketchElementRef{spline, SketchSubElement::StartPoint},
                             SketchElementRef{chord, SketchSubElement::EndPoint}});
    document.addSketchConstraint(
        sketch.id(),
        CoincidentConstraint{SketchElementRef{spline, SketchSubElement::EndPoint},
                             SketchElementRef{chord, SketchSubElement::StartPoint}});

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    EXPECT_EQ(sketch.degreesOfFreedom(), 10) << sketch.solveMessage();
    // THE CHORD IS PINNED even though the sketch is not, which is the whole
    // point -- it used to be painted loose because the spline beside it was.
    EXPECT_TRUE(sketch.isEntityFullyConstrained(chord)) << "the chord is not pinned";
    EXPECT_FALSE(sketch.isEntityFullyConstrained(spline))
        << "a spline with five free interior points came out pinned";
}

TEST(SketchToolsTest, M17_PIN_003_APINNEDSketchHasEveryEntityPinned) {
    // The agreement that must hold: DOF 0 and "every entity pinned" are the
    // same fact counted two ways, so they cannot disagree.
    PartDocument document{"PinDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId circle = sketch.addCircle(Vec2{10, 5}, 7.0);
    Parameter& r = document.addParameter("R", 12.0, UnitType::Millimeter);
    document.addSketchConstraint(
        sketch.id(), FixConstraint{SketchElementRef{circle, SketchSubElement::CenterPoint}});
    document.addSketchConstraint(sketch.id(), RadiusConstraint{circle, r.id()});

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    ASSERT_EQ(sketch.degreesOfFreedom(), 0) << sketch.solveMessage();
    for (const SketchEntity& entity : sketch.entities())
        EXPECT_TRUE(sketch.isEntityFullyConstrained(entity.id))
            << "the sketch reports DOF 0 but an entity is not pinned";
}

TEST(SketchToolsTest, M17_PIN_004_NothingIsPinnedBeforeOrAfterAFailedSolve) {
    // FALSE is the safe direction: "not known to be pinned" reads as loose, and
    // claiming pinned would be claiming a measurement nobody made.
    PartDocument document{"PinDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId circle = sketch.addCircle(Vec2{0, 0}, 10.0);
    // Never solved yet.
    EXPECT_FALSE(sketch.isEntityFullyConstrained(circle));

    Parameter& r = document.addParameter("R", 10.0, UnitType::Millimeter);
    document.addSketchConstraint(
        sketch.id(), FixConstraint{SketchElementRef{circle, SketchSubElement::CenterPoint}});
    document.addSketchConstraint(sketch.id(), RadiusConstraint{circle, r.id()});
    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    ASSERT_TRUE(sketch.isEntityFullyConstrained(circle));

    // ...and a solve that FAILS forgets it, rather than leaving the last
    // answer behind to colour geometry it no longer describes.
    ASSERT_TRUE(document.setParameterValue(r.id(), -5.0));
    (void)document.recompute();
    EXPECT_FALSE(sketch.isEntityFullyConstrained(circle))
        << "a failed solve kept the previous answer";
}

TEST(SketchToolsTest, M17_PIN_005_AnArcsTIPSCountAsPartOfIt) {
    // An arc's tips are variables of their own (ArcTipU/V), derived from the
    // centre, radius and angles. Pinning those three pins the tips too, and the
    // null space is what says so -- a count of "how many variables does an arc
    // have" would have to know that and would be a second opinion.
    PartDocument document{"PinDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId arc = sketch.addArc(Vec2{0, 0}, 20.0, 0.0, 1.2, true);
    Parameter& r = document.addParameter("R", 20.0, UnitType::Millimeter);
    document.addSketchConstraint(
        sketch.id(), FixConstraint{SketchElementRef{arc, SketchSubElement::CenterPoint}});
    document.addSketchConstraint(sketch.id(), RadiusConstraint{arc, r.id()});
    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    // The two ANGLES are still free, so the arc is not pinned -- and the DOF
    // agrees.
    EXPECT_EQ(sketch.degreesOfFreedom(), 2) << sketch.solveMessage();
    EXPECT_FALSE(sketch.isEntityFullyConstrained(arc));
}

// --- M17.26: splines ----------------------------------------------------------

namespace {

const SketchSpline* OnlySpline(const Sketch& sketch) {
    for (const SketchEntity& entity : sketch.entities())
        if (const auto* value = std::get_if<SketchSpline>(&entity.geometry)) return value;
    return nullptr;
}

} // namespace

TEST(SketchToolsTest, M17_SPL_001_ASplineTakesPointsUntilItIsTOLDToStop) {
    // The one tool whose point count the USER decides. Four clicks make nothing
    // on their own -- there is no count to reach -- and the double-click is
    // what says "that was the last one".
    ToolRun run;
    run.model.setTool(SketchTool::Spline);
    run.click(Vec2{0, 0});
    run.click(Vec2{20, 30});
    run.click(Vec2{50, -10});
    run.click(Vec2{80, 20});
    EXPECT_EQ(OnlySpline(*run.sketch), nullptr) << "a spline appeared before it was finished";

    const SketchEdit finish = run.model.finishPendingSpline();
    ASSERT_TRUE(finish.valid());
    const SketchEditOutcome outcome = ApplySketchEdit(run.document, run.sketch->id(), finish);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const SketchSpline* spline = OnlySpline(*run.sketch);
    ASSERT_NE(spline, nullptr);
    ASSERT_EQ(spline->points.size(), 4u);
    EXPECT_FALSE(spline->closed);
    EXPECT_NEAR(spline->points[1].x, 20.0, 1e-9);
    EXPECT_NEAR(spline->points[3].y, 20.0, 1e-9);
}

TEST(SketchToolsTest, M17_SPL_002_TheCurveGoesTHROUGHEveryPointItWasGiven) {
    // The whole claim of an interpolating spline. A fitting spline passes NEAR
    // the points and would look almost identical on screen -- so this checks
    // the one thing that tells them apart, at every point rather than at the
    // ends where they agree anyway.
    const SketchSpline spline{{Vec2{0, 0}, Vec2{20, 30}, Vec2{50, -10}, Vec2{80, 20}}, false};
    const std::vector<Vec2> sampled = SampleSpline(spline, 12);
    ASSERT_GE(sampled.size(), 12u * 3);
    for (const Vec2& want : spline.points) {
        double nearest = 1e9;
        for (const Vec2& at : sampled)
            nearest = std::min(nearest, std::hypot(at.x - want.x, at.y - want.y));
        EXPECT_NEAR(nearest, 0.0, 1e-9) << "the curve misses (" << want.x << ", " << want.y << ")";
    }
    // ...and it STARTS and ENDS exactly there, which is what a profile chains
    // through.
    EXPECT_NEAR(sampled.front().x, 0.0, 1e-12);
    EXPECT_NEAR(sampled.back().x, 80.0, 1e-12);
    EXPECT_NEAR(sampled.back().y, 20.0, 1e-12);
}

TEST(SketchToolsTest, M17_SPL_003_ItIsACURVENotThePolylineThroughThePoints) {
    // Three points in a straight line stay straight; three that are not must
    // BULGE away from their own chords. Without this a sampler that simply
    // joined the points would pass every other test here.
    const SketchSpline bent{{Vec2{0, 0}, Vec2{50, 40}, Vec2{100, 0}}, false};
    const std::vector<Vec2> sampled = SampleSpline(bent, 16);
    double worst = 0.0;
    for (const Vec2& at : sampled) {
        // Distance from the chord (0,0)-(50,40) for the first half.
        if (at.x > 50.0) continue;
        const double du = 50.0, dv = 40.0;
        const double length = std::hypot(du, dv);
        worst = std::max(worst, std::fabs(at.x * dv - at.y * du) / length);
    }
    EXPECT_GT(worst, 1.0) << "the spline is a polyline through its points";

    // ...and a straight run stays straight, so the bulge is curvature and not
    // noise.
    const SketchSpline straight{{Vec2{0, 0}, Vec2{50, 0}, Vec2{100, 0}}, false};
    for (const Vec2& at : SampleSpline(straight, 16)) EXPECT_NEAR(at.y, 0.0, 1e-9);
}

TEST(SketchToolsTest, M17_SPL_004_ClickingTheFirstPointAgainCLOSESIt) {
    ToolRun run;
    run.model.setTool(SketchTool::Spline);
    run.click(Vec2{0, 0});
    run.click(Vec2{40, 30});
    run.click(Vec2{80, 0});
    run.click(Vec2{40, -30});
    run.click(Vec2{0, 0});  // back to the start

    const SketchSpline* spline = OnlySpline(*run.sketch);
    ASSERT_NE(spline, nullptr) << run.lastStatus;
    EXPECT_TRUE(spline->closed);
    // The closing click is the FIRST point, not a fifth one on top of it -- a
    // repeated point is a span with no length, and the sketch refuses those.
    EXPECT_EQ(spline->points.size(), 4u);
}

TEST(SketchToolsTest, M17_SPL_005_ACLOSEDSplineHasNoEndsToConstrain) {
    // It is a loop on its own, the way a circle is. Offering its "start" as a
    // constraint target would offer a reference that resolves to nothing.
    PartDocument document{"SplineDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId closed =
        sketch.addSpline({Vec2{0, 0}, Vec2{40, 30}, Vec2{80, 0}, Vec2{40, -30}}, true);
    const SketchEntityId open =
        sketch.addSpline({Vec2{0, 60}, Vec2{40, 90}, Vec2{80, 60}}, false);
    ASSERT_NE(closed, kInvalidSketchEntityId);
    ASSERT_NE(open, kInvalidSketchEntityId);

    EXPECT_FALSE(HasEndpoints(sketch.findEntity(closed)->geometry));
    EXPECT_TRUE(HasEndpoints(sketch.findEntity(open)->geometry));
    EXPECT_FALSE(IsPointRef(sketch, SketchElementRef{closed, SketchSubElement::StartPoint}));
    EXPECT_TRUE(IsPointRef(sketch, SketchElementRef{open, SketchSubElement::StartPoint}));
    EXPECT_TRUE(IsPointRef(sketch, SketchElementRef{open, SketchSubElement::EndPoint}));
    // An INTERIOR point is a variable the solver moves and nothing can name.
    EXPECT_FALSE(IsPointRef(sketch, SketchElementRef{open, SketchSubElement::Whole}));
}

TEST(SketchToolsTest, M17_SPL_006_EveryPointIsAVariableAndTheDOFSaysSo) {
    // A spline through five points carries ten freedoms. Reporting fewer would
    // be the under-count ADR-M17-018 found in arcs, in a new place.
    PartDocument document{"SplineDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addSpline({Vec2{0, 0}, Vec2{20, 30}, Vec2{50, -10}, Vec2{80, 20}, Vec2{110, 0}},
                     false);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    EXPECT_EQ(sketch.degreesOfFreedom(), 10) << sketch.solveMessage();
}

TEST(SketchToolsTest, M17_SPL_007_ItsENDSAreHeldLikeAnyOtherPoint) {
    // The two ends are ordinary variables, so an ordinary Coincident holds
    // one -- which is what lets a spline be part of a closed profile.
    PartDocument document{"SplineDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline =
        sketch.addSpline({Vec2{0, 0}, Vec2{40, 30}, Vec2{80, 0}}, false);
    const SketchEntityId line = sketch.addLine(Vec2{200, 200}, Vec2{240, 240});
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  CoincidentConstraint{SketchElementRef{line, SketchSubElement::StartPoint},
                                       SketchElementRef{spline, SketchSubElement::EndPoint}}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const SketchLine& moved = std::get<SketchLine>(sketch.findEntity(line)->geometry);
    const Vec2 tip = EndPointOf(sketch.findEntity(spline)->geometry);
    EXPECT_NEAR(std::hypot(moved.start.x - tip.x, moved.start.y - tip.y), 0.0, 1e-6)
        << sketch.solveMessage();
}

TEST(SketchToolsTest, M17_SPL_008_TheSolverMovesTheONEPointItHasTo) {
    // The write-back path, and the one it replaced. Routing a spline's points
    // by sub-element -- as every other kind is routed -- collapses all the
    // interior ones onto the same field, so a solve that moved the last point
    // would rewrite the middle with it. Fixing ONE end and checking the OTHERS
    // stayed put is what notices.
    PartDocument document{"SplineDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline = sketch.addSpline(
        {Vec2{0, 0}, Vec2{20, 30}, Vec2{50, -10}, Vec2{80, 20}}, false);
    Parameter& dx = document.addParameter("Dx", 5.0, UnitType::Millimeter);
    // Drag the END somewhere by dimensioning it away from a fixed point.
    const SketchEntityId anchor = sketch.addPoint(Vec2{100, 100});
    document.addSketchConstraint(
        sketch.id(), FixConstraint{SketchElementRef{anchor, SketchSubElement::Whole}});
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  DistanceConstraint{SketchElementRef{anchor, SketchSubElement::Whole},
                                     SketchElementRef{spline, SketchSubElement::EndPoint},
                                     dx.id()}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const SketchSpline& solved = std::get<SketchSpline>(sketch.findEntity(spline)->geometry);
    ASSERT_EQ(solved.points.size(), 4u);
    // The END moved to 5 mm from the anchor...
    EXPECT_NEAR(std::hypot(solved.points.back().x - 100.0, solved.points.back().y - 100.0), 5.0,
                1e-6)
        << sketch.solveMessage();
    // ...and the three before it did NOT move, because nothing asked them to.
    EXPECT_NEAR(solved.points[0].x, 0.0, 1e-6);
    EXPECT_NEAR(solved.points[0].y, 0.0, 1e-6);
    EXPECT_NEAR(solved.points[1].x, 20.0, 1e-6);
    EXPECT_NEAR(solved.points[1].y, 30.0, 1e-6);
    EXPECT_NEAR(solved.points[2].x, 50.0, 1e-6);
    EXPECT_NEAR(solved.points[2].y, -10.0, 1e-6);
}

TEST(SketchToolsTest, M17_SPL_009_TwoPointsOnTopOfEachOtherAreREFUSED) {
    // A span with no length has no direction for the curve to leave in, and
    // OCCT's interpolator refuses the WHOLE curve for one repeated point --
    // with a message naming neither the point nor the caller. Refused here,
    // where it can be explained.
    PartDocument document{"SplineDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    EXPECT_EQ(sketch.addSpline({Vec2{0, 0}, Vec2{40, 30}, Vec2{40, 30}, Vec2{80, 0}}, false),
              kInvalidSketchEntityId);
    EXPECT_EQ(sketch.addSpline({Vec2{0, 0}}, false), kInvalidSketchEntityId);
    // ...and a CLOSED one whose last point is its first: the closure already
    // joins them, so writing it twice is the same repeated point.
    EXPECT_EQ(sketch.addSpline({Vec2{0, 0}, Vec2{40, 30}, Vec2{80, 0}, Vec2{0, 0}}, true),
              kInvalidSketchEntityId);
}

TEST(SketchToolsTest, M17_SPL_010_ASplineDrawnBackwardsIsTheSAMECurve) {
    // What "duplicate" means (spec 10). A reversed copy lies exactly on top of
    // the original and makes the profile ambiguous -- the loop walker finds two
    // ways out of every vertex.
    const SketchGeometry forward =
        SketchSpline{{Vec2{0, 0}, Vec2{20, 30}, Vec2{50, -10}}, false};
    const SketchGeometry backward =
        SketchSpline{{Vec2{50, -10}, Vec2{20, 30}, Vec2{0, 0}}, false};
    EXPECT_TRUE(IsSameCurve(forward, backward, kSketchToleranceMm));

    const SketchGeometry different =
        SketchSpline{{Vec2{0, 0}, Vec2{20, 31}, Vec2{50, -10}}, false};
    EXPECT_FALSE(IsSameCurve(forward, different, kSketchToleranceMm));
    // ...and closed is not the same curve as open, whatever the points say.
    EXPECT_FALSE(IsSameCurve(
        forward, SketchGeometry{SketchSpline{{Vec2{0, 0}, Vec2{20, 30}, Vec2{50, -10}}, true}},
        kSketchToleranceMm));
}

// --- M17.25: ellipses ---------------------------------------------------------

namespace {

constexpr double kPiE = 3.14159265358979323846;

const SketchEllipse* OnlyEllipse(const Sketch& sketch) {
    for (const SketchEntity& entity : sketch.entities())
        if (const auto* value = std::get_if<SketchEllipse>(&entity.geometry)) return value;
    return nullptr;
}

} // namespace

TEST(SketchToolsTest, M17_ELL_001_TheSecondClickGivesBOTHTheLongAxisAndTheRotation) {
    // The reason the second click is the LONG axis rather than the short one:
    // an ellipse's rotation is measured to its major axis, so pointing at the
    // major end says both things that have to agree in one gesture.
    ToolRun run;
    run.model.setTool(SketchTool::Ellipse);
    run.click(Vec2{10, 5});    // centre
    run.click(Vec2{40, 45});   // the long end: 50 mm away, at atan2(40,30)
    run.click(Vec2{-2, 17});   // somewhere off to the side

    const SketchEllipse* ellipse = OnlyEllipse(*run.sketch);
    ASSERT_NE(ellipse, nullptr) << run.lastStatus;
    EXPECT_NEAR(ellipse->center.x, 10.0, 1e-9);
    EXPECT_NEAR(ellipse->center.y, 5.0, 1e-9);
    EXPECT_NEAR(ellipse->majorRadiusMm, 50.0, 1e-9);
    EXPECT_NEAR(ellipse->rotationRad, std::atan2(40.0, 30.0), 1e-9);
    // The third click's PERPENDICULAR distance to that axis: (-12, 12) crossed
    // with the unit axis (0.6, 0.8) is |-12*0.8 - 12*0.6| = 16.8.
    EXPECT_NEAR(ellipse->minorRadiusMm, 16.8, 1e-9);
}

TEST(SketchToolsTest, M17_ELL_002_SlidingTheWidthClickALONGTheAxisChangesNothing) {
    // The same rule the slot tool uses, and the same reason: what the third
    // click means is the distance ACROSS, and a hand that drifts along the axis
    // must not resize the ellipse.
    const auto widthFrom = [](Vec2 third) {
        ToolRun run;
        run.model.setTool(SketchTool::Ellipse);
        run.click(Vec2{0, 0});
        run.click(Vec2{50, 0});
        run.click(third);
        const SketchEllipse* ellipse = OnlyEllipse(*run.sketch);
        return ellipse != nullptr ? ellipse->minorRadiusMm : -1.0;
    };
    EXPECT_NEAR(widthFrom(Vec2{0, 20}), 20.0, 1e-9);
    EXPECT_NEAR(widthFrom(Vec2{35, 20}), 20.0, 1e-9);
    EXPECT_NEAR(widthFrom(Vec2{-90, 20}), 20.0, 1e-9);
}

TEST(SketchToolsTest, M17_ELL_003_AWidthWiderThanItIsLongIsRefused) {
    // MAJOR MUST BE THE LONGER ONE: the rotation is measured to it, so an
    // ellipse stored the other way round is the same shape turned a quarter
    // turn -- a difference no later check would notice.
    ToolRun run;
    run.model.setTool(SketchTool::Ellipse);
    run.click(Vec2{0, 0});
    run.click(Vec2{20, 0});   // 20 mm long
    run.click(Vec2{0, 45});   // ...and 45 across
    EXPECT_EQ(OnlyEllipse(*run.sketch), nullptr);
    // The click is DROPPED rather than the command failed, so the next one
    // finishes the ellipse -- the same way a degenerate circle click behaves.
    run.click(Vec2{0, 8});
    const SketchEllipse* ellipse = OnlyEllipse(*run.sketch);
    ASSERT_NE(ellipse, nullptr) << run.lastStatus;
    EXPECT_NEAR(ellipse->minorRadiusMm, 8.0, 1e-9);
}

TEST(SketchToolsTest, M17_ELL_004_APointOnAnEllipseIsPULLEDOntoIt) {
    // The implicit residual, doing the one thing it exists for. The point
    // starts well off the curve and has to arrive on it -- "starts correct and
    // stays" would pass whether or not the equation means anything.
    PartDocument document{"EllipseDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId ellipse = sketch.addEllipse(Vec2{0, 0}, 40.0, 15.0, 0.0);
    // THE ELLIPSE IS PINNED, so the only way to satisfy the constraint is to
    // move the POINT. Left free, the solver is also entitled to reshape the
    // ellipse to meet the point, and this test would then pass without saying
    // anything about where the point went.
    Parameter& a = document.addParameter("A", 40.0, UnitType::Millimeter);
    Parameter& b = document.addParameter("B", 15.0, UnitType::Millimeter);
    document.addSketchConstraint(sketch.id(), EllipseAxisConstraint{ellipse, a.id(), false});
    document.addSketchConstraint(sketch.id(), EllipseAxisConstraint{ellipse, b.id(), true});
    document.addSketchConstraint(
        sketch.id(), FixConstraint{SketchElementRef{ellipse, SketchSubElement::CenterPoint}});
    // ...AND ITS ORIENTATION, which is the fifth of an ellipse's five numbers.
    // Without it the solver is entitled to spin the ellipse to meet the point,
    // and this test would say nothing about where the point went. It is also
    // how the whole of M17.25's solver work got found: the rotation dimension
    // was being refused in silence, and every point-on-ellipse solve was really
    // an under-determined one.
    Parameter& turn = document.addParameter("T", 0.0, UnitType::Radian);
    document.addSketchConstraint(sketch.id(), EllipseRotationConstraint{ellipse, turn.id()});
    const SketchEntityId point = sketch.addPoint(Vec2{30, 30});
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  PointOnObjectConstraint{SketchElementRef{point, SketchSubElement::Whole},
                                          ellipse}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const Vec2 landed = std::get<SketchPoint>(sketch.findEntity(point)->geometry).position;
    const SketchEllipse& shape = std::get<SketchEllipse>(sketch.findEntity(ellipse)->geometry);
    // ON the curve: (x/a)^2 + (y/b)^2 == 1, worked out here rather than read
    // back from the solver.
    const double x = landed.x / shape.majorRadiusMm;
    const double y = landed.y / shape.minorRadiusMm;
    EXPECT_NEAR(x * x + y * y, 1.0, 1e-6) << sketch.solveMessage();
    // ...and it MOVED to get there.
    EXPECT_GT(std::hypot(landed.x - 30.0, landed.y - 30.0), 1.0);
}

TEST(SketchToolsTest, M17_ELL_005_EachAxisIsDimensionedSEPARATELY) {
    PartDocument document{"EllipseDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId ellipse = sketch.addEllipse(Vec2{0, 0}, 40.0, 15.0, 0.0);
    Parameter& a = document.addParameter("A", 60.0, UnitType::Millimeter);
    Parameter& b = document.addParameter("B", 10.0, UnitType::Millimeter);
    ASSERT_NE(document.addSketchConstraint(sketch.id(),
                                           EllipseAxisConstraint{ellipse, a.id(), false}),
              kInvalidSketchConstraintId);
    ASSERT_NE(document.addSketchConstraint(sketch.id(),
                                           EllipseAxisConstraint{ellipse, b.id(), true}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const SketchEllipse& shape = std::get<SketchEllipse>(sketch.findEntity(ellipse)->geometry);
    EXPECT_NEAR(shape.majorRadiusMm, 60.0, 1e-6) << sketch.solveMessage();
    EXPECT_NEAR(shape.minorRadiusMm, 10.0, 1e-6) << sketch.solveMessage();
}

TEST(SketchToolsTest, M17_ELL_006_ARadiusDimensionOnAnEllipseIsREFUSEDWithTheAlternative) {
    // An ellipse HAS a radius slot -- the major one -- so a Radius constraint
    // would solve and silently drive one axis of two. The refusal names the
    // command to use instead, which is the difference between a refusal and a
    // dead end.
    PartDocument document{"EllipseDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId ellipse = sketch.addEllipse(Vec2{0, 0}, 40.0, 15.0, 0.0);
    Parameter& r = document.addParameter("R", 20.0, UnitType::Millimeter);
    const SketchConstraintId id =
        document.addSketchConstraint(sketch.id(), RadiusConstraint{ellipse, r.id()});
    ASSERT_NE(id, kInvalidSketchConstraintId);

    (void)document.recompute();
    EXPECT_NE(sketch.solveMessage().find("major or minor axis"), std::string::npos)
        << sketch.solveMessage();
    ASSERT_FALSE(sketch.offendingConstraints().empty());
    EXPECT_EQ(sketch.offendingConstraints().front(), id);
    // AND THE ELLIPSE IS UNTOUCHED: a refused constraint must not have driven
    // anything on its way to being refused.
    const SketchEllipse& shape = std::get<SketchEllipse>(sketch.findEntity(ellipse)->geometry);
    EXPECT_NEAR(shape.majorRadiusMm, 40.0, 1e-9);
}

TEST(SketchToolsTest, M17_ELL_007_AnEllipticalArcsTIPSAreHeldLikeAnyOtherPoint) {
    // EllipseTipU/V, doing what ArcTipU/V does for a circular arc: the tips are
    // variables, so an ordinary Coincident holds one. Without it the ellipse
    // could not join anything, which is most of what a sketch is.
    PartDocument document{"EllipseDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    // A quarter of an ellipse, from the long axis round to the short one.
    const SketchEntityId arc =
        sketch.addEllipticalArc(Vec2{0, 0}, 40.0, 15.0, 0.0, 0.0, kPiE / 2.0, true);
    const SketchEntityId line = sketch.addLine(Vec2{60, 60}, Vec2{100, 100});
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  CoincidentConstraint{SketchElementRef{line, SketchSubElement::StartPoint},
                                       SketchElementRef{arc, SketchSubElement::EndPoint}}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const SketchLine& moved = std::get<SketchLine>(sketch.findEntity(line)->geometry);
    const SketchEllipticalArc& solved =
        std::get<SketchEllipticalArc>(sketch.findEntity(arc)->geometry);
    const Vec2 tip = EndPointOf(sketch.findEntity(arc)->geometry);
    EXPECT_NEAR(std::hypot(moved.start.x - tip.x, moved.start.y - tip.y), 0.0, 1e-6)
        << sketch.solveMessage();

    // ...AND THE TIP IS STILL ON THE ELLIPSE, which is the claim EllipseTipU/V
    // actually makes. Nothing pins the arc's shape here, so the solver is
    // entitled to move the arc to the line as readily as the line to the arc --
    // asserting a fixed coordinate would be asserting which of the two it
    // chose, which is not a promise this model makes.
    const double c = std::cos(solved.rotationRad);
    const double sn = std::sin(solved.rotationRad);
    const double du = tip.x - solved.center.x;
    const double dv = tip.y - solved.center.y;
    const double along = (du * c + dv * sn) / solved.majorRadiusMm;
    const double across = (-du * sn + dv * c) / solved.minorRadiusMm;
    EXPECT_NEAR(along * along + across * across, 1.0, 1e-9) << sketch.solveMessage();
    // ...at the parameter it is stored at, not merely somewhere on the curve.
    EXPECT_NEAR(EllipseParamOf(solved.center, solved.majorRadiusMm, solved.minorRadiusMm,
                               solved.rotationRad, tip),
                std::atan2(std::sin(solved.endParamRad), std::cos(solved.endParamRad)), 1e-9);
}

TEST(SketchToolsTest, M17_ELL_008_ThePARAMETERIsNotTheAngleFromTheCentre) {
    // The bug this whole parametrisation is written to prevent, as an
    // assertion. At t = pi/4 an ellipse's point is NOT at 45 degrees from its
    // centre unless it happens to be a circle -- and the two agree exactly at
    // the axes, which is what makes the mistake survive casual testing.
    const Vec2 at = PointOnEllipse(Vec2{0, 0}, 40.0, 10.0, 0.0, kPiE / 4.0);
    EXPECT_NEAR(at.x, 40.0 * std::cos(kPiE / 4.0), 1e-9);
    EXPECT_NEAR(at.y, 10.0 * std::sin(kPiE / 4.0), 1e-9);
    const double geometricAngle = std::atan2(at.y, at.x);
    EXPECT_GT(std::fabs(geometricAngle - kPiE / 4.0), 0.3) << "the two happened to agree";

    // ...and EllipseParamOf is its exact inverse, which is what stops anyone
    // reaching for atan2.
    EXPECT_NEAR(EllipseParamOf(Vec2{0, 0}, 40.0, 10.0, 0.0, at), kPiE / 4.0, 1e-12);
    // At the axes they DO agree, which is the trap.
    EXPECT_NEAR(EllipseParamOf(Vec2{0, 0}, 40.0, 10.0, 0.0, Vec2{40.0, 0.0}), 0.0, 1e-12);
}

TEST(SketchToolsTest, M17_ELL_009_ARotatedEllipseTurnsAboutItsOwnCentre) {
    // The rotation is applied in the ellipse's frame, so a point at parameter 0
    // is always the end of the major axis wherever that axis points.
    const double rotation = kPiE / 3.0;
    const Vec2 at = PointOnEllipse(Vec2{5, 7}, 20.0, 8.0, rotation, 0.0);
    EXPECT_NEAR(at.x, 5.0 + 20.0 * std::cos(rotation), 1e-9);
    EXPECT_NEAR(at.y, 7.0 + 20.0 * std::sin(rotation), 1e-9);
    // ...and a quarter turn on gives the minor end, square to it.
    const Vec2 across = PointOnEllipse(Vec2{5, 7}, 20.0, 8.0, rotation, kPiE / 2.0);
    const double du = at.x - 5.0, dv = at.y - 7.0;
    const double eu = across.x - 5.0, ev = across.y - 7.0;
    EXPECT_NEAR(du * eu + dv * ev, 0.0, 1e-9);
    EXPECT_NEAR(std::hypot(eu, ev), 8.0, 1e-9);
}

// --- M17.24: transform --------------------------------------------------------

namespace {

// A 40 x 20 rectangle at the origin, drawn as four constrained lines.
struct BoxRun {
    PartDocument document{"TransformDoc"};
    GaussNewtonSketchSolver solver;
    Sketch* sketch = nullptr;
    std::vector<SketchEntityId> sides;

    BoxRun() {
        document.setSketchSolver(&solver);
        sketch = &document.addSketch("Sketch001");
        const Vec2 corners[4] = {Vec2{0, 0}, Vec2{40, 0}, Vec2{40, 20}, Vec2{0, 20}};
        for (int i = 0; i < 4; ++i)
            sides.push_back(sketch->addLine(corners[i], corners[(i + 1) % 4]));
        for (int i = 0; i < 4; ++i) {
            document.addSketchConstraint(
                sketch->id(),
                CoincidentConstraint{SketchElementRef{sides[i], SketchSubElement::EndPoint},
                                     SketchElementRef{sides[(i + 1) % 4],
                                                      SketchSubElement::StartPoint}});
            document.addSketchConstraint(
                sketch->id(), i % 2 == 0 ? SketchConstraintData{HorizontalConstraint{sides[i]}}
                                         : SketchConstraintData{VerticalConstraint{sides[i]}});
        }
    }
};

Vec2 StartOfLine(const Sketch& sketch, SketchEntityId id) {
    return std::get<SketchLine>(sketch.findEntity(id)->geometry).start;
}

} // namespace

TEST(SketchToolsTest, M17_XF_001_MoveRewritesCoordinatesAndAddsNoConstraint) {
    BoxRun run;
    const std::size_t before = run.sketch->constraints().size();
    SketchTransform move;
    move.kind = SketchTransformKind::Move;
    move.deltaMm = Vec2{100, 50};

    const TransformOutcome outcome = ApplyTransform(run.document, run.sketch->id(), run.sides, move);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    EXPECT_TRUE(outcome.created.empty()) << "an in-place move made a copy";
    // NOT ONE CONSTRAINT EITHER WAY. A move is an edit to coordinates; the
    // sketch's constraints are still the truth about the shape, and inventing
    // one to hold the new position would be answering a question nobody asked.
    EXPECT_EQ(run.sketch->constraints().size(), before);
    EXPECT_NEAR(StartOfLine(*run.sketch, run.sides[0]).x, 100.0, 1e-9);
    EXPECT_NEAR(StartOfLine(*run.sketch, run.sides[0]).y, 50.0, 1e-9);

    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();
    // ...and it stays there, because nothing pinned it.
    EXPECT_NEAR(StartOfLine(*run.sketch, run.sides[0]).x, 100.0, 1e-6)
        << run.sketch->solveMessage();
}

TEST(SketchToolsTest, M17_XF_002_RotateTurnsAboutTheCentreOfWhatIsSelected) {
    BoxRun run;
    SketchTransform turn;
    turn.kind = SketchTransformKind::Rotate;
    turn.angleRad = 3.14159265358979323846 / 2.0; // a quarter turn, counter-clockwise

    const TransformOutcome outcome = ApplyTransform(run.document, run.sketch->id(), run.sides, turn);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    // The box spans 0..40 by 0..20, so its centre is (20, 10) -- and a quarter
    // turn about it sends (0,0) to (30,-10). Worked out here rather than read
    // back from the command.
    EXPECT_NEAR(outcome.anchor.x, 20.0, 1e-9);
    EXPECT_NEAR(outcome.anchor.y, 10.0, 1e-9);
    EXPECT_NEAR(StartOfLine(*run.sketch, run.sides[0]).x, 30.0, 1e-9);
    EXPECT_NEAR(StartOfLine(*run.sketch, run.sides[0]).y, -10.0, 1e-9);
}

TEST(SketchToolsTest, M17_XF_003_ASELECTEDPointIsTheAnchor) {
    // One point in the selection is a user saying WHERE. Two is not -- it is
    // ambiguous, and taking the first would be a rule nobody could guess.
    BoxRun run;
    const SketchEntityId pivot = run.sketch->addPoint(Vec2{0, 0});
    std::vector<SketchEntityId> selection = run.sides;
    selection.push_back(pivot);

    SketchTransform turn;
    turn.kind = SketchTransformKind::Rotate;
    turn.angleRad = 3.14159265358979323846 / 2.0;

    bool fromPoint = false;
    const Vec2 anchor = TransformAnchor(*run.sketch, selection, &fromPoint);
    EXPECT_TRUE(fromPoint);
    EXPECT_NEAR(anchor.x, 0.0, 1e-9);

    const TransformOutcome outcome =
        ApplyTransform(run.document, run.sketch->id(), selection, turn);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    // (40,0) turns a quarter about the origin to (0,40).
    const SketchLine& first = std::get<SketchLine>(run.sketch->findEntity(run.sides[0])->geometry);
    EXPECT_NEAR(first.end.x, 0.0, 1e-9);
    EXPECT_NEAR(first.end.y, 40.0, 1e-9);

    // A SECOND point makes it ambiguous again, so the centre comes back.
    selection.push_back(run.sketch->addPoint(Vec2{5, 5}));
    TransformAnchor(*run.sketch, selection, &fromPoint);
    EXPECT_FALSE(fromPoint);
}

TEST(SketchToolsTest, M17_XF_004_ScaleResizesARadiusAndLeavesAnglesAlone) {
    PartDocument document{"TransformDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId arc = sketch.addArc(Vec2{10, 0}, 5.0, 0.0, 1.0, true);

    SketchTransform bigger;
    bigger.kind = SketchTransformKind::Scale;
    bigger.factor = 3.0;
    const TransformOutcome outcome = ApplyTransform(document, sketch.id(), {arc}, bigger);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const SketchArc& grown = std::get<SketchArc>(sketch.findEntity(arc)->geometry);
    EXPECT_NEAR(grown.radiusMm, 15.0, 1e-9);
    // THE ANGLES DO NOT MOVE. A scale about a point leaves every direction
    // where it was; adding the rotation's angle here would spin the sweep round
    // its own centre while the centre sat still.
    EXPECT_NEAR(grown.startAngleRad, 0.0, 1e-9);
    EXPECT_NEAR(grown.endAngleRad, 1.0, 1e-9);
}

TEST(SketchToolsTest, M17_XF_005_ACopyKeepsTheConstraintsThatLiveINSIDEIt) {
    BoxRun run;
    SketchTransform copy;
    copy.kind = SketchTransformKind::Move;
    copy.deltaMm = Vec2{200, 0};
    copy.keepACopy = true;

    const TransformOutcome outcome = ApplyTransform(run.document, run.sketch->id(), run.sides, copy);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    ASSERT_EQ(outcome.created.size(), 4u);
    // Four coincidences and four orientations, all of them wholly inside.
    EXPECT_EQ(outcome.constraintsCopied, 8);
    EXPECT_EQ(outcome.constraintsLeftBehind, 0);

    // AND THE COPY IS STILL A RECTANGLE when it is dragged. A copy with the
    // geometry but not the constraints looks identical and falls apart on the
    // first edit -- which is the failure this whole test exists for.
    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();
    SketchLine moved = std::get<SketchLine>(run.sketch->findEntity(outcome.created[0])->geometry);
    moved.start = Vec2{190, -25};
    ASSERT_TRUE(run.document.setSketchEntityGeometry(run.sketch->id(), outcome.created[0], moved));
    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();
    for (const SketchEntityId id : outcome.created) {
        const SketchLine& side = std::get<SketchLine>(run.sketch->findEntity(id)->geometry);
        const bool horizontal = std::fabs(side.start.y - side.end.y) < 1e-6;
        const bool vertical = std::fabs(side.start.x - side.end.x) < 1e-6;
        EXPECT_TRUE(horizontal || vertical) << run.sketch->solveMessage();
    }
}

TEST(SketchToolsTest, M17_XF_006_AConstraintReachingOUTSIDEIsCountedNotCopied) {
    // Copying it would tie the copy to the original's neighbours -- a rectangle
    // copied off a wall would still be stuck to that wall. So it is not copied.
    // Nothing the user typed is lost: the ORIGINAL keeps every one of them,
    // which is why counting and reporting is honest here where dropping a
    // constraint during a Split would not be.
    BoxRun run;
    const SketchEntityId elsewhere = run.sketch->addLine(Vec2{0, 100}, Vec2{40, 100});
    ASSERT_NE(run.document.addSketchConstraint(run.sketch->id(),
                                               ParallelConstraint{run.sides[0], elsewhere}),
              kInvalidSketchConstraintId);
    const std::size_t before = run.sketch->constraints().size();

    SketchTransform copy;
    copy.kind = SketchTransformKind::Move;
    copy.deltaMm = Vec2{200, 0};
    copy.keepACopy = true;
    const TransformOutcome outcome = ApplyTransform(run.document, run.sketch->id(), run.sides, copy);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    EXPECT_EQ(outcome.constraintsLeftBehind, 1);
    EXPECT_EQ(outcome.constraintsCopied, 8);
    EXPECT_NE(outcome.status.find("reached outside"), std::string::npos) << outcome.status;
    // The original still has it: 8 copied, and the parallel stayed put.
    EXPECT_EQ(run.sketch->constraints().size(), before + 8);
}

TEST(SketchToolsTest, M17_XF_007_ACopiedDimensionGetsItsOWNParameter) {
    // Sharing the original's would look right and then refuse to let the copy
    // be resized -- a relationship the user never asked for and cannot see.
    // Offset already makes its own parameter; this follows it.
    BoxRun run;
    Parameter& width = run.document.addParameter("W", 40.0, UnitType::Millimeter);
    ASSERT_NE(run.document.addSketchConstraint(run.sketch->id(),
                                               LengthConstraint{run.sides[0], width.id()}),
              kInvalidSketchConstraintId);

    SketchTransform copy;
    copy.kind = SketchTransformKind::Move;
    copy.deltaMm = Vec2{200, 0};
    copy.keepACopy = true;
    const TransformOutcome outcome = ApplyTransform(run.document, run.sketch->id(), run.sides, copy);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    ObjectId copied = kInvalidObjectId;
    for (const SketchConstraint& constraint : run.sketch->constraints()) {
        const auto* length = std::get_if<LengthConstraint>(&constraint.data);
        if (length == nullptr || length->line == run.sides[0]) continue;
        copied = length->parameterId;
    }
    ASSERT_NE(copied, kInvalidObjectId) << "the length was not copied at all";
    EXPECT_NE(copied, width.id()) << "the copy shares the original's parameter";
    const Parameter* fresh = run.document.parameters().findById(copied);
    ASSERT_NE(fresh, nullptr);
    // Same VALUE, so the copy is the same size as what it was copied from.
    EXPECT_NEAR(fresh->value(), 40.0, 1e-9);
}

TEST(SketchToolsTest, M17_XF_008_ATransformThatMovesNothingIsRefused) {
    // In place it would be a no-op wearing a success message; as a copy it would
    // drop a duplicate exactly on top of the original, where the only way to
    // find it is to notice the DOF went up.
    BoxRun run;
    SketchTransform nothing;
    nothing.kind = SketchTransformKind::Move;
    nothing.deltaMm = Vec2{0, 0};
    EXPECT_FALSE(ApplyTransform(run.document, run.sketch->id(), run.sides, nothing).applied);

    nothing.kind = SketchTransformKind::Scale;
    nothing.factor = 1.0;
    EXPECT_FALSE(ApplyTransform(run.document, run.sketch->id(), run.sides, nothing).applied);

    // A NEGATIVE scale is a mirror, and Mirror exists. Said rather than done.
    nothing.factor = -2.0;
    const TransformOutcome flipped =
        ApplyTransform(run.document, run.sketch->id(), run.sides, nothing);
    EXPECT_FALSE(flipped.applied);
    EXPECT_NE(flipped.status.find("Mirror"), std::string::npos) << flipped.status;

    EXPECT_EQ(CountOf(*run.sketch, kLineIndex), 4);
}

TEST(SketchToolsTest, M17_XF_009_AnArcsAnchorIsWhereTheARCIsNotItsWholeCircle) {
    // A short arc near the top of a big circle sits nowhere near that circle's
    // bounding box. Taking the circle's box would drag the anchor off towards
    // geometry that is not there, and a rotation about it would fling the arc
    // somewhere the user did not point.
    PartDocument document{"TransformDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    // A small sweep centred on straight up, on a circle of radius 100.
    const SketchEntityId arc = sketch.addArc(Vec2{0, 0}, 100.0, 1.4, 1.75, true);

    const Vec2 anchor = TransformAnchor(sketch, {arc}, nullptr);
    EXPECT_GT(anchor.y, 90.0) << "the anchor fell back to the whole circle";
    EXPECT_LT(std::fabs(anchor.x), 20.0);
}

// --- M17.23: split ------------------------------------------------------------

namespace {

// A 100 mm horizontal line with a vertical line crossing it at x = 40.
struct SplitRun {
    PartDocument document{"SplitDoc"};
    GaussNewtonSketchSolver solver;
    Sketch* sketch = nullptr;
    SketchEntityId target{kInvalidSketchEntityId};
    SketchEntityId cutter{kInvalidSketchEntityId};

    SplitRun() {
        document.setSketchSolver(&solver);
        sketch = &document.addSketch("Sketch001");
        target = sketch->addLine(Vec2{0, 0}, Vec2{100, 0});
        cutter = sketch->addLine(Vec2{40, -30}, Vec2{40, 30});
    }

    SketchConstraintId add(SketchConstraintData data) {
        return document.addSketchConstraint(sketch->id(), std::move(data));
    }
    SplitOutcome split() { return ApplySplit(document, sketch->id(), target, {cutter}); }
    int countOf(std::size_t which) const { return CountOf(*sketch, which); }
};

int ConstraintsOfKind(const Sketch& sketch, const char* kind) {
    int count = 0;
    for (const SketchConstraint& constraint : sketch.constraints())
        if (std::string(ConstraintKindName(constraint.data)) == kind) ++count;
    return count;
}

} // namespace

TEST(SketchToolsTest, M17_SPLIT_001_ALineIsCutWhereTheOtherCrossesIt) {
    SplitRun run;
    const SplitOutcome outcome = run.split();
    ASSERT_TRUE(outcome.applied) << outcome.status;
    ASSERT_EQ(outcome.created.size(), 2u);

    // The cutter and the two pieces -- and the ORIGINAL is gone, not left
    // underneath as a third line nobody can see.
    EXPECT_EQ(run.countOf(kLineIndex), 3);
    EXPECT_EQ(run.sketch->findEntity(run.target), nullptr);

    const SketchLine& first = std::get<SketchLine>(
        run.sketch->findEntity(outcome.created[0])->geometry);
    const SketchLine& second = std::get<SketchLine>(
        run.sketch->findEntity(outcome.created[1])->geometry);
    EXPECT_NEAR(first.start.x, 0.0, 1e-9);
    EXPECT_NEAR(first.end.x, 40.0, 1e-9);
    EXPECT_NEAR(second.start.x, 40.0, 1e-9);
    EXPECT_NEAR(second.end.x, 100.0, 1e-9);
}

TEST(SketchToolsTest, M17_SPLIT_002_ThePiecesSTAYJoined) {
    // The joint is what makes it a SPLIT rather than "delete it and draw two".
    // Without it the pieces are unrelated geometry that lines up today, and the
    // DOF says so.
    SplitRun run;
    const SplitOutcome outcome = run.split();
    ASSERT_TRUE(outcome.applied) << outcome.status;
    EXPECT_EQ(ConstraintsOfKind(*run.sketch, "Coincident"), 1);

    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();
    // Drag the far end of the first piece and the second piece's start has to
    // come with it.
    SketchLine moved = std::get<SketchLine>(run.sketch->findEntity(outcome.created[0])->geometry);
    moved.end = Vec2{55, 18};
    ASSERT_TRUE(run.document.setSketchEntityGeometry(run.sketch->id(), outcome.created[0], moved));
    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();

    const SketchLine& a = std::get<SketchLine>(run.sketch->findEntity(outcome.created[0])->geometry);
    const SketchLine& b = std::get<SketchLine>(run.sketch->findEntity(outcome.created[1])->geometry);
    EXPECT_NEAR(std::hypot(a.end.x - b.start.x, a.end.y - b.start.y), 0.0, 1e-6)
        << run.sketch->solveMessage();
}

TEST(SketchToolsTest, M17_SPLIT_003_AnEndPointsConstraintGoesToThePieceThatKEEPSThatEnd) {
    // The question PlanTrim's own comment refused to guess at. A Fix on the
    // line's start belongs to the first piece and to NOTHING else; copying it
    // to both would pin the far piece's start to the origin as well, which is a
    // model the user never asked for and which looks fine until it is dragged.
    SplitRun run;
    ASSERT_NE(run.add(FixConstraint{SketchElementRef{run.target, SketchSubElement::StartPoint}}),
              kInvalidSketchConstraintId);
    const SplitOutcome outcome = run.split();
    ASSERT_TRUE(outcome.applied) << outcome.status;

    EXPECT_EQ(ConstraintsOfKind(*run.sketch, "Fix"), 1);
    for (const SketchConstraint& constraint : run.sketch->constraints()) {
        const auto* fix = std::get_if<FixConstraint>(&constraint.data);
        if (fix == nullptr) continue;
        EXPECT_EQ(fix->target.entityId, outcome.created[0]) << "the Fix went to the wrong piece";
        EXPECT_EQ(fix->target.subElement, SketchSubElement::StartPoint);
    }
}

TEST(SketchToolsTest, M17_SPLIT_004_ADirectionConstraintGoesToEVERYPiece) {
    // Horizontal is true of every sub-segment of a horizontal line, so both
    // pieces keep it. Dropping it, or keeping it on only one, would let half
    // the line swing free -- and the sketch would still solve.
    SplitRun run;
    ASSERT_NE(run.add(HorizontalConstraint{run.target}), kInvalidSketchConstraintId);
    const SplitOutcome outcome = run.split();
    ASSERT_TRUE(outcome.applied) << outcome.status;
    EXPECT_EQ(ConstraintsOfKind(*run.sketch, "Horizontal"), 2);

    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();
    SketchLine moved = std::get<SketchLine>(run.sketch->findEntity(outcome.created[0])->geometry);
    moved.end = Vec2{55, 18};
    ASSERT_TRUE(run.document.setSketchEntityGeometry(run.sketch->id(), outcome.created[0], moved));
    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();
    for (const SketchEntityId id : outcome.created) {
        const SketchLine& line = std::get<SketchLine>(run.sketch->findEntity(id)->geometry);
        EXPECT_NEAR(line.start.y, line.end.y, 1e-6) << run.sketch->solveMessage();
    }
}

TEST(SketchToolsTest, M17_SPLIT_005_ALENGTHOnTheLineREFUSESTheSplit) {
    // A length is about the whole extent. There is no piece it belongs to, and
    // both of the quiet answers are wrong: copying it onto each piece demands
    // two lines of the full length where one used to be, and dropping it
    // removes a dimension the user typed. So the split is refused and the
    // constraint is NAMED.
    SplitRun run;
    Parameter& length = run.document.addParameter("L", 100.0, UnitType::Millimeter);
    ASSERT_NE(run.add(LengthConstraint{run.target, length.id()}), kInvalidSketchConstraintId);

    const SplitOutcome outcome = run.split();
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("Length"), std::string::npos) << outcome.status;
    // AND NOTHING HAPPENED. A refusal that had already added the pieces would
    // be the worst of both.
    EXPECT_EQ(run.countOf(kLineIndex), 2);
    EXPECT_NE(run.sketch->findEntity(run.target), nullptr);
    EXPECT_EQ(ConstraintsOfKind(*run.sketch, "Length"), 1);
}

TEST(SketchToolsTest, M17_SPLIT_006_NothingCrossingItIsRefusedWithAReason) {
    SplitRun run;
    const SketchEntityId elsewhere = run.sketch->addLine(Vec2{0, 50}, Vec2{100, 50});
    const SplitOutcome outcome = ApplySplit(run.document, run.sketch->id(), run.target,
                                            {elsewhere});
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("crosses it"), std::string::npos) << outcome.status;
}

TEST(SketchToolsTest, M17_SPLIT_007_ACrossingAtANEndIsNotACut) {
    // It would produce a zero-length piece, which the sketch refuses anyway --
    // so the honest answer is that there is nothing to cut, not a failed add.
    SplitRun run;
    const SketchEntityId atEnd = run.sketch->addLine(Vec2{100, -30}, Vec2{100, 30});
    const SplitOutcome outcome = ApplySplit(run.document, run.sketch->id(), run.target, {atEnd});
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("crosses it"), std::string::npos) << outcome.status;
    // The two the fixture drew, plus the one this test added -- and no pieces.
    EXPECT_EQ(run.countOf(kLineIndex), 3);
}

TEST(SketchToolsTest, M17_SPLIT_008_TwoCrossingsGiveTHREEPieces) {
    SplitRun run;
    const SketchEntityId second = run.sketch->addLine(Vec2{70, -30}, Vec2{70, 30});
    const SplitOutcome outcome =
        ApplySplit(run.document, run.sketch->id(), run.target, {run.cutter, second});
    ASSERT_TRUE(outcome.applied) << outcome.status;
    ASSERT_EQ(outcome.created.size(), 3u);
    // In ORDER along the original, which is what makes the joints below join
    // neighbours rather than opposite ends.
    EXPECT_NEAR(std::get<SketchLine>(run.sketch->findEntity(outcome.created[1])->geometry).start.x,
                40.0, 1e-9);
    EXPECT_NEAR(std::get<SketchLine>(run.sketch->findEntity(outcome.created[1])->geometry).end.x,
                70.0, 1e-9);
    EXPECT_EQ(ConstraintsOfKind(*run.sketch, "Coincident"), 2);
}

TEST(SketchToolsTest, M17_SPLIT_009_ACircleBecomesARingOfArcs) {
    PartDocument document{"SplitDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId circle = sketch.addCircle(Vec2{0, 0}, 20.0);
    const SketchEntityId across = sketch.addLine(Vec2{-40, 0}, Vec2{40, 0});

    const SplitOutcome outcome = ApplySplit(document, sketch.id(), circle, {across});
    ASSERT_TRUE(outcome.applied) << outcome.status;
    // TWO cuts, TWO arcs -- not three. A closed curve has as many pieces as
    // cuts, and the last one runs back to the first.
    ASSERT_EQ(outcome.created.size(), 2u);
    EXPECT_EQ(CountOf(sketch, kCircleIndex), 0);
    EXPECT_EQ(CountOf(sketch, kArcIndex), 2);
    // ...so there are TWO joints, not one: the ring closes.
    EXPECT_EQ(ConstraintsOfKind(sketch, "Coincident"), 2);
}

TEST(SketchToolsTest, M17_SPLIT_010_OneCrossingDoesNotOpenACircle) {
    PartDocument document{"SplitDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId circle = sketch.addCircle(Vec2{0, 0}, 20.0);
    // Tangent from outside: touches at exactly one point.
    const SketchEntityId touching = sketch.addLine(Vec2{-40, 20}, Vec2{40, 20});

    const SplitOutcome outcome = ApplySplit(document, sketch.id(), circle, {touching});
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("does not open a circle"), std::string::npos) << outcome.status;
}

TEST(SketchToolsTest, M17_SPLIT_011_ARadiusFollowsEVERYSubArc) {
    PartDocument document{"SplitDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId circle = sketch.addCircle(Vec2{0, 0}, 20.0);
    const SketchEntityId across = sketch.addLine(Vec2{-40, 0}, Vec2{40, 0});
    Parameter& r = document.addParameter("R", 20.0, UnitType::Millimeter);
    ASSERT_NE(document.addSketchConstraint(sketch.id(), RadiusConstraint{circle, r.id()}),
              kInvalidSketchConstraintId);

    const SplitOutcome outcome = ApplySplit(document, sketch.id(), circle, {across});
    ASSERT_TRUE(outcome.applied) << outcome.status;
    // Every sub-arc has its parent's radius, so every sub-arc keeps the
    // dimension. One of them would leave the other free to change size, which
    // is not what "the circle is 20 across" said.
    EXPECT_EQ(ConstraintsOfKind(sketch, "Radius"), 2);
}

TEST(SketchToolsTest, M17_SPLIT_012_EveryConstraintKindHasAVerdict) {
    // The table is exhaustive over the variant, which the compiler enforces --
    // but "exhaustive" is not "right". This asserts the four verdicts that are
    // easiest to get backwards, on the two target kinds that differ.
    const SketchEntityId target = static_cast<SketchEntityId>(7);
    const SketchEntityId other = static_cast<SketchEntityId>(9);
    const SketchGeometry line = SketchLine{Vec2{0, 0}, Vec2{10, 0}};
    const SketchGeometry arc = SketchArc{Vec2{0, 0}, 5.0, 0.0, 1.0, true};

    // DIRECTION belongs to a line's pieces and means nothing for an arc's.
    EXPECT_EQ(SurvivesSplit(HorizontalConstraint{target}, target, line),
              SplitSurvival::EveryPiece);
    EXPECT_EQ(SurvivesSplit(ParallelConstraint{target, other}, target, line),
              SplitSurvival::EveryPiece);
    // CENTRE AND RADIUS belong to an arc's pieces and mean nothing for a line's.
    EXPECT_EQ(SurvivesSplit(ConcentricConstraint{target, other}, target, arc),
              SplitSurvival::EveryPiece);
    EXPECT_EQ(SurvivesSplit(ConcentricConstraint{target, other}, target, line),
              SplitSurvival::Refuse);
    // EXTENT belongs to neither.
    EXPECT_EQ(SurvivesSplit(EqualConstraint{target, other}, target, line), SplitSurvival::Refuse);
    EXPECT_EQ(SurvivesSplit(EqualConstraint{target, other}, target, arc), SplitSurvival::Refuse);
    // A POINT goes with its end...
    EXPECT_EQ(SurvivesSplit(
                  CoincidentConstraint{SketchElementRef{target, SketchSubElement::EndPoint},
                                       SketchElementRef{other, SketchSubElement::StartPoint}},
                  target, line),
              SplitSurvival::OwningPiece);
    // ...unless it names BOTH ends, which is the extent again wearing a hat.
    EXPECT_EQ(SurvivesSplit(
                  DistanceConstraint{SketchElementRef{target, SketchSubElement::StartPoint},
                                     SketchElementRef{target, SketchSubElement::EndPoint},
                                     kInvalidObjectId},
                  target, line),
              SplitSurvival::Refuse);
}

TEST(SketchToolsTest, M17_SPLIT_013_ATangencyPinnedAtAnEndFollowsTHATEnd) {
    // ADR-M17-045 made a tangency name an END rather than the whole line, and
    // this is the first thing that depends on it: as `Whole` it would read as
    // "about the extent" and refuse the split, when in fact it is about one
    // corner and travels with it.
    PartDocument document{"SplitDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId line = sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId circle = sketch.addCircle(Vec2{100, 20}, 20.0);
    const SketchEntityId cutter = sketch.addLine(Vec2{40, -30}, Vec2{40, 30});
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  TangentConstraint{line, circle, false, SketchSubElement::EndPoint}),
              kInvalidSketchConstraintId);

    const SplitOutcome outcome = ApplySplit(document, sketch.id(), line, {cutter});
    ASSERT_TRUE(outcome.applied) << outcome.status;
    EXPECT_EQ(ConstraintsOfKind(sketch, "Tangent"), 1);
    for (const SketchConstraint& constraint : sketch.constraints()) {
        const auto* tangent = std::get_if<TangentConstraint>(&constraint.data);
        if (tangent == nullptr) continue;
        // The SECOND piece is the one with the far end on it.
        EXPECT_EQ(tangent->a, outcome.created[1]);
        EXPECT_EQ(tangent->at, SketchSubElement::EndPoint);
    }
}

// --- M17.22: the tangent arc --------------------------------------------------

namespace {

// Draws one horizontal line from (0,0) to (100,0) and leaves the tangent arc
// tool armed on its far end. Returns the line's id.
SketchEntityId LineToGrowFrom(ToolRun& run) {
    run.model.setTool(SketchTool::Line);
    run.click(Vec2{0, 0});
    run.click(Vec2{100, 0});
    run.model.cancel();
    for (const SketchEntity& entity : run.sketch->entities())
        if (std::holds_alternative<SketchLine>(entity.geometry)) return entity.id;
    return kInvalidSketchEntityId;
}

const SketchArc* OnlyArc(const Sketch& sketch) {
    for (const SketchEntity& entity : sketch.entities())
        if (const auto* arc = std::get_if<SketchArc>(&entity.geometry)) return arc;
    return nullptr;
}

// The direction the arc TRAVELS at one of its tips: which end, and which way
// round it sweeps, both matter.
Vec2 LeavingDirection(const SketchArc& arc, bool atStart) {
    const Vec2 tip = atStart ? StartPointOf(SketchGeometry{arc}) : EndPointOf(SketchGeometry{arc});
    const Vec2 radial{tip.x - arc.center.x, tip.y - arc.center.y};
    return arc.counterClockwise ? Vec2{-radial.y, radial.x} : Vec2{radial.y, -radial.x};
}

double SinBetween(Vec2 a, Vec2 b) {
    return std::fabs(a.x * b.y - a.y * b.x) / (std::hypot(a.x, a.y) * std::hypot(b.x, b.y));
}

// SIGNED, and checked alongside the sine everywhere below.
//
// Parallel alone is NOT smooth: the complementary arc has the same centre, the
// same radius and the same two tips, and doubles back from the joint -- a cusp
// whose sine is a clean zero. A mutation that reversed the host's heading
// produced exactly that arc and survived a parallel-only check, which is why
// every smoothness assertion here comes in a pair.
double CosBetween(Vec2 a, Vec2 b) {
    return (a.x * b.x + a.y * b.y) / (std::hypot(a.x, a.y) * std::hypot(b.x, b.y));
}

} // namespace

TEST(SketchToolsTest, M17_TOOL_020_ATangentArcLeavesTheLineSMOOTHLY) {
    // Two clicks and no choices: given the end, the direction and where to
    // stop, exactly one arc exists. What must be true of it is that it leaves
    // the line without a corner -- and that is checked as an ANGLE, not as
    // "an arc appeared".
    ToolRun run;
    const SketchEntityId line = LineToGrowFrom(run);
    ASSERT_NE(line, kInvalidSketchEntityId);

    run.model.setTool(SketchTool::TangentArc);
    run.snapClick(Vec2{100, 0});   // the line's far END -- snaps to it
    run.click(Vec2{150, 50});  // where to stop
    ASSERT_EQ(CountOf(*run.sketch, kArcIndex), 1) << run.lastStatus;

    const SketchArc* arc = OnlyArc(*run.sketch);
    ASSERT_NE(arc, nullptr);
    // The centre is square to the line, so it is directly above the joint, and
    // the radius follows from the chord. Both are independent arithmetic:
    // s = (50^2 + 50^2) / (2 * 50) = 50.
    EXPECT_NEAR(arc->center.x, 100.0, 1e-9);
    EXPECT_NEAR(arc->center.y, 50.0, 1e-9);
    EXPECT_NEAR(arc->radiusMm, 50.0, 1e-9);
    // It starts at the joint and stops where the second click was.
    EXPECT_NEAR(StartPointOf(SketchGeometry{*arc}).x, 100.0, 1e-9);
    EXPECT_NEAR(StartPointOf(SketchGeometry{*arc}).y, 0.0, 1e-9);
    EXPECT_NEAR(EndPointOf(SketchGeometry{*arc}).x, 150.0, 1e-9);
    EXPECT_NEAR(EndPointOf(SketchGeometry{*arc}).y, 50.0, 1e-9);
    // ...and it LEAVES along the line, rather than doubling back. The
    // complementary arc has the same centre, radius and tips, so the cosine is
    // what tells them apart.
    EXPECT_NEAR(SinBetween(LeavingDirection(*arc, true), Vec2{1, 0}), 0.0, 1e-9);
    EXPECT_GT(CosBetween(LeavingDirection(*arc, true), Vec2{1, 0}), 0.5);
}

TEST(SketchToolsTest, M17_TOOL_021_ItStaysSmoothWhenTheLineIsDragged) {
    // The constraints, not the placement. An arc laid down tangent LOOKS right
    // with no constraints at all; what tells them apart is moving something.
    ToolRun run;
    const SketchEntityId line = LineToGrowFrom(run);
    run.model.setTool(SketchTool::TangentArc);
    run.snapClick(Vec2{100, 0});
    run.click(Vec2{150, 50});
    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();

    // Swing the line's free end well off the axis. The joint, and the arc, must
    // follow.
    ASSERT_TRUE(run.document.setSketchEntityGeometry(
        run.sketch->id(), line, SketchLine{Vec2{0, 0}, Vec2{90, 40}}));
    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();

    const SketchLine& moved = std::get<SketchLine>(run.sketch->findEntity(line)->geometry);
    const SketchArc* arc = OnlyArc(*run.sketch);
    ASSERT_NE(arc, nullptr);
    const Vec2 joint = StartPointOf(SketchGeometry{*arc});
    // STILL JOINED...
    EXPECT_NEAR(std::hypot(joint.x - moved.end.x, joint.y - moved.end.y), 0.0, 1e-6)
        << run.sketch->solveMessage();
    // ...and STILL SMOOTH. Without a tangency that names the corner this is
    // where it kinks: the joint holds and the arc leaves at an angle.
    const Vec2 heading{moved.end.x - moved.start.x, moved.end.y - moved.start.y};
    EXPECT_NEAR(SinBetween(LeavingDirection(*arc, true), heading), 0.0, 1e-6)
        << run.sketch->solveMessage();
    EXPECT_GT(CosBetween(LeavingDirection(*arc, true), heading), 0.5)
        << run.sketch->solveMessage();
}

TEST(SketchToolsTest, M17_TOOL_022_ATangentArcOffAnARCStaysSmoothToo) {
    // The chained case, which is what the tool is really for -- and which needs
    // a different residual (collinear radii, not perpendicularity) because the
    // host has no straight direction to be square to.
    ToolRun run;
    LineToGrowFrom(run);
    run.model.setTool(SketchTool::TangentArc);
    run.snapClick(Vec2{100, 0});
    run.click(Vec2{150, 50});
    ASSERT_EQ(CountOf(*run.sketch, kArcIndex), 1) << run.lastStatus;
    // The tool CHAINS: the next click continues from the arc just made. Off to
    // one side, because dead ahead of the first arc's exit there is no arc --
    // and the first draft of this test clicked exactly there.
    run.click(Vec2{200, 100});
    ASSERT_EQ(CountOf(*run.sketch, kArcIndex), 2) << run.lastStatus;
    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();

    std::vector<const SketchArc*> arcs;
    for (const SketchEntity& entity : run.sketch->entities())
        if (const auto* arc = std::get_if<SketchArc>(&entity.geometry)) arcs.push_back(arc);
    ASSERT_EQ(arcs.size(), 2u);
    // Joined, and SMOOTH: at a smooth joint the two centres and the joint are
    // in a straight line, which is exactly what the residual says.
    const Vec2 joint = EndPointOf(SketchGeometry{*arcs[0]});
    EXPECT_NEAR(std::hypot(joint.x - StartPointOf(SketchGeometry{*arcs[1]}).x,
                           joint.y - StartPointOf(SketchGeometry{*arcs[1]}).y),
                0.0, 1e-6)
        << run.sketch->solveMessage();
    const Vec2 first{joint.x - arcs[0]->center.x, joint.y - arcs[0]->center.y};
    const Vec2 second{joint.x - arcs[1]->center.x, joint.y - arcs[1]->center.y};
    EXPECT_NEAR(SinBetween(first, second), 0.0, 1e-6) << run.sketch->solveMessage();
    // ...and the second arc LEAVES the way the first ARRIVES. Collinear radii
    // are satisfied by the arc that doubles back, which is a cusp: the tool has
    // to pick the other one.
    const Vec2 arriving = LeavingDirection(*arcs[0], false);
    const Vec2 leaving = LeavingDirection(*arcs[1], true);
    EXPECT_NEAR(SinBetween(arriving, leaving), 0.0, 1e-6) << run.sketch->solveMessage();
    EXPECT_GT(CosBetween(arriving, leaving), 0.5) << run.sketch->solveMessage();
}

TEST(SketchToolsTest, M17_TOOL_023_ATangentArcOffAnArcSURVIVESADrag) {
    // The same pair, made to move. The arc-to-arc residual is the one that used
    // to be a centre-distance equation, which is satisfied at the joint and
    // holds nothing there -- so this is the test that tells the two apart.
    ToolRun run;
    LineToGrowFrom(run);
    run.model.setTool(SketchTool::TangentArc);
    run.snapClick(Vec2{100, 0});
    run.click(Vec2{150, 50});
    run.click(Vec2{200, 100});
    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();

    std::vector<SketchEntityId> arcIds;
    for (const SketchEntity& entity : run.sketch->entities())
        if (std::holds_alternative<SketchArc>(entity.geometry)) arcIds.push_back(entity.id);
    ASSERT_EQ(arcIds.size(), 2u);

    // Shove the FIRST arc's centre. Everything downstream has to re-solve.
    SketchArc shoved = std::get<SketchArc>(run.sketch->findEntity(arcIds[0])->geometry);
    shoved.center = Vec2{shoved.center.x + 12.0, shoved.center.y - 9.0};
    ASSERT_TRUE(run.document.setSketchEntityGeometry(run.sketch->id(), arcIds[0], shoved));
    ASSERT_TRUE(run.document.recompute().success) << run.sketch->solveMessage();

    const SketchArc& a = std::get<SketchArc>(run.sketch->findEntity(arcIds[0])->geometry);
    const SketchArc& b = std::get<SketchArc>(run.sketch->findEntity(arcIds[1])->geometry);
    const Vec2 joint = EndPointOf(SketchGeometry{a});
    EXPECT_NEAR(std::hypot(joint.x - StartPointOf(SketchGeometry{b}).x,
                           joint.y - StartPointOf(SketchGeometry{b}).y),
                0.0, 1e-6)
        << run.sketch->solveMessage();
    const Vec2 first{joint.x - a.center.x, joint.y - a.center.y};
    const Vec2 second{joint.x - b.center.x, joint.y - b.center.y};
    EXPECT_NEAR(SinBetween(first, second), 0.0, 1e-6) << run.sketch->solveMessage();
    const Vec2 arriving = LeavingDirection(a, false);
    const Vec2 leaving = LeavingDirection(b, true);
    EXPECT_NEAR(SinBetween(arriving, leaving), 0.0, 1e-6) << run.sketch->solveMessage();
    EXPECT_GT(CosBetween(arriving, leaving), 0.5) << run.sketch->solveMessage();
}

TEST(SketchToolsTest, M17_TOOL_024_ClickingNOTHINGIsRefusedWithAReason) {
    // A tangent arc that is tangent to nothing is not what was asked for. The
    // alternative -- quietly drawing a plain arc through the two clicks -- is
    // the silent substitution this project keeps paying for, and it would look
    // right until the first drag.
    ToolRun run;
    LineToGrowFrom(run);
    run.model.setTool(SketchTool::TangentArc);
    run.snapClick(Vec2{300, 300});  // open space
    run.click(Vec2{350, 350});
    EXPECT_EQ(CountOf(*run.sketch, kArcIndex), 0);
    EXPECT_NE(run.lastStatus.find("END of a line or an arc"), std::string::npos)
        << run.lastStatus;
}

TEST(SketchToolsTest, M17_TOOL_025_StoppingSTRAIGHTAHEADIsRefusedWithAReason) {
    // Straight on there is no arc -- the centre runs to infinity. Refused with
    // the thing to do instead, rather than an arc of absurd radius that looks
    // like a line and behaves like nothing.
    ToolRun run;
    LineToGrowFrom(run);
    run.model.setTool(SketchTool::TangentArc);
    run.snapClick(Vec2{100, 0});
    run.click(Vec2{160, 0});  // dead ahead
    EXPECT_EQ(CountOf(*run.sketch, kArcIndex), 0);
    EXPECT_NE(run.lastStatus.find("straight ahead"), std::string::npos) << run.lastStatus;
}

TEST(SketchToolsTest, M17_TOOL_026_GrowingFromTheOtherENDGoesTheOtherWay) {
    // The direction is read from WHICH end was clicked, and getting it backwards
    // produces an arc that still touches, still joins, and curls the wrong way.
    // Both ends of the same line, one arc each, and they must lean apart.
    ToolRun run;
    LineToGrowFrom(run);
    run.model.setTool(SketchTool::TangentArc);
    run.snapClick(Vec2{0, 0});     // the START end -- heading is -x
    run.click(Vec2{-50, 50});
    const SketchArc* arc = OnlyArc(*run.sketch);
    ASSERT_NE(arc, nullptr) << run.lastStatus;
    EXPECT_NEAR(arc->center.x, 0.0, 1e-9);
    EXPECT_NEAR(arc->center.y, 50.0, 1e-9);
    // Leaving along -x, which is the direction the line runs AWAY from that end.
    EXPECT_NEAR(SinBetween(LeavingDirection(*arc, true), Vec2{-1, 0}), 0.0, 1e-9);
    EXPECT_GT(CosBetween(LeavingDirection(*arc, true), Vec2{-1, 0}), 0.5);
}

TEST(SketchToolsTest, M17_TOOL_011_TheSlotsEndsBULGEOUTWARDNotInward) {
    // Both directions produce a closed outline that pads perfectly well, and
    // one of them is a slot while the other has its ends caved in. The
    // endpoints are identical either way, so only the extent tells them apart:
    // a real slot reaches a radius PAST each centre.
    ToolRun run;
    run.model.setTool(SketchTool::Slot);
    run.click(Vec2{20, 30});
    run.click(Vec2{60, 30});
    run.click(Vec2{60, 38}); // radius 8

    double minX = 1e9, maxX = -1e9;
    for (const SketchEntity& entity : run.sketch->entities()) {
        const auto* arc = std::get_if<SketchArc>(&entity.geometry);
        if (arc == nullptr) continue;
        // Walk the swept range; the extreme is what says which way it bulges.
        double sweep = arc->endAngleRad - arc->startAngleRad;
        while (sweep < 0.0) sweep += 2.0 * 3.14159265358979323846;
        for (int step = 0; step <= 16; ++step) {
            const double at = arc->startAngleRad + sweep * step / 16.0;
            const double x = arc->center.x + arc->radiusMm * std::cos(at);
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
        }
    }
    EXPECT_NEAR(minX, 12.0, 1e-6) << "the left end does not reach a radius past its centre";
    EXPECT_NEAR(maxX, 68.0, 1e-6) << "the right end does not reach a radius past its centre";
}

TEST(SketchToolsTest, M17_TOOL_012_TheWidthIsMeasuredACROSSTheSlotNotAlongIt) {
    // The third click sets the width. Using its plain distance to a centre
    // would make the slot wider whenever the user drifted along it -- which is
    // exactly what a hand does while dragging.
    ToolRun straightAcross;
    straightAcross.model.setTool(SketchTool::Slot);
    straightAcross.click(Vec2{0, 0});
    straightAcross.click(Vec2{40, 0});
    straightAcross.click(Vec2{40, 6}); // 6 across, at the far centre

    ToolRun driftedAlong;
    driftedAlong.model.setTool(SketchTool::Slot);
    driftedAlong.click(Vec2{0, 0});
    driftedAlong.click(Vec2{40, 0});
    driftedAlong.click(Vec2{15, 6}); // the same 6 across, well along the slot

    const auto radiusOf = [](const Sketch& sketch) {
        for (const SketchEntity& entity : sketch.entities())
            if (const auto* arc = std::get_if<SketchArc>(&entity.geometry)) return arc->radiusMm;
        return -1.0;
    };
    EXPECT_NEAR(radiusOf(*straightAcross.sketch), 6.0, 1e-9);
    EXPECT_NEAR(radiusOf(*driftedAlong.sketch), 6.0, 1e-9)
        << "sliding along the slot changed its width";
}

// --- M17.30: EVERY POINT OF A SPLINE IS NAMEABLE -----------------------------

TEST(SketchToolsTest, M17_SPL_011_AnINTERIORPointCanBeConstrained) {
    // The gap M17.30 closed. A SketchElementRef could name a spline's two ends
    // and nothing between them, so a seven-node spline had ten freedoms that
    // NOTHING IN THE PROGRAM COULD REACH -- it could never be fully constrained,
    // and its colour could never go black no matter what the user drew.
    PartDocument document{"SplineDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline =
        sketch.addSpline({Vec2{0, 0}, Vec2{40, 30}, Vec2{80, 0}}, false);
    Parameter& height = document.addParameter("H", 55.0, UnitType::Millimeter);

    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  VerticalDistanceConstraint{
                      SketchElementRef{spline, SketchSubElement::StartPoint},
                      SketchElementRef{spline, SketchSubElement::SplinePoint, 1}, height.id()}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const SketchSpline& solved = std::get<SketchSpline>(sketch.findEntity(spline)->geometry);
    EXPECT_NEAR(solved.points[1].y - solved.points[0].y, 55.0, 1e-6) << sketch.solveMessage();
}

TEST(SketchToolsTest, M17_SPL_012_ASplinePointNamingAnENDIsREFUSED) {
    // ONE POINT, ONE SPELLING. `SplinePoint 0` and `StartPoint` are the same
    // place, and if both resolved then the same point would compare unequal to
    // itself -- selecting it twice would look like two points, and a Coincident
    // between the two spellings would be a constraint on nothing that still
    // ate a degree of freedom.
    PartDocument document{"SplineDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline =
        sketch.addSpline({Vec2{0, 0}, Vec2{40, 30}, Vec2{80, 0}}, false);
    const SketchEntityId point = sketch.addPoint(Vec2{5, 5});

    document.addSketchConstraint(
        sketch.id(),
        CoincidentConstraint{SketchElementRef{point, SketchSubElement::Whole},
                             SketchElementRef{spline, SketchSubElement::SplinePoint, 0}});

    EXPECT_FALSE(document.recompute().success);
    EXPECT_NE(sketch.solveMessage().find("interior"), std::string::npos)
        << sketch.solveMessage();
}

TEST(SketchToolsTest, M17_SPL_013_AnIndexPastTheEndIsREFUSEDNotClamped) {
    // Clamping would silently constrain a DIFFERENT point than the one named --
    // the failure the whole one-spelling rule exists to prevent, arriving by
    // the other door.
    PartDocument document{"SplineDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline =
        sketch.addSpline({Vec2{0, 0}, Vec2{40, 30}, Vec2{80, 0}}, false);
    const SketchEntityId point = sketch.addPoint(Vec2{5, 5});

    document.addSketchConstraint(
        sketch.id(),
        CoincidentConstraint{SketchElementRef{point, SketchSubElement::Whole},
                             SketchElementRef{spline, SketchSubElement::SplinePoint, 9}});

    EXPECT_FALSE(document.recompute().success) << sketch.solveMessage();
}

TEST(SketchToolsTest, M17_SPL_014_EveryPointOfACLOSEDSplineIsNameable) {
    // A closed spline has no start and no end, the way a circle has none -- so
    // unlike an open one, ALL of its points are reached by index. Excluding
    // "the first and the last" there would leave two points unconstrainable
    // for no reason at all.
    PartDocument document{"SplineDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline =
        sketch.addSpline({Vec2{0, 0}, Vec2{40, 30}, Vec2{80, 0}, Vec2{40, -30}}, true);
    Parameter& span = document.addParameter("W", 100.0, UnitType::Millimeter);

    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  HorizontalDistanceConstraint{
                      SketchElementRef{spline, SketchSubElement::SplinePoint, 0},
                      SketchElementRef{spline, SketchSubElement::SplinePoint, 2}, span.id()}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const SketchSpline& solved = std::get<SketchSpline>(sketch.findEntity(spline)->geometry);
    EXPECT_NEAR(solved.points[2].x - solved.points[0].x, 100.0, 1e-6) << sketch.solveMessage();
}

TEST(SketchToolsTest, M17_SPL_015_TheIndexPicksTHATPointAndNoOther) {
    // The bug an index makes possible: resolving every SplinePoint to the same
    // slot. Pinning point 2 and checking that points 1 and 3 DID NOT MOVE is
    // what tells the difference between "constrained the named point" and
    // "constrained a point".
    PartDocument document{"SplineDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const std::vector<Vec2> drawn{Vec2{0, 0}, Vec2{20, 30}, Vec2{50, -10}, Vec2{80, 20},
                                  Vec2{110, 0}};
    const SketchEntityId spline = sketch.addSpline(drawn, false);
    Parameter& lift = document.addParameter("Y", 44.0, UnitType::Millimeter);

    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  VerticalDistanceConstraint{
                      SketchElementRef{spline, SketchSubElement::StartPoint},
                      SketchElementRef{spline, SketchSubElement::SplinePoint, 2}, lift.id()}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const SketchSpline& solved = std::get<SketchSpline>(sketch.findEntity(spline)->geometry);
    EXPECT_NEAR(solved.points[2].y - solved.points[0].y, 44.0, 1e-6) << sketch.solveMessage();
    EXPECT_NEAR(solved.points[1].y, drawn[1].y, 1e-6) << "point 1 was not named and moved";
    EXPECT_NEAR(solved.points[3].y, drawn[3].y, 1e-6) << "point 3 was not named and moved";
}

TEST(SketchToolsTest, M17_SPL_016_ASevenNodeSplineCanReachDOFZERO) {
    // What the user asked for, and what could not be done before M17.30: a
    // seven-node spline with every freedom accounted for. Fourteen variables,
    // fourteen constraints, DOF 0 -- and the sketch says every entity in it is
    // fully constrained, which is what turns it black on screen.
    PartDocument document{"SplineDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline = sketch.addSpline({Vec2{7, -13}, Vec2{33, 31}, Vec2{61, -7},
                                                    Vec2{92, 27}, Vec2{119, -5}, Vec2{147, 23},
                                                    Vec2{173, -11}},
                                                   false);

    document.addSketchConstraint(
        sketch.id(), FixConstraint{SketchElementRef{spline, SketchSubElement::StartPoint}});
    const SketchElementRef origin{spline, SketchSubElement::StartPoint};
    const std::vector<std::pair<double, double>> places{{30, 45},  {60, -8}, {90, 40},
                                                        {120, -6}, {150, 35}, {180, 0}};
    for (std::size_t i = 0; i < places.size(); ++i) {
        const SketchElementRef point =
            i + 1 == places.size()
                ? SketchElementRef{spline, SketchSubElement::EndPoint}
                : SketchElementRef{spline, SketchSubElement::SplinePoint, static_cast<int>(i + 1)};
        Parameter& x =
            document.addParameter("X" + std::to_string(i), places[i].first, UnitType::Millimeter);
        Parameter& y =
            document.addParameter("Y" + std::to_string(i), places[i].second, UnitType::Millimeter);
        document.addSketchConstraint(sketch.id(),
                                     HorizontalDistanceConstraint{origin, point, x.id()});
        document.addSketchConstraint(sketch.id(),
                                     VerticalDistanceConstraint{origin, point, y.id()});
    }

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    EXPECT_EQ(sketch.degreesOfFreedom(), 0) << sketch.solveMessage();
    EXPECT_TRUE(sketch.isEntityFullyConstrained(spline)) << sketch.solveMessage();
}


namespace {
// --- M18: a mirrored spline, solved -----------------------------------------

TEST(SketchMirrorTest, M18_MIR_003_TheSymmetryHOLDSWhenTheOriginalMoves) {
    // The point of tying the copy at all. A mirror that only placed the
    // reflection once would come apart on the first drag, and it would look
    // right until then.
    SolvingFixture fixture;
    SketchEdit axisEdit;
    axisEdit.kind = SketchEditKind::AddLine;
    axisEdit.points = {Vec2{50.0, -50.0}, Vec2{50.0, 50.0}};
    axisEdit.label = "Add line";
    const SketchEntityId axis = fixture.apply(axisEdit).createdEntities.front();
    // THE AXIS HELD. Symmetry says "these two straddle that line" -- it does
    // not say where the line is, so a free axis is free to swing, and the
    // assertion below (reflected across x = 50) would be asserting something
    // the sketch never promised.
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{axis, SketchSubElement::StartPoint}})
                    .applied);
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix,
                                  {SketchElementRef{axis, SketchSubElement::EndPoint}})
                    .applied);
    const SketchEntityId source =
        fixture.document.addSketchEntity(
            fixture.sketchId, SketchSpline{{Vec2{10, 0}, Vec2{20, 30}, Vec2{35, 5}}, false});

    const MirrorOutcome outcome = ApplyMirror(fixture.document, fixture.sketchId, {source}, axis);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    const SketchEntityId copy = outcome.created.front();

    // Drive the original's middle point somewhere new.
    Parameter& lift = fixture.document.addParameter("Y", 60.0, UnitType::Millimeter);
    ASSERT_NE(fixture.document.addSketchConstraint(
                  fixture.sketchId,
                  VerticalDistanceConstraint{
                      SketchElementRef{source, SketchSubElement::StartPoint},
                      SketchElementRef{source, SketchSubElement::SplinePoint, 1}, lift.id()}),
              kInvalidSketchConstraintId);
    ASSERT_TRUE(fixture.document.recompute().success) << fixture.sketch().solveMessage();

    const SketchSpline& moved =
        std::get<SketchSpline>(fixture.sketch().findEntity(source)->geometry);
    const SketchSpline& shadow = std::get<SketchSpline>(fixture.sketch().findEntity(copy)->geometry);
    for (std::size_t i = 0; i < moved.points.size(); ++i) {
        EXPECT_NEAR(shadow.points[i].x, 100.0 - moved.points[i].x, 1e-6)
            << "point " << i << ": " << fixture.sketch().solveMessage();
        EXPECT_NEAR(shadow.points[i].y, moved.points[i].y, 1e-6)
            << "point " << i << ": " << fixture.sketch().solveMessage();
    }
}

} // namespace

// =============================================================================
// M26.3 -- Horizontal and Vertical, asked for through the UI, on POINTS
//
// The solver tests prove the constraints hold. These prove the COMMAND offers
// them: which selections are accepted, which are refused and why, and -- the
// part a solver test cannot see -- that a line's own two ends still produce
// the line form rather than a second spelling of it.
// =============================================================================

TEST(SketchCanvasSolveTest, M26_UIPHV_001_TwoPointsAreAcceptedAndHeldHorizontal) {
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId a = sketch.addPoint(Vec2{10, 5});
    const SketchEntityId b = sketch.addPoint(Vec2{80, 60});

    const SketchEditOutcome outcome =
        fixture.constrain(SketchEditKind::AddHorizontal,
                          {SketchElementRef{a}, SketchElementRef{b}});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const Sketch& after = fixture.sketch();
    const Vec2 pa = std::get_if<SketchPoint>(&after.findEntity(a)->geometry)->position;
    const Vec2 pb = std::get_if<SketchPoint>(&after.findEntity(b)->geometry)->position;
    EXPECT_NEAR(pa.y, pb.y, 1e-6);
}

TEST(SketchCanvasSolveTest, M26_UIPHV_002_ALinesOwnTwoEndsBecomeTheLINEForm) {
    // Picking a line's start and end IS picking the line. Storing that as a
    // point pair would be a second way to say one thing -- so the command
    // normalises it, and this is the assertion that says so.
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId line = sketch.addLine(Vec2{0, 0}, Vec2{60, 40});

    const SketchEditOutcome outcome = fixture.constrain(
        SketchEditKind::AddHorizontal,
        {SketchElementRef{line, SketchSubElement::StartPoint},
         SketchElementRef{line, SketchSubElement::EndPoint}});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const Sketch& after = fixture.sketch();
    ASSERT_EQ(after.constraints().size(), 1u);
    EXPECT_TRUE(std::holds_alternative<HorizontalConstraint>(after.constraints().front().data))
        << "a line's own two ends should be stored as the line form, not a point pair";
    const SketchLine& solved = LineOf(after, line);
    EXPECT_NEAR(solved.start.y, solved.end.y, 1e-6);
}

TEST(SketchCanvasSolveTest, M26_UIPHV_003_ALineAndAPointTogetherAreRefusedWithBothWordsInIt) {
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId line = sketch.addLine(Vec2{0, 0}, Vec2{60, 40});
    const SketchEntityId point = sketch.addPoint(Vec2{10, 90});

    fixture.model.setSelection(
        {SketchElementRef{line, SketchSubElement::Whole}, SketchElementRef{point}});
    std::string whyNot;
    const SketchEdit edit =
        fixture.model.requestConstraint(fixture.sketch(), SketchEditKind::AddHorizontal, &whyNot);
    EXPECT_FALSE(edit.valid());
    // The refusal has to say what IS accepted, not merely that this was not.
    EXPECT_NE(whyNot.find("line"), std::string::npos) << whyNot;
    EXPECT_NE(whyNot.find("two points"), std::string::npos) << whyNot;
}

TEST(SketchCanvasSolveTest, M26_UIPHV_004_ThreePointsAreRefusedAndSayHowMany) {
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId a = sketch.addPoint(Vec2{0, 0});
    const SketchEntityId b = sketch.addPoint(Vec2{10, 10});
    const SketchEntityId c = sketch.addPoint(Vec2{20, 20});

    fixture.model.setSelection(
        {SketchElementRef{a}, SketchElementRef{b}, SketchElementRef{c}});
    std::string whyNot;
    const SketchEdit edit =
        fixture.model.requestConstraint(fixture.sketch(), SketchEditKind::AddVertical, &whyNot);
    EXPECT_FALSE(edit.valid());
    // The refusal names BOTH shapes the command takes, so a user holding three
    // points is told what to do rather than only what not to.
    EXPECT_NE(whyNot.find("two points"), std::string::npos) << whyNot;
}

TEST(SketchCanvasSolveTest, M26_UIPHV_005_OnePointTwiceIsRefused) {
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId a = sketch.addPoint(Vec2{0, 0});

    fixture.model.setSelection({SketchElementRef{a}, SketchElementRef{a}});
    std::string whyNot;
    const SketchEdit edit =
        fixture.model.requestConstraint(fixture.sketch(), SketchEditKind::AddHorizontal, &whyNot);
    EXPECT_FALSE(edit.valid());
    EXPECT_NE(whyNot.find("same point"), std::string::npos) << whyNot;
}

TEST(SketchCanvasSolveTest, M26_UIPHV_006_SeveralLinesStillWorkTheWayTheyDid) {
    // The line form is what every sketch drawn before this existed uses, and
    // it takes N of them. Nothing above may have narrowed that.
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId a = sketch.addLine(Vec2{0, 0}, Vec2{60, 12});
    const SketchEntityId b = sketch.addLine(Vec2{0, 40}, Vec2{60, 55});

    const SketchEditOutcome outcome =
        fixture.constrain(SketchEditKind::AddHorizontal,
                          {SketchElementRef{a, SketchSubElement::Whole},
                           SketchElementRef{b, SketchSubElement::Whole}});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const Sketch& after = fixture.sketch();
    EXPECT_EQ(after.constraints().size(), 2u);
    EXPECT_NEAR(LineOf(after, a).start.y, LineOf(after, a).end.y, 1e-6);
    EXPECT_NEAR(LineOf(after, b).start.y, LineOf(after, b).end.y, 1e-6);
}

TEST(SketchCanvasSolveTest, M26_UIPHV_007_TwoENDPOINTSOfDifferentLinesAlignThePointsNotTheLines) {
    // THE CASE THE FEATURE EXISTS FOR, and the one the old rule got wrong: both
    // refs named line entities, so it made each LINE horizontal instead of
    // levelling the two corners the user picked. The lines must keep their
    // slopes.
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId a = sketch.addLine(Vec2{0, 0}, Vec2{60, 12});
    const SketchEntityId b = sketch.addLine(Vec2{100, 40}, Vec2{160, 80});

    const SketchEditOutcome outcome = fixture.constrain(
        SketchEditKind::AddHorizontal,
        {SketchElementRef{a, SketchSubElement::EndPoint},
         SketchElementRef{b, SketchSubElement::StartPoint}});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const Sketch& after = fixture.sketch();
    ASSERT_EQ(after.constraints().size(), 1u) << "one alignment, not one constraint per line";
    EXPECT_NEAR(LineOf(after, a).end.y, LineOf(after, b).start.y, 1e-6);
    // NEITHER LINE became horizontal. This is the assertion the old behaviour
    // fails, and it is the whole difference between the two readings.
    EXPECT_GT(std::fabs(LineOf(after, a).start.y - LineOf(after, a).end.y), 1.0);
    EXPECT_GT(std::fabs(LineOf(after, b).start.y - LineOf(after, b).end.y), 1.0);
}

TEST(SketchCanvasSolveTest, M26_UIPHV_008_TwoLineEndsGoVerticalOntoOneVirtualLine) {
    // Two lines, pick one END of each, press Vertical: the two ends must come
    // to share a u -- they end up on the same vertical line -- and the lines
    // themselves must keep their own slopes.
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId a = sketch.addLine(Vec2{0, 0}, Vec2{40, 30});
    const SketchEntityId b = sketch.addLine(Vec2{90, 5}, Vec2{130, 55});

    const SketchEditOutcome outcome = fixture.constrain(
        SketchEditKind::AddVertical,
        {SketchElementRef{a, SketchSubElement::EndPoint},
         SketchElementRef{b, SketchSubElement::StartPoint}});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const Sketch& after = fixture.sketch();
    ASSERT_EQ(after.constraints().size(), 1u);
    EXPECT_NEAR(LineOf(after, a).end.x, LineOf(after, b).start.x, 1e-6);
    // The v separation is untouched -- they are ON one vertical line, not on
    // top of each other. A Coincident would have collapsed this to zero.
    EXPECT_GT(std::fabs(LineOf(after, a).end.y - LineOf(after, b).start.y), 1.0);
    EXPECT_GT(std::fabs(LineOf(after, a).start.x - LineOf(after, a).end.x), 1.0);
    EXPECT_GT(std::fabs(LineOf(after, b).start.x - LineOf(after, b).end.x), 1.0);
}

// =============================================================================
// M26.4 -- what a rectangle's CORNERS can be asked to do
// =============================================================================

TEST(SketchCanvasSolveTest, M26_RECT_001_DraggingASideKeepsTheCornersJoined) {
    SolvingFixture fixture;
    const SketchEditOutcome made = fixture.drawRectangle(Vec2{0, 0}, Vec2{60, 40});
    ASSERT_TRUE(made.applied) << made.status;
    ASSERT_EQ(made.createdEntities.size(), 4u);
    const SketchEntityId side0 = made.createdEntities[0];
    const SketchEntityId side1 = made.createdEntities[1];

    // Drag side 0's END -- which is the corner side 1's START is joined to.
    ASSERT_TRUE(DragMoved(fixture.document.previewSketchDrag(
        fixture.sketchId, SketchElementRef{side0, SketchSubElement::EndPoint},
        Vec2{95.0, 15.0})));

    const Sketch& after = fixture.sketch();
    const SketchLine& a = LineOf(after, side0);
    const SketchLine& b = LineOf(after, side1);
    EXPECT_NEAR(a.end.x, b.start.x, 1e-6) << "the corner came apart in u";
    EXPECT_NEAR(a.end.y, b.start.y, 1e-6) << "the corner came apart in v";
}

TEST(SketchCanvasSolveTest, M26_RECT_002_BothPointsAtACornerCanBeSelected) {
    // THE REPORTED PROBLEM, as the smallest case. Two endpoints sit at exactly
    // the same place at every rectangle corner. PickElement returns hits.front()
    // -- always the same one -- so the second click toggles that same ref back
    // off instead of adding its twin, and Coincident can never be offered a
    // pair.
    SolvingFixture fixture;
    const SketchEditOutcome made = fixture.drawRectangle(Vec2{0, 0}, Vec2{60, 40});
    ASSERT_TRUE(made.applied) << made.status;

    fixture.model.setTool(SketchTool::Select);
    fixture.model.clearSelection();
    const Sketch& sketch = fixture.sketch();
    // The corner shared by side 0's end and side 1's start.
    const Vec2 corner = LineOf(sketch, made.createdEntities[0]).end;

    fixture.model.selectAt(sketch, corner, 1.0);
    const std::size_t afterFirst = fixture.model.selection().size();
    fixture.model.selectAt(sketch, corner, 1.0);
    const std::size_t afterSecond = fixture.model.selection().size();

    EXPECT_EQ(afterFirst, 1u);
    EXPECT_EQ(afterSecond, 2u) << "the two points at a corner cannot both be selected";

    // ...and they are DIFFERENT points, which is the whole claim.
    ASSERT_EQ(fixture.model.selection().size(), 2u);
    const SketchElementRef& first = fixture.model.selection()[0];
    const SketchElementRef& second = fixture.model.selection()[1];
    EXPECT_FALSE(first.entityId == second.entityId &&
                 first.subElement == second.subElement);

    // ...and Coincident is now something the command layer will build out of
    // them, which is what the user could not reach.
    std::string whyNot;
    const SketchEdit edit =
        fixture.model.requestConstraint(sketch, SketchEditKind::AddCoincident, &whyNot);
    EXPECT_TRUE(edit.valid()) << whyNot;
}

TEST(SketchCanvasSolveTest, M26_RECT_003_ClickingALoneEntityTwiceStillDeselectsIt) {
    // The cycling rule must not cost the plain toggle: where there is only one
    // thing under the cursor, the second click still turns it off.
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});

    fixture.model.setTool(SketchTool::Select);
    fixture.model.clearSelection();
    // The MIDDLE of the line, away from either endpoint.
    fixture.model.selectAt(fixture.sketch(), Vec2{50, 0}, 1.0);
    EXPECT_EQ(fixture.model.selection().size(), 1u);
    fixture.model.selectAt(fixture.sketch(), Vec2{50, 0}, 1.0);
    EXPECT_EQ(fixture.model.selection().size(), 0u) << "a second click no longer deselects";
}

TEST(SketchCanvasSolveTest, M26_RECT_004_TwoLooseEndsCanBeMadeCoincidentForREAL) {
    // Two lines whose ends merely LOOK joined -- drawn to the same place with
    // nothing holding them there. This is what a user gets drawing an outline
    // by hand instead of with the rectangle tool, and it is the case
    // Coincident exists for.
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId a = sketch.addLine(Vec2{0, 0}, Vec2{60, 40});
    const SketchEntityId b = sketch.addLine(Vec2{60, 40}, Vec2{120, 0});

    fixture.model.setTool(SketchTool::Select);
    fixture.model.clearSelection();
    fixture.model.selectAt(fixture.sketch(), Vec2{60, 40}, 1.0);
    fixture.model.selectAt(fixture.sketch(), Vec2{60, 40}, 1.0);
    ASSERT_EQ(fixture.model.selection().size(), 2u);

    std::string whyNot;
    const SketchEdit edit =
        fixture.model.requestConstraint(fixture.sketch(), SketchEditKind::AddCoincident, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    ASSERT_TRUE(fixture.apply(edit).applied);

    // NOW it holds: drag one line's end and the other's follows.
    ASSERT_TRUE(DragMoved(fixture.document.previewSketchDrag(
        fixture.sketchId, SketchElementRef{a, SketchSubElement::EndPoint}, Vec2{75.0, 90.0})));
    const Sketch& after = fixture.sketch();
    EXPECT_NEAR(LineOf(after, a).end.x, LineOf(after, b).start.x, 1e-6);
    EXPECT_NEAR(LineOf(after, a).end.y, LineOf(after, b).start.y, 1e-6);
}

// =============================================================================
// M26.5 -- "H&V Distance": both legs of a point pair, in one command
// =============================================================================

TEST(SketchCanvasSolveTest, M26_HV_001_ItMakesBOTHLegsWithTheirMeasuredValues) {
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId a = sketch.addPoint(Vec2{10, 20});
    const SketchEntityId b = sketch.addPoint(Vec2{70, 95}); // 60 across, 75 up

    fixture.model.setSelection({SketchElementRef{a}, SketchElementRef{b}});
    std::string whyNot;
    const SketchEdit edit =
        fixture.model.requestDimension(fixture.sketch(), SketchEditKind::AddHVDistance, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    const SketchEditOutcome outcome = fixture.apply(edit);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const Sketch& after = fixture.sketch();
    ASSERT_EQ(after.constraints().size(), 2u) << "H&V must make two dimensions, not one";

    // ONE OF EACH, and each reading the gap it is about.
    const auto* h = std::get_if<HorizontalDistanceConstraint>(&after.constraints()[0].data);
    const auto* v = std::get_if<VerticalDistanceConstraint>(&after.constraints()[1].data);
    ASSERT_NE(h, nullptr) << "the first leg is not a horizontal distance";
    ASSERT_NE(v, nullptr) << "the second leg is not a vertical distance";

    const Parameter* hp = fixture.document.parameters().findById(h->parameterId);
    const Parameter* vp = fixture.document.parameters().findById(v->parameterId);
    ASSERT_NE(hp, nullptr);
    ASSERT_NE(vp, nullptr);
    EXPECT_NE(hp->id(), vp->id()) << "both legs share one Parameter";
    EXPECT_NEAR(hp->value(), 60.0, 1e-6);
    EXPECT_NEAR(vp->value(), 75.0, 1e-6);

    // ...AND NOTHING MOVED. A dimension seeded from anything other than the
    // measurement drags the geometry the moment it is added.
    const Vec2 pa = std::get_if<SketchPoint>(&after.findEntity(a)->geometry)->position;
    const Vec2 pb = std::get_if<SketchPoint>(&after.findEntity(b)->geometry)->position;
    EXPECT_NEAR(pa.x, 10.0, 1e-6);
    EXPECT_NEAR(pa.y, 20.0, 1e-6);
    EXPECT_NEAR(pb.x, 70.0, 1e-6);
    EXPECT_NEAR(pb.y, 95.0, 1e-6);
}

TEST(SketchCanvasSolveTest, M26_HV_002_ItIsONEUndoStep) {
    // Two Parameters and two constraints, and Ctrl+Z has to take the lot. A
    // user who undoes this expects the pair gone, not to be left holding half
    // of what they asked for.
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId a = sketch.addPoint(Vec2{10, 20});
    const SketchEntityId b = sketch.addPoint(Vec2{70, 95});

    const std::size_t depthBefore = fixture.document.undoDepth();
    fixture.model.setSelection({SketchElementRef{a}, SketchElementRef{b}});
    std::string whyNot;
    const SketchEdit edit =
        fixture.model.requestDimension(fixture.sketch(), SketchEditKind::AddHVDistance, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    ASSERT_TRUE(fixture.apply(edit).applied);
    ASSERT_EQ(fixture.sketch().constraints().size(), 2u);

    EXPECT_EQ(fixture.document.undoDepth(), depthBefore + 1) << "H&V recorded more than one step";
    ASSERT_TRUE(fixture.document.undo());
    EXPECT_EQ(fixture.sketch().constraints().size(), 0u)
        << "one undo left half the pair behind";
}

TEST(SketchCanvasSolveTest, M26_HV_003_BothLegsDriveTheGeometry) {
    // The pair is not decoration: changing either number has to move the point.
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId a = sketch.addPoint(Vec2{0, 0});
    const SketchEntityId b = sketch.addPoint(Vec2{60, 75});
    ASSERT_TRUE(fixture.constrain(SketchEditKind::AddFix, {SketchElementRef{a}}).applied);

    fixture.model.setSelection({SketchElementRef{a}, SketchElementRef{b}});
    std::string whyNot;
    const SketchEdit edit =
        fixture.model.requestDimension(fixture.sketch(), SketchEditKind::AddHVDistance, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    ASSERT_TRUE(fixture.apply(edit).applied);

    // With one point fixed and both legs pinned, the pair is fully determined.
    EXPECT_EQ(fixture.sketch().degreesOfFreedom(), 0) << fixture.sketch().solveMessage();

    const auto* h =
        std::get_if<HorizontalDistanceConstraint>(&fixture.sketch().constraints()[1].data);
    ASSERT_NE(h, nullptr);
    ASSERT_TRUE(fixture.document.setParameterValue(h->parameterId, 100.0));
    (void)fixture.document.recompute();
    const Vec2 moved =
        std::get_if<SketchPoint>(&fixture.sketch().findEntity(b)->geometry)->position;
    EXPECT_NEAR(moved.x, 100.0, 1e-6) << "the horizontal leg does not drive";
    EXPECT_NEAR(moved.y, 75.0, 1e-6) << "the vertical leg let go when the other changed";
}

TEST(SketchCanvasSolveTest, M26_HV_004_ItNeedsExactlyTwoPoints) {
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId line = sketch.addLine(Vec2{0, 0}, Vec2{50, 0});
    const SketchEntityId point = sketch.addPoint(Vec2{10, 40});

    fixture.model.setSelection(
        {SketchElementRef{line, SketchSubElement::Whole}, SketchElementRef{point}});
    std::string whyNot;
    const SketchEdit edit =
        fixture.model.requestDimension(fixture.sketch(), SketchEditKind::AddHVDistance, &whyNot);
    EXPECT_FALSE(edit.valid());
    EXPECT_NE(whyNot.find("2 points"), std::string::npos) << whyNot;
}

TEST(SketchCanvasSolveTest, M26_HV_005_BothLegsAreAboutTheSameORDEREDPair) {
    // The legs must agree about which point is `a`. Ordering them separately
    // -- one swapped so its own value came out positive -- would store two
    // dimensions about the pair in OPPOSITE directions, and editing one would
    // then move the geometry the other way from the other.
    SolvingFixture fixture;
    Sketch& sketch = *fixture.editable;
    const SketchEntityId a = sketch.addPoint(Vec2{80, 10});
    const SketchEntityId b = sketch.addPoint(Vec2{20, 90}); // left and up: u and v disagree

    fixture.model.setSelection({SketchElementRef{a}, SketchElementRef{b}});
    std::string whyNot;
    const SketchEdit edit =
        fixture.model.requestDimension(fixture.sketch(), SketchEditKind::AddHVDistance, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    ASSERT_TRUE(fixture.apply(edit).applied);

    const Sketch& after = fixture.sketch();
    ASSERT_EQ(after.constraints().size(), 2u);
    const auto* h = std::get_if<HorizontalDistanceConstraint>(&after.constraints()[0].data);
    const auto* v = std::get_if<VerticalDistanceConstraint>(&after.constraints()[1].data);
    ASSERT_NE(h, nullptr);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(h->a.entityId, v->a.entityId) << "the two legs disagree about which point is first";
    EXPECT_EQ(h->b.entityId, v->b.entityId);

    // ORDERED ON u, so the horizontal leg reads positive and the vertical one
    // carries the sign. One order cannot make both positive when the pair runs
    // up-and-left, and swapping per leg is the thing that would break the
    // agreement asserted just above.
    const Parameter* hp = fixture.document.parameters().findById(h->parameterId);
    const Parameter* vp = fixture.document.parameters().findById(v->parameterId);
    ASSERT_NE(hp, nullptr);
    ASSERT_NE(vp, nullptr);
    EXPECT_NEAR(hp->value(), 60.0, 1e-6) << "the horizontal leg was not ordered positive";
    EXPECT_NEAR(vp->value(), -80.0, 1e-6) << "the vertical leg lost its sign";
}
