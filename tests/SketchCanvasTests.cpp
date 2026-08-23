// M12.1 / M12.2 -- the DECIDING half of the sketch UI.
//
// Everything here is Qt-free, which is the point: a test calls it directly.
// What it deliberately CANNOT answer is whether any of it reaches the screen --
// that is the M6.14 defect, and the viewer smoke test (`--selftest --sample
// m12-sketch`) is what answers it, by asking the widget what it painted.
//
// Nothing here needs a solver, so nothing here asserts a DOF. The solve-backed
// half of the story lives in tests/Solver/SketchCanvasSolveTests.cpp.

#include "Core/Document/PartDocument.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"
#include "Viewer/SketchCanvas.h"
#include "Viewer/SketchCommands.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kTol = 1e-9;
constexpr double kQuarterPi = 0.78539816339744830961;
constexpr double kHalfPi = 1.57079632679489661923;

// A document with one empty sketch, which is what "New Sketch" leaves behind.
struct Fixture {
    PartDocument document{"Doc"};
    ObjectId sketchId{kInvalidObjectId};

    Fixture() { sketchId = document.addSketch("Sketch1").id(); }

    const Sketch& sketch() const { return *document.findSketch(sketchId); }

    // For the kinds a SketchEdit has no tool for -- a spline built from a
    // point list, an ellipse with a chosen rotation. Through the document's own
    // entity API, which is the same door the commands under test use; the click
    // path every other helper here takes belongs to those tools' own tests.
    SketchEntityId add(SketchGeometry geometry) {
        return document.addSketchEntity(sketchId, std::move(geometry));
    }

    SketchEditOutcome apply(const SketchEdit& edit) {
        return ApplySketchEdit(document, sketchId, edit);
    }

    // Drives the model exactly as a click does, then applies whatever it
    // produced -- the same sequence SketchCanvasWidget::clickAt runs.
    SketchEditOutcome click(SketchCanvasModel& model, Vec2 point, double tolerance = 1.0) {
        // The model's suppression flag, not a hard-coded false: clickAt() reads
        // it from the model, and a helper that did not would let the modifier
        // key look tested while nothing downstream of it ever saw the flag.
        const SnapResult snap =
            SnapCursor(sketch(), point, tolerance, 0.0, model.suppressInference());
        const SketchEdit edit = model.click(snap);
        if (!edit.valid()) return SketchEditOutcome{};
        const SketchEditOutcome outcome = apply(edit);
        model.afterApply(outcome.createdEntities);
        return outcome;
    }
};

SketchEntityId LineFrom(Fixture& fixture, Vec2 a, Vec2 b) {
    SketchEdit edit;
    edit.kind = SketchEditKind::AddLine;
    edit.points = {a, b};
    edit.label = "Add line";
    const SketchEditOutcome outcome = fixture.apply(edit);
    EXPECT_TRUE(outcome.applied) << outcome.status;
    return outcome.createdEntities.empty() ? kInvalidSketchEntityId : outcome.createdEntities.front();
}

SketchEntityId CircleAt(Fixture& fixture, Vec2 center, double radius) {
    SketchEdit edit;
    edit.kind = SketchEditKind::AddCircle;
    edit.points = {center, Vec2{center.x + radius, center.y}};
    edit.label = "Add circle";
    const SketchEditOutcome outcome = fixture.apply(edit);
    EXPECT_TRUE(outcome.applied) << outcome.status;
    return outcome.createdEntities.empty() ? kInvalidSketchEntityId : outcome.createdEntities.front();
}

} // namespace

// =============================================================================
// The view transform
// =============================================================================

TEST(SketchCanvasViewTest, M12_VIEW_001_PixelsAndMillimetresRoundTrip) {
    CanvasView view;
    view.widthPx = 800;
    view.heightPx = 600;
    view.pixelsPerMm = 3.0;
    view.centerMm = Vec2{10.0, -5.0};

    const Vec2 pixels = view.toPixels(Vec2{25.0, 40.0});
    const Vec2 back = view.toSketch(pixels);
    EXPECT_NEAR(back.x, 25.0, 1e-9);
    EXPECT_NEAR(back.y, 40.0, 1e-9);
}

TEST(SketchCanvasViewTest, M12_VIEW_002_SketchVIsUpAndPixelYIsDown) {
    CanvasView view;
    view.widthPx = 800;
    view.heightPx = 600;
    // A point ABOVE the centre must land at a SMALLER pixel y. Getting this
    // backwards produces a canvas that draws every sketch mirrored, and every
    // round-trip test still passes because the flip is its own inverse.
    const Vec2 higher = view.toPixels(Vec2{0.0, 10.0});
    const Vec2 lower = view.toPixels(Vec2{0.0, -10.0});
    EXPECT_LT(higher.y, lower.y);
}

TEST(SketchCanvasViewTest, M12_VIEW_003_ZoomKeepsThePointUnderTheCursor) {
    CanvasView view;
    view.widthPx = 800;
    view.heightPx = 600;
    view.pixelsPerMm = 4.0;

    const Vec2 cursor{620.0, 130.0};
    const Vec2 before = view.toSketch(cursor);
    view.zoomAt(cursor, 1.15);
    const Vec2 after = view.toSketch(cursor);
    EXPECT_NEAR(before.x, after.x, 1e-9);
    EXPECT_NEAR(before.y, after.y, 1e-9);
    EXPECT_GT(view.pixelsPerMm, 4.0);
}

TEST(SketchCanvasViewTest, M12_VIEW_004_ZoomIsClampedSoCoordinatesStayFinite) {
    CanvasView view;
    for (int i = 0; i < 500; ++i) view.zoomAt(Vec2{400.0, 300.0}, 4.0);
    EXPECT_LE(view.pixelsPerMm, kMaxPixelsPerMm);
    for (int i = 0; i < 500; ++i) view.zoomAt(Vec2{400.0, 300.0}, 0.25);
    EXPECT_GE(view.pixelsPerMm, kMinPixelsPerMm);
    const Vec2 sketchPoint = view.toSketch(Vec2{0.0, 0.0});
    EXPECT_TRUE(std::isfinite(sketchPoint.x));
    EXPECT_TRUE(std::isfinite(sketchPoint.y));
}

TEST(SketchCanvasViewTest, M12_VIEW_005_FittingAnEmptySketchDoesNotProduceAnInfiniteScale) {
    Fixture fixture;
    const CanvasView view = FitView(fixture.sketch(), 800, 600);
    EXPECT_TRUE(std::isfinite(view.pixelsPerMm));
    EXPECT_GT(view.pixelsPerMm, 0.0);
    EXPECT_LE(view.pixelsPerMm, kMaxPixelsPerMm);
}

TEST(SketchCanvasViewTest, M12_VIEW_006_GridStepIsNeverZero) {
    CanvasView view;
    view.pixelsPerMm = kMaxPixelsPerMm;
    EXPECT_GT(GridStepMm(view), 0.0);
    view.pixelsPerMm = kMinPixelsPerMm;
    EXPECT_GT(GridStepMm(view), 0.0);
}

// =============================================================================
// Snapping and picking
// =============================================================================

TEST(SketchSnapTest, M12_SNAP_001_ADefinedPointBeatsTheCurveItSitsOn) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});

    // Just past the line's END, which is also ON the line. Without the point
    // priority the user could never attach a constraint to an endpoint.
    const SnapResult snap = SnapCursor(fixture.sketch(), Vec2{99.5, 0.1}, 2.0, 0.0, false);
    EXPECT_EQ(snap.kind, SnapKind::Endpoint);
    EXPECT_EQ(snap.ref.entityId, line);
    EXPECT_EQ(snap.ref.subElement, SketchSubElement::EndPoint);
    EXPECT_NEAR(snap.point.x, 100.0, kTol);
}

TEST(SketchSnapTest, M12_SNAP_002_SuppressionReturnsTheRawPointAndNoReference) {
    Fixture fixture;
    LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});

    const SnapResult snap = SnapCursor(fixture.sketch(), Vec2{99.5, 0.1}, 2.0, 0.0, /*suppress=*/true);
    EXPECT_EQ(snap.kind, SnapKind::Free);
    EXPECT_FALSE(snap.hasRef());
    EXPECT_NEAR(snap.point.x, 99.5, kTol);
    EXPECT_NEAR(snap.point.y, 0.1, kTol);
}

TEST(SketchSnapTest, M12_SNAP_003_OnCurveSnappingCarriesNoReference) {
    Fixture fixture;
    CircleAt(fixture, Vec2{0.0, 0.0}, 50.0);

    // Near the rim, far from the centre. A reference here would resolve to the
    // circle's RADIUS variable, and an inferred coincidence would then
    // constrain a point to a radius.
    const SnapResult snap = SnapCursor(fixture.sketch(), Vec2{49.0, 0.0}, 3.0, 0.0, false);
    EXPECT_EQ(snap.kind, SnapKind::OnCurve);
    EXPECT_FALSE(snap.hasRef());
    EXPECT_NEAR(snap.point.x, 50.0, 1e-6);
}

TEST(SketchSnapTest, M12_SNAP_004_TheOriginSnapsAndHasNoEntity) {
    Fixture fixture;
    const SnapResult snap = SnapCursor(fixture.sketch(), Vec2{0.4, -0.3}, 2.0, 0.0, false);
    EXPECT_EQ(snap.kind, SnapKind::Origin);
    EXPECT_FALSE(snap.hasRef());
    EXPECT_NEAR(snap.point.x, 0.0, kTol);
    EXPECT_NEAR(snap.point.y, 0.0, kTol);
}

TEST(SketchSnapTest, M12_SNAP_005_TheGridIsTheLastResortAndOnlyWhenEnabled) {
    Fixture fixture;
    const SnapResult withGrid =
        SnapCursor(fixture.sketch(), Vec2{40.4, 19.6}, 2.0, /*gridMm=*/10.0, false);
    EXPECT_EQ(withGrid.kind, SnapKind::Grid);
    EXPECT_NEAR(withGrid.point.x, 40.0, kTol);
    EXPECT_NEAR(withGrid.point.y, 20.0, kTol);

    const SnapResult withoutGrid =
        SnapCursor(fixture.sketch(), Vec2{40.4, 19.6}, 2.0, /*gridMm=*/0.0, false);
    EXPECT_EQ(withoutGrid.kind, SnapKind::Free);
}

TEST(SketchSnapTest, M12_SNAP_006_AnArcOffersItsCentreANDBothTips) {
    Fixture fixture;
    SketchEdit edit;
    edit.kind = SketchEditKind::AddArc;
    edit.points = {Vec2{0.0, 0.0}, Vec2{20.0, 0.0}, Vec2{0.0, 20.0}};
    edit.label = "Add arc";
    const SketchEditOutcome outcome = fixture.apply(edit);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    const SketchEntityId arc = outcome.createdEntities.front();

    const SnapResult centre = SnapCursor(fixture.sketch(), Vec2{0.2, 0.2}, 2.0, 0.0, false);
    EXPECT_EQ(centre.kind, SnapKind::CenterPoint);
    EXPECT_EQ(centre.ref.subElement, SketchSubElement::CenterPoint);

    // THE ARC'S TIPS ARE POINTS NOW, and this test used to say the opposite.
    //
    // Its reason was that the solver had no variable for them -- true when it
    // was written, and false since ADR-M17-018 made an arc's tips real
    // variables held by ArcTipU/V. Everything since has depended on that: the
    // fillet's joints, the slot's corners, the tangent arc's chain. What had
    // not caught up was the SNAPPER, so a user could not click an arc's end to
    // start something from it -- the one gesture all of that geometry is for.
    const SnapResult tip = SnapCursor(fixture.sketch(), Vec2{19.8, 0.0}, 2.0, 0.0, false);
    EXPECT_EQ(tip.kind, SnapKind::Endpoint);
    ASSERT_TRUE(tip.hasRef());
    EXPECT_EQ(tip.ref.entityId, arc);
    EXPECT_EQ(tip.ref.subElement, SketchSubElement::StartPoint);
    EXPECT_TRUE(IsPointRef(fixture.sketch(),
                           SketchElementRef{arc, SketchSubElement::StartPoint}));
    // ...and it lands ON the tip, not merely near it.
    EXPECT_NEAR(tip.point.x, 20.0, 1e-9);
    EXPECT_NEAR(tip.point.y, 0.0, 1e-9);
}

TEST(SketchSnapTest, M12_SNAP_007_HitTestRanksPointsAheadOfCurves) {
    Fixture fixture;
    LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const std::vector<SketchElementRef> hits =
        HitTest(fixture.sketch(), Vec2{0.2, 0.0}, 5.0);
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits.front().subElement, SketchSubElement::StartPoint);
}

// =============================================================================
// The tool state machine
// =============================================================================

TEST(SketchToolTest, M12_TOOL_001_ALineNeedsTwoClicksAndProducesOneEntity) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Line);

    EXPECT_FALSE(fixture.click(model, Vec2{0.0, 0.0}).applied);   // first click: nothing yet
    const SketchEditOutcome second = fixture.click(model, Vec2{50.0, 0.0});
    EXPECT_TRUE(second.applied) << second.status;
    EXPECT_EQ(fixture.sketch().entities().size(), 1u);
}

TEST(SketchToolTest, M12_TOOL_002_ChainedLinesAreJoinedByAConstraintNotByCoordinates) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Line);

    fixture.click(model, Vec2{0.0, 0.0});
    const SketchEditOutcome first = fixture.click(model, Vec2{50.0, 0.0});
    ASSERT_TRUE(first.applied) << first.status;
    // The chain start needs no second click: it is already pending.
    const SketchEditOutcome second = fixture.click(model, Vec2{50.0, 40.0});
    ASSERT_TRUE(second.applied) << second.status;

    EXPECT_EQ(fixture.sketch().entities().size(), 2u);
    // THE POINT OF THIS TEST: the corner is a real Coincident constraint, so
    // moving it later moves both segments. Two lines that merely agree
    // numerically come apart on the first solve.
    ASSERT_EQ(second.createdConstraints.size(), 1u);
    const SketchConstraint* constraint =
        fixture.sketch().findConstraint(second.createdConstraints.front());
    ASSERT_NE(constraint, nullptr);
    EXPECT_TRUE(std::holds_alternative<CoincidentConstraint>(constraint->data));
}

TEST(SketchToolTest, M12_TOOL_003_AFailedApplyLeavesTheChainWithoutAnInventedReference) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Line);

    // Driven WITHOUT the fixture helper, because the helper reports success
    // honestly and this test is about the failure path: the edit is produced,
    // applied, and then afterApply is told -- truthfully -- that nothing came
    // back, which is what a refused apply looks like.
    SnapResult first;
    first.point = Vec2{0.0, 0.0};
    ASSERT_FALSE(model.click(first).valid());
    SnapResult second;
    second.point = Vec2{50.0, 0.0};
    const SketchEdit lineEdit = model.click(second);
    ASSERT_TRUE(lineEdit.valid());
    ASSERT_TRUE(fixture.apply(lineEdit).applied);
    model.afterApply({});

    const SketchEditOutcome chained = fixture.click(model, Vec2{50.0, 40.0});
    ASSERT_TRUE(chained.applied) << chained.status;
    // No coincidence, because there was no id to attach one to -- and inventing
    // one would attach the chain to whatever entity happened to be first.
    EXPECT_TRUE(chained.createdConstraints.empty());
}

TEST(SketchToolTest, M17_CHAIN_001_UndoingMidChainDoesNotBreakTheNextSegment) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Line);

    fixture.click(model, Vec2{10.0, 10.0});
    ASSERT_TRUE(fixture.click(model, Vec2{50.0, 10.0}).applied);

    // Ctrl+Z in the middle of a polyline. The chain is still holding a
    // reference to the line that just went away.
    ASSERT_TRUE(fixture.document.undo());
    ASSERT_EQ(fixture.sketch().entities().size(), 0u);

    // The next click must still draw. Without the guard the inferred
    // coincidence names a deleted entity, Sketch::addConstraint refuses it, and
    // ApplySketchEdit fails the WHOLE edit -- so the user cannot draw at all
    // until they press Esc, with a message about a constraint they never asked
    // for.
    const SketchEditOutcome next = fixture.click(model, Vec2{50.0, 60.0});
    EXPECT_TRUE(next.applied) << next.status;
    EXPECT_EQ(fixture.sketch().entities().size(), 1u);
    // And no coincidence was invented against the entity that is gone.
    EXPECT_TRUE(next.createdConstraints.empty());
}

TEST(SketchToolTest, M12_TOOL_004_EscapeLeavesTheToolInOnePress) {
    SketchCanvasModel model;
    model.setTool(SketchTool::Line);
    SnapResult snap;
    snap.point = Vec2{5.0, 5.0};
    model.click(snap);
    EXPECT_EQ(model.pendingPoints().size(), 1u);

    // ONE press: the half-drawn line goes AND the arrow comes back. Two presses
    // meant a user who pressed Esc once and then clicked to select something
    // drew a stray line instead.
    EXPECT_TRUE(model.cancel());
    EXPECT_TRUE(model.pendingPoints().empty());
    EXPECT_EQ(model.tool(), SketchTool::Select);

    // Under Select, Esc then clears the selection, and then has nothing to do.
    model.setSelection({SketchElementRef{static_cast<SketchEntityId>(7),
                                         SketchSubElement::Whole}});
    EXPECT_TRUE(model.cancel());
    EXPECT_TRUE(model.selection().empty());
    EXPECT_FALSE(model.cancel());
}

TEST(SketchToolTest, M12_TOOL_005_SwitchingToolsDiscardsAHalfDrawnShape) {
    SketchCanvasModel model;
    model.setTool(SketchTool::Line);
    SnapResult snap;
    snap.point = Vec2{5.0, 5.0};
    model.click(snap);
    model.setTool(SketchTool::Circle);
    EXPECT_TRUE(model.pendingPoints().empty());
}

TEST(SketchToolTest, M12_TOOL_006_ADegenerateSecondClickIsDroppedRatherThanApplied) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Circle);
    fixture.click(model, Vec2{10.0, 10.0});
    // Same point again: a zero radius. The click is discarded and the tool
    // keeps waiting, instead of failing a command the user cannot interpret.
    const SketchEditOutcome outcome = fixture.click(model, Vec2{10.0, 10.0});
    EXPECT_FALSE(outcome.applied);
    EXPECT_EQ(fixture.sketch().entities().size(), 0u);
    EXPECT_EQ(model.pendingPoints().size(), 1u);
}

TEST(SketchToolTest, M12_TOOL_007_SelectionTogglesWithNoModifierKey) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchCanvasModel model;

    const SketchElementRef whole{line, SketchSubElement::Whole};
    model.toggleSelection(whole);
    EXPECT_TRUE(model.isSelected(whole));
    EXPECT_EQ(model.selection().size(), 1u);
    model.toggleSelection(whole);
    EXPECT_FALSE(model.isSelected(whole));
}

TEST(SketchToolTest, M12_TOOL_008_APromptIsNeverEmpty) {
    SketchCanvasModel model;
    for (const SketchTool tool : {SketchTool::Select, SketchTool::Point, SketchTool::Line,
                                  SketchTool::Rectangle, SketchTool::Circle, SketchTool::Arc}) {
        model.setTool(tool);
        EXPECT_FALSE(model.prompt().empty()) << SketchToolName(tool);
    }
}

TEST(SketchToolTest, M12_TOOL_009_SuppressionIsVisibleInThePrompt) {
    SketchCanvasModel model;
    model.setTool(SketchTool::Line);
    const std::string plain = model.prompt();
    model.setSuppressInference(true);
    const std::string suppressed = model.prompt();
    EXPECT_NE(plain, suppressed);
    EXPECT_NE(suppressed.find("suppressed"), std::string::npos);
}

// =============================================================================
// Rectangle: four lines plus constraints, not a rectangle type
// =============================================================================

TEST(SketchRectangleTest, M12_RECT_001_ProducesFourLinesAndEightConstraints) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Rectangle);
    // Away from the origin, so this test measures the rectangle alone. The
    // corner-on-origin case is M12_ORIGIN_003's.
    fixture.click(model, Vec2{10.0, 10.0});
    const SketchEditOutcome outcome = fixture.click(model, Vec2{110.0, 60.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    EXPECT_EQ(outcome.createdEntities.size(), 4u);
    // 2 horizontal + 2 vertical + 4 coincident corners.
    EXPECT_EQ(outcome.createdConstraints.size(), 8u);
    for (const SketchEntityId id : outcome.createdEntities) {
        const SketchEntity* entity = fixture.sketch().findEntity(id);
        ASSERT_NE(entity, nullptr);
        EXPECT_TRUE(std::holds_alternative<SketchLine>(entity->geometry));
    }
}

TEST(SketchRectangleTest, M12_RECT_002_TheWholeRectangleIsOneUndoStep) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Rectangle);
    // On the origin ON PURPOSE here: the inferred Fix is part of the same edit,
    // so it has to come and go with the rectangle in ONE undo step rather than
    // surviving as an orphan pinning a point that no longer exists.
    fixture.click(model, Vec2{0.0, 0.0});
    ASSERT_TRUE(fixture.click(model, Vec2{100.0, 50.0}).applied);
    ASSERT_EQ(fixture.sketch().entities().size(), 4u);

    ASSERT_TRUE(fixture.document.undo());
    EXPECT_EQ(fixture.sketch().entities().size(), 0u);
    EXPECT_EQ(fixture.sketch().constraints().size(), 0u);

    ASSERT_TRUE(fixture.document.redo());
    EXPECT_EQ(fixture.sketch().entities().size(), 4u);
    EXPECT_EQ(fixture.sketch().constraints().size(), 9u);
}

