// M17.6 -- projected reference geometry, and the tool that uses it.
//
// Two claims are under test here, and they fail in opposite ways.
//
// The PROJECTION must be complete or say it is not. An underlay missing three
// of a face's edges looks exactly like a face with three fewer edges: the user
// traces what they can see, gets the wrong outline, and nothing anywhere
// reports a problem. So every skipped edge is counted and named.
//
// The CONVERSION must produce an entity that behaves like a drawn one -- same
// creation path, same undo, same constraints -- and must not produce a second
// copy of an edge that is already there. A duplicate curve lying exactly on
// top of another is invisible on screen and makes the profile ambiguous.
//
// It lives in the SOLVER suite because two of its claims -- that a converted
// line really reaches zero degrees of freedom, and that a converted arc is not
// over-constrained by its own Fix -- can only be shown by a real solver. A Fix
// on a sub-element the solver has no variable for would sit in the constraint
// list looking perfectly correct.

#include "Core/Document/PartDocument.h"
#include "Core/Kernel/FaceGeometry.h"
#include "Core/Sketch/Sketch.h"
#include "Viewer/FaceSketch.h"
#include "Viewer/SketchCanvas.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Viewer/PropertyEditing.h"
#include <sstream>
#include "Viewer/SketchCommands.h"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

// The sketch frame on the top face of a 20 mm-tall box: z = 20, u = +X,
// v = +Y. Built through the real planner, so these tests exercise the same
// frame the application uses rather than one made up here.
SketchFrame TopFaceFrame() {
    PickedFace face;
    face.isFace = true;
    face.planar = true;
    face.point = Vec3{0, 0, 20};
    face.normal = Vec3{0, 0, 1};
    const FaceSketchPlan plan = PlanSketchOnFace(face);
    EXPECT_TRUE(plan.ok) << plan.message;
    return plan.frame;
}

FaceCurve Line3D(Vec3 start, Vec3 end) {
    FaceCurve curve;
    curve.kind = FaceCurve::Kind::Line;
    curve.start = start;
    curve.end = end;
    return curve;
}

FaceCurve Circle3D(Vec3 centre, double radius, Vec3 axis) {
    FaceCurve curve;
    curve.kind = FaceCurve::Kind::Circle;
    curve.center = centre;
    curve.radiusMm = radius;
    curve.axis = axis;
    return curve;
}

int CountOf(const ProjectedBoundary& boundary, std::size_t which) {
    int count = 0;
    for (const SketchGeometry& geometry : boundary.geometry)
        if (geometry.index() == which) ++count;
    return count;
}

constexpr std::size_t kPointIndex = 0;
constexpr std::size_t kLineIndex = 1;
constexpr std::size_t kCircleIndex = 2;
constexpr std::size_t kArcIndex = 3;

} // namespace

// --- The projection ---------------------------------------------------------

TEST(ReferenceProjectionTest, M17_PROJ_001_ASquareFaceProjectsToFourLinesAndFourVertices) {
    // The square sits at z = 20, so a projection that forgot to subtract the
    // frame origin would put every line 20 mm from where it belongs -- and
    // still draw a perfectly convincing square.
    const FaceBoundary boundary = {
        Line3D(Vec3{0, 0, 20}, Vec3{40, 0, 20}), Line3D(Vec3{40, 0, 20}, Vec3{40, 30, 20}),
        Line3D(Vec3{40, 30, 20}, Vec3{0, 30, 20}), Line3D(Vec3{0, 30, 20}, Vec3{0, 0, 20})};

    const ProjectedBoundary projected = ProjectBoundaryOntoSketch(boundary, TopFaceFrame());
    EXPECT_EQ(projected.skipped, 0);
    EXPECT_EQ(CountOf(projected, kLineIndex), 4);
    // FOUR, not eight: the corners are shared, and an underlay with two
    // coincident snap points at every corner is one where the user cannot tell
    // which one they caught.
    EXPECT_EQ(CountOf(projected, kPointIndex), 4);

    const auto* first = std::get_if<SketchLine>(&projected.geometry.front());
    ASSERT_NE(first, nullptr);
    EXPECT_NEAR(first->start.x, 0.0, 1e-9);
    EXPECT_NEAR(first->start.y, 0.0, 1e-9);
    EXPECT_NEAR(first->end.x, 40.0, 1e-9);
    EXPECT_NEAR(first->end.y, 0.0, 1e-9);
}