TEST(SketchRectangleTest, M12_RECT_003_ADegenerateRectangleIsRefusedNotStored) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Rectangle);
    fixture.click(model, Vec2{0.0, 0.0});
    const SketchEditOutcome outcome = fixture.click(model, Vec2{100.0, 0.0}); // zero height
    EXPECT_FALSE(outcome.applied);
    EXPECT_EQ(fixture.sketch().entities().size(), 0u);
}

namespace {

// The coincidences among `outcome`'s constraints, in creation order.
std::vector<CoincidentConstraint> CoincidencesIn(Fixture& fixture,
                                                 const SketchEditOutcome& outcome) {
    std::vector<CoincidentConstraint> found;
    for (const SketchConstraintId id : outcome.createdConstraints) {
        const SketchConstraint* constraint = fixture.sketch().findConstraint(id);
        if (constraint == nullptr) continue;
        if (const auto* c = std::get_if<CoincidentConstraint>(&constraint->data))
            found.push_back(*c);
    }
    return found;
}

// True when `pair` joins `ref` to something, in either order.
bool Joins(const CoincidentConstraint& pair, const SketchElementRef& ref) {
    const auto same = [&](const SketchElementRef& other) {
        return other.entityId == ref.entityId && other.subElement == ref.subElement;
    };
    return same(pair.a) || same(pair.b);
}

} // namespace

TEST(SketchRectangleTest, M12_RECT_004_AFirstCornerOnAnExistingPointIsJoinedToIt) {
    Fixture fixture;
    const SketchEntityId existing = LineFrom(fixture, Vec2{-40.0, 10.0}, Vec2{10.0, 10.0});
    const SketchElementRef anchor{existing, SketchSubElement::EndPoint};

    SketchCanvasModel model;
    model.setTool(SketchTool::Rectangle);
    fixture.click(model, Vec2{10.2, 9.9}); // within tolerance of the line's end
    const SketchEditOutcome outcome = fixture.click(model, Vec2{110.0, 60.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;
    ASSERT_EQ(outcome.createdEntities.size(), 4u);

    // The rectangle's own four corner coincidences, plus the one that joins it
    // to the line already in the sketch.
    EXPECT_EQ(outcome.createdConstraints.size(), 9u);
    const std::vector<CoincidentConstraint> joints = CoincidencesIn(fixture, outcome);
    ASSERT_EQ(joints.size(), 5u);

    const SketchElementRef corner{outcome.createdEntities[0], SketchSubElement::StartPoint};
    const auto joined = std::find_if(
        joints.begin(), joints.end(), [&](const CoincidentConstraint& pair) {
            return Joins(pair, anchor);
        });
    ASSERT_NE(joined, joints.end()) << "the snapped corner was not joined to the line";
    EXPECT_TRUE(Joins(*joined, corner))
        << "the coincidence to the line does not name side 0's start point";

    // And the corner really moved to the snapped position, not the raw click.
    const SketchEntity* side0 = fixture.sketch().findEntity(corner.entityId);
    ASSERT_NE(side0, nullptr);
    EXPECT_NEAR(std::get<SketchLine>(side0->geometry).start.x, 10.0, 1e-6);
    EXPECT_NEAR(std::get<SketchLine>(side0->geometry).start.y, 10.0, 1e-6);
}

TEST(SketchRectangleTest, M12_RECT_005_ASecondCornerOnAnExistingPointIsJoinedOnItsOwnSide) {
    Fixture fixture;
    const SketchEntityId existing = LineFrom(fixture, Vec2{200.0, 90.0}, Vec2{110.0, 60.0});
    const SketchElementRef anchor{existing, SketchSubElement::EndPoint};

    SketchCanvasModel model;
    model.setTool(SketchTool::Rectangle);
    fixture.click(model, Vec2{10.0, 10.0});
    const SketchEditOutcome outcome = fixture.click(model, Vec2{109.8, 60.1});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    EXPECT_EQ(outcome.createdConstraints.size(), 9u);
    const std::vector<CoincidentConstraint> joints = CoincidencesIn(fixture, outcome);
    const auto joined = std::find_if(
        joints.begin(), joints.end(), [&](const CoincidentConstraint& pair) {
            return Joins(pair, anchor);
        });
    ASSERT_NE(joined, joints.end());
    // Side 2, not side 0. Getting this wrong joins the sketch by a corner
    // diagonally opposite the one the user pointed at -- and it looks right
    // until something moves.
    EXPECT_TRUE(Joins(*joined,
                      SketchElementRef{outcome.createdEntities[2], SketchSubElement::StartPoint}));
}

TEST(SketchRectangleTest, M12_RECT_006_BothCornersCanBeJoinedAtOnce) {
    Fixture fixture;
    const SketchEntityId diagonal = LineFrom(fixture, Vec2{10.0, 10.0}, Vec2{110.0, 60.0});

    SketchCanvasModel model;
    model.setTool(SketchTool::Rectangle);
    fixture.click(model, Vec2{10.0, 10.0});
    const SketchEditOutcome outcome = fixture.click(model, Vec2{110.0, 60.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // Four internal corners plus two joins.
    EXPECT_EQ(outcome.createdConstraints.size(), 10u);
    const std::vector<CoincidentConstraint> joints = CoincidencesIn(fixture, outcome);
    ASSERT_EQ(joints.size(), 6u);
    EXPECT_TRUE(std::any_of(joints.begin(), joints.end(), [&](const CoincidentConstraint& p) {
        return Joins(p, SketchElementRef{diagonal, SketchSubElement::StartPoint});
    }));
    EXPECT_TRUE(std::any_of(joints.begin(), joints.end(), [&](const CoincidentConstraint& p) {
        return Joins(p, SketchElementRef{diagonal, SketchSubElement::EndPoint});
    }));
}

TEST(SketchRectangleTest, M12_RECT_007_SuppressionLeavesTheRectangleUnjoined) {
    Fixture fixture;
    LineFrom(fixture, Vec2{-40.0, 10.0}, Vec2{10.0, 10.0});

    SketchCanvasModel model;
    model.setTool(SketchTool::Rectangle);
    model.setSuppressInference(true);
    fixture.click(model, Vec2{10.0, 10.0});
    const SketchEditOutcome outcome = fixture.click(model, Vec2{110.0, 60.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // Its own 8 and nothing else: the modifier is the documented way to draw
    // geometry that happens to touch without being related to it (4.2 point 3).
    EXPECT_EQ(outcome.createdConstraints.size(), 8u);
}

TEST(SketchRectangleTest, M12_RECT_008_AnOriginCornerFixesAndDoesNotAlsoJoin) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Rectangle);
    fixture.click(model, Vec2{0.0, 0.0});
    const SketchEditOutcome outcome = fixture.click(model, Vec2{100.0, 50.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // The two inferences are mutually exclusive by construction: SnapCursor
    // only reaches the origin once no defined point was in range, so a corner
    // is either joined to something or pinned, never both.
    EXPECT_EQ(outcome.createdConstraints.size(), 9u);
    EXPECT_EQ(CoincidencesIn(fixture, outcome).size(), 4u);
}

// =============================================================================
// Snapping a drawn point onto the origin (roadmap 4.2)
//
// The origin is the only snap target with no element behind it, so the
// coincidence every other snap earns cannot express it. Fix is what does, and
// 4.2 is what says it must exist at all: a snap that moves the cursor without
// producing a SketchConstraintId is drawing-time magnetism, and it makes the
// DOF readout lie about a sketch the user believes is anchored.
// =============================================================================

namespace {

using namespace paramcad;

// The one Fix among `outcome`'s constraints, or a null pointer. Fails the test
// when there is more than one -- two Fixes where one was earned is the
// redundancy 8.2 requires a sketch to keep distinguishable from a conflict.
const SketchConstraint* SoleFix(Fixture& fixture, const SketchEditOutcome& outcome) {
    const SketchConstraint* found = nullptr;
    for (const SketchConstraintId id : outcome.createdConstraints) {
        const SketchConstraint* constraint = fixture.sketch().findConstraint(id);
        if (constraint == nullptr) continue;
        if (!std::holds_alternative<FixConstraint>(constraint->data)) continue;
        EXPECT_EQ(found, nullptr) << "more than one Fix was inferred";
        found = constraint;
    }
    return found;
}

} // namespace

TEST(SketchOriginFixTest, M12_ORIGIN_001_ALineStartedOnTheOriginIsFixedThere) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Line);

    fixture.click(model, Vec2{0.2, -0.1}); // within tolerance of (0,0)
    const SketchEditOutcome outcome = fixture.click(model, Vec2{50.0, 0.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;
    ASSERT_EQ(outcome.createdEntities.size(), 1u);

    const SketchConstraint* fix = SoleFix(fixture, outcome);
    ASSERT_NE(fix, nullptr) << "the click on the origin produced no constraint at all";
    const FixConstraint& data = std::get<FixConstraint>(fix->data);
    EXPECT_EQ(data.target.entityId, outcome.createdEntities.front());
    // The START, not the line: a Fix on the whole line would pin one end and
    // claim to have pinned both.
    EXPECT_EQ(data.target.subElement, SketchSubElement::StartPoint);

    // And it really is AT the origin -- the snap moved the click there, so a
    // Fix that pinned the raw cursor position would anchor the sketch 0.2 mm
    // off and nothing on screen would show it.
    const SketchEntity* line = fixture.sketch().findEntity(outcome.createdEntities.front());
    ASSERT_NE(line, nullptr);
    EXPECT_NEAR(std::get<SketchLine>(line->geometry).start.x, 0.0, kTol);
    EXPECT_NEAR(std::get<SketchLine>(line->geometry).start.y, 0.0, kTol);
}

TEST(SketchOriginFixTest, M12_ORIGIN_002_ALineEndedOnTheOriginIsFixedAtItsEnd) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Line);

    fixture.click(model, Vec2{50.0, 30.0});
    const SketchEditOutcome outcome = fixture.click(model, Vec2{0.0, 0.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const SketchConstraint* fix = SoleFix(fixture, outcome);
    ASSERT_NE(fix, nullptr);
    EXPECT_EQ(std::get<FixConstraint>(fix->data).target.subElement, SketchSubElement::EndPoint);
}

TEST(SketchOriginFixTest, M12_ORIGIN_003_ARectangleCornerOnTheOriginFixesThatCornerOnly) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Rectangle);
    fixture.click(model, Vec2{0.0, 0.0});
    const SketchEditOutcome outcome = fixture.click(model, Vec2{100.0, 50.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // The rectangle's own 8, plus one.
    EXPECT_EQ(outcome.createdConstraints.size(), 9u);
    const SketchConstraint* fix = SoleFix(fixture, outcome);
    ASSERT_NE(fix, nullptr);
    // Side 0 runs from the first clicked corner, so that corner is its START.
    // Getting this wrong pins the rectangle by a corner the user never touched,
    // which looks identical until the sketch is dragged.
    const FixConstraint& data = std::get<FixConstraint>(fix->data);
    EXPECT_EQ(data.target.entityId, outcome.createdEntities.front());
    EXPECT_EQ(data.target.subElement, SketchSubElement::StartPoint);

    const SketchEntity* side0 = fixture.sketch().findEntity(data.target.entityId);
    ASSERT_NE(side0, nullptr);
    EXPECT_NEAR(std::get<SketchLine>(side0->geometry).start.x, 0.0, kTol);
    EXPECT_NEAR(std::get<SketchLine>(side0->geometry).start.y, 0.0, kTol);
}

TEST(SketchOriginFixTest, M12_ORIGIN_004_TheSecondClickedCornerIsFixedOnItsOwnSide) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Rectangle);
    fixture.click(model, Vec2{-100.0, -50.0});
    const SketchEditOutcome outcome = fixture.click(model, Vec2{0.0, 0.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    ASSERT_EQ(outcome.createdEntities.size(), 4u);
    const SketchConstraint* fix = SoleFix(fixture, outcome);
    ASSERT_NE(fix, nullptr);
    // The opposite corner belongs to side 2, not side 0.
    const FixConstraint& data = std::get<FixConstraint>(fix->data);
    EXPECT_EQ(data.target.entityId, outcome.createdEntities[2]);
    EXPECT_EQ(data.target.subElement, SketchSubElement::StartPoint);

    const SketchEntity* side2 = fixture.sketch().findEntity(data.target.entityId);
    ASSERT_NE(side2, nullptr);
    EXPECT_NEAR(std::get<SketchLine>(side2->geometry).start.x, 0.0, kTol);
    EXPECT_NEAR(std::get<SketchLine>(side2->geometry).start.y, 0.0, kTol);
}

TEST(SketchOriginFixTest, M12_ORIGIN_005_ACircleCentredOnTheOriginFixesItsCentre) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Circle);
    fixture.click(model, Vec2{0.0, 0.0});
    const SketchEditOutcome outcome = fixture.click(model, Vec2{25.0, 0.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const SketchConstraint* fix = SoleFix(fixture, outcome);
    ASSERT_NE(fix, nullptr);
    EXPECT_EQ(std::get<FixConstraint>(fix->data).target.subElement, SketchSubElement::CenterPoint);
}

TEST(SketchOriginFixTest, M12_ORIGIN_006_ARimClickOnTheOriginFixesNothing) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Circle);
    fixture.click(model, Vec2{25.0, 0.0});
    const SketchEditOutcome outcome = fixture.click(model, Vec2{0.0, 0.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // A circle has no point at its rim -- only a radius. Fixing the centre
    // because the RIM was clicked would pin a point 25 mm from where the user
    // pointed, and fixing "the rim" is not something the model can express.
    EXPECT_EQ(SoleFix(fixture, outcome), nullptr);
}

TEST(SketchOriginFixTest, M12_ORIGIN_007_AnArcTipOnTheOriginFixesNothing) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Arc);
    fixture.click(model, Vec2{40.0, 0.0}); // centre
    fixture.click(model, Vec2{40.0, 30.0}); // radius 30
    const SketchEditOutcome outcome = fixture.click(model, Vec2{0.0, 0.0}); // end: an ANGLE only
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // The third click contributes an angle; the tip lands on the radius the
    // first two clicks set, 30 mm from the centre and nowhere near the origin.
    // A Fix here would pin that projected point and call it the origin.
    EXPECT_EQ(SoleFix(fixture, outcome), nullptr);
}

TEST(SketchOriginFixTest, M12_ORIGIN_008_SuppressionSuppressesTheFixToo) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Line);
    model.setSuppressInference(true);

    fixture.click(model, Vec2{0.0, 0.0});
    const SketchEditOutcome outcome = fixture.click(model, Vec2{50.0, 0.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;
    // Roadmap 4.2 point 3: the modifier suppresses INFERENCE, and an inferred
    // Fix is inference. A suppression that still anchored the sketch would be
    // the one constraint a user cannot see themselves creating.
    EXPECT_TRUE(outcome.createdConstraints.empty());
}

TEST(SketchOriginFixTest, M12_ORIGIN_009_ASecondPointAtTheOriginCoincidesRatherThanFixingAgain) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Line);

    fixture.click(model, Vec2{0.0, 0.0});
    const SketchEditOutcome first = fixture.click(model, Vec2{50.0, 0.0});
    ASSERT_TRUE(first.applied) << first.status;
    ASSERT_NE(SoleFix(fixture, first), nullptr);

    model.cancel(); // end the chain
    model.setTool(SketchTool::Line);
    fixture.click(model, Vec2{0.0, 0.0});
    const SketchEditOutcome second = fixture.click(model, Vec2{0.0, 50.0});
    ASSERT_TRUE(second.applied) << second.status;

    // The first line's start is now a DEFINED POINT sitting at (0,0), and
    // defined points outrank the origin in SnapCursor. So the second line joins
    // it instead of being independently pinned -- which is the difference
    // between a corner that moves as one and two points that merely agree.
    EXPECT_EQ(SoleFix(fixture, second), nullptr);
    ASSERT_EQ(second.createdConstraints.size(), 1u);
    const SketchConstraint* joint = fixture.sketch().findConstraint(second.createdConstraints[0]);
    ASSERT_NE(joint, nullptr);
    EXPECT_TRUE(std::holds_alternative<CoincidentConstraint>(joint->data));
}

TEST(SketchOriginFixTest, M12_ORIGIN_010_AChainedLineIsNotFixedASecondTime) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Line);

    fixture.click(model, Vec2{50.0, 0.0});
    const SketchEditOutcome first = fixture.click(model, Vec2{0.0, 0.0}); // ends on the origin
    ASSERT_TRUE(first.applied) << first.status;
    ASSERT_NE(SoleFix(fixture, first), nullptr);

    // The chain carries that end into the next segment. It is already fixed;
    // a second Fix on the far side of the chain's coincidence would be a
    // redundant constraint the user never asked for.
    const SketchEditOutcome second = fixture.click(model, Vec2{0.0, 40.0});
    ASSERT_TRUE(second.applied) << second.status;
    EXPECT_EQ(SoleFix(fixture, second), nullptr);
    ASSERT_EQ(second.createdConstraints.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<CoincidentConstraint>(
        fixture.sketch().findConstraint(second.createdConstraints[0])->data));
}

TEST(SketchOriginFixTest, M12_ORIGIN_011_ThePointToolFixesAPointDroppedOnTheOrigin) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Point);
    const SketchEditOutcome outcome = fixture.click(model, Vec2{0.3, 0.3});
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const SketchConstraint* fix = SoleFix(fixture, outcome);
    ASSERT_NE(fix, nullptr);
    EXPECT_EQ(std::get<FixConstraint>(fix->data).target.entityId,
              outcome.createdEntities.front());
}

TEST(SketchOriginFixTest, M12_ORIGIN_012_TheFixIsAnOrdinaryConstraintAndCanBeDeleted) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Line);
    fixture.click(model, Vec2{0.0, 0.0});
    const SketchEditOutcome outcome = fixture.click(model, Vec2{50.0, 0.0});
    ASSERT_TRUE(outcome.applied) << outcome.status;
    const SketchConstraint* fix = SoleFix(fixture, outcome);
    ASSERT_NE(fix, nullptr);

    // Roadmap 6.2: an automatically generated constraint is a first-class one.
    // It has an id, it is listed, and the user can throw it away -- an anchor
    // that cannot be removed is a sketch that cannot be re-anchored.
    SketchEdit remove;
    remove.kind = SketchEditKind::DeleteConstraints;
    remove.constraints = {fix->id};
    remove.label = "Delete constraint";
    ASSERT_TRUE(fixture.apply(remove).applied);
    EXPECT_EQ(fixture.sketch().findConstraint(fix->id), nullptr);
    // The line it was pinning is untouched.
    EXPECT_EQ(fixture.sketch().entities().size(), 1u);
}

// =============================================================================
// Symmetric and Mirror (M17)
// =============================================================================

TEST(SketchMirrorTest, M17_SYM_001_SymmetricNeeds2PointsAndALine) {
    Fixture fixture;
    const SketchEntityId axis = LineFrom(fixture, Vec2{50.0, -50.0}, Vec2{50.0, 50.0});
    const SketchEntityId a = LineFrom(fixture, Vec2{10.0, 0.0}, Vec2{20.0, 0.0});

    SketchCanvasModel model;
    std::string whyNot;
    // Two points and no line.
    model.setSelection({SketchElementRef{a, SketchSubElement::StartPoint},
                        SketchElementRef{a, SketchSubElement::EndPoint}});
    EXPECT_FALSE(model.requestConstraint(fixture.sketch(), SketchEditKind::AddSymmetric, &whyNot)
                     .valid());
    EXPECT_FALSE(whyNot.empty());

    // Two points and a line, in whichever order -- roadmap 13.1 makes the
    // ORDER the user's business, so the line is picked out by being a line.
    model.setSelection({SketchElementRef{axis, SketchSubElement::Whole},
                        SketchElementRef{a, SketchSubElement::StartPoint},
                        SketchElementRef{a, SketchSubElement::EndPoint}});
    const SketchEdit edit =
        model.requestConstraint(fixture.sketch(), SketchEditKind::AddSymmetric, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    // Points first, mirror last, whatever order the clicks came in.
    ASSERT_EQ(edit.refs.size(), 3u);
    EXPECT_EQ(edit.refs[2].entityId, axis);
}

TEST(SketchMirrorTest, M17_SYM_002_APointCannotBeMirroredAcrossItsOwnLine) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{line, SketchSubElement::StartPoint},
                        SketchElementRef{line, SketchSubElement::EndPoint},
                        SketchElementRef{line, SketchSubElement::Whole}});
    std::string whyNot;
    EXPECT_FALSE(model.requestConstraint(fixture.sketch(), SketchEditKind::AddSymmetric, &whyNot)
                     .valid());
    EXPECT_FALSE(whyNot.empty());
}

TEST(SketchMirrorTest, M17_MIR_001_ALineIsReflectedAndTiedAtBOTHEnds) {
    Fixture fixture;
    const SketchEntityId axis = LineFrom(fixture, Vec2{50.0, -50.0}, Vec2{50.0, 50.0});
    const SketchEntityId source = LineFrom(fixture, Vec2{10.0, 10.0}, Vec2{20.0, 30.0});

    const MirrorOutcome outcome = ApplyMirror(fixture.document, fixture.sketchId, {source}, axis);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    ASSERT_EQ(outcome.created.size(), 1u);

    // Reflected across x = 50.
    const SketchLine& copy =
        std::get<SketchLine>(fixture.sketch().findEntity(outcome.created.front())->geometry);
    EXPECT_NEAR(copy.start.x, 90.0, 1e-9);
    EXPECT_NEAR(copy.start.y, 10.0, 1e-9);
    EXPECT_NEAR(copy.end.x, 80.0, 1e-9);
    EXPECT_NEAR(copy.end.y, 30.0, 1e-9);

    // TWO symmetries, one per end. One would leave the copy free to swing
    // about the other, which is a mirror in appearance only.
    int symmetries = 0;
    for (const SketchConstraint& constraint : fixture.sketch().constraints())
        if (std::holds_alternative<SymmetricConstraint>(constraint.data)) ++symmetries;
    EXPECT_EQ(symmetries, 2);
}