TEST(ReferenceProjectionTest, M17_PROJ_002_AHoleInTheFaceProjectsToACircleAndItsCentre) {
    const FaceBoundary boundary = {Circle3D(Vec3{20, 15, 20}, 5.0, Vec3{0, 0, 1})};
    const ProjectedBoundary projected = ProjectBoundaryOntoSketch(boundary, TopFaceFrame());

    EXPECT_EQ(projected.skipped, 0);
    ASSERT_EQ(CountOf(projected, kCircleIndex), 1);
    const auto* circle = std::get_if<SketchCircle>(&projected.geometry.front());
    ASSERT_NE(circle, nullptr);
    EXPECT_NEAR(circle->center.x, 20.0, 1e-9);
    EXPECT_NEAR(circle->center.y, 15.0, 1e-9);
    EXPECT_NEAR(circle->radiusMm, 5.0, 1e-9);
    // The centre is a snap target in every CAD program, and it is the one point
    // of a circle a user most often measures from.
    EXPECT_EQ(CountOf(projected, kPointIndex), 1);
}

TEST(ReferenceProjectionTest, M17_PROJ_003_ACircleSeenAtAnAngleIsSKIPPEDNotFlattened) {
    // THE test this feature needed. A circle whose plane is tilted relative to
    // the sketch projects to an ELLIPSE, and EP3D has no ellipse entity. The
    // available wrong answers are both plausible and both silent: draw it as a
    // circle of the original radius (too wide), or of the projected minor axis
    // (too narrow). Either produces a drawing that looks right and measures
    // wrong.
    const FaceBoundary boundary = {Circle3D(Vec3{10, 10, 20}, 5.0, Vec3{0, 1, 0})};
    const ProjectedBoundary projected = ProjectBoundaryOntoSketch(boundary, TopFaceFrame());

    EXPECT_EQ(CountOf(projected, kCircleIndex), 0);
    EXPECT_EQ(projected.skipped, 1);
    EXPECT_NE(projected.skippedReason.find("angle"), std::string::npos)
        << projected.skippedReason;
}

TEST(ReferenceProjectionTest, M17_PROJ_004_ASplineEdgeIsCountedAsUnsupported) {
    FaceCurve spline; // Kind::Unsupported by default -- what the kernel reports
    const ProjectedBoundary projected =
        ProjectBoundaryOntoSketch(FaceBoundary{spline}, TopFaceFrame());

    EXPECT_TRUE(projected.geometry.empty());
    EXPECT_EQ(projected.skipped, 1);
    EXPECT_NE(projected.skippedReason.find("spline"), std::string::npos)
        << projected.skippedReason;
}

TEST(ReferenceProjectionTest, M17_PROJ_005_AnEdgeOnLineCollapsesToItsPointAndIsCounted) {
    // A line perpendicular to the sketch plane projects to a single point. The
    // point is still a useful snap target; the LINE is gone, and saying so is
    // the difference between an underlay and a lie.
    const FaceBoundary boundary = {Line3D(Vec3{5, 5, 0}, Vec3{5, 5, 20})};
    const ProjectedBoundary projected = ProjectBoundaryOntoSketch(boundary, TopFaceFrame());

    EXPECT_EQ(CountOf(projected, kLineIndex), 0);
    EXPECT_EQ(CountOf(projected, kPointIndex), 1);
    EXPECT_EQ(projected.skipped, 1);
    EXPECT_NE(projected.skippedReason.find("edge-on"), std::string::npos)
        << projected.skippedReason;
}

TEST(ReferenceProjectionTest, M17_PROJ_006_AnArcKeepsItsSweepAndIsAlwaysStoredCounterClockwise) {
    FaceCurve arc;
    arc.kind = FaceCurve::Kind::Arc;
    arc.center = Vec3{10, 10, 20};
    arc.radiusMm = 4.0;
    arc.start = Vec3{14, 10, 20}; // 0 degrees
    arc.end = Vec3{10, 14, 20};   // 90 degrees
    // Axis pointing DOWN: the same quarter arc, traversed the other way. The
    // projection must swap the tips rather than store a clockwise arc, because
    // every consumer of the underlay assumes the one direction.
    arc.axis = Vec3{0, 0, -1};

    const ProjectedBoundary projected =
        ProjectBoundaryOntoSketch(FaceBoundary{arc}, TopFaceFrame());
    ASSERT_EQ(CountOf(projected, kArcIndex), 1);
    const auto* projectedArc = std::get_if<SketchArc>(&projected.geometry.front());
    ASSERT_NE(projectedArc, nullptr);
    EXPECT_TRUE(projectedArc->counterClockwise);
    EXPECT_NEAR(projectedArc->radiusMm, 4.0, 1e-9);
    // Swapped: it now runs from 90 degrees to 0, counter-clockwise -- which is
    // the same 270-degree path the original described clockwise from 0 to 90.
    EXPECT_NEAR(projectedArc->startAngleRad, kPi / 2.0, 1e-9);
    EXPECT_NEAR(projectedArc->endAngleRad, 0.0, 1e-9);
}

TEST(ReferenceProjectionTest, M17_PROJ_007_OnASideFaceTheProjectionUsesThatFacesAxes) {
    // The case where u and v are not world X and Y. The +X face of a box: u is
    // world +Y, v is world +Z, origin at (40, 0, 0). A projection that assumed
    // world axes would put this line somewhere else entirely.
    PickedFace side;
    side.isFace = true;
    side.planar = true;
    side.point = Vec3{40, 0, 0};
    side.normal = Vec3{1, 0, 0};
    const FaceSketchPlan plan = PlanSketchOnFace(side);
    ASSERT_TRUE(plan.ok) << plan.message;

    const FaceBoundary boundary = {Line3D(Vec3{40, 0, 0}, Vec3{40, 30, 0})};
    const ProjectedBoundary projected = ProjectBoundaryOntoSketch(boundary, plan.frame);
    ASSERT_EQ(CountOf(projected, kLineIndex), 1);
    const auto* line = std::get_if<SketchLine>(&projected.geometry.front());
    ASSERT_NE(line, nullptr);
    EXPECT_NEAR(line->start.x, 0.0, 1e-9);
    EXPECT_NEAR(line->start.y, 0.0, 1e-9);
    EXPECT_NEAR(line->end.x, 30.0, 1e-9); // 30 mm along u, which is world +Y
    EXPECT_NEAR(line->end.y, 0.0, 1e-9);
}

TEST(ReferenceProjectionTest, M17_PROJ_008_PlanSketchOnFaceCarriesTheProjectionAndSaysHowMuch) {
    PickedFace face;
    face.isFace = true;
    face.planar = true;
    face.point = Vec3{0, 0, 20};
    face.normal = Vec3{0, 0, 1};
    face.boundary = {Line3D(Vec3{0, 0, 20}, Vec3{10, 0, 20}), FaceCurve{}};

    const FaceSketchPlan plan = PlanSketchOnFace(face);
    ASSERT_TRUE(plan.ok) << plan.message;
    EXPECT_EQ(plan.reference.skipped, 1);
    // The count is in the message, because a user comparing it against the face
    // they clicked is the only check that the underlay is complete.
    EXPECT_NE(plan.message.find("reference item"), std::string::npos) << plan.message;
    EXPECT_NE(plan.message.find("not projected"), std::string::npos) << plan.message;
}

// --- Storage: references are NOT entities -----------------------------------