TEST(SketchMirrorTest, M17_MIR_002_ACircleIsReflectedAndKeptTheSameSize) {
    Fixture fixture;
    const SketchEntityId axis = LineFrom(fixture, Vec2{0.0, -50.0}, Vec2{0.0, 50.0});
    const SketchEntityId source = CircleAt(fixture, Vec2{30.0, 10.0}, 12.0);

    const MirrorOutcome outcome = ApplyMirror(fixture.document, fixture.sketchId, {source}, axis);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    const SketchCircle& copy =
        std::get<SketchCircle>(fixture.sketch().findEntity(outcome.created.front())->geometry);
    EXPECT_NEAR(copy.center.x, -30.0, 1e-9);
    EXPECT_NEAR(copy.center.y, 10.0, 1e-9);
    EXPECT_NEAR(copy.radiusMm, 12.0, 1e-9);

    // Symmetry places the CENTRE; Equal is what keeps the size in step. Without
    // it the copy could be resized on its own and still be "symmetric".
    bool symmetric = false;
    bool equal = false;
    for (const SketchConstraint& constraint : fixture.sketch().constraints()) {
        if (std::holds_alternative<SymmetricConstraint>(constraint.data)) symmetric = true;
        if (std::holds_alternative<EqualConstraint>(constraint.data)) equal = true;
    }
    EXPECT_TRUE(symmetric);
    EXPECT_TRUE(equal);
}

TEST(SketchMirrorTest, M17_MIR_003_AnArcIsREFLECTEDIncludingItsSweep) {
    Fixture fixture;
    const SketchEntityId axis = LineFrom(fixture, Vec2{50.0, -50.0}, Vec2{50.0, 50.0});
    SketchEdit arcEdit;
    arcEdit.kind = SketchEditKind::AddArc;
    // Centre (10,10), radius 10, sweeping from due east to due north.
    arcEdit.points = {Vec2{10.0, 10.0}, Vec2{20.0, 10.0}, Vec2{10.0, 20.0}};
    arcEdit.label = "Add arc";
    const SketchEditOutcome arc = fixture.apply(arcEdit);
    ASSERT_TRUE(arc.applied) << arc.status;

    const MirrorOutcome outcome =
        ApplyMirror(fixture.document, fixture.sketchId, {arc.createdEntities.front()}, axis);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    const SketchArc& copy =
        std::get<SketchArc>(fixture.sketch().findEntity(outcome.created.front())->geometry);
    EXPECT_NEAR(copy.center.x, 90.0, 1e-9);
    EXPECT_NEAR(copy.center.y, 10.0, 1e-9);
    EXPECT_NEAR(copy.radiusMm, 10.0, 1e-9);

    // THE SWEEP IS MIRRORED, not merely copied. The image of the original's
    // two tips are the copy's tips; keeping the angles in order would draw the
    // COMPLEMENTARY arc -- the same circle, the piece nobody mirrored.
    const auto tip = [&](double angle) {
        return Vec2{copy.center.x + copy.radiusMm * std::cos(angle),
                    copy.center.y + copy.radiusMm * std::sin(angle)};
    };
    const Vec2 a = tip(copy.startAngleRad);
    const Vec2 b = tip(copy.endAngleRad);
    // The originals were (20,10) and (10,20); reflected across x=50 that is
    // (80,10) and (90,20).
    const double toEast = std::min(std::hypot(a.x - 80.0, a.y - 10.0),
                                   std::hypot(b.x - 80.0, b.y - 10.0));
    const double toNorth = std::min(std::hypot(a.x - 90.0, a.y - 20.0),
                                    std::hypot(b.x - 90.0, b.y - 20.0));
    EXPECT_LT(toEast, 1e-6);
    EXPECT_LT(toNorth, 1e-6);

    // AND IT SWEEPS THE SAME 90 DEGREES. The tips land in the right places
    // whichever way round the angles go -- they are the same two points. What
    // tells the mirror from its complement is the SWEEP: leaving the angles
    // unswapped draws the other 270 degrees of the same circle, through the
    // space the original never occupied. A mutation that skipped the swap
    // survived a version of this test that stopped at the tips.
    double sweep = copy.endAngleRad - copy.startAngleRad;
    while (sweep < 0.0) sweep += 2.0 * 3.14159265358979323846;
    EXPECT_NEAR(sweep, kHalfPi, 1e-6);
}

TEST(SketchMirrorTest, M17_MIR_006_AMirroredArcIsTiedByItsTIPSAndNotOverConstrained) {
    Fixture fixture;
    const SketchEntityId axis = LineFrom(fixture, Vec2{50.0, -50.0}, Vec2{50.0, 50.0});
    SketchEdit arcEdit;
    arcEdit.kind = SketchEditKind::AddArc;
    arcEdit.points = {Vec2{10.0, 10.0}, Vec2{20.0, 10.0}, Vec2{10.0, 20.0}};
    arcEdit.label = "Add arc";
    const SketchEditOutcome arc = fixture.apply(arcEdit);
    ASSERT_TRUE(arc.applied) << arc.status;

    const MirrorOutcome outcome =
        ApplyMirror(fixture.document, fixture.sketchId, {arc.createdEntities.front()}, axis);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // TWO symmetries and one Equal -- exactly five equations for the copy's
    // five freedoms. A centre symmetry as well would be two more for freedoms
    // already spent: consistent, but every mirrored arc would then read as
    // over-constrained, and roadmap 8.2 wants that reading to mean something.
    int symmetries = 0;
    int equals = 0;
    for (const SketchConstraint& constraint : fixture.sketch().constraints()) {
        if (std::holds_alternative<EqualConstraint>(constraint.data)) ++equals;
        const auto* symmetric = std::get_if<SymmetricConstraint>(&constraint.data);
        if (symmetric == nullptr) continue;
        ++symmetries;
        // The TIPS, never the centre.
        for (const SketchElementRef& ref : {symmetric->a, symmetric->b})
            EXPECT_NE(ref.subElement, SketchSubElement::CenterPoint);
    }
    EXPECT_EQ(symmetries, 2);
    EXPECT_EQ(equals, 1);
}

TEST(SketchTrimTest, M17_TRIM_010_AnArcIsTrimmedByMovingAnAngle) {
    Fixture fixture;
    SketchEdit arcEdit;
    arcEdit.kind = SketchEditKind::AddArc;
    // Centre at the origin, radius 20, a quarter sweep from east to north.
    arcEdit.points = {Vec2{0.0, 0.0}, Vec2{20.0, 0.0}, Vec2{0.0, 20.0}};
    arcEdit.label = "Add arc";
    const SketchEditOutcome added = fixture.apply(arcEdit);
    ASSERT_TRUE(added.applied) << added.status;
    const SketchEntityId target = added.createdEntities.front();
    // A cutter crossing the arc at 45 degrees.
    const SketchEntityId cutter = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{30.0, 30.0});

    // Click the piece BEYOND the crossing -- the part near due north.
    const TrimPlan plan = PlanTrim(fixture.sketch(), target, {cutter}, Vec2{2.0, 19.9});
    ASSERT_TRUE(plan.ok) << plan.why;
    const SketchArc& trimmed = std::get<SketchArc>(plan.result);
    // The END angle moved back to 45 degrees; the start is untouched.
    EXPECT_NEAR(trimmed.startAngleRad, 0.0, 1e-6);
    EXPECT_NEAR(trimmed.endAngleRad, kQuarterPi, 1e-6);
    EXPECT_NEAR(trimmed.radiusMm, 20.0, 1e-9);
}

TEST(SketchTrimTest, M17_TRIM_011_ClickingTheOtherEndOfAnArcTrimsTheOtherAngle) {
    Fixture fixture;
    SketchEdit arcEdit;
    arcEdit.kind = SketchEditKind::AddArc;
    arcEdit.points = {Vec2{0.0, 0.0}, Vec2{20.0, 0.0}, Vec2{0.0, 20.0}};
    arcEdit.label = "Add arc";
    const SketchEditOutcome added = fixture.apply(arcEdit);
    ASSERT_TRUE(added.applied) << added.status;
    const SketchEntityId target = added.createdEntities.front();
    const SketchEntityId cutter = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{30.0, 30.0});

    // Near due east this time.
    const TrimPlan plan = PlanTrim(fixture.sketch(), target, {cutter}, Vec2{19.9, 2.0});
    ASSERT_TRUE(plan.ok) << plan.why;
    const SketchArc& trimmed = std::get<SketchArc>(plan.result);
    EXPECT_NEAR(trimmed.startAngleRad, kQuarterPi, 1e-6);
    EXPECT_NEAR(trimmed.endAngleRad, kHalfPi, 1e-6);
}

TEST(SketchTrimTest, M17_TRIM_012_AnArcCutsAnotherArcOnlyWhereBothActuallyReach) {
    Fixture fixture;
    SketchEdit big;
    big.kind = SketchEditKind::AddArc;
    big.points = {Vec2{0.0, 0.0}, Vec2{20.0, 0.0}, Vec2{0.0, 20.0}};
    big.label = "Add arc";
    const SketchEditOutcome target = fixture.apply(big);
    ASSERT_TRUE(target.applied) << target.status;

    // A circle centred to the right, crossing the target's circle twice -- once
    // inside the target's quarter sweep and once below it.
    const SketchEntityId cutter = CircleAt(fixture, Vec2{20.0, 0.0}, 15.0);
    const std::vector<double> cuts = TrimCutsAlongArc(
        fixture.sketch(),
        std::get<SketchArc>(fixture.sketch().findEntity(target.createdEntities.front())->geometry),
        target.createdEntities.front(), cutter);
    // Only crossings ON the target's own sweep count -- the circle through it
    // reaches further, and cutting there would trim where nothing is drawn.
    for (const double t : cuts) {
        EXPECT_GE(t, -1e-9);
        EXPECT_LE(t, 1.0 + 1e-9);
    }
}

TEST(SketchMirrorTest, M17_MIR_004_TheMirrorLineCannotMirrorItself) {
    Fixture fixture;
    const SketchEntityId axis = LineFrom(fixture, Vec2{50.0, -50.0}, Vec2{50.0, 50.0});
    const MirrorOutcome outcome = ApplyMirror(fixture.document, fixture.sketchId, {axis}, axis);
    EXPECT_FALSE(outcome.applied);
    EXPECT_FALSE(outcome.status.empty());
}

TEST(SketchMirrorTest, M17_MIR_005_MirroringSeveralIsONEUndoStep) {
    Fixture fixture;
    const SketchEntityId axis = LineFrom(fixture, Vec2{50.0, -50.0}, Vec2{50.0, 50.0});
    const SketchEntityId a = LineFrom(fixture, Vec2{10.0, 10.0}, Vec2{20.0, 10.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{10.0, 20.0}, Vec2{20.0, 20.0});
    const std::size_t before = fixture.sketch().entities().size();

    ASSERT_TRUE(ApplyMirror(fixture.document, fixture.sketchId, {a, b}, axis).applied);
    EXPECT_EQ(fixture.sketch().entities().size(), before + 2);

    ASSERT_TRUE(fixture.document.undo());
    // One command, one Ctrl+Z -- not one per copy.
    EXPECT_EQ(fixture.sketch().entities().size(), before);
    for (const SketchConstraint& constraint : fixture.sketch().constraints())
        EXPECT_FALSE(std::holds_alternative<SymmetricConstraint>(constraint.data));
}

// =============================================================================
// Extend and Chamfer (M17), on the same in-place primitive
// =============================================================================

TEST(SketchExtendTest, M17_EXT_001_AnEndStretchesToTheFirstThingBeyondIt) {
    Fixture fixture;
    const SketchEntityId target = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{50.0, 0.0});
    const SketchEntityId wall = LineFrom(fixture, Vec2{80.0, -20.0}, Vec2{80.0, 20.0});

    // Click near the far end: that is the end to grow.
    const TrimPlan plan = PlanExtend(fixture.sketch(), target, {wall}, Vec2{45.0, 0.0});
    ASSERT_TRUE(plan.ok) << plan.why;
    EXPECT_FALSE(plan.trimmedStart);
    EXPECT_NEAR(std::get<SketchLine>(plan.result).end.x, 80.0, 1e-9);
    EXPECT_NEAR(std::get<SketchLine>(plan.result).start.x, 0.0, 1e-9);
}

TEST(SketchExtendTest, M17_EXT_002_ClickingTheNearEndStretchesTheOtherWay) {
    Fixture fixture;
    const SketchEntityId target = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{50.0, 0.0});
    const SketchEntityId wall = LineFrom(fixture, Vec2{-30.0, -20.0}, Vec2{-30.0, 20.0});

    const TrimPlan plan = PlanExtend(fixture.sketch(), target, {wall}, Vec2{5.0, 0.0});
    ASSERT_TRUE(plan.ok) << plan.why;
    EXPECT_TRUE(plan.trimmedStart);
    EXPECT_NEAR(std::get<SketchLine>(plan.result).start.x, -30.0, 1e-9);
    EXPECT_NEAR(std::get<SketchLine>(plan.result).end.x, 50.0, 1e-9);
}

TEST(SketchExtendTest, M17_EXT_003_ItStopsAtTheNEARESTBoundaryNotTheFurthest) {
    Fixture fixture;
    const SketchEntityId target = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{50.0, 0.0});
    const SketchEntityId near = LineFrom(fixture, Vec2{70.0, -20.0}, Vec2{70.0, 20.0});
    const SketchEntityId far = LineFrom(fixture, Vec2{120.0, -20.0}, Vec2{120.0, 20.0});

    const TrimPlan plan = PlanExtend(fixture.sketch(), target, {near, far}, Vec2{45.0, 0.0});
    ASSERT_TRUE(plan.ok) << plan.why;
    // Growing past the first thing in the way would cross geometry the user can
    // see it should have stopped at.
    EXPECT_NEAR(std::get<SketchLine>(plan.result).end.x, 70.0, 1e-9);
}

TEST(SketchExtendTest, M17_EXT_004_ABoundaryBEHINDTheEndIsNotSomethingToGrowTo) {
    Fixture fixture;
    const SketchEntityId target = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{50.0, 0.0});
    // A wall the line already crosses: extending backwards to reach it would
    // SHRINK the line while calling itself extend.
    const SketchEntityId wall = LineFrom(fixture, Vec2{25.0, -20.0}, Vec2{25.0, 20.0});

    const TrimPlan plan = PlanExtend(fixture.sketch(), target, {wall}, Vec2{45.0, 0.0});
    EXPECT_FALSE(plan.ok);
    EXPECT_FALSE(plan.why.empty());
}

TEST(SketchExtendTest, M17_EXT_005_ExtendKeepsTheConstraintsToo) {
    Fixture fixture;
    const SketchEntityId target = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{50.0, 0.0});
    const SketchEntityId wall = LineFrom(fixture, Vec2{80.0, -20.0}, Vec2{80.0, 20.0});
    SketchEdit horizontal;
    horizontal.kind = SketchEditKind::AddHorizontal;
    horizontal.refs = {SketchElementRef{target, SketchSubElement::Whole}};
    horizontal.label = "Horizontal";
    const SketchEditOutcome constrained = fixture.apply(horizontal);
    ASSERT_TRUE(constrained.applied);

    const TrimPlan plan = PlanExtend(fixture.sketch(), target, {wall}, Vec2{45.0, 0.0});
    ASSERT_TRUE(plan.ok) << plan.why;
    ASSERT_TRUE(
        fixture.document.setSketchEntityGeometry(fixture.sketchId, plan.target, plan.result));
    EXPECT_NE(fixture.sketch().findConstraint(constrained.createdConstraints.front()), nullptr);
}

// --- Chamfer -----------------------------------------------------------------

// Two lines meeting at a right-angled corner at (100,0), joined by a Coincident.
struct CorneredFixture {
    Fixture fixture;
    SketchEntityId lineA{kInvalidSketchEntityId};
    SketchEntityId lineB{kInvalidSketchEntityId};
    SketchConstraintId joint{kInvalidSketchConstraintId};

    CorneredFixture() {
        lineA = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
        lineB = LineFrom(fixture, Vec2{100.0, 0.0}, Vec2{100.0, 100.0});
        SketchEdit join;
        join.kind = SketchEditKind::AddCoincident;
        join.refs = {SketchElementRef{lineA, SketchSubElement::EndPoint},
                     SketchElementRef{lineB, SketchSubElement::StartPoint}};
        join.label = "Coincident";
        const SketchEditOutcome outcome = fixture.apply(join);
        EXPECT_TRUE(outcome.applied) << outcome.status;
        if (!outcome.createdConstraints.empty()) joint = outcome.createdConstraints.front();
    }
};

TEST(SketchChamferTest, M17_CHA_001_TheCornerIsCutBackAndALinePutAcrossIt) {
    CorneredFixture corner;
    const ChamferOutcome outcome = ApplyChamfer(corner.fixture.document, corner.fixture.sketchId,
                                                corner.lineA, corner.lineB, 20.0, 30.0);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const Sketch& sketch = corner.fixture.sketch();
    EXPECT_EQ(sketch.entities().size(), 3u);
    // A is cut back 20 from the corner, along itself.
    const SketchLine& a = std::get<SketchLine>(sketch.findEntity(corner.lineA)->geometry);
    EXPECT_NEAR(a.end.x, 80.0, 1e-9);
    EXPECT_NEAR(a.end.y, 0.0, 1e-9);
    // B is cut back 30, along itself.
    const SketchLine& b = std::get<SketchLine>(sketch.findEntity(corner.lineB)->geometry);
    EXPECT_NEAR(b.start.x, 100.0, 1e-9);
    EXPECT_NEAR(b.start.y, 30.0, 1e-9);
    // ...and the chamfer runs between the two setbacks.
    const SketchLine& chamfer = std::get<SketchLine>(sketch.findEntity(outcome.created)->geometry);
    EXPECT_NEAR(chamfer.start.x, 80.0, 1e-9);
    EXPECT_NEAR(chamfer.end.y, 30.0, 1e-9);
}

TEST(SketchChamferTest, M17_CHA_002_TheCornersOwnCoincidentIsDELETED) {
    CorneredFixture corner;
    ASSERT_NE(corner.joint, kInvalidSketchConstraintId);
    const ChamferOutcome outcome = ApplyChamfer(corner.fixture.document, corner.fixture.sketchId,
                                                corner.lineA, corner.lineB, 20.0, 20.0);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // It said the two ends were one point, which the chamfer has just stopped
    // being true. Leaving it would hand the user a conflict they did not make.
    EXPECT_EQ(corner.fixture.sketch().findConstraint(corner.joint), nullptr);
    // Replaced by two: each line joined to one end of the chamfer.
    int coincidences = 0;
    for (const SketchConstraint& constraint : corner.fixture.sketch().constraints())
        if (std::holds_alternative<CoincidentConstraint>(constraint.data)) ++coincidences;
    EXPECT_EQ(coincidences, 2);
}

TEST(SketchChamferTest, M17_CHA_003_TheThreePiecesStayOneConnectedRun) {
    CorneredFixture corner;
    const ChamferOutcome outcome = ApplyChamfer(corner.fixture.document, corner.fixture.sketchId,
                                                corner.lineA, corner.lineB, 20.0, 20.0);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // Each new coincidence names the chamfer AND one of the two lines: a
    // chamfer joined to nothing is a line lying in the gap it just made.
    bool joinedA = false;
    bool joinedB = false;
    for (const SketchConstraint& constraint : corner.fixture.sketch().constraints()) {
        const auto* c = std::get_if<CoincidentConstraint>(&constraint.data);
        if (c == nullptr) continue;
        const bool touchesChamfer =
            c->a.entityId == outcome.created || c->b.entityId == outcome.created;
        if (!touchesChamfer) continue;
        if (c->a.entityId == corner.lineA || c->b.entityId == corner.lineA) joinedA = true;
        if (c->a.entityId == corner.lineB || c->b.entityId == corner.lineB) joinedB = true;
    }
    EXPECT_TRUE(joinedA);
    EXPECT_TRUE(joinedB);
}

TEST(SketchChamferTest, M17_CHA_004_ASetbackDeeperThanTheLineIsRefusedAndChangesNOTHING) {
    CorneredFixture corner;
    const std::size_t entitiesBefore = corner.fixture.sketch().entities().size();
    const std::size_t constraintsBefore = corner.fixture.sketch().constraints().size();

    const ChamferOutcome outcome = ApplyChamfer(corner.fixture.document, corner.fixture.sketchId,
                                                corner.lineA, corner.lineB, 200.0, 20.0);
    EXPECT_FALSE(outcome.applied);
    EXPECT_FALSE(outcome.status.empty());
    // The transaction rolled back whole: a half-applied chamfer would have
    // released the corner and put nothing in its place.
    EXPECT_EQ(corner.fixture.sketch().entities().size(), entitiesBefore);
    EXPECT_EQ(corner.fixture.sketch().constraints().size(), constraintsBefore);
    EXPECT_NE(corner.fixture.sketch().findConstraint(corner.joint), nullptr);
}

TEST(SketchChamferTest, M17_CHA_005_TwoLinesThatDoNotMeetHaveNoCorner) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{40.0, 0.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{60.0, 10.0}, Vec2{60.0, 80.0});
    const ChamferOutcome outcome =
        ApplyChamfer(fixture.document, fixture.sketchId, a, b, 5.0, 5.0);
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("meet"), std::string::npos) << outcome.status;
}