TEST(ReferenceStorageTest, M17_REF_001_AReferenceIsInvisibleToEverythingThatWalksEntities) {
    // The whole reason references live in their own container. If they were
    // entities with a flag, every one of these would need a skip, and one
    // forgotten skip is a reference edge silently becoming part of the solid.
    PartDocument document{"RefDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId drawn = sketch.addLine(Vec2{0, 0}, Vec2{10, 0});
    const SketchReferenceId reference = sketch.addReference(SketchLine{Vec2{0, 5}, Vec2{10, 5}});

    ASSERT_NE(reference, kInvalidSketchReferenceId);
    EXPECT_EQ(sketch.entities().size(), 1u);
    EXPECT_EQ(sketch.references().size(), 1u);
    EXPECT_NE(sketch.findEntity(drawn), nullptr);
    // The id spaces come from the same generator, so a reference id can never
    // be mistaken for an entity id that happens to exist.
    EXPECT_EQ(sketch.findEntity(static_cast<SketchEntityId>(ToObjectId(reference))), nullptr);
    EXPECT_NE(sketch.findReference(reference), nullptr);
}

TEST(ReferenceStorageTest, M17_REF_002_DegenerateProjectionsAreRefusedAtTheDoor) {
    PartDocument document{"RefDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    // A zero-length line is not an underlay anybody could click, and letting it
    // in would give the Use tool something it then has to refuse.
    EXPECT_EQ(sketch.addReference(SketchLine{Vec2{3, 3}, Vec2{3, 3}}), kInvalidSketchReferenceId);
    EXPECT_EQ(sketch.addReference(SketchCircle{Vec2{0, 0}, 0.0}), kInvalidSketchReferenceId);
    EXPECT_TRUE(sketch.references().empty());
}

TEST(ReferenceStorageTest, M17_REF_003_PickingPrefersAVertexOverTheEdgesThroughIt) {
    PartDocument document{"RefDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchReferenceId line = sketch.addReference(SketchLine{Vec2{0, 0}, Vec2{10, 0}});
    const SketchReferenceId corner = sketch.addReference(SketchPoint{Vec2{0, 0}});
    ASSERT_NE(line, kInvalidSketchReferenceId);
    ASSERT_NE(corner, kInvalidSketchReferenceId);

    // A click AT the corner: the point and the line are both exactly 0 away, so
    // without the point-wins rule the corner of a face could never be picked.
    EXPECT_EQ(ReferenceAt(sketch, Vec2{0, 0}, 0.5), corner);
    // And well along the line, the line is what is picked.
    EXPECT_EQ(ReferenceAt(sketch, Vec2{5, 0.1}, 0.5), line);
    // Nothing within reach is nothing, not the nearest thing at any distance.
    EXPECT_EQ(ReferenceAt(sketch, Vec2{5, 40}, 0.5), kInvalidSketchReferenceId);
}

// --- The Use tool -----------------------------------------------------------

TEST(UseReferenceTest, M17_USE_001_AReferenceLineBecomesAnOrdinaryFixedLine) {
    PartDocument document{"UseDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchReferenceId reference =
        sketch.addReference(SketchLine{Vec2{0, 0}, Vec2{40, 0}});
    ASSERT_NE(reference, kInvalidSketchReferenceId);

    const ConvertReferencePlan plan = PlanConvertReference(sketch, reference);
    ASSERT_TRUE(plan.ok) << plan.message;
    EXPECT_EQ(plan.edit.kind, SketchEditKind::AddLine);
    // BOTH endpoints fixed: four residuals for a line's four degrees of
    // freedom. One Fix would leave the line free to swing about that end.
    EXPECT_EQ(plan.edit.autoConstraints.size(), 2u);

    const SketchEditOutcome outcome = ApplySketchEdit(document, sketch.id(), plan.edit);
    ASSERT_TRUE(outcome.applied) << outcome.status;
    ASSERT_EQ(sketch.entities().size(), 1u);
    const auto* line = std::get_if<SketchLine>(&sketch.entities().front().geometry);
    ASSERT_NE(line, nullptr);
    EXPECT_NEAR(line->start.x, 0.0, 1e-9);
    EXPECT_NEAR(line->end.x, 40.0, 1e-9);
    EXPECT_EQ(sketch.constraints().size(), 2u);

    // The reference is STILL THERE. Using an edge does not consume it -- the
    // underlay is what the face looked like, and that did not change.
    EXPECT_EQ(sketch.references().size(), 1u);
}

TEST(UseReferenceTest, M17_USE_002_TheConvertedGeometryIsREALLYSolvedToZeroDOF) {
    // The claim that matters about the Fix constraints: not that they exist,
    // but that the solver agrees the line is pinned. A Fix on a sub-element the
    // solver has no variable for would sit in the list looking correct.
    PartDocument document{"UseDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchReferenceId reference =
        sketch.addReference(SketchLine{Vec2{2, 3}, Vec2{42, 3}});

    const ConvertReferencePlan plan = PlanConvertReference(sketch, reference);
    ASSERT_TRUE(plan.ok) << plan.message;
    ASSERT_TRUE(ApplySketchEdit(document, sketch.id(), plan.edit).applied);
    ASSERT_TRUE(document.recompute().success);

    EXPECT_EQ(sketch.solveStatus(), SketchSolveStatus::Solved);
    EXPECT_EQ(sketch.degreesOfFreedom(), 0) << sketch.solveMessage();
    // And it did not MOVE while being solved.
    const auto* line = std::get_if<SketchLine>(&sketch.entities().front().geometry);
    ASSERT_NE(line, nullptr);
    EXPECT_NEAR(line->start.x, 2.0, 1e-6);
    EXPECT_NEAR(line->end.x, 42.0, 1e-6);
}

TEST(UseReferenceTest, M17_USE_003_ACircleFixesItsCentreAndSaysTheRadiusIsFree) {
    PartDocument document{"UseDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchReferenceId reference =
        sketch.addReference(SketchCircle{Vec2{20, 15}, 5.0});

    const ConvertReferencePlan plan = PlanConvertReference(sketch, reference);
    ASSERT_TRUE(plan.ok) << plan.message;
    EXPECT_EQ(plan.edit.kind, SketchEditKind::AddCircle);
    EXPECT_EQ(plan.edit.autoConstraints.size(), 1u);
    // Said out loud. A user who reads "fixed" and then finds the radius moving
    // has been told something untrue.
    EXPECT_NE(plan.message.find("radius is free"), std::string::npos) << plan.message;

    ASSERT_TRUE(ApplySketchEdit(document, sketch.id(), plan.edit).applied);
    const auto* circle = std::get_if<SketchCircle>(&sketch.entities().front().geometry);
    ASSERT_NE(circle, nullptr);
    EXPECT_NEAR(circle->radiusMm, 5.0, 1e-9);
}

TEST(UseReferenceTest, M17_USE_004_AnArcIsNotOverConstrainedByItsOwnConversion) {
    // An arc has five degrees of freedom. Pinning its centre AND both tips
    // would be six residuals, and the solver would name a constraint the user
    // never added as the offender.
    PartDocument document{"UseDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchReferenceId reference =
        sketch.addReference(SketchArc{Vec2{10, 10}, 4.0, 0.0, kPi / 2.0, true});

    const ConvertReferencePlan plan = PlanConvertReference(sketch, reference);
    ASSERT_TRUE(plan.ok) << plan.message;
    ASSERT_TRUE(ApplySketchEdit(document, sketch.id(), plan.edit).applied);
    ASSERT_TRUE(document.recompute().success);

    // UNDER-constrained is the CORRECT answer here, and saying so is the point
    // of the test. An arc whose centre is pinned still has its radius and both
    // angles free; what must never happen is the OTHER outcome -- a conversion
    // that over-constrains geometry the user never touched and then blames a
    // constraint they never added.
    EXPECT_EQ(sketch.solveStatus(), SketchSolveStatus::UnderConstrained)
        << sketch.solveMessage();
    EXPECT_NE(sketch.solveStatus(), SketchSolveStatus::OverConstrained);
    EXPECT_NE(sketch.solveStatus(), SketchSolveStatus::Conflicting);
    EXPECT_TRUE(sketch.offendingConstraints().empty());
    // Two of the arc's five degrees of freedom are gone; the radius and the two
    // angles remain, exactly as PlanConvertReference's message says.
    EXPECT_EQ(sketch.degreesOfFreedom(), 3) << sketch.solveMessage();
    // And the arc did not move while the solver looked at it.
    const auto* arc = std::get_if<SketchArc>(&sketch.entities().front().geometry);
    ASSERT_NE(arc, nullptr);
    EXPECT_NEAR(arc->center.x, 10.0, 1e-6);
    EXPECT_NEAR(arc->center.y, 10.0, 1e-6);
    EXPECT_NEAR(arc->radiusMm, 4.0, 1e-6);
}

TEST(UseReferenceTest, M17_USE_005_TheSameEdgeIsREFUSEDTheSecondTime) {
    // A duplicate curve exactly on top of another is invisible on screen and
    // gives the profile walker two ways out of every vertex.
    PartDocument document{"UseDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchReferenceId reference =
        sketch.addReference(SketchLine{Vec2{0, 0}, Vec2{40, 0}});

    const ConvertReferencePlan first = PlanConvertReference(sketch, reference);
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(ApplySketchEdit(document, sketch.id(), first.edit).applied);

    const ConvertReferencePlan second = PlanConvertReference(sketch, reference);
    EXPECT_FALSE(second.ok);
    EXPECT_NE(second.message.find("already"), std::string::npos) << second.message;
    EXPECT_EQ(sketch.entities().size(), 1u);
}

TEST(UseReferenceTest, M17_USE_006_AVanishedReferenceIsRefusedInWordsNotDereferenced) {
    PartDocument document{"UseDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const ConvertReferencePlan plan =
        PlanConvertReference(sketch, static_cast<SketchReferenceId>(9999));
    EXPECT_FALSE(plan.ok);
    EXPECT_FALSE(plan.message.empty());
}

TEST(UseReferenceTest, M17_USE_007_UsingAnEdgeIsONEUndoStep) {
    // The entity and its Fix constraints arrived together, so they leave
    // together. Two steps would mean an undo that leaves a Fix behind pointing
    // at geometry that no longer exists.
    PartDocument document{"UseDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchReferenceId reference =
        sketch.addReference(SketchLine{Vec2{0, 0}, Vec2{40, 0}});
    const std::size_t before = document.undoDepth();

    const ConvertReferencePlan plan = PlanConvertReference(sketch, reference);
    ASSERT_TRUE(plan.ok);
    ASSERT_TRUE(ApplySketchEdit(document, sketch.id(), plan.edit).applied);
    EXPECT_EQ(document.undoDepth(), before + 1);

    ASSERT_TRUE(document.undo());
    EXPECT_TRUE(sketch.entities().empty());
    EXPECT_TRUE(sketch.constraints().empty());
    // And the underlay survives the undo, because using an edge never changed
    // it in the first place.
    EXPECT_EQ(sketch.references().size(), 1u);
}

// --- M17.19: reference (driven) dimensions -----------------------------------
//
// A driven dimension MEASURES instead of driving. Two claims, and they fail in
// opposite directions: it must take NO degree of freedom away (or it is just a
// dimension with a confusing label), and its number must FOLLOW the geometry
// (or it is a stale figure that looks authoritative).

namespace {

// A rectangle with one length dimension on the bottom edge.
struct DimensionedSketch {
    PartDocument document{"DrivenDoc"};
    GaussNewtonSketchSolver solver;
    Sketch* sketch = nullptr;
    SketchEntityId bottom{kInvalidSketchEntityId};
    SketchConstraintId length{kInvalidSketchConstraintId};
    Parameter* parameter = nullptr;

    DimensionedSketch() {
        document.setSketchSolver(&solver);
        sketch = &document.addSketch("Sketch001");
        bottom = sketch->addLine(Vec2{0, 0}, Vec2{80, 0});
        parameter = &document.addParameter("Width", 80.0, UnitType::Millimeter);
        length = document.addSketchConstraint(sketch->id(),
                                              LengthConstraint{bottom, parameter->id()});
    }
};

} // namespace

TEST(DrivenDimensionTest, M17_DRIVEN_001_ADrivingDimensionTakesADegreeOfFreedomAndADrivenOneDoesNot) {
    // The difference, measured rather than described. Same sketch, same
    // constraint, one flag.
    DimensionedSketch driving;
    ASSERT_TRUE(driving.document.recompute().success);
    const int withDriving = driving.sketch->degreesOfFreedom();

    DimensionedSketch driven;
    ASSERT_TRUE(driven.document.setSketchConstraintDriven(driven.sketch->id(), driven.length,
                                                          true));
    ASSERT_TRUE(driven.document.recompute().success);
    const int withDriven = driven.sketch->degreesOfFreedom();

    EXPECT_EQ(withDriven, withDriving + 1)
        << "a reference dimension removed a freedom it should only be watching";
}

TEST(DrivenDimensionTest, M17_DRIVEN_002_ItsNumberFOLLOWSTheGeometry) {
    // The other half. A reference dimension whose number never updates is a
    // stale figure that looks authoritative -- worse than no dimension at all.
    DimensionedSketch fx;
    ASSERT_TRUE(fx.document.setSketchConstraintDriven(fx.sketch->id(), fx.length, true));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_NEAR(fx.parameter->value(), 80.0, 1e-6);

    // Move the line's end. Nothing is driving it now, so the geometry is free
    // to be what it is -- and the dimension has to say so.
    ASSERT_TRUE(fx.document.setSketchEntityGeometry(fx.sketch->id(), fx.bottom,
                                                    SketchLine{Vec2{0, 0}, Vec2{125, 0}}));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_NEAR(fx.parameter->value(), 125.0, 1e-6)
        << "the reference dimension did not follow the geometry";
}

TEST(DrivenDimensionTest, M17_DRIVEN_003_ADrivenDimensionCannotCONFLICT) {
    // Two dimensions on one line is the classic over-constraint. Making the
    // second a reference is exactly how a user gets to SEE a second number
    // without being told their sketch is broken.
    DimensionedSketch fx;
    Parameter& second = fx.document.addParameter("Alt", 40.0, UnitType::Millimeter);
    const SketchConstraintId extra = fx.document.addSketchConstraint(
        fx.sketch->id(), LengthConstraint{fx.bottom, second.id()});
    ASSERT_NE(extra, kInvalidSketchConstraintId);

    // Driving, it conflicts: 80 and 40 cannot both be the length.
    fx.document.recompute();
    EXPECT_NE(fx.sketch->solveStatus(), SketchSolveStatus::Solved);

    // As a reference, it simply reports.
    ASSERT_TRUE(fx.document.setSketchConstraintDriven(fx.sketch->id(), extra, true));
    ASSERT_TRUE(fx.document.recompute().success) << fx.sketch->solveMessage();
    EXPECT_TRUE(fx.sketch->offendingConstraints().empty());
    EXPECT_NEAR(second.value(), 80.0, 1e-6)
        << "the reference dimension kept its old number instead of measuring";
}

TEST(DrivenDimensionTest, M17_DRIVEN_004_OnlyADimensionCanBeDriven) {
    // "A driven Horizontal" describes nothing, and accepting it would put a
    // flag in the file no reader could act on.
    DimensionedSketch fx;
    const SketchConstraintId horizontal =
        fx.document.addSketchConstraint(fx.sketch->id(), HorizontalConstraint{fx.bottom});
    ASSERT_NE(horizontal, kInvalidSketchConstraintId);
    EXPECT_FALSE(fx.document.setSketchConstraintDriven(fx.sketch->id(), horizontal, true));
    EXPECT_FALSE(fx.sketch->isConstraintDriven(horizontal));
}

TEST(DrivenDimensionTest, M17_DRIVEN_005_SwitchingIsONEUndoStep) {
    DimensionedSketch fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const std::size_t before = fx.document.undoDepth();

    ASSERT_TRUE(fx.document.setSketchConstraintDriven(fx.sketch->id(), fx.length, true));
    EXPECT_EQ(fx.document.undoDepth(), before + 1);
    EXPECT_TRUE(fx.sketch->isConstraintDriven(fx.length));

    ASSERT_TRUE(fx.document.undo());
    EXPECT_FALSE(fx.sketch->isConstraintDriven(fx.length))
        << "undo did not put the dimension back to driving";
}

TEST(DrivenDimensionTest, M17_DRIVEN_006_MeasuringUsesTHESAMEFormulaTheResidualDrives) {
    // The reason MeasureConstraint exists in one place. If the measurement and
    // the residual disagreed, a driving dimension and a reference dimension on
    // the SAME geometry would report different numbers -- and both would look
    // right on their own.
    DimensionedSketch fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const SketchConstraint* constraint = fx.sketch->findConstraint(fx.length);
    ASSERT_NE(constraint, nullptr);
    const std::optional<double> measured = MeasureConstraint(*fx.sketch, constraint->data);
    ASSERT_TRUE(measured.has_value());
    // The driving dimension solved the line to exactly its parameter, so the
    // measurement must agree with that parameter to the solver's tolerance.
    EXPECT_NEAR(*measured, fx.parameter->value(), 1e-6);
}

TEST(DrivenDimensionTest, M17_DRIVEN_007_ItsValueCannotBeTYPEDOverInThePanel) {
    // The worst kind of editable field is one that accepts a value, looks
    // changed, and is overwritten by the next recompute without saying why.
    DimensionedSketch fx;
    ASSERT_TRUE(fx.document.setSketchConstraintDriven(fx.sketch->id(), fx.length, true));
    ASSERT_TRUE(fx.document.recompute().success);

    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(fx.document, fx.parameter->id(), PropertyField::Value, "999");
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("reference dimension"), std::string::npos) << outcome.status;
    EXPECT_NEAR(fx.parameter->value(), 80.0, 1e-6);
}

TEST(DrivenDimensionTest, M17_DRIVEN_008_ItIsDrawnInBRACKETSNotJustColoured) {
    // The drafting convention, and a channel a monochrome print keeps (A06).
    // A number that looks like it controls the part and does not is worse than
    // no number.
    DimensionedSketch fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const std::vector<DimensionAnnotation> driving =
        DimensionAnnotationsFor(fx.document, *fx.sketch);
    ASSERT_EQ(driving.size(), 1u);
    EXPECT_EQ(driving.front().text.find('('), std::string::npos) << driving.front().text;

    ASSERT_TRUE(fx.document.setSketchConstraintDriven(fx.sketch->id(), fx.length, true));
    ASSERT_TRUE(fx.document.recompute().success);
    const std::vector<DimensionAnnotation> reference =
        DimensionAnnotationsFor(fx.document, *fx.sketch);
    ASSERT_EQ(reference.size(), 1u);
    EXPECT_EQ(reference.front().text.front(), '(') << reference.front().text;
    EXPECT_EQ(reference.front().text.back(), ')') << reference.front().text;
}

TEST(DrivenDimensionTest, M17_DRIVEN_009_TheFlagSurvivesASaveAndLoad) {
    DimensionedSketch fx;
    ASSERT_TRUE(fx.document.setSketchConstraintDriven(fx.sketch->id(), fx.length, true));

    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(fx.document, out));
    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;

    const Sketch* restored = loaded.document->sketches().front();
    ASSERT_NE(restored, nullptr);
    ASSERT_EQ(restored->constraints().size(), 1u);
    EXPECT_TRUE(restored->constraints().front().driven)
        << "the reference dimension came back driving, which changes the sketch";
}