TEST(SketchChamferTest, M17_CHA_006_TheWholeChamferIsOneUndoStep) {
    CorneredFixture corner;
    const std::size_t entitiesBefore = corner.fixture.sketch().entities().size();
    ASSERT_TRUE(ApplyChamfer(corner.fixture.document, corner.fixture.sketchId, corner.lineA,
                             corner.lineB, 20.0, 20.0)
                    .applied);
    ASSERT_TRUE(corner.fixture.document.undo());

    // Three geometry edits, a delete and two adds -- and ONE Ctrl+Z, because it
    // was one thing the user did.
    EXPECT_EQ(corner.fixture.sketch().entities().size(), entitiesBefore);
    EXPECT_NE(corner.fixture.sketch().findConstraint(corner.joint), nullptr);
    const SketchLine& a =
        std::get<SketchLine>(corner.fixture.sketch().findEntity(corner.lineA)->geometry);
    EXPECT_NEAR(a.end.x, 100.0, 1e-9);
}

// --- Fillet ------------------------------------------------------------------

TEST(SketchFilletTest, M17_FIL_001_TheCornerIsRoundedAndBothLinesTrimmedToTheTangents) {
    CorneredFixture corner;
    // A right angle, so the setback equals the radius.
    const ChamferOutcome outcome = ApplyFillet(corner.fixture.document,
                                               corner.fixture.sketchId, corner.lineA,
                                               corner.lineB, 20.0);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const Sketch& sketch = corner.fixture.sketch();
    EXPECT_EQ(sketch.entities().size(), 3u);
    const SketchLine& a = std::get<SketchLine>(sketch.findEntity(corner.lineA)->geometry);
    EXPECT_NEAR(a.end.x, 80.0, 1e-9);
    const SketchLine& b = std::get<SketchLine>(sketch.findEntity(corner.lineB)->geometry);
    EXPECT_NEAR(b.start.y, 20.0, 1e-9);

    // The arc's centre sits r in from BOTH lines, which for a right angle at
    // (100,0) is (80,20).
    const SketchArc& arc = std::get<SketchArc>(sketch.findEntity(outcome.created)->geometry);
    EXPECT_NEAR(arc.center.x, 80.0, 1e-6);
    EXPECT_NEAR(arc.center.y, 20.0, 1e-6);
    EXPECT_NEAR(arc.radiusMm, 20.0, 1e-9);
}

TEST(SketchFilletTest, M17_FIL_002_TheArcIsTANGENTAndJOINEDNotJustTouching) {
    CorneredFixture corner;
    const ChamferOutcome outcome = ApplyFillet(corner.fixture.document,
                                               corner.fixture.sketchId, corner.lineA,
                                               corner.lineB, 20.0);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    int tangents = 0;
    int joints = 0;
    for (const SketchConstraint& constraint : corner.fixture.sketch().constraints()) {
        if (std::holds_alternative<TangentConstraint>(constraint.data)) ++tangents;
        const auto* coincident = std::get_if<CoincidentConstraint>(&constraint.data);
        if (coincident == nullptr) continue;
        if (coincident->a.entityId == outcome.created ||
            coincident->b.entityId == outcome.created)
            ++joints;
    }
    // THE POINT OF THE WHOLE FEATURE. Without the tangencies the corner is
    // smooth today and kinked after the first parameter change; without the
    // coincidences the arc is a decoration lying in the gap.
    EXPECT_EQ(tangents, 2);
    EXPECT_EQ(joints, 2);
    // ...and the corner's own coincidence is gone, as it is for a chamfer.
    EXPECT_EQ(corner.fixture.sketch().findConstraint(corner.joint), nullptr);
}

TEST(SketchFilletTest, M17_FIL_003_TheJointsNameTheARCTIPSWhichNowHaveVariables) {
    CorneredFixture corner;
    const ChamferOutcome outcome = ApplyFillet(corner.fixture.document,
                                               corner.fixture.sketchId, corner.lineA,
                                               corner.lineB, 20.0);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // Each joint names a TIP -- StartPoint or EndPoint of the arc. Before M17 a
    // reference like this resolved to the arc's RADIUS variable, which is
    // exactly why fillet could not be built.
    for (const SketchConstraint& constraint : corner.fixture.sketch().constraints()) {
        const auto* coincident = std::get_if<CoincidentConstraint>(&constraint.data);
        if (coincident == nullptr) continue;
        for (const SketchElementRef& ref : {coincident->a, coincident->b}) {
            if (ref.entityId != outcome.created) continue;
            EXPECT_TRUE(ref.subElement == SketchSubElement::StartPoint ||
                        ref.subElement == SketchSubElement::EndPoint);
        }
    }
}

TEST(SketchFilletTest, M17_FIL_004_ARadiusTooBigForTheCornerIsRefusedAndChangesNOTHING) {
    CorneredFixture corner;
    const std::size_t entitiesBefore = corner.fixture.sketch().entities().size();
    const std::size_t constraintsBefore = corner.fixture.sketch().constraints().size();

    // The lines are 100 long; a 200 radius eats past the end of both.
    const ChamferOutcome outcome = ApplyFillet(corner.fixture.document,
                                               corner.fixture.sketchId, corner.lineA,
                                               corner.lineB, 200.0);
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("too big"), std::string::npos) << outcome.status;
    EXPECT_EQ(corner.fixture.sketch().entities().size(), entitiesBefore);
    EXPECT_EQ(corner.fixture.sketch().constraints().size(), constraintsBefore);
    EXPECT_NE(corner.fixture.sketch().findConstraint(corner.joint), nullptr);
}

TEST(SketchFilletTest, M17_FIL_005_TwoLinesInLineHaveNoCornerToRound) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{50.0, 0.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{50.0, 0.0}, Vec2{100.0, 0.0});
    const ChamferOutcome outcome =
        ApplyFillet(fixture.document, fixture.sketchId, a, b, 5.0);
    // Collinear: the inscribed circle's tangent length is infinite, so there is
    // nothing to round. Refused with the reason rather than a vast arc.
    EXPECT_FALSE(outcome.applied);
    EXPECT_FALSE(outcome.status.empty());
}

TEST(SketchFilletTest, M17_FIL_006_TheWholeFilletIsOneUndoStep) {
    CorneredFixture corner;
    const std::size_t entitiesBefore = corner.fixture.sketch().entities().size();
    ASSERT_TRUE(ApplyFillet(corner.fixture.document, corner.fixture.sketchId, corner.lineA,
                            corner.lineB, 20.0)
                    .applied);
    ASSERT_TRUE(corner.fixture.document.undo());
    EXPECT_EQ(corner.fixture.sketch().entities().size(), entitiesBefore);
    EXPECT_NE(corner.fixture.sketch().findConstraint(corner.joint), nullptr);
    const SketchLine& a =
        std::get<SketchLine>(corner.fixture.sketch().findEntity(corner.lineA)->geometry);
    EXPECT_NEAR(a.end.x, 100.0, 1e-9);
}


// =============================================================================
// Trim (M17): reshape in place, so the constraints survive
// =============================================================================

TEST(SketchTrimTest, M17_TRIM_001_TheOverhangPastACrossingIsRemoved) {
    Fixture fixture;
    const SketchEntityId target = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    LineFrom(fixture, Vec2{60.0, -20.0}, Vec2{60.0, 20.0});

    // Click the piece BEYOND the crossing.
    const TrimPlan plan = PlanTrim(fixture.sketch(), target, {}, Vec2{80.0, 0.0});
    // No cutters passed: nothing crosses, so there is nothing to remove.
    EXPECT_FALSE(plan.ok);
    EXPECT_FALSE(plan.why.empty());

    std::vector<SketchEntityId> cutters;
    for (const SketchEntity& entity : fixture.sketch().entities())
        if (entity.id != target) cutters.push_back(entity.id);
    const TrimPlan real = PlanTrim(fixture.sketch(), target, cutters, Vec2{80.0, 0.0});
    ASSERT_TRUE(real.ok) << real.why;
    EXPECT_FALSE(real.trimmedStart);
    EXPECT_NEAR(std::get<SketchLine>(real.result).end.x, 60.0, 1e-9);
    EXPECT_NEAR(std::get<SketchLine>(real.result).start.x, 0.0, 1e-9);
}

TEST(SketchTrimTest, M17_TRIM_002_ClickingTheOtherSideTrimsTheOtherEnd) {
    Fixture fixture;
    const SketchEntityId target = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId cutter = LineFrom(fixture, Vec2{60.0, -20.0}, Vec2{60.0, 20.0});

    const TrimPlan plan = PlanTrim(fixture.sketch(), target, {cutter}, Vec2{20.0, 0.0});
    ASSERT_TRUE(plan.ok) << plan.why;
    // The START moves up to the crossing; the far end is untouched.
    EXPECT_TRUE(plan.trimmedStart);
    EXPECT_NEAR(std::get<SketchLine>(plan.result).start.x, 60.0, 1e-9);
    EXPECT_NEAR(std::get<SketchLine>(plan.result).end.x, 100.0, 1e-9);
}

TEST(SketchTrimTest, M17_TRIM_003_AMiddlePieceIsREFUSEDRatherThanSplit) {
    Fixture fixture;
    const SketchEntityId target = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId left = LineFrom(fixture, Vec2{30.0, -20.0}, Vec2{30.0, 20.0});
    const SketchEntityId right = LineFrom(fixture, Vec2{70.0, -20.0}, Vec2{70.0, 20.0});

    // Between the two crossings: removing it would leave two lines, and which
    // half keeps the constraints is not something to guess.
    const TrimPlan plan = PlanTrim(fixture.sketch(), target, {left, right}, Vec2{50.0, 0.0});
    EXPECT_FALSE(plan.ok);
    EXPECT_NE(plan.why.find("split"), std::string::npos) << plan.why;
}

TEST(SketchTrimTest, M17_TRIM_004_TrimmingKEEPSTheConstraintsOnTheLine) {
    Fixture fixture;
    const SketchEntityId target = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId cutter = LineFrom(fixture, Vec2{60.0, -20.0}, Vec2{60.0, 20.0});

    SketchEdit horizontal;
    horizontal.kind = SketchEditKind::AddHorizontal;
    horizontal.refs = {SketchElementRef{target, SketchSubElement::Whole}};
    horizontal.label = "Horizontal";
    const SketchEditOutcome constrained = fixture.apply(horizontal);
    ASSERT_TRUE(constrained.applied);

    const TrimPlan plan = PlanTrim(fixture.sketch(), target, {cutter}, Vec2{80.0, 0.0});
    ASSERT_TRUE(plan.ok) << plan.why;
    ASSERT_TRUE(fixture.document.setSketchEntityGeometry(fixture.sketchId, plan.target,
                                                         plan.result));

    // THE WHOLE REASON the primitive reshapes in place. Delete-and-recreate
    // would issue a new id and cascade the Horizontal away (ADR-M5-009), and
    // the user would be left with a line that had quietly stopped being
    // horizontal.
    EXPECT_NE(fixture.sketch().findConstraint(constrained.createdConstraints.front()), nullptr);
    EXPECT_EQ(fixture.sketch().entities().size(), 2u);
    const SketchEntity* trimmed = fixture.sketch().findEntity(target);
    ASSERT_NE(trimmed, nullptr);
    EXPECT_NEAR(std::get<SketchLine>(trimmed->geometry).end.x, 60.0, 1e-9);
}

TEST(SketchTrimTest, M17_TRIM_005_TrimIsOneUndoStepAndComesBackWhole) {
    Fixture fixture;
    const SketchEntityId target = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId cutter = LineFrom(fixture, Vec2{60.0, -20.0}, Vec2{60.0, 20.0});

    const TrimPlan plan = PlanTrim(fixture.sketch(), target, {cutter}, Vec2{80.0, 0.0});
    ASSERT_TRUE(plan.ok) << plan.why;
    ASSERT_TRUE(fixture.document.setSketchEntityGeometry(fixture.sketchId, plan.target,
                                                         plan.result));
    ASSERT_TRUE(fixture.document.undo());
    const SketchEntity* restored = fixture.sketch().findEntity(target);
    ASSERT_NE(restored, nullptr);
    EXPECT_NEAR(std::get<SketchLine>(restored->geometry).end.x, 100.0, 1e-9);

    ASSERT_TRUE(fixture.document.redo());
    EXPECT_NEAR(
        std::get<SketchLine>(fixture.sketch().findEntity(target)->geometry).end.x, 60.0, 1e-9);
}

TEST(SketchTrimTest, M17_TRIM_006_ACircleCutsWhereItActuallyIs) {
    Fixture fixture;
    const SketchEntityId target = LineFrom(fixture, Vec2{-50.0, 0.0}, Vec2{50.0, 0.0});
    const SketchEntityId circle = CircleAt(fixture, Vec2{0.0, 0.0}, 20.0);

    // A line through a circle crosses it twice.
    const std::vector<double> cuts = TrimCutsAlongLine(fixture.sketch(), target, circle);
    ASSERT_EQ(cuts.size(), 2u);
    // The line spans -50..50, so the crossings at -20 and +20 sit at 0.3 / 0.7.
    EXPECT_NEAR(std::min(cuts[0], cuts[1]), 0.3, 1e-9);
    EXPECT_NEAR(std::max(cuts[0], cuts[1]), 0.7, 1e-9);
}

TEST(SketchTrimTest, M17_TRIM_007_AnArcDoesNotCutWhereOnlyItsCircleReaches) {
    Fixture fixture;
    const SketchEntityId target = LineFrom(fixture, Vec2{-50.0, 0.0}, Vec2{50.0, 0.0});
    // A quarter arc in the UPPER half: its circle crosses the line twice, the
    // arc itself never does.
    SketchEdit arcEdit;
    arcEdit.kind = SketchEditKind::AddArc;
    arcEdit.points = {Vec2{0.0, 0.0}, Vec2{0.0, 20.0}, Vec2{-20.0, 0.0}};
    arcEdit.label = "Add arc";
    const SketchEditOutcome arc = fixture.apply(arcEdit);
    ASSERT_TRUE(arc.applied) << arc.status;

    const std::vector<double> cuts =
        TrimCutsAlongLine(fixture.sketch(), target, arc.createdEntities.front());
    // At most the one tip that touches the line -- never the far side, where
    // there is nothing drawn to cut against.
    for (const double t : cuts) EXPECT_LT(t, 0.55) << "cut on the side the arc does not reach";
}

TEST(SketchTrimTest, M17_TRIM_008_ACircleCannotBeTheTarget) {
    Fixture fixture;
    const SketchEntityId circle = CircleAt(fixture, Vec2{0.0, 0.0}, 20.0);
    LineFrom(fixture, Vec2{-50.0, 0.0}, Vec2{50.0, 0.0});

    const TrimPlan plan = PlanTrim(fixture.sketch(), circle, {}, Vec2{20.0, 0.0});
    EXPECT_FALSE(plan.ok);
    // Not "an arc has no variables" any more -- ADR-M17-018 gave it some, and
    // arcs trim. A CIRCLE is refused for a different reason that has not
    // changed: cutting one turns it into an arc, a change of KIND, and every
    // constraint naming the circle would suddenly name something else.
    EXPECT_NE(plan.why.find("arc"), std::string::npos) << plan.why;
}

TEST(SketchTrimTest, M17_TRIM_009_TrimmingToNothingIsRefused) {
    Fixture fixture;
    const SketchEntityId target = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    // A crossing a hair from the end leaves a sliver; one AT the end leaves the
    // line untouched and is dropped as a boundary, so there is nothing to cut.
    const SketchEntityId cutter = LineFrom(fixture, Vec2{100.0, -20.0}, Vec2{100.0, 20.0});
    const TrimPlan plan = PlanTrim(fixture.sketch(), target, {cutter}, Vec2{50.0, 0.0});
    EXPECT_FALSE(plan.ok);
    EXPECT_FALSE(plan.why.empty());
}

// =============================================================================
// Offset (M17): a copy WITH the constraints that make it a copy
// =============================================================================

TEST(SketchOffsetTest, M17_OFF_001_ALineOffsetsParallelAtTheDistanceAsked) {
    Fixture fixture;
    const SketchEntityId source = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});

    std::string whyNot;
    const SketchEdit edit = MakeOffsetEdit(fixture.sketch(), source, 10.0, 1.0, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    const SketchEditOutcome outcome = fixture.apply(edit);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    ASSERT_EQ(outcome.createdEntities.size(), 1u);

    // LEFT of start->end, which for a rightward line is +v.
    const SketchEntity* copy = fixture.sketch().findEntity(outcome.createdEntities.front());
    ASSERT_NE(copy, nullptr);
    const SketchLine& geometry = std::get<SketchLine>(copy->geometry);
    EXPECT_NEAR(geometry.start.y, 10.0, 1e-9);
    EXPECT_NEAR(geometry.end.y, 10.0, 1e-9);
    EXPECT_NEAR(geometry.start.x, 0.0, 1e-9);
}

TEST(SketchOffsetTest, M17_OFF_002_TheOtherSideIsTheOtherSide) {
    Fixture fixture;
    const SketchEntityId source = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    std::string whyNot;
    const SketchEditOutcome outcome =
        fixture.apply(MakeOffsetEdit(fixture.sketch(), source, 10.0, -1.0, &whyNot));
    ASSERT_TRUE(outcome.applied) << outcome.status;
    const SketchEntity* copy = fixture.sketch().findEntity(outcome.createdEntities.front());
    ASSERT_NE(copy, nullptr);
    EXPECT_NEAR(std::get<SketchLine>(copy->geometry).start.y, -10.0, 1e-9);
}

TEST(SketchOffsetTest, M17_OFF_003_ALineOffsetCarriesParallelEqualAndADistance) {
    Fixture fixture;
    const SketchEntityId source = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    std::string whyNot;
    const SketchEditOutcome outcome =
        fixture.apply(MakeOffsetEdit(fixture.sketch(), source, 10.0, 1.0, &whyNot));
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // THE POINT OF THE WHOLE COMMAND. Without these the copy is a second line
    // that happens to be parallel today, and the DOF readout counts freedoms
    // the user believes they have spent.
    ASSERT_EQ(outcome.createdConstraints.size(), 3u);
    int parallel = 0;
    int equal = 0;
    int distance = 0;
    for (const SketchConstraintId id : outcome.createdConstraints) {
        const SketchConstraint* constraint = fixture.sketch().findConstraint(id);
        ASSERT_NE(constraint, nullptr);
        if (std::holds_alternative<ParallelConstraint>(constraint->data)) ++parallel;
        if (std::holds_alternative<EqualConstraint>(constraint->data)) ++equal;
        if (std::holds_alternative<PointLineDistanceConstraint>(constraint->data)) ++distance;
    }
    EXPECT_EQ(parallel, 1);
    EXPECT_EQ(equal, 1);
    EXPECT_EQ(distance, 1);

    // SEEDED AT WHAT IT MEASURES, so creating the offset moves nothing -- and
    // measured is -10, not +10: side +1 is the LEFT of start->end, and the
    // residual calls the left negative. Seeding the REQUESTED +10 here would
    // put the parameter at odds with its own geometry and the first solve would
    // flip the copy to the other side of the source.
    const Parameter* parameter = fixture.document.parameters().findById(outcome.createdParameter);
    ASSERT_NE(parameter, nullptr);
    EXPECT_NEAR(parameter->value(), -10.0, 1e-9);
}

TEST(SketchOffsetTest, M17_OFF_004_ACircleOffsetsConcentricWithARadius) {
    Fixture fixture;
    const SketchEntityId source = CircleAt(fixture, Vec2{20.0, 20.0}, 30.0);
    std::string whyNot;
    const SketchEditOutcome outcome =
        fixture.apply(MakeOffsetEdit(fixture.sketch(), source, 5.0, 1.0, &whyNot));
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const SketchEntity* copy = fixture.sketch().findEntity(outcome.createdEntities.front());
    ASSERT_NE(copy, nullptr);
    const SketchCircle& circle = std::get<SketchCircle>(copy->geometry);
    EXPECT_NEAR(circle.radiusMm, 35.0, 1e-9);
    EXPECT_NEAR(circle.center.x, 20.0, 1e-9);

    ASSERT_EQ(outcome.createdConstraints.size(), 2u);
    bool concentric = false;
    bool radius = false;
    for (const SketchConstraintId id : outcome.createdConstraints) {
        const SketchConstraint* constraint = fixture.sketch().findConstraint(id);
        ASSERT_NE(constraint, nullptr);
        if (std::holds_alternative<ConcentricConstraint>(constraint->data)) concentric = true;
        if (std::holds_alternative<RadiusConstraint>(constraint->data)) radius = true;
    }
    EXPECT_TRUE(concentric);
    EXPECT_TRUE(radius);
}

TEST(SketchOffsetTest, M17_OFF_005_OffsettingACircleInwardPastItsCentreIsREFUSED) {
    Fixture fixture;
    const SketchEntityId source = CircleAt(fixture, Vec2{0.0, 0.0}, 10.0);
    std::string whyNot;
    // -1 side on a curve is inward, and 20 mm inward from r=10 is not a circle.
    const SketchEdit edit = MakeOffsetEdit(fixture.sketch(), source, 20.0, -1.0, &whyNot);
    EXPECT_FALSE(edit.valid());
    // The message names the NUMBER, so the user knows by how much they missed.
    EXPECT_NE(whyNot.find("20"), std::string::npos) << whyNot;
    EXPECT_EQ(fixture.sketch().entities().size(), 1u);
}

TEST(SketchOffsetTest, M17_OFF_006_AZeroOffsetIsRefusedRatherThanDuplicating) {
    Fixture fixture;
    const SketchEntityId source = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    std::string whyNot;
    EXPECT_FALSE(MakeOffsetEdit(fixture.sketch(), source, 0.0, 1.0, &whyNot).valid());
    EXPECT_FALSE(whyNot.empty());
    // A duplicate on top of the original is the worst outcome: it looks like
    // nothing happened and leaves two entities where the user sees one.
    EXPECT_EQ(fixture.sketch().entities().size(), 1u);
}

TEST(SketchOffsetTest, M17_OFF_007_AnArcOffsetKeepsItsSweep) {
    Fixture fixture;
    SketchEdit arcEdit;
    arcEdit.kind = SketchEditKind::AddArc;
    arcEdit.points = {Vec2{0.0, 0.0}, Vec2{20.0, 0.0}, Vec2{0.0, 20.0}};
    arcEdit.label = "Add arc";
    const SketchEditOutcome arc = fixture.apply(arcEdit);
    ASSERT_TRUE(arc.applied) << arc.status;
    const SketchArc before =
        std::get<SketchArc>(fixture.sketch().findEntity(arc.createdEntities.front())->geometry);

    std::string whyNot;
    const SketchEditOutcome outcome = fixture.apply(
        MakeOffsetEdit(fixture.sketch(), arc.createdEntities.front(), 5.0, 1.0, &whyNot));
    ASSERT_TRUE(outcome.applied) << outcome.status;
    const SketchArc after =
        std::get<SketchArc>(fixture.sketch().findEntity(outcome.createdEntities.front())->geometry);

    // Further out, same span: an offset arc subtends the same angles.
    EXPECT_NEAR(after.radiusMm, before.radiusMm + 5.0, 1e-9);
    EXPECT_NEAR(after.startAngleRad, before.startAngleRad, 1e-9);
    EXPECT_NEAR(after.endAngleRad, before.endAngleRad, 1e-9);
}

TEST(SketchOffsetTest, M17_OFF_008_APointCannotBeOffset) {
    Fixture fixture;
    SketchEdit point;
    point.kind = SketchEditKind::AddPoint;
    point.points = {Vec2{5.0, 5.0}};
    point.label = "Add point";
    const SketchEditOutcome added = fixture.apply(point);
    ASSERT_TRUE(added.applied);

    std::string whyNot;
    EXPECT_FALSE(
        MakeOffsetEdit(fixture.sketch(), added.createdEntities.front(), 5.0, 1.0, &whyNot).valid());
    EXPECT_FALSE(whyNot.empty());
}

// =============================================================================
// Construction geometry (roadmap 4.1.1): a FLAG, not a type
// =============================================================================

TEST(SketchConstructionTest, M17_CONS_001_ConstructionGeometryContributesNoProfileEdge) {
    Fixture fixture;
    // A closed square, plus a diagonal across it.
    LineFrom(fixture, Vec2{0, 0}, Vec2{100, 0});
    LineFrom(fixture, Vec2{100, 0}, Vec2{100, 100});
    LineFrom(fixture, Vec2{100, 100}, Vec2{0, 100});
    LineFrom(fixture, Vec2{0, 100}, Vec2{0, 0});
    const SketchEntityId diagonal = LineFrom(fixture, Vec2{0, 0}, Vec2{100, 100});

    // The diagonal makes the sketch unchainable: five edges cannot form one
    // closed loop, and BuildProfile says so rather than guessing.
    EXPECT_FALSE(static_cast<bool>(BuildProfile(fixture.sketch())));

    // Mark it construction and the same sketch profiles cleanly. THIS is what
    // construction geometry is for -- a line you measure from that the pad does
    // not sweep.
    ASSERT_EQ(fixture.document.setSketchEntitiesConstruction(fixture.sketchId, {diagonal}, true),
              1u);
    const ProfileResult profiled = BuildProfile(fixture.sketch());
    EXPECT_TRUE(static_cast<bool>(profiled)) << profiled.message;
}

TEST(SketchConstructionTest, M17_CONS_002_ItIsAFlagAndNotADifferentKindOfEntity) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0, 0}, Vec2{100, 0});
    ASSERT_EQ(fixture.document.setSketchEntitiesConstruction(fixture.sketchId, {line}, true), 1u);

    // Still a Line, with the same id and the same geometry. Roadmap 4.1.1
    // point 2: construction geometry is not a parallel type hierarchy.
    const SketchEntity* entity = fixture.sketch().findEntity(line);
    ASSERT_NE(entity, nullptr);
    EXPECT_TRUE(std::holds_alternative<SketchLine>(entity->geometry));
    EXPECT_TRUE(entity->construction);
    EXPECT_TRUE(fixture.sketch().isConstruction(line));

    // And still fully constrainable -- a centreline you cannot dimension is
    // useless, so the solver treats it exactly like anything else.
    SketchEdit horizontal;
    horizontal.kind = SketchEditKind::AddHorizontal;
    horizontal.refs = {SketchElementRef{line, SketchSubElement::Whole}};
    horizontal.label = "Horizontal";
    EXPECT_TRUE(fixture.apply(horizontal).applied);
}

TEST(SketchConstructionTest, M17_CONS_003_TheWholeSelectionSwitchesInOneUndoStep) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId b = LineFrom(fixture, Vec2{0, 10}, Vec2{100, 10});
    const SketchEntityId c = LineFrom(fixture, Vec2{0, 20}, Vec2{100, 20});

    ASSERT_EQ(fixture.document.setSketchEntitiesConstruction(fixture.sketchId, {a, b, c}, true),
              3u);
    ASSERT_TRUE(fixture.document.undo());
    // ALL THREE come back. One undo step per user action, not per entity.
    EXPECT_FALSE(fixture.sketch().isConstruction(a));
    EXPECT_FALSE(fixture.sketch().isConstruction(b));
    EXPECT_FALSE(fixture.sketch().isConstruction(c));

    ASSERT_TRUE(fixture.document.redo());
    EXPECT_TRUE(fixture.sketch().isConstruction(a));
    EXPECT_TRUE(fixture.sketch().isConstruction(c));
}

TEST(SketchConstructionTest, M17_CONS_004_AMixedSelectionUndoesToWhatEachOneWas) {
    Fixture fixture;
    const SketchEntityId already = LineFrom(fixture, Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId notYet = LineFrom(fixture, Vec2{0, 10}, Vec2{100, 10});
    ASSERT_EQ(fixture.document.setSketchEntitiesConstruction(fixture.sketchId, {already}, true),
              1u);

    // Switching both: only one actually changes.
    ASSERT_EQ(
        fixture.document.setSketchEntitiesConstruction(fixture.sketchId, {already, notYet}, true),
        1u);
    ASSERT_TRUE(fixture.document.undo());
    // The one that was ALREADY construction stays construction. A single delta
    // saying "these two became construction" could not express that.
    EXPECT_TRUE(fixture.sketch().isConstruction(already));
    EXPECT_FALSE(fixture.sketch().isConstruction(notYet));
}

TEST(SketchConstructionTest, M17_CONS_005_ANoOpSwitchLeavesNoUndoStep) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0, 0}, Vec2{100, 0});
    ASSERT_EQ(fixture.document.setSketchEntitiesConstruction(fixture.sketchId, {line}, true), 1u);

    const std::size_t depth = fixture.document.undoDepth();
    // Already construction: nothing to do, and nothing on the stack for the
    // user to walk back through.
    EXPECT_EQ(fixture.document.setSketchEntitiesConstruction(fixture.sketchId, {line}, true), 0u);
    EXPECT_EQ(fixture.document.undoDepth(), depth);
}

// =============================================================================
// Selection handles: click a line, then click the end you meant
// =============================================================================

TEST(SketchHandleTest, M17_HANDLE_001_EachEntityOffersThePointsTheSolverKnows) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId circle = CircleAt(fixture, Vec2{50.0, 50.0}, 20.0);

    const std::vector<SketchElementRef> lineHandles = EntityHandles(fixture.sketch(), line);
    ASSERT_EQ(lineHandles.size(), 2u);
    EXPECT_EQ(lineHandles[0].subElement, SketchSubElement::StartPoint);
    EXPECT_EQ(lineHandles[1].subElement, SketchSubElement::EndPoint);

    // A circle offers its centre and NOT its rim: the rim is a radius, not a
    // point the solver has a variable for.
    const std::vector<SketchElementRef> circleHandles = EntityHandles(fixture.sketch(), circle);
    ASSERT_EQ(circleHandles.size(), 1u);
    EXPECT_EQ(circleHandles[0].subElement, SketchSubElement::CenterPoint);
}

TEST(SketchHandleTest, M17_HANDLE_002_AnArcOffersItsCentreANDBothTips) {
    Fixture fixture;
    SketchEdit arc;
    arc.kind = SketchEditKind::AddArc;
    arc.points = {Vec2{0.0, 0.0}, Vec2{20.0, 0.0}, Vec2{0.0, 20.0}};
    arc.label = "Add arc";
    const SketchEditOutcome outcome = fixture.apply(arc);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // ADR-M12-003 said a reference to an arc tip resolved to the RADIUS
    // variable, so a handle there would offer a target no constraint could
    // hold. ADR-M17-018 replaced that: the tips are variables of their own now,
    // and a handle the user cannot grab is a constraint they cannot make.
    const std::vector<SketchElementRef> handles =
        EntityHandles(fixture.sketch(), outcome.createdEntities.front());
    ASSERT_EQ(handles.size(), 3u);
    EXPECT_EQ(handles[0].subElement, SketchSubElement::CenterPoint);
    EXPECT_EQ(handles[1].subElement, SketchSubElement::StartPoint);
    EXPECT_EQ(handles[2].subElement, SketchSubElement::EndPoint);
}

TEST(SketchHandleTest, M17_HANDLE_003_ClickingAnEndOfASelectedLineNarrowsTheSelection) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});

    SketchCanvasModel model;
    // Click the middle of the line: the line itself.
    ASSERT_TRUE(model.selectAt(fixture.sketch(), Vec2{50.0, 0.0}, 1.0));
    ASSERT_EQ(model.selection().size(), 1u);
    EXPECT_EQ(model.selection()[0].subElement, SketchSubElement::Whole);

    // Now click its end. THE POINT OF THIS TEST: one thing stays selected, and
    // it is the endpoint. Adding instead would leave a line AND a point
    // selected, which roadmap 7.1 has no dimension for -- so the flow the
    // handles exist to support would end in a refusal.
    ASSERT_TRUE(model.selectAt(fixture.sketch(), Vec2{100.0, 0.0}, 1.0));
    ASSERT_EQ(model.selection().size(), 1u);
    EXPECT_EQ(model.selection()[0].entityId, line);
    EXPECT_EQ(model.selection()[0].subElement, SketchSubElement::EndPoint);
}

TEST(SketchHandleTest, M17_HANDLE_004_AHandleOfSomethingElseStillADDSToTheSelection) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{0.0, 40.0}, Vec2{100.0, 40.0});

    SketchCanvasModel model;
    ASSERT_TRUE(model.selectAt(fixture.sketch(), Vec2{50.0, 0.0}, 1.0)); // line a
    ASSERT_TRUE(model.selectAt(fixture.sketch(), Vec2{0.0, 40.0}, 1.0)); // b's start

    // Narrowing applies only to the entity that was already selected. Roadmap
    // 13.1's no-modifier multi-select is untouched.
    ASSERT_EQ(model.selection().size(), 2u);
    EXPECT_EQ(model.selection()[0].entityId, a);
    EXPECT_EQ(model.selection()[0].subElement, SketchSubElement::Whole);
    EXPECT_EQ(model.selection()[1].entityId, b);
    EXPECT_EQ(model.selection()[1].subElement, SketchSubElement::StartPoint);
}

TEST(SketchHandleTest, M17_HANDLE_005_TwoEndsPickedThisWayCanBeDimensioned) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{40.0, 0.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{10.0, 30.0}, Vec2{40.0, 30.0});

    SketchCanvasModel model;
    model.selectAt(fixture.sketch(), Vec2{20.0, 0.0}, 1.0);  // line a
    model.selectAt(fixture.sketch(), Vec2{0.0, 0.0}, 1.0);   // narrowed to a.start
    model.selectAt(fixture.sketch(), Vec2{25.0, 30.0}, 1.0); // line b
    model.selectAt(fixture.sketch(), Vec2{40.0, 30.0}, 1.0); // narrowed to b.end
    ASSERT_EQ(model.selection().size(), 2u);

    // The whole reason the handles exist: two ends, picked by clicking the
    // lines and then the ends, are a dimensionable pair.
    std::string whyNot;
    const SketchEdit edit =
        model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    EXPECT_EQ(edit.kind, SketchEditKind::AddDistance);
    const SketchEditOutcome applied = fixture.apply(edit);
    ASSERT_TRUE(applied.applied) << applied.status;
    const Parameter* parameter = fixture.document.parameters().findById(applied.createdParameter);
    ASSERT_NE(parameter, nullptr);
    EXPECT_NEAR(parameter->value(), 50.0, 1e-6); // (0,0) to (40,30): a 3-4-5

    // And the two axis dimensions the same pair admits.
    model.setSelection({SketchElementRef{a, SketchSubElement::StartPoint},
                        SketchElementRef{b, SketchSubElement::EndPoint}});
    const SketchEdit horizontal =
        model.requestDimension(fixture.sketch(), SketchEditKind::AddHorizontalDistance, &whyNot);
    ASSERT_TRUE(horizontal.valid()) << whyNot;
    const SketchEditOutcome dx = fixture.apply(horizontal);
    ASSERT_TRUE(dx.applied) << dx.status;
    EXPECT_NEAR(fixture.document.parameters().findById(dx.createdParameter)->value(), 40.0, 1e-6);
}

TEST(SketchHandleTest, M17_HANDLE_006_ANarrowedHandleCanBeClickedBackOff) {
    Fixture fixture;
    LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});

    SketchCanvasModel model;
    model.selectAt(fixture.sketch(), Vec2{50.0, 0.0}, 1.0);
    model.selectAt(fixture.sketch(), Vec2{100.0, 0.0}, 1.0);
    ASSERT_EQ(model.selection().size(), 1u);
    // Clicking the SAME handle again toggles it off, as every other pick does
    // (roadmap 13.1). Narrowing replaced one ref with another; it did not
    // create a thing that cannot be deselected.
    ASSERT_TRUE(model.selectAt(fixture.sketch(), Vec2{100.0, 0.0}, 1.0));
    EXPECT_TRUE(model.selection().empty());
}

// =============================================================================
// The sketch origin as a real, measurable point
// =============================================================================

TEST(SketchOriginPointTest, M12_ORIGINPT_001_TheEditMakesAFixedPointAtZero) {
    Fixture fixture;
    const SketchEditOutcome outcome = fixture.apply(MakeOriginPointEdit());
    ASSERT_TRUE(outcome.applied) << outcome.status;
    ASSERT_EQ(outcome.createdEntities.size(), 1u);
    ASSERT_EQ(outcome.createdConstraints.size(), 1u);

    const SketchEntity* point = fixture.sketch().findEntity(outcome.createdEntities.front());
    ASSERT_NE(point, nullptr);
    const SketchPoint& position = std::get<SketchPoint>(point->geometry);
    EXPECT_NEAR(position.position.x, 0.0, kTol);
    EXPECT_NEAR(position.position.y, 0.0, kTol);

    // FIXED, or it is not an origin -- it is a point that happens to start
    // there and drifts on the first solve.
    const SketchConstraint* fix =
        fixture.sketch().findConstraint(outcome.createdConstraints.front());
    ASSERT_NE(fix, nullptr);
    EXPECT_TRUE(std::holds_alternative<FixConstraint>(fix->data));
    EXPECT_EQ(std::get<FixConstraint>(fix->data).target.entityId,
              outcome.createdEntities.front());
}

TEST(SketchOriginPointTest, M12_ORIGINPT_002_TheOriginIsFoundAndAnEmptySketchHasNone) {
    Fixture fixture;
    EXPECT_EQ(FindSketchOrigin(fixture.sketch()), kInvalidSketchEntityId);

    const SketchEditOutcome outcome = fixture.apply(MakeOriginPointEdit());
    ASSERT_TRUE(outcome.applied) << outcome.status;
    EXPECT_EQ(FindSketchOrigin(fixture.sketch()), outcome.createdEntities.front());
}

TEST(SketchOriginPointTest, M12_ORIGINPT_003_APointSomewhereElseIsNotTheOrigin) {
    Fixture fixture;
    SketchEdit stray;
    stray.kind = SketchEditKind::AddPoint;
    stray.points = {Vec2{10.0, 0.0}};
    stray.label = "Add point";
    ASSERT_TRUE(fixture.apply(stray).applied);
    // Otherwise "add an origin if there isn't one" would find any stray point
    // and measure from the wrong place.
    EXPECT_EQ(FindSketchOrigin(fixture.sketch()), kInvalidSketchEntityId);
}

TEST(SketchOriginPointTest, M12_ORIGINPT_004_TheOriginCanBeDimensionedToAPoint) {
    Fixture fixture;
    const SketchEditOutcome origin = fixture.apply(MakeOriginPointEdit());
    ASSERT_TRUE(origin.applied) << origin.status;
    const SketchEntityId line = LineFrom(fixture, Vec2{30.0, 40.0}, Vec2{80.0, 40.0});

    // THE POINT OF THE WHOLE CHANGE: origin + an endpoint infers a Distance,
    // through the ordinary two-points rule and with no special case anywhere.
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{origin.createdEntities.front(), SketchSubElement::Whole},
                        SketchElementRef{line, SketchSubElement::StartPoint}});
    std::string whyNot;
    const SketchEdit edit =
        model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    EXPECT_EQ(edit.kind, SketchEditKind::AddDistance);

    const SketchEditOutcome applied = fixture.apply(edit);
    ASSERT_TRUE(applied.applied) << applied.status;
    // Seeded at what it MEASURES: 3-4-5, so 50 mm.
    const Parameter* parameter = fixture.document.parameters().findById(applied.createdParameter);
    ASSERT_NE(parameter, nullptr);
    EXPECT_NEAR(parameter->value(), 50.0, 1e-6);
}

TEST(SketchOriginPointTest, M12_ORIGINPT_005_AnOriginPointIsWhatACornerSnapsTo) {
    Fixture fixture;
    const SketchEditOutcome origin = fixture.apply(MakeOriginPointEdit());
    ASSERT_TRUE(origin.applied) << origin.status;

    SketchCanvasModel model;
    model.setTool(SketchTool::Rectangle);
    fixture.click(model, Vec2{0.1, -0.1});
    const SketchEditOutcome rectangle = fixture.click(model, Vec2{100.0, 50.0});
    ASSERT_TRUE(rectangle.applied) << rectangle.status;

    // A defined point outranks the bare origin in SnapCursor, so the corner is
    // JOINED to the origin point rather than independently pinned. Same
    // geometry, and it is what makes the origin worth having: the corner and
    // the origin are now one thing the solver can move together.
    ASSERT_EQ(rectangle.createdConstraints.size(), 9u);
    int coincidences = 0;
    int fixes = 0;
    for (const SketchConstraintId id : rectangle.createdConstraints) {
        const SketchConstraint* constraint = fixture.sketch().findConstraint(id);
        ASSERT_NE(constraint, nullptr);
        if (std::holds_alternative<CoincidentConstraint>(constraint->data)) ++coincidences;
        if (std::holds_alternative<FixConstraint>(constraint->data)) ++fixes;
    }
    EXPECT_EQ(coincidences, 5); // 4 corners + the join to the origin
    EXPECT_EQ(fixes, 0);        // the origin point already carries the Fix
}

// =============================================================================
// Constraint badges: the H / V / X / o marks on the canvas (roadmap 6.3)
//
// The LAYOUT lives here rather than in the widget because painting and
// hit-testing both read it, and a badge that cannot be clicked where it is
// drawn is the defect that pattern exists to prevent.
// =============================================================================

TEST(SketchBadgeTest, M12_BADGE_001_EveryNonDimensionalConstraintGetsAGlyph) {
    Fixture fixture;
    SketchCanvasModel model;
    model.setTool(SketchTool::Rectangle);
    fixture.click(model, Vec2{0.0, 0.0});
    ASSERT_TRUE(fixture.click(model, Vec2{100.0, 50.0}).applied);

    const std::vector<ConstraintBadge> badges = ConstraintBadgesFor(fixture.sketch());
    // 2 H + 2 V + 4 coincident + the origin Fix.
    EXPECT_EQ(badges.size(), 9u);
    for (const ConstraintBadge& badge : badges) {
        EXPECT_FALSE(badge.glyph.empty()) << "a badge with no mark is an empty box";
        EXPECT_NE(badge.id, kInvalidSketchConstraintId);
    }
    const auto glyphs = [&](const char* want) {
        return std::count_if(badges.begin(), badges.end(), [&](const ConstraintBadge& b) {
            return b.glyph == want;
        });
    };
    EXPECT_EQ(glyphs("H"), 2);
    EXPECT_EQ(glyphs("V"), 2);
    EXPECT_EQ(glyphs("o"), 4);
    EXPECT_EQ(glyphs("X"), 1);
}

TEST(SketchBadgeTest, M12_BADGE_002_ADimensionHasNoBadge) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});

    SketchCanvasModel model;
    model.setSelection({SketchElementRef{line, SketchSubElement::Whole}});
    std::string whyNot;
    const SketchEdit edit =
        model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    ASSERT_TRUE(fixture.apply(edit).applied);

    // Dimensions are DRAWN as dimensions -- a number, extension lines and
    // arrows. A badge as well would be the same fact printed twice.
    EXPECT_TRUE(ConstraintBadgesFor(fixture.sketch()).empty());
}

TEST(SketchBadgeTest, M12_BADGE_003_BadgesOnOneEntityAreStackedNotOverlaid) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});

    SketchEdit horizontal;
    horizontal.kind = SketchEditKind::AddHorizontal;
    horizontal.refs = {SketchElementRef{line, SketchSubElement::Whole}};
    horizontal.label = "Horizontal";
    ASSERT_TRUE(fixture.apply(horizontal).applied);

    SketchEdit fix;
    fix.kind = SketchEditKind::AddFix;
    fix.refs = {SketchElementRef{line, SketchSubElement::StartPoint}};
    fix.label = "Fix";
    ASSERT_TRUE(fixture.apply(fix).applied);

    const std::vector<ConstraintBadge> badges = ConstraintBadgesFor(fixture.sketch());
    ASSERT_EQ(badges.size(), 2u);
    // Same anchor, DIFFERENT slots. Without the slot the second badge prints
    // exactly on top of the first and one of the two becomes unreadable and
    // unclickable at the same time.
    EXPECT_NEAR(badges[0].anchorMm.x, badges[1].anchorMm.x, kTol);
    EXPECT_NEAR(badges[0].anchorMm.y, badges[1].anchorMm.y, kTol);
    EXPECT_NE(badges[0].slot, badges[1].slot);
}

TEST(SketchBadgeTest, M12_BADGE_004_ALineHangsItsBadgeOffTheQuarterPointNotTheMiddle) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchEdit horizontal;
    horizontal.kind = SketchEditKind::AddHorizontal;
    horizontal.refs = {SketchElementRef{line, SketchSubElement::Whole}};
    horizontal.label = "Horizontal";
    ASSERT_TRUE(fixture.apply(horizontal).applied);

    const std::vector<ConstraintBadge> badges = ConstraintBadgesFor(fixture.sketch());
    ASSERT_EQ(badges.size(), 1u);
    // The midpoint is where a dimension puts its VALUE, so a badge there would
    // sit on top of the number for every dimensioned line.
    EXPECT_NEAR(badges[0].anchorMm.x, 25.0, 1e-6);
    EXPECT_NEAR(badges[0].anchorMm.y, 0.0, 1e-6);
}

TEST(SketchBadgeTest, M12_BADGE_005_ABadgeCarriesTheSolversVerdict) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchEdit horizontal;
    horizontal.kind = SketchEditKind::AddHorizontal;
    horizontal.refs = {SketchElementRef{line, SketchSubElement::Whole}};
    horizontal.label = "Horizontal";
    const SketchEditOutcome added = fixture.apply(horizontal);
    ASSERT_TRUE(added.applied);

    // No solver in this fixture, so nothing is blamed -- which is the point:
    // `offending` must come from the sketch's own verdict and never be
    // guessed, or a badge would go red on a sketch that solved cleanly.
    std::vector<ConstraintBadge> badges = ConstraintBadgesFor(fixture.sketch());
    ASSERT_EQ(badges.size(), 1u);
    EXPECT_FALSE(badges[0].offending);
}

// =============================================================================
// Undo of sketch geometry and constraints (M12.0, Core)
// =============================================================================

TEST(SketchUndoTest, M12_UNDO_001_DrawingALineIsUndoableAndRedoable) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    ASSERT_NE(line, kInvalidSketchEntityId);

    ASSERT_TRUE(fixture.document.undo());
    EXPECT_EQ(fixture.sketch().findEntity(line), nullptr);

    ASSERT_TRUE(fixture.document.redo());
    ASSERT_NE(fixture.sketch().findEntity(line), nullptr);
    // THE IDENTITY RULE (A03): the same entity comes back under the SAME id, or
    // every constraint that referenced it would be orphaned by an undo.
    EXPECT_EQ(fixture.sketch().entities().front().id, line);
}

TEST(SketchUndoTest, M12_UNDO_002_DeletingGeometryRestoresItsCascadedConstraints) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});

    SketchEdit horizontal;
    horizontal.kind = SketchEditKind::AddHorizontal;
    horizontal.refs = {SketchElementRef{a, SketchSubElement::Whole}};
    horizontal.label = "Add horizontal";
    ASSERT_TRUE(fixture.apply(horizontal).applied);
    ASSERT_EQ(fixture.sketch().constraints().size(), 1u);
    const SketchConstraintId constraintId = fixture.sketch().constraints().front().id;

    SketchEdit remove;
    remove.kind = SketchEditKind::DeleteEntities;
    remove.refs = {SketchElementRef{a, SketchSubElement::Whole}};
    remove.label = "Delete sketch geometry";
    ASSERT_TRUE(fixture.apply(remove).applied);
    EXPECT_EQ(fixture.sketch().entities().size(), 0u);
    EXPECT_EQ(fixture.sketch().constraints().size(), 0u);

    // The undo has to put the ENTITY back before the CONSTRAINT, or the
    // constraint has nothing to attach to. That ordering is what the delete
    // path records, and this is what proves it.
    ASSERT_TRUE(fixture.document.undo());
    ASSERT_EQ(fixture.sketch().entities().size(), 1u);
    ASSERT_EQ(fixture.sketch().constraints().size(), 1u);
    EXPECT_EQ(fixture.sketch().constraints().front().id, constraintId);
}

TEST(SketchUndoTest, M12_UNDO_003_ADimensionUndoesItsParameterTogetherWithItsConstraint) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});

    SketchCanvasModel model;
    model.setSelection({SketchElementRef{line, SketchSubElement::Whole}});
    std::string whyNot;
    const SketchEdit edit = model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    const SketchEditOutcome outcome = fixture.apply(edit);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    ASSERT_NE(outcome.createdParameter, kInvalidObjectId);

    ASSERT_TRUE(fixture.document.undo());
    // An undo that removed the constraint and left the parameter behind would
    // leave an orphan in the tree and in the saved file, every single time.
    EXPECT_EQ(fixture.sketch().constraints().size(), 0u);
    EXPECT_EQ(fixture.document.parameters().findById(outcome.createdParameter), nullptr);
}

// =============================================================================
// Dimensions: inference, seeding and editing
// =============================================================================

TEST(SketchDimensionTest, M12_DIM_001_OneLineInfersALengthSeededAtWhatItMeasures) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{line, SketchSubElement::Whole}});

    std::string whyNot;
    const SketchEdit edit = model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    EXPECT_EQ(edit.kind, SketchEditKind::AddLength);

    const SketchEditOutcome outcome = fixture.apply(edit);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    const Parameter* parameter = fixture.document.parameters().findById(outcome.createdParameter);
    ASSERT_NE(parameter, nullptr);
    // SEEDED AT THE MEASURED VALUE, so adding a dimension never moves anything.
    EXPECT_NEAR(parameter->value(), 100.0, 1e-9);
    EXPECT_EQ(parameter->unit(), UnitType::Millimeter);
}

TEST(SketchDimensionTest, M12_DIM_002_ACircleInfersDiameterAndAnArcInfersRadius) {
    Fixture fixture;
    const SketchEntityId circle = CircleAt(fixture, Vec2{0.0, 0.0}, 25.0);

    SketchCanvasModel model;
    model.setSelection({SketchElementRef{circle, SketchSubElement::Whole}});
    std::string whyNot;
    EXPECT_EQ(model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot).kind,
              SketchEditKind::AddDiameter);

    SketchEdit arcEdit;
    arcEdit.kind = SketchEditKind::AddArc;
    arcEdit.points = {Vec2{200.0, 0.0}, Vec2{220.0, 0.0}, Vec2{200.0, 20.0}};
    arcEdit.label = "Add arc";
    const SketchEditOutcome arcOutcome = fixture.apply(arcEdit);
    ASSERT_TRUE(arcOutcome.applied) << arcOutcome.status;

    model.setSelection({SketchElementRef{arcOutcome.createdEntities.front(),
                                         SketchSubElement::Whole}});
    EXPECT_EQ(model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot).kind,
              SketchEditKind::AddRadius);
}

TEST(SketchDimensionTest, M12_DIM_003_AnExplicitKindOverridesTheInference) {
    Fixture fixture;
    const SketchEntityId circle = CircleAt(fixture, Vec2{0.0, 0.0}, 25.0);
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{circle, SketchSubElement::Whole}});

    std::string whyNot;
    const SketchEdit edit =
        model.requestDimension(fixture.sketch(), SketchEditKind::AddRadius, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    EXPECT_EQ(edit.kind, SketchEditKind::AddRadius);
    const SketchEditOutcome outcome = fixture.apply(edit);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    EXPECT_NEAR(fixture.document.parameters().findById(outcome.createdParameter)->value(), 25.0,
                1e-9);
}

TEST(SketchDimensionTest, M12_DIM_004_AnUninterpretableSelectionIsRefusedWithTheTable) {
    Fixture fixture;
    LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchCanvasModel model;

    std::string whyNot;
    const SketchEdit edit = model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot);
    EXPECT_FALSE(edit.valid());
    // A refusal with no reason is the failure mode roadmap 8 is written
    // against; the message has to say what WOULD work.
    EXPECT_FALSE(whyNot.empty());
}

TEST(SketchDimensionTest, M12_DIM_005_AnAngleIsStoredInRadiansAndShownInDegrees) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{0.0, 100.0});

    SketchCanvasModel model;
    model.setSelection({SketchElementRef{a, SketchSubElement::Whole},
                        SketchElementRef{b, SketchSubElement::Whole}});
    std::string whyNot;
    const SketchEdit edit = model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    EXPECT_EQ(edit.kind, SketchEditKind::AddAngle);

    const SketchEditOutcome outcome = fixture.apply(edit);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    const Parameter* parameter = fixture.document.parameters().findById(outcome.createdParameter);
    ASSERT_NE(parameter, nullptr);
    EXPECT_EQ(parameter->unit(), UnitType::Radian);
    EXPECT_NEAR(parameter->value(), 3.14159265358979323846 / 2.0, 1e-9);

    // The EDITOR shows degrees.
    const std::string shown = DimensionEditText(fixture.document, fixture.sketch(),
                                                outcome.createdConstraints.front());
    EXPECT_EQ(shown, "90");
}

TEST(SketchDimensionTest, M12_DIM_006_TypingDegreesIntoAnAngleStoresRadians) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{0.0, 100.0});
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{a, SketchSubElement::Whole},
                        SketchElementRef{b, SketchSubElement::Whole}});
    std::string whyNot;
    const SketchEditOutcome outcome =
        fixture.apply(model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot));
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const SketchEditOutcome commit = CommitDimensionValue(
        fixture.document, fixture.sketch(), outcome.createdConstraints.front(), "45");
    ASSERT_TRUE(commit.applied) << commit.status;
    EXPECT_NEAR(fixture.document.parameters().findById(outcome.createdParameter)->value(),
                3.14159265358979323846 / 4.0, 1e-9);
}

TEST(SketchDimensionTest, M12_DIM_007_ADimensionAcceptsAnExpression) {
    Fixture fixture;
    fixture.document.addParameter("Width", 80.0, UnitType::Millimeter);
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{line, SketchSubElement::Whole}});
    std::string whyNot;
    const SketchEditOutcome outcome =
        fixture.apply(model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot));
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const SketchEditOutcome commit = CommitDimensionValue(
        fixture.document, fixture.sketch(), outcome.createdConstraints.front(), "#Width / 2");
    ASSERT_TRUE(commit.applied) << commit.status;
    const Parameter* parameter = fixture.document.parameters().findById(outcome.createdParameter);
    ASSERT_NE(parameter, nullptr);
    EXPECT_EQ(parameter->expression(), "#Width / 2");
    // The EDITOR shows the formula, not its value: showing 40 would make the
    // first keystroke destroy the expression.
    EXPECT_EQ(DimensionEditText(fixture.document, fixture.sketch(),
                                outcome.createdConstraints.front()),
              "#Width / 2");
}

TEST(SketchDimensionTest, M12_DIM_008_ARefusedExpressionExplainsItselfAndChangesNothing) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{line, SketchSubElement::Whole}});
    std::string whyNot;
    const SketchEditOutcome outcome =
        fixture.apply(model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot));
    ASSERT_TRUE(outcome.applied) << outcome.status;

    const SketchEditOutcome commit = CommitDimensionValue(
        fixture.document, fixture.sketch(), outcome.createdConstraints.front(), "#Nope * 2");
    EXPECT_FALSE(commit.applied);
    EXPECT_FALSE(commit.status.empty());
    // The three-line caret rendering, so the user can see WHERE it went wrong.
    EXPECT_NE(commit.detail.find('^'), std::string::npos);
    EXPECT_NEAR(fixture.document.parameters().findById(outcome.createdParameter)->value(), 100.0,
                1e-9);
}

// =============================================================================
// M14 -- dimensions are drawn AS dimensions
// =============================================================================
//
// Extension lines, a dimension line standing off the geometry, arrowheads at
// its ends, and the value on the line. All of it is geometry computed here, in
// the Qt-free layer, so it can be asserted rather than eyeballed.

namespace {

// A dimension on the only line in a fresh sketch.
DimensionAnnotation LengthDimensionOf(Fixture& fixture, SketchEntityId line) {
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{line, SketchSubElement::Whole}});
    std::string whyNot;
    EXPECT_TRUE(
        fixture.apply(model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot))
            .applied)
        << whyNot;
    const std::vector<DimensionAnnotation> annotations =
        DimensionAnnotationsFor(fixture.document, fixture.sketch());
    EXPECT_EQ(annotations.size(), 1u);
    return annotations.empty() ? DimensionAnnotation{} : annotations.front();
}

double Length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
Vec2 Minus(Vec2 a, Vec2 b) { return Vec2{a.x - b.x, a.y - b.y}; }
double Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }

} // namespace

TEST(SketchDimensionDrawingTest, M14_DRAW_001_ALengthGetsTwoExtensionLinesAndTwoArrows) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const DimensionAnnotation annotation = LengthDimensionOf(fixture, line);

    EXPECT_EQ(annotation.text, "100");
    ASSERT_EQ(annotation.extensionLines.size(), 2u);
    ASSERT_EQ(annotation.dimensionLines.size(), 1u);
    ASSERT_EQ(annotation.arrows.size(), 2u);
    EXPECT_FALSE(annotation.hasArc);
}

TEST(SketchDimensionDrawingTest, M14_DRAW_002_TheArrowsPointOutwardAlongTheDimensionLine) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const DimensionAnnotation annotation = LengthDimensionOf(fixture, line);
    ASSERT_EQ(annotation.arrows.size(), 2u);

    // Opposed, and each pointing AWAY from the other -- that is what makes a
    // pair of heads read as "this span" rather than as two unrelated markers.
    // Two heads pointing the same way would satisfy a mere count.
    EXPECT_NEAR(Dot(annotation.arrows[0].directionMm, annotation.arrows[1].directionMm), -1.0,
                1e-9);
    const Vec2 tipToTip =
        Minus(annotation.arrows[1].tipMm, annotation.arrows[0].tipMm);
    EXPECT_GT(Dot(tipToTip, annotation.arrows[1].directionMm), 0.0);
    EXPECT_LT(Dot(tipToTip, annotation.arrows[0].directionMm), 0.0);
    // Each direction is a UNIT vector: the widget scales it to a pixel length.
    for (const DimensionArrow& arrow : annotation.arrows)
        EXPECT_NEAR(Length(arrow.directionMm), 1.0, 1e-9);
}

TEST(SketchDimensionDrawingTest, M14_DRAW_003_TheDimensionLineStandsOffTheGeometry) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const DimensionAnnotation annotation = LengthDimensionOf(fixture, line);
    ASSERT_EQ(annotation.dimensionLines.size(), 1u);

    // Offset perpendicular, not drawn on top of the line it measures.
    const DimensionSegment& dim = annotation.dimensionLines.front();
    EXPECT_GT(std::abs(dim.fromMm.y), 1.0);
    EXPECT_NEAR(dim.fromMm.y, dim.toMm.y, 1e-9);
    // ...and it spans the same extent as the geometry.
    EXPECT_NEAR(Length(Minus(dim.toMm, dim.fromMm)), 100.0, 1e-9);
    // The label sits ON the dimension line, which is where a draughtsman looks
    // for it -- and where dimensionAt() hit-tests for the editor.
    EXPECT_NEAR(annotation.labelMm.y, dim.fromMm.y, 1e-9);
    EXPECT_NEAR(annotation.labelMm.x, 50.0, 1e-9);
}

TEST(SketchDimensionDrawingTest, M14_DRAW_004_TheOffsetScalesWithWhatIsMeasured) {
    Fixture small;
    const DimensionAnnotation tiny = LengthDimensionOf(
        small, LineFrom(small, Vec2{0.0, 0.0}, Vec2{8.0, 0.0}));
    Fixture large;
    const DimensionAnnotation big = LengthDimensionOf(
        large, LineFrom(large, Vec2{0.0, 0.0}, Vec2{400.0, 0.0}));

    // A fixed offset would bury an 8 mm feature and look lost on a 400 mm one.
    EXPECT_LT(std::abs(tiny.dimensionLines.front().fromMm.y),
              std::abs(big.dimensionLines.front().fromMm.y));
    // ...but it is clamped at both ends, so neither degenerates.
    EXPECT_GT(std::abs(tiny.dimensionLines.front().fromMm.y), 1.0);
    EXPECT_LT(std::abs(big.dimensionLines.front().fromMm.y), 40.0);
}

TEST(SketchDimensionDrawingTest, M14_DRAW_005_TextNeverReadsUpsideDown) {
    // A line drawn right-to-left runs at 180 degrees. Rotating the text with it
    // would print the value upside down, which is half the dimensions on any
    // rectangle drawn clockwise.
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{100.0, 0.0}, Vec2{0.0, 0.0});
    const DimensionAnnotation annotation = LengthDimensionOf(fixture, line);
    EXPECT_LE(std::abs(annotation.textAngleRad), 3.14159265358979323846 / 2.0 + 1e-9);
}

TEST(SketchDimensionDrawingTest, M14_DRAW_006_ARadiusIsALeaderWithOneArrowAtTheRim) {
    Fixture fixture;
    const SketchEntityId circle = CircleAt(fixture, Vec2{0.0, 0.0}, 25.0);
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{circle, SketchSubElement::Whole}});
    std::string whyNot;
    ASSERT_TRUE(fixture
                    .apply(model.requestDimension(fixture.sketch(), SketchEditKind::AddRadius,
                                                  &whyNot))
                    .applied)
        << whyNot;

    const std::vector<DimensionAnnotation> annotations =
        DimensionAnnotationsFor(fixture.document, fixture.sketch());
    ASSERT_EQ(annotations.size(), 1u);
    const DimensionAnnotation& annotation = annotations.front();

    // "R" prefix: a leader alone cannot say whether a value is a radius or a
    // diameter, and getting that wrong doubles or halves the part.
    EXPECT_EQ(annotation.text, "R25");
    ASSERT_EQ(annotation.arrows.size(), 1u);
    ASSERT_EQ(annotation.dimensionLines.size(), 1u);
    // The leader runs centre -> rim, and the head is AT the rim pointing out.
    EXPECT_NEAR(Length(Minus(annotation.dimensionLines.front().toMm,
                             annotation.dimensionLines.front().fromMm)),
                25.0, 1e-9);
    EXPECT_NEAR(Length(annotation.arrows.front().tipMm), 25.0, 1e-9);
}

TEST(SketchDimensionDrawingTest, M14_DRAW_007_ADiameterRunsThroughTheCentreWithTwoArrows) {
    Fixture fixture;
    const SketchEntityId circle = CircleAt(fixture, Vec2{0.0, 0.0}, 25.0);
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{circle, SketchSubElement::Whole}});
    std::string whyNot;
    ASSERT_TRUE(
        fixture.apply(model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot))
            .applied)
        << whyNot;

    const std::vector<DimensionAnnotation> annotations =
        DimensionAnnotationsFor(fixture.document, fixture.sketch());
    ASSERT_EQ(annotations.size(), 1u);
    const DimensionAnnotation& annotation = annotations.front();

    EXPECT_EQ(annotation.text, "D50");
    ASSERT_EQ(annotation.arrows.size(), 2u);
    ASSERT_EQ(annotation.dimensionLines.size(), 1u);
    // Full diameter, not a radius drawn twice.
    EXPECT_NEAR(Length(Minus(annotation.dimensionLines.front().toMm,
                             annotation.dimensionLines.front().fromMm)),
                50.0, 1e-9);
    // Label at the centre, between the two heads.
    EXPECT_NEAR(Length(annotation.labelMm), 0.0, 1e-9);
}

TEST(SketchDimensionDrawingTest, M14_DRAW_008_AnAngleIsAnArcSweptAboutTheCorner) {
    Fixture fixture;
    // A right angle at the origin.
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{0.0, 80.0});
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{a, SketchSubElement::Whole},
                        SketchElementRef{b, SketchSubElement::Whole}});
    std::string whyNot;
    ASSERT_TRUE(
        fixture.apply(model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot))
            .applied)
        << whyNot;

    const std::vector<DimensionAnnotation> annotations =
        DimensionAnnotationsFor(fixture.document, fixture.sketch());
    ASSERT_EQ(annotations.size(), 1u);
    const DimensionAnnotation& annotation = annotations.front();

    EXPECT_EQ(annotation.text, "90deg");
    ASSERT_TRUE(annotation.hasArc);
    // Centred on the CORNER -- the arc has to sit where the two lines meet, not
    // between their midpoints.
    EXPECT_NEAR(Length(annotation.arc.centreMm), 0.0, 1e-6);
    EXPECT_GT(annotation.arc.radiusMm, 0.0);
    EXPECT_NEAR(annotation.arc.endRad - annotation.arc.startRad,
                3.14159265358979323846 / 2.0, 1e-6);
    ASSERT_EQ(annotation.arrows.size(), 2u);
    // Angular values read upright, never rotated with the arc.
    EXPECT_NEAR(annotation.textAngleRad, 0.0, 1e-12);
}

TEST(SketchDimensionDrawingTest, M14_DRAW_009_ParallelLinesStillShowTheirValue) {
    Fixture fixture;
    // Two parallel lines have no corner to sweep an arc about. The dimension
    // must still SHOW -- degrading to a bare value is honest; drawing an arc of
    // an invented radius somewhere would not be.
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{0.0, 40.0}, Vec2{100.0, 40.0});
    fixture.document.addParameter("a1", 0.0, UnitType::Radian);
    const ObjectId angleParameter =
        fixture.document.parameters().findByName("a1")->id();
    ASSERT_NE(fixture.document.addSketchConstraint(fixture.sketchId,
                                                   AngleConstraint{a, b, angleParameter}),
              kInvalidSketchConstraintId);

    const std::vector<DimensionAnnotation> annotations =
        DimensionAnnotationsFor(fixture.document, fixture.sketch());
    ASSERT_EQ(annotations.size(), 1u);
    EXPECT_FALSE(annotations.front().hasArc);
    EXPECT_FALSE(annotations.front().text.empty());
    EXPECT_TRUE(std::isfinite(annotations.front().labelMm.x));
    EXPECT_TRUE(std::isfinite(annotations.front().labelMm.y));
}

TEST(SketchDimensionDrawingTest, M14_DRAW_010_TheLabelIsWhereTheEditorHitTests) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const DimensionAnnotation annotation = LengthDimensionOf(fixture, line);
    // Double-clicking a dimension finds it by its LABEL position. If the label
    // moved to the dimension line while the hit test still looked at the
    // geometry, the editor would open only when clicking empty space.
    EXPECT_GT(std::abs(annotation.labelMm.y), 1.0);
    EXPECT_NEAR(annotation.labelMm.x, 50.0, 1e-9);
}

TEST(SketchDimensionTest, M12_DIM_010_AMalformedDimensionEditIsRefusedNotUndefined) {
    Fixture fixture;
    LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});

    // Hand-built the way a second consumer (the OCCT overlay) could build one,
    // with the references missing. `refs.front()` on an empty vector is
    // undefined behaviour, not a failed command, so the arity is checked before
    // anything reads it.
    for (const SketchEditKind kind :
         {SketchEditKind::AddLength, SketchEditKind::AddRadius, SketchEditKind::AddDiameter,
          SketchEditKind::AddDistance, SketchEditKind::AddAngle}) {
        SketchEdit edit;
        edit.kind = kind;
        edit.label = "Malformed";
        const SketchEditOutcome outcome = fixture.apply(edit);
        EXPECT_FALSE(outcome.applied) << SketchEditKindName(kind);
        EXPECT_FALSE(outcome.status.empty()) << SketchEditKindName(kind);
    }
    // And a one-reference Distance, which passes an "is it empty" check and
    // still reads refs[1].
    SketchEdit half;
    half.kind = SketchEditKind::AddDistance;
    half.refs = {SketchElementRef{fixture.sketch().entities().front().id,
                                 SketchSubElement::StartPoint}};
    half.label = "Malformed";
    EXPECT_FALSE(fixture.apply(half).applied);
    EXPECT_EQ(fixture.sketch().constraints().size(), 0u);
}

// =============================================================================
// Constraint applicability and the constraint manager
// =============================================================================

TEST(SketchConstraintCommandTest, M12_CON_001_HorizontalRefusesSomethingThatIsNotALine) {
    Fixture fixture;
    const SketchEntityId circle = CircleAt(fixture, Vec2{0.0, 0.0}, 20.0);
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{circle, SketchSubElement::Whole}});

    std::string whyNot;
    const SketchEdit edit =
        model.requestConstraint(fixture.sketch(), SketchEditKind::AddHorizontal, &whyNot);
    EXPECT_FALSE(edit.valid());
    EXPECT_NE(whyNot.find("not a line"), std::string::npos) << whyNot;
}

TEST(SketchConstraintCommandTest, M12_CON_002_CoincidentNeedsExactlyTwoDistinctPoints) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchCanvasModel model;
    std::string whyNot;

    model.setSelection({SketchElementRef{a, SketchSubElement::StartPoint}});
    EXPECT_FALSE(model.requestConstraint(fixture.sketch(), SketchEditKind::AddCoincident, &whyNot)
                     .valid());

    model.setSelection({SketchElementRef{a, SketchSubElement::StartPoint},
                        SketchElementRef{a, SketchSubElement::StartPoint}});
    EXPECT_FALSE(model.requestConstraint(fixture.sketch(), SketchEditKind::AddCoincident, &whyNot)
                     .valid());

    const SketchEntityId b = LineFrom(fixture, Vec2{100.0, 0.0}, Vec2{100.0, 50.0});
    model.setSelection({SketchElementRef{a, SketchSubElement::EndPoint},
                        SketchElementRef{b, SketchSubElement::StartPoint}});
    EXPECT_TRUE(model.requestConstraint(fixture.sketch(), SketchEditKind::AddCoincident, &whyNot)
                    .valid())
        << whyNot;
}

TEST(SketchConstraintCommandTest, M12_CON_003_HorizontalOnAnEndpointConstrainsTheWholeLineOnce) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 5.0});
    SketchCanvasModel model;
    // Selected by its endpoint, which is how a user most easily hits a line.
    model.setSelection({SketchElementRef{line, SketchSubElement::EndPoint}});

    std::string whyNot;
    const SketchEdit edit =
        model.requestConstraint(fixture.sketch(), SketchEditKind::AddHorizontal, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    ASSERT_EQ(edit.refs.size(), 1u);
    EXPECT_EQ(edit.refs.front().subElement, SketchSubElement::Whole);
    EXPECT_TRUE(fixture.apply(edit).applied);
    EXPECT_EQ(fixture.sketch().constraints().size(), 1u);
}

TEST(SketchConstraintCommandTest, M12_CON_004_ConstraintRowsNameTheirElementsAndTheirParameter) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{line, SketchSubElement::Whole}});
    std::string whyNot;
    ASSERT_TRUE(
        fixture.apply(model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot))
            .applied);

    const std::vector<ConstraintRow> rows = ConstraintRowsFor(fixture.document, fixture.sketch());
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front().kind, std::string("Length"));
    EXPECT_NE(rows.front().elements.find("Line"), std::string::npos);
    EXPECT_NE(rows.front().parameter.find("100 mm"), std::string::npos);
    EXPECT_TRUE(rows.front().dimensional);
    EXPECT_FALSE(rows.front().offending);
}

TEST(SketchConstraintCommandTest, M12_CON_005_DeletingGeometryTakesItsConstraintsWithIt) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{line, SketchSubElement::Whole}});
    std::string whyNot;
    ASSERT_TRUE(
        fixture.apply(model.requestConstraint(fixture.sketch(), SketchEditKind::AddHorizontal,
                                              &whyNot))
            .applied);

    model.setSelection({SketchElementRef{line, SketchSubElement::EndPoint},
                        SketchElementRef{line, SketchSubElement::Whole}});
    const SketchEdit remove = model.requestDelete(fixture.sketch(), &whyNot);
    ASSERT_TRUE(remove.valid()) << whyNot;
    // The two selections are the SAME entity: it must be deleted once.
    EXPECT_EQ(remove.refs.size(), 1u);
    EXPECT_TRUE(fixture.apply(remove).applied);
    EXPECT_EQ(fixture.sketch().entities().size(), 0u);
    EXPECT_EQ(fixture.sketch().constraints().size(), 0u);
}

// =============================================================================
// M16 -- dimension placement is document state
// =============================================================================

TEST(SketchDimensionPlacementTest, M16_PLACE_001_APlacedDimensionReplacesTheComputedLayout) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const DimensionAnnotation automatic = LengthDimensionOf(fixture, line);
    const SketchConstraintId id = automatic.id;
    EXPECT_FALSE(automatic.userPlaced);

    // Dragged well away from where the layout put it.
    ASSERT_TRUE(fixture.document.setSketchDimensionPlacement(fixture.sketchId, id,
                                                            Vec2{50.0, 40.0}));
    const std::vector<DimensionAnnotation> placed =
        DimensionAnnotationsFor(fixture.document, fixture.sketch());
    ASSERT_EQ(placed.size(), 1u);
    EXPECT_TRUE(placed.front().userPlaced);
    // The OFFSET follows the label; the span still matches the geometry.
    EXPECT_NEAR(placed.front().labelMm.y, 40.0, 1e-9);
    ASSERT_EQ(placed.front().dimensionLines.size(), 1u);
    EXPECT_NEAR(placed.front().dimensionLines.front().fromMm.y, 40.0, 1e-9);
    EXPECT_NEAR(placed.front().dimensionLines.front().fromMm.x, 0.0, 1e-9);
    EXPECT_NEAR(placed.front().dimensionLines.front().toMm.x, 100.0, 1e-9);
    // Still a dimension: two extension lines and two arrows.
    EXPECT_EQ(placed.front().extensionLines.size(), 2u);
    EXPECT_EQ(placed.front().arrows.size(), 2u);
}

TEST(SketchDimensionPlacementTest, M16_PLACE_002_PlacementIsUndoableAndReturnsToAutomatic) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchConstraintId id = LengthDimensionOf(fixture, line).id;

    ASSERT_TRUE(fixture.document.setSketchDimensionPlacement(fixture.sketchId, id,
                                                            Vec2{50.0, 40.0}));
    ASSERT_NE(fixture.sketch().dimensionPlacement(id), nullptr);

    ASSERT_TRUE(fixture.document.undo());
    // Back to AUTOMATIC, not back to some coordinate that was never chosen --
    // which is why the delta records whether there was a placement at all.
    EXPECT_EQ(fixture.sketch().dimensionPlacement(id), nullptr);

    ASSERT_TRUE(fixture.document.redo());
    ASSERT_NE(fixture.sketch().dimensionPlacement(id), nullptr);
    EXPECT_NEAR(fixture.sketch().dimensionPlacement(id)->y, 40.0, 1e-9);
}

TEST(SketchDimensionPlacementTest, M16_PLACE_003_MovingTwiceUndoesOneStepAtATime) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchConstraintId id = LengthDimensionOf(fixture, line).id;

    ASSERT_TRUE(
        fixture.document.setSketchDimensionPlacement(fixture.sketchId, id, Vec2{50.0, 20.0}));
    ASSERT_TRUE(
        fixture.document.setSketchDimensionPlacement(fixture.sketchId, id, Vec2{50.0, 60.0}));

    ASSERT_TRUE(fixture.document.undo());
    ASSERT_NE(fixture.sketch().dimensionPlacement(id), nullptr);
    EXPECT_NEAR(fixture.sketch().dimensionPlacement(id)->y, 20.0, 1e-9);
}

TEST(SketchDimensionPlacementTest, M16_PLACE_004_APlacementIsRefusedForANonDimension) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchEdit edit;
    edit.kind = SketchEditKind::AddHorizontal;
    edit.refs = {SketchElementRef{line, SketchSubElement::Whole}};
    edit.label = "Add horizontal";
    const SketchEditOutcome outcome = fixture.apply(edit);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    // Placing a Horizontal has no meaning. Storing one would leave an entry
    // nothing reads and the serializer still has to write.
    EXPECT_FALSE(fixture.document.setSketchDimensionPlacement(
        fixture.sketchId, outcome.createdConstraints.front(), Vec2{10.0, 10.0}));
    EXPECT_TRUE(fixture.sketch().dimensionPlacements().empty());
}

TEST(SketchDimensionPlacementTest, M16_PLACE_005_DeletingTheDimensionTakesItsPlacementAndUndoBringsBothBack) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchConstraintId id = LengthDimensionOf(fixture, line).id;
    ASSERT_TRUE(
        fixture.document.setSketchDimensionPlacement(fixture.sketchId, id, Vec2{50.0, 40.0}));

    ASSERT_TRUE(fixture.document.removeSketchConstraint(fixture.sketchId, id));
    EXPECT_TRUE(fixture.sketch().dimensionPlacements().empty());

    ASSERT_TRUE(fixture.document.undo());
    ASSERT_NE(fixture.sketch().findConstraint(id), nullptr);
    // The dimension comes back WHERE THE USER PUT IT, not snapped to automatic.
    ASSERT_NE(fixture.sketch().dimensionPlacement(id), nullptr);
    EXPECT_NEAR(fixture.sketch().dimensionPlacement(id)->y, 40.0, 1e-9);
}

TEST(SketchDimensionPlacementTest, M16_PLACE_006_AutomaticLabelIsReportedForAnUnplacedDimension) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const DimensionAnnotation automatic = LengthDimensionOf(fixture, line);

    bool ok = false;
    const Vec2 reported =
        AutomaticDimensionLabel(fixture.document, fixture.sketch(), automatic.id, &ok);
    ASSERT_TRUE(ok);
    // The canvas grabs a dimension from wherever it currently sits, so this has
    // to agree with what was drawn or the label jumps the moment it is touched.
    EXPECT_NEAR(reported.x, automatic.labelMm.x, 1e-9);
    EXPECT_NEAR(reported.y, automatic.labelMm.y, 1e-9);

    // ...and it keeps reporting the AUTOMATIC position after a placement, which
    // is what lets the canvas tell "moved" from "never moved".
    ASSERT_TRUE(fixture.document.setSketchDimensionPlacement(fixture.sketchId, automatic.id,
                                                             Vec2{50.0, 90.0}));
    const Vec2 stillAutomatic =
        AutomaticDimensionLabel(fixture.document, fixture.sketch(), automatic.id, &ok);
    ASSERT_TRUE(ok);
    EXPECT_NEAR(stillAutomatic.y, automatic.labelMm.y, 1e-9);
}

// =============================================================================
// M16 -- arrowheads flip, and labels stop colliding
// =============================================================================

TEST(SketchDimensionLayoutTest, M16_FIT_001_ANarrowSpanTurnsItsArrowheadsOutward) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchConstraintId id = LengthDimensionOf(fixture, line).id;
    (void)id;

    // Zoomed WAY out: 100 mm is a handful of pixels, so the value and two
    // heads cannot possibly fit between the extension lines.
    const std::vector<DimensionAnnotation> cramped =
        DimensionAnnotationsFor(fixture.document, fixture.sketch(), /*pixelsPerMm=*/0.3);
    ASSERT_EQ(cramped.size(), 1u);
    ASSERT_EQ(cramped.front().arrows.size(), 2u);

    // Heads now point INWARD at each other -- the drawing convention when the
    // span is too tight to hold them.
    const Vec2 tipToTip = Vec2{cramped.front().arrows[1].tipMm.x - cramped.front().arrows[0].tipMm.x,
                               cramped.front().arrows[1].tipMm.y - cramped.front().arrows[0].tipMm.y};
    EXPECT_GT(tipToTip.x * cramped.front().arrows[0].directionMm.x +
                  tipToTip.y * cramped.front().arrows[0].directionMm.y,
              0.0);
    // ...and the value has moved out of the way rather than sitting on them.
    EXPECT_GT(cramped.front().labelMm.x, 100.0);
}

TEST(SketchDimensionLayoutTest, M16_FIT_002_ARoomySpanKeepsItsArrowheadsInward) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    (void)LengthDimensionOf(fixture, line);

    const std::vector<DimensionAnnotation> roomy =
        DimensionAnnotationsFor(fixture.document, fixture.sketch(), /*pixelsPerMm=*/5.0);
    ASSERT_EQ(roomy.size(), 1u);
    ASSERT_EQ(roomy.front().arrows.size(), 2u);
    // Pointing OUTWARD, and the value still on the line between them.
    const Vec2 tipToTip = Vec2{roomy.front().arrows[1].tipMm.x - roomy.front().arrows[0].tipMm.x,
                               roomy.front().arrows[1].tipMm.y - roomy.front().arrows[0].tipMm.y};
    EXPECT_LT(tipToTip.x * roomy.front().arrows[0].directionMm.x +
                  tipToTip.y * roomy.front().arrows[0].directionMm.y,
              0.0);
    EXPECT_NEAR(roomy.front().labelMm.x, 50.0, 1e-9);
}

TEST(SketchDimensionLayoutTest, M16_FIT_003_TwoDimensionsOnTheSameSpanArePushedApart) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchCanvasModel model;
    std::string whyNot;
    // Two Length dimensions on ONE line: without a separation pass they land
    // on exactly the same point and print on top of each other.
    for (int i = 0; i < 2; ++i) {
        model.setSelection({SketchElementRef{line, SketchSubElement::Whole}});
        ASSERT_TRUE(
            fixture.apply(model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot))
                .applied)
            << whyNot;
    }

    const std::vector<DimensionAnnotation> annotations =
        DimensionAnnotationsFor(fixture.document, fixture.sketch(), /*pixelsPerMm=*/4.0);
    ASSERT_EQ(annotations.size(), 2u);
    const double gap = std::hypot(annotations[1].labelMm.x - annotations[0].labelMm.x,
                                  annotations[1].labelMm.y - annotations[0].labelMm.y);
    EXPECT_GT(gap, 1.0);
}

TEST(SketchDimensionLayoutTest, M16_FIT_004_AUserPlacedDimensionIsNeverPushedAround) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchCanvasModel model;
    std::string whyNot;
    std::vector<SketchConstraintId> ids;
    for (int i = 0; i < 2; ++i) {
        model.setSelection({SketchElementRef{line, SketchSubElement::Whole}});
        const SketchEditOutcome outcome =
            fixture.apply(model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot));
        ASSERT_TRUE(outcome.applied) << whyNot;
        ids.push_back(outcome.createdConstraints.front());
    }
    // Both dragged to the SAME point, deliberately.
    const Vec2 chosen{50.0, 25.0};
    for (const SketchConstraintId id : ids)
        ASSERT_TRUE(fixture.document.setSketchDimensionPlacement(fixture.sketchId, id, chosen));

    const std::vector<DimensionAnnotation> annotations =
        DimensionAnnotationsFor(fixture.document, fixture.sketch(), /*pixelsPerMm=*/4.0);
    ASSERT_EQ(annotations.size(), 2u);
    // NEITHER is moved. A dimension somebody dragged somewhere is a decision,
    // and the layout quietly overruling it is worse than an overlap they can
    // see and fix.
    for (const DimensionAnnotation& annotation : annotations) {
        EXPECT_TRUE(annotation.userPlaced);
        EXPECT_NEAR(annotation.labelMm.x, chosen.x, 1e-9);
        EXPECT_NEAR(annotation.labelMm.y, chosen.y, 1e-9);
    }
}

TEST(SketchDimensionLayoutTest, M16_FIT_005_NoZoomMeansNoFitDecisions) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    (void)LengthDimensionOf(fixture, line);
    // pixelsPerMm = 0 means "do not judge what fits" -- a caller that only
    // wants the measured geometry must not have to invent a zoom.
    const std::vector<DimensionAnnotation> plain =
        DimensionAnnotationsFor(fixture.document, fixture.sketch(), 0.0);
    ASSERT_EQ(plain.size(), 1u);
    EXPECT_NEAR(plain.front().labelMm.x, 50.0, 1e-9);
    EXPECT_EQ(plain.front().arrows.size(), 2u);
}

// =============================================================================
// M16 -- prefix, suffix and tolerance
// =============================================================================

namespace {

Sketch::DimensionFormat MakeFormat(const char* prefix, const char* suffix, double plus,
                                   double minus) {
    Sketch::DimensionFormat format;
    format.prefix = prefix;
    format.suffix = suffix;
    format.plusTolerance = plus;
    format.minusTolerance = minus;
    return format;
}

} // namespace

TEST(SketchDimensionFormatTest, M16_FMT_001_APrefixAndSuffixWrapTheValue) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchConstraintId id = LengthDimensionOf(fixture, line).id;

    ASSERT_TRUE(fixture.document.setSketchDimensionFormat(fixture.sketchId, id,
                                                          MakeFormat("2x ", " REF", 0.0, 0.0)));
    const std::vector<DimensionAnnotation> annotations =
        DimensionAnnotationsFor(fixture.document, fixture.sketch());
    ASSERT_EQ(annotations.size(), 1u);
    EXPECT_EQ(annotations.front().text, "2x 100 REF");
}

TEST(SketchDimensionFormatTest, M16_FMT_002_ASymmetricToleranceUsesThePlusMinusForm) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchConstraintId id = LengthDimensionOf(fixture, line).id;

    ASSERT_TRUE(fixture.document.setSketchDimensionFormat(fixture.sketchId, id,
                                                          MakeFormat("", "", 0.1, 0.1)));
    EXPECT_EQ(DimensionAnnotationsFor(fixture.document, fixture.sketch()).front().text,
              "100 +/-0.1");
}

TEST(SketchDimensionFormatTest, M16_FMT_003_AnAsymmetricToleranceShowsBothLimits) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchConstraintId id = LengthDimensionOf(fixture, line).id;

    ASSERT_TRUE(fixture.document.setSketchDimensionFormat(fixture.sketchId, id,
                                                          MakeFormat("", "", 0.2, 0.05)));
    EXPECT_EQ(DimensionAnnotationsFor(fixture.document, fixture.sketch()).front().text,
              "100 +0.2/-0.05");
}

TEST(SketchDimensionFormatTest, M16_FMT_004_ADefaultFormatStoresNothing) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchConstraintId id = LengthDimensionOf(fixture, line).id;

    ASSERT_TRUE(fixture.document.setSketchDimensionFormat(fixture.sketchId, id,
                                                          MakeFormat("2x ", "", 0.0, 0.0)));
    ASSERT_EQ(fixture.sketch().dimensionFormats().size(), 1u);

    // Setting it back to nothing REMOVES the entry rather than storing an empty
    // one: a record that says "nothing special" is one more thing to serialize
    // and to keep in step.
    ASSERT_TRUE(fixture.document.setSketchDimensionFormat(fixture.sketchId, id,
                                                          MakeFormat("", "", 0.0, 0.0)));
    EXPECT_TRUE(fixture.sketch().dimensionFormats().empty());
    EXPECT_EQ(DimensionAnnotationsFor(fixture.document, fixture.sketch()).front().text, "100");
}

TEST(SketchDimensionFormatTest, M16_FMT_005_AnAngularToleranceIsShownInDegrees) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{0.0, 100.0});
    SketchCanvasModel model;
    model.setSelection({SketchElementRef{a, SketchSubElement::Whole},
                        SketchElementRef{b, SketchSubElement::Whole}});
    std::string whyNot;
    const SketchEditOutcome outcome =
        fixture.apply(model.requestDimension(fixture.sketch(), SketchEditKind::None, &whyNot));
    ASSERT_TRUE(outcome.applied) << whyNot;
    const SketchConstraintId id = outcome.createdConstraints.front();

    // Half a degree, STORED in radians like the value it qualifies.
    const double halfDegree = 0.5 * 3.14159265358979323846 / 180.0;
    ASSERT_TRUE(fixture.document.setSketchDimensionFormat(
        fixture.sketchId, id, MakeFormat("", "", halfDegree, halfDegree)));

    // A tolerance that did not make the same radians->degrees trip as its value
    // would print 0.0087 here.
    EXPECT_EQ(DimensionAnnotationsFor(fixture.document, fixture.sketch()).front().text,
              "90deg +/-0.5");
}

TEST(SketchDimensionFormatTest, M16_FMT_006_FormatIsUndoableAndDiesWithItsDimension) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchConstraintId id = LengthDimensionOf(fixture, line).id;
    ASSERT_TRUE(fixture.document.setSketchDimensionFormat(fixture.sketchId, id,
                                                          MakeFormat("2x ", " REF", 0.1, 0.1)));

    ASSERT_TRUE(fixture.document.undo());
    EXPECT_EQ(fixture.sketch().dimensionFormat(id), nullptr);
    ASSERT_TRUE(fixture.document.redo());
    ASSERT_NE(fixture.sketch().dimensionFormat(id), nullptr);

    // Deleting the dimension takes the format, and ONE undo brings both back.
    ASSERT_TRUE(fixture.document.removeSketchConstraint(fixture.sketchId, id));
    EXPECT_TRUE(fixture.sketch().dimensionFormats().empty());
    ASSERT_TRUE(fixture.document.undo());
    ASSERT_NE(fixture.sketch().findConstraint(id), nullptr);
    ASSERT_NE(fixture.sketch().dimensionFormat(id), nullptr);
    EXPECT_EQ(fixture.sketch().dimensionFormat(id)->prefix, "2x ");
}

TEST(SketchDimensionFormatTest, M16_FMT_007_ANonDimensionCannotBeFormatted) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchEdit edit;
    edit.kind = SketchEditKind::AddHorizontal;
    edit.refs = {SketchElementRef{line, SketchSubElement::Whole}};
    edit.label = "Add horizontal";
    const SketchEditOutcome outcome = fixture.apply(edit);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    EXPECT_FALSE(fixture.document.setSketchDimensionFormat(
        fixture.sketchId, outcome.createdConstraints.front(), MakeFormat("2x ", "", 0.0, 0.0)));
    EXPECT_TRUE(fixture.sketch().dimensionFormats().empty());
}

// =============================================================================
// M13 -- the geometric constraint commands
// =============================================================================

TEST(SketchGeometricCommandTest, M13_CMD_001_EveryNewKindIsRecognisedAsAConstraintCommand) {
    // The shell builds its command list from this predicate. A kind that is
    // implemented but not listed here is a command with no button.
    for (const SketchEditKind kind :
         {SketchEditKind::AddParallel, SketchEditKind::AddPerpendicular,
          SketchEditKind::AddEqual, SketchEditKind::AddConcentric,
          SketchEditKind::AddMidpoint, SketchEditKind::AddPointOnObject,
          SketchEditKind::AddTangent})
        EXPECT_TRUE(SketchCanvasModel::IsConstraintCommand(kind)) << SketchEditKindName(kind);
    // ...and a dimension is not one of them.
    EXPECT_FALSE(SketchCanvasModel::IsConstraintCommand(SketchEditKind::AddLength));
}

TEST(SketchGeometricCommandTest, M13_CMD_002_ParallelAndPerpendicularNeedTwoDistinctLines) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{0.0, 40.0}, Vec2{90.0, 70.0});
    const SketchEntityId circle = CircleAt(fixture, Vec2{200.0, 0.0}, 10.0);
    SketchCanvasModel model;
    std::string whyNot;

    for (const SketchEditKind kind :
         {SketchEditKind::AddParallel, SketchEditKind::AddPerpendicular}) {
        model.setSelection({SketchElementRef{a, SketchSubElement::Whole}});
        EXPECT_FALSE(model.requestConstraint(fixture.sketch(), kind, &whyNot).valid());

        model.setSelection({SketchElementRef{a, SketchSubElement::Whole},
                            SketchElementRef{circle, SketchSubElement::Whole}});
        EXPECT_FALSE(model.requestConstraint(fixture.sketch(), kind, &whyNot).valid());
        EXPECT_NE(whyNot.find("not a line"), std::string::npos) << whyNot;

        model.setSelection({SketchElementRef{a, SketchSubElement::Whole},
                            SketchElementRef{a, SketchSubElement::EndPoint}});
        EXPECT_FALSE(model.requestConstraint(fixture.sketch(), kind, &whyNot).valid())
            << "the same line twice must be refused";

        model.setSelection({SketchElementRef{a, SketchSubElement::Whole},
                            SketchElementRef{b, SketchSubElement::Whole}});
        const SketchEdit edit = model.requestConstraint(fixture.sketch(), kind, &whyNot);
        ASSERT_TRUE(edit.valid()) << whyNot;
        EXPECT_TRUE(fixture.apply(edit).applied);
    }
    EXPECT_EQ(fixture.sketch().constraints().size(), 2u);
}

TEST(SketchGeometricCommandTest, M13_CMD_003_EqualTakesTwoLinesOrTwoCurvesButNotOneOfEach) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId other = LineFrom(fixture, Vec2{0.0, 40.0}, Vec2{30.0, 40.0});
    const SketchEntityId circleA = CircleAt(fixture, Vec2{200.0, 0.0}, 25.0);
    const SketchEntityId circleB = CircleAt(fixture, Vec2{300.0, 0.0}, 8.0);
    SketchCanvasModel model;
    std::string whyNot;

    model.setSelection({SketchElementRef{line, SketchSubElement::Whole},
                        SketchElementRef{circleA, SketchSubElement::Whole}});
    EXPECT_FALSE(
        model.requestConstraint(fixture.sketch(), SketchEditKind::AddEqual, &whyNot).valid());
    // The refusal SAYS what would work, rather than only that this did not.
    EXPECT_NE(whyNot.find("two lines"), std::string::npos) << whyNot;

    model.setSelection({SketchElementRef{line, SketchSubElement::Whole},
                        SketchElementRef{other, SketchSubElement::Whole}});
    EXPECT_TRUE(
        model.requestConstraint(fixture.sketch(), SketchEditKind::AddEqual, &whyNot).valid())
        << whyNot;

    model.setSelection({SketchElementRef{circleA, SketchSubElement::Whole},
                        SketchElementRef{circleB, SketchSubElement::Whole}});
    EXPECT_TRUE(
        model.requestConstraint(fixture.sketch(), SketchEditKind::AddEqual, &whyNot).valid())
        << whyNot;
}

TEST(SketchGeometricCommandTest, M13_CMD_004_ConcentricPointsAUserAtCoincidentForAPoint) {
    Fixture fixture;
    const SketchEntityId circle = CircleAt(fixture, Vec2{0.0, 0.0}, 25.0);
    SketchEdit pointEdit;
    pointEdit.kind = SketchEditKind::AddPoint;
    pointEdit.points = {Vec2{80.0, 80.0}};
    pointEdit.label = "Add point";
    const SketchEntityId point = fixture.apply(pointEdit).createdEntities.front();

    SketchCanvasModel model;
    std::string whyNot;
    model.setSelection({SketchElementRef{circle, SketchSubElement::Whole},
                        SketchElementRef{point, SketchSubElement::Whole}});
    EXPECT_FALSE(
        model.requestConstraint(fixture.sketch(), SketchEditKind::AddConcentric, &whyNot)
            .valid());
    // Naming the command that DOES do it is the difference between a dead end
    // and a hint.
    EXPECT_NE(whyNot.find("Coincident"), std::string::npos) << whyNot;
}

TEST(SketchGeometricCommandTest, M13_CMD_005_MidpointAndPointOnObjectAcceptEitherClickOrder) {
    Fixture fixture;
    const SketchEntityId line = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    SketchEdit pointEdit;
    pointEdit.kind = SketchEditKind::AddPoint;
    pointEdit.points = {Vec2{20.0, 60.0}};
    pointEdit.label = "Add point";
    const SketchEntityId point = fixture.apply(pointEdit).createdEntities.front();

    SketchCanvasModel model;
    std::string whyNot;
    const SketchElementRef pointRef{point, SketchSubElement::Whole};
    const SketchElementRef lineRef{line, SketchSubElement::Whole};

    for (const SketchEditKind kind :
         {SketchEditKind::AddMidpoint, SketchEditKind::AddPointOnObject}) {
        // Point first...
        model.setSelection({pointRef, lineRef});
        const SketchEdit forward = model.requestConstraint(fixture.sketch(), kind, &whyNot);
        ASSERT_TRUE(forward.valid()) << whyNot;
        // ...and line first. The USER should not have to remember an order.
        model.setSelection({lineRef, pointRef});
        const SketchEdit reversed = model.requestConstraint(fixture.sketch(), kind, &whyNot);
        ASSERT_TRUE(reversed.valid()) << whyNot;

        // Both normalise to the SAME roles, so nothing downstream has to
        // re-derive which selection was the point.
        ASSERT_EQ(forward.refs.size(), 2u);
        ASSERT_EQ(reversed.refs.size(), 2u);
        EXPECT_EQ(forward.refs[0].entityId, point);
        EXPECT_EQ(reversed.refs[0].entityId, point);
        EXPECT_EQ(forward.refs[1].entityId, line);
        EXPECT_EQ(reversed.refs[1].entityId, line);
    }
}

TEST(SketchGeometricCommandTest, M13_CMD_006_MidpointRefusesACurveWhilePointOnObjectAcceptsIt) {
    Fixture fixture;
    const SketchEntityId circle = CircleAt(fixture, Vec2{0.0, 0.0}, 25.0);
    SketchEdit pointEdit;
    pointEdit.kind = SketchEditKind::AddPoint;
    pointEdit.points = {Vec2{80.0, 80.0}};
    pointEdit.label = "Add point";
    const SketchEntityId point = fixture.apply(pointEdit).createdEntities.front();

    SketchCanvasModel model;
    std::string whyNot;
    model.setSelection({SketchElementRef{point, SketchSubElement::Whole},
                        SketchElementRef{circle, SketchSubElement::Whole}});
    // A circle has no midpoint...
    EXPECT_FALSE(
        model.requestConstraint(fixture.sketch(), SketchEditKind::AddMidpoint, &whyNot).valid());
    // ...but it does have a rim.
    EXPECT_TRUE(
        model.requestConstraint(fixture.sketch(), SketchEditKind::AddPointOnObject, &whyNot)
            .valid())
        << whyNot;
}

TEST(SketchGeometricCommandTest, M13_CMD_007_TangentPicksItsBranchFromTheCurrentGeometry) {
    Fixture fixture;
    // Nested: the small circle sits INSIDE the big one, so the tangency the
    // user can plausibly mean is the internal one.
    const SketchEntityId outer = CircleAt(fixture, Vec2{0.0, 0.0}, 30.0);
    const SketchEntityId inner = CircleAt(fixture, Vec2{4.0, 0.0}, 12.0);
    // Apart: these can only touch from the outside.
    const SketchEntityId farA = CircleAt(fixture, Vec2{300.0, 0.0}, 10.0);
    const SketchEntityId farB = CircleAt(fixture, Vec2{400.0, 0.0}, 10.0);

    SketchCanvasModel model;
    std::string whyNot;

    model.setSelection({SketchElementRef{outer, SketchSubElement::Whole},
                        SketchElementRef{inner, SketchSubElement::Whole}});
    const SketchEdit nested =
        model.requestConstraint(fixture.sketch(), SketchEditKind::AddTangent, &whyNot);
    ASSERT_TRUE(nested.valid()) << whyNot;
    EXPECT_TRUE(nested.tangentInternal);

    model.setSelection({SketchElementRef{farA, SketchSubElement::Whole},
                        SketchElementRef{farB, SketchSubElement::Whole}});
    const SketchEdit apart =
        model.requestConstraint(fixture.sketch(), SketchEditKind::AddTangent, &whyNot);
    ASSERT_TRUE(apart.valid()) << whyNot;
    EXPECT_FALSE(apart.tangentInternal);

    // And the flag reaches the CONSTRAINT, not just the edit.
    const SketchEditOutcome outcome = fixture.apply(nested);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    const SketchConstraint* stored =
        fixture.sketch().findConstraint(outcome.createdConstraints.front());
    ASSERT_NE(stored, nullptr);
    const auto* tangent = std::get_if<TangentConstraint>(&stored->data);
    ASSERT_NE(tangent, nullptr);
    EXPECT_TRUE(tangent->internal);
}

TEST(SketchGeometricCommandTest, M13_CMD_008_TangentRefusesTwoLines) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{0.0, 40.0}, Vec2{90.0, 70.0});
    SketchCanvasModel model;
    std::string whyNot;
    model.setSelection({SketchElementRef{a, SketchSubElement::Whole},
                        SketchElementRef{b, SketchSubElement::Whole}});
    EXPECT_FALSE(
        model.requestConstraint(fixture.sketch(), SketchEditKind::AddTangent, &whyNot).valid());
    EXPECT_NE(whyNot.find("a line and a curve"), std::string::npos) << whyNot;
}

TEST(SketchGeometricCommandTest, M13_CMD_009_EachNewConstraintIsOneUndoStep) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{0.0, 40.0}, Vec2{90.0, 70.0});
    SketchCanvasModel model;
    std::string whyNot;
    model.setSelection({SketchElementRef{a, SketchSubElement::Whole},
                        SketchElementRef{b, SketchSubElement::Whole}});
    const SketchEdit edit =
        model.requestConstraint(fixture.sketch(), SketchEditKind::AddParallel, &whyNot);
    ASSERT_TRUE(edit.valid()) << whyNot;
    ASSERT_TRUE(fixture.apply(edit).applied);
    ASSERT_EQ(fixture.sketch().constraints().size(), 1u);

    ASSERT_TRUE(fixture.document.undo());
    EXPECT_EQ(fixture.sketch().constraints().size(), 0u);
    ASSERT_TRUE(fixture.document.redo());
    ASSERT_EQ(fixture.sketch().constraints().size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ParallelConstraint>(
        fixture.sketch().constraints().front().data));
}

TEST(SketchGeometricCommandTest, M13_CMD_010_TheConstraintPanelNamesEachNewKind) {
    Fixture fixture;
    const SketchEntityId a = LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchEntityId b = LineFrom(fixture, Vec2{0.0, 40.0}, Vec2{90.0, 70.0});
    const SketchEntityId circleA = CircleAt(fixture, Vec2{200.0, 0.0}, 25.0);
    const SketchEntityId circleB = CircleAt(fixture, Vec2{260.0, 0.0}, 10.0);

    fixture.document.addSketchConstraint(fixture.sketchId, ParallelConstraint{a, b});
    fixture.document.addSketchConstraint(fixture.sketchId, PerpendicularConstraint{a, b});
    fixture.document.addSketchConstraint(fixture.sketchId, EqualConstraint{a, b});
    fixture.document.addSketchConstraint(fixture.sketchId,
                                         ConcentricConstraint{circleA, circleB});
    fixture.document.addSketchConstraint(fixture.sketchId,
                                         TangentConstraint{circleA, circleB, true});

    const std::vector<ConstraintRow> rows =
        ConstraintRowsFor(fixture.document, fixture.sketch());
    ASSERT_EQ(rows.size(), 5u);
    for (const ConstraintRow& row : rows) {
        EXPECT_FALSE(row.kind.empty());
        EXPECT_NE(row.elements, "(no elements)") << row.kind;
        EXPECT_FALSE(row.dimensional) << row.kind;
        EXPECT_TRUE(row.parameter.empty()) << row.kind;
    }
    EXPECT_EQ(rows[0].kind, std::string("Parallel"));
    EXPECT_EQ(rows[4].kind, std::string("Tangent"));
    // The tangent row says WHICH tangency, because the two are different
    // models and the list is where a user checks what they asked for.
    EXPECT_NE(rows[4].elements.find("inner"), std::string::npos) << rows[4].elements;
}

// =============================================================================
// Status reporting
// =============================================================================

TEST(SketchStatusTest, M12_STATUS_001_EveryStateHasABadgeAndASentence) {
    Fixture fixture;
    const SketchStatusLine line = DescribeSketchStatus(fixture.sketch());
    // A06: colour is never the only channel, so both text fields must be
    // populated in EVERY state -- including the one a sketch starts in.
    EXPECT_FALSE(line.badge.empty());
    EXPECT_FALSE(line.text.empty());
}

TEST(SketchStatusTest, M12_STATUS_002_AnUnsolvedSketchDoesNotClaimZeroDegreesOfFreedom) {
    Fixture fixture;
    LineFrom(fixture, Vec2{0.0, 0.0}, Vec2{100.0, 0.0});
    const SketchStatusLine line = DescribeSketchStatus(fixture.sketch());
    // 0 means FULLY CONSTRAINED in this project. A sketch that has never been
    // solved must not say it.
    EXPECT_EQ(line.text.find("DOF 0"), std::string::npos) << line.text;
}

// --- M18: MIRRORING A SPLINE -------------------------------------------------

TEST(SketchMirrorTest, M18_MIR_001_ASplineIsReflectedPointForPoint) {
    // Refused until M18, and the reason it gave -- "only its two ends can be
    // named by a constraint" -- stopped being true at M17.30. A refusal that
    // outlives its cause tells the user something false about their own
    // program, which is worse than the missing feature.
    Fixture fixture;
    const SketchEntityId axis = LineFrom(fixture, Vec2{50.0, -50.0}, Vec2{50.0, 50.0});
    const SketchEntityId source =
        fixture.add(SketchSpline{{Vec2{10, 0}, Vec2{20, 30}, Vec2{35, 5}, Vec2{45, 25}}, false});

    const MirrorOutcome outcome = ApplyMirror(fixture.document, fixture.sketchId, {source}, axis);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    ASSERT_EQ(outcome.created.size(), 1u);

    const SketchSpline& copy =
        std::get<SketchSpline>(fixture.sketch().findEntity(outcome.created.front())->geometry);
    ASSERT_EQ(copy.points.size(), 4u);
    // Reflected across x = 50, IN THE SAME ORDER -- unlike an arc, whose two
    // ends had to be swapped because its direction is stored as angles.
    EXPECT_NEAR(copy.points[0].x, 90.0, 1e-9);
    EXPECT_NEAR(copy.points[0].y, 0.0, 1e-9);
    EXPECT_NEAR(copy.points[1].x, 80.0, 1e-9);
    EXPECT_NEAR(copy.points[3].x, 55.0, 1e-9);
    EXPECT_NEAR(copy.points[3].y, 25.0, 1e-9);
}

TEST(SketchMirrorTest, M18_MIR_002_EveryPointIsTiedAndTheCopyIsNotOverConstrained) {
    // Four points, four symmetries: eight equations for the copy's eight
    // freedoms. Exactly determined -- and there is no Equal on top, the way a
    // circle needs one for its radius, because a spline has no size apart from
    // where its points are. An extra tie would make every mirrored spline read
    // as over-constrained, and roadmap 8.2 asks that reading to mean something.
    Fixture fixture;
    const SketchEntityId axis = LineFrom(fixture, Vec2{50.0, -50.0}, Vec2{50.0, 50.0});
    const SketchEntityId source =
        fixture.add(SketchSpline{{Vec2{10, 0}, Vec2{20, 30}, Vec2{35, 5}, Vec2{45, 25}}, false});

    const MirrorOutcome outcome = ApplyMirror(fixture.document, fixture.sketchId, {source}, axis);
    ASSERT_TRUE(outcome.applied) << outcome.status;

    int symmetries = 0;
    int equals = 0;
    for (const SketchConstraint& constraint : fixture.sketch().constraints()) {
        if (std::holds_alternative<SymmetricConstraint>(constraint.data)) ++symmetries;
        if (std::holds_alternative<EqualConstraint>(constraint.data)) ++equals;
    }
    EXPECT_EQ(symmetries, 4);
    EXPECT_EQ(equals, 0);
    // That these equations SOLVE, and do not read as over-constrained, is
    // M18_MIR_003 next door -- no test in this file owns a solver, and adding
    // one here would link the solver into a suite whose job is the commands.
}

TEST(SketchMirrorTest, M18_MIR_004_AnEllipseIsStillREFUSEDAndSaysWhy) {
    // Not done, and said. Reflecting one is easy -- the centre reflects and the
    // rotation becomes 2*phi - rotation -- but a mirror in this project TIES
    // the copy to the original, and there is no equality between two ellipses
    // to tie their shapes with. A copy that moved with the original in position
    // and drifted in shape is worse than not having the command.
    Fixture fixture;
    const SketchEntityId axis = LineFrom(fixture, Vec2{50.0, -50.0}, Vec2{50.0, 50.0});
    const SketchEntityId source = fixture.add(SketchEllipse{Vec2{10, 10}, 30.0, 12.0, 0.3});

    const MirrorOutcome outcome = ApplyMirror(fixture.document, fixture.sketchId, {source}, axis);
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("same shape"), std::string::npos) << outcome.status;
}
