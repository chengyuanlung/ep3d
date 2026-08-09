// M5-F/G/H/I: constraints on a real Sketch, translated to the neutral problem,
// solved, and committed (spec 19 "Constraints" and "Identity").
//
// These go through Sketch and the ObjectRegistry rather than hand-built
// problems, so they exercise the translation layer -- which is where a
// reference can silently resolve to the wrong scalar.

#include "Core/Document/PartDocument.h"
#include "Core/Sketch/SketchSolveSession.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;
constexpr double kTol = 1e-6;

// A rectangle drawn deliberately sloppily: the solver must square it up.
struct RectangleSketch {
    PartDocument document{"ConstraintDoc"};
    Sketch* sketch = nullptr;
    Parameter* width = nullptr;
    Parameter* height = nullptr;
    SketchEntityId bottom{}, right{}, top{}, left{};

    RectangleSketch(double w = 100.0, double h = 50.0) {
        width = &document.addParameter("Width", w, UnitType::Millimeter);
        height = &document.addParameter("Height", h, UnitType::Millimeter);
        sketch = &document.addSketch("Sketch001");
        bottom = sketch->addLine(Vec2{0, 0}, Vec2{93, 4});
        right = sketch->addLine(Vec2{93, 4}, Vec2{97, 46});
        top = sketch->addLine(Vec2{97, 46}, Vec2{2, 52});
        left = sketch->addLine(Vec2{2, 52}, Vec2{0, 0});
    }

    void constrainFully() {
        // Corners coincident, per spec 14.
        sketch->addConstraint(CoincidentConstraint{
            {bottom, SketchSubElement::EndPoint}, {right, SketchSubElement::StartPoint}});
        sketch->addConstraint(CoincidentConstraint{
            {right, SketchSubElement::EndPoint}, {top, SketchSubElement::StartPoint}});
        sketch->addConstraint(CoincidentConstraint{
            {top, SketchSubElement::EndPoint}, {left, SketchSubElement::StartPoint}});
        sketch->addConstraint(CoincidentConstraint{
            {left, SketchSubElement::EndPoint}, {bottom, SketchSubElement::StartPoint}});
        sketch->addConstraint(HorizontalConstraint{bottom});
        sketch->addConstraint(HorizontalConstraint{top});
        sketch->addConstraint(VerticalConstraint{right});
        sketch->addConstraint(VerticalConstraint{left});
        sketch->addConstraint(FixConstraint{{bottom, SketchSubElement::StartPoint}});
        sketch->addConstraint(LengthConstraint{bottom, width->id()});
        sketch->addConstraint(LengthConstraint{right, height->id()});
    }

    SketchSolveResult solve() {
        const BuildProblemResult built = BuildSolveProblem(*sketch, document.objectRegistry());
        if (!built) {
            SketchSolveResult bad;
            bad.status = SketchSolveStatus::InvalidInput;
            bad.message = built.message;
            bad.offendingConstraints = built.invalidConstraints;
            return bad;
        }
        GaussNewtonSketchSolver solver;
        SketchSolveResult result = solver.solve(built.problem);
        if (result) CommitSolvedGeometry(*sketch, built.problem, result);
        return result;
    }

    Vec2 startOf(SketchEntityId id) const {
        return StartPointOf(sketch->findEntity(id)->geometry);
    }
    Vec2 endOf(SketchEntityId id) const {
        return EndPointOf(sketch->findEntity(id)->geometry);
    }
};

// --- Identity (spec 19) -----------------------------------------------------

TEST(SketchConstraintTest, M5_ID_001_ConstraintIdsAreUniqueAndStable) {
    RectangleSketch fx;
    const SketchConstraintId a = fx.sketch->addConstraint(HorizontalConstraint{fx.bottom});
    const SketchConstraintId b = fx.sketch->addConstraint(VerticalConstraint{fx.right});
    EXPECT_NE(a, kInvalidSketchConstraintId);
    EXPECT_NE(b, kInvalidSketchConstraintId);
    EXPECT_NE(a, b);

    // Adding and removing others must not disturb these.
    const SketchConstraintId c = fx.sketch->addConstraint(HorizontalConstraint{fx.top});
    ASSERT_TRUE(fx.sketch->removeConstraint(c));
    EXPECT_NE(fx.sketch->findConstraint(a), nullptr);
    EXPECT_NE(fx.sketch->findConstraint(b), nullptr);
    EXPECT_EQ(fx.sketch->findConstraint(c), nullptr);
}

TEST(SketchConstraintTest, M5_ID_002_DuplicateRestoredConstraintIdRejected) {
    RectangleSketch fx;
    const SketchConstraintId id{555000};
    EXPECT_TRUE(fx.sketch->restoreConstraint(id, HorizontalConstraint{fx.bottom}));
    EXPECT_FALSE(fx.sketch->restoreConstraint(id, VerticalConstraint{fx.right}))
        << "a duplicate constraint id was accepted";
    EXPECT_EQ(fx.sketch->constraints().size(), 1u);
}

TEST(SketchConstraintTest, M5_ID_003_ConstraintReferencingAnUnknownEntityIsRejected) {
    RectangleSketch fx;
    EXPECT_EQ(fx.sketch->addConstraint(HorizontalConstraint{SketchEntityId{987654}}),
              kInvalidSketchConstraintId);
    EXPECT_TRUE(fx.sketch->constraints().empty());
}

TEST(SketchConstraintTest, M5_ID_004_ConstraintsReferencingAnEntityAreFindable) {
    RectangleSketch fx;
    fx.constrainFully();
    const std::vector<SketchConstraintId> touching =
        fx.sketch->constraintsReferencing(fx.bottom);
    // bottom appears in: 2 coincidents, 1 horizontal, 1 fix, 1 length.
    EXPECT_EQ(touching.size(), 5u);
}

// --- The fully constrained rectangle (spec 14, Gate A geometry) ------------

TEST(SketchConstraintTest, M5_RECT_001_SloppyRectangleSolvesToExactDimensions) {
    RectangleSketch fx(100.0, 50.0);
    fx.constrainFully();
    const SketchSolveResult r = fx.solve();
    ASSERT_TRUE(r) << r.message;
    EXPECT_EQ(r.status, SketchSolveStatus::Solved);
    EXPECT_EQ(r.degreesOfFreedom, 0);

    // Expected corners derived from the constraints, not from the solver.
    EXPECT_NEAR(fx.startOf(fx.bottom).x, 0.0, kTol);
    EXPECT_NEAR(fx.startOf(fx.bottom).y, 0.0, kTol);
    EXPECT_NEAR(fx.endOf(fx.bottom).x, 100.0, kTol);
    EXPECT_NEAR(fx.endOf(fx.bottom).y, 0.0, kTol);
    EXPECT_NEAR(fx.endOf(fx.right).x, 100.0, kTol);
    EXPECT_NEAR(fx.endOf(fx.right).y, 50.0, kTol);
}

TEST(SketchConstraintTest, M5_RECT_002_WidthParameterDrivesTheGeometry) {
    // The central product proof, at sketch level: change the Parameter, solve,
    // and the geometry follows.
    RectangleSketch fx(100.0, 50.0);
    fx.constrainFully();
    ASSERT_TRUE(fx.solve());
    ASSERT_NEAR(fx.endOf(fx.bottom).x, 100.0, kTol);

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    const SketchSolveResult r = fx.solve();
    ASSERT_TRUE(r) << r.message;
    EXPECT_EQ(r.degreesOfFreedom, 0);
    EXPECT_NEAR(fx.endOf(fx.bottom).x, 120.0, kTol) << "width did not follow its Parameter";
    EXPECT_NEAR(fx.endOf(fx.right).y, 50.0, kTol) << "height moved when only width changed";
}

TEST(SketchConstraintTest, M5_RECT_003_EntityIdsSurviveSolving) {
    // Solving rewrites coordinates; it must not touch identity, or every
    // constraint referencing an entity would break on the next solve.
    RectangleSketch fx;
    fx.constrainFully();
    const std::vector<SketchEntityId> before = {fx.bottom, fx.right, fx.top, fx.left};
    ASSERT_TRUE(fx.solve());
    for (SketchEntityId id : before)
        EXPECT_NE(fx.sketch->findEntity(id), nullptr) << "an entity id changed during solve";
    EXPECT_EQ(fx.sketch->entities().size(), 4u);
}

// --- Individual constraint semantics (spec 11/19) --------------------------

TEST(SketchConstraintTest, M5_CONS_001_HorizontalAndVerticalHold) {
    RectangleSketch fx;
    fx.constrainFully();
    ASSERT_TRUE(fx.solve());
    EXPECT_NEAR(fx.startOf(fx.bottom).y, fx.endOf(fx.bottom).y, kTol) << "bottom not horizontal";
    EXPECT_NEAR(fx.startOf(fx.right).x, fx.endOf(fx.right).x, kTol) << "right not vertical";
}

TEST(SketchConstraintTest, M5_CONS_002_CoincidentClosesTheCorners) {
    RectangleSketch fx;
    fx.constrainFully();
    ASSERT_TRUE(fx.solve());
    EXPECT_NEAR(fx.endOf(fx.bottom).x, fx.startOf(fx.right).x, kTol);
    EXPECT_NEAR(fx.endOf(fx.bottom).y, fx.startOf(fx.right).y, kTol);
    EXPECT_NEAR(fx.endOf(fx.left).x, fx.startOf(fx.bottom).x, kTol);
    EXPECT_NEAR(fx.endOf(fx.left).y, fx.startOf(fx.bottom).y, kTol);
}

TEST(SketchConstraintTest, M5_CONS_003_RadiusDrivesACircle) {
    PartDocument document{"CircleDoc"};
    Parameter& radius = document.addParameter("Radius", 10.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId circle = sketch.addCircle(Vec2{3, -2}, 7.0);
    sketch.addConstraint(FixConstraint{{circle, SketchSubElement::CenterPoint}});
    sketch.addConstraint(RadiusConstraint{circle, radius.id()});

    const BuildProblemResult built = BuildSolveProblem(sketch, document.objectRegistry());
    ASSERT_TRUE(built) << built.message;
    GaussNewtonSketchSolver solver;
    SketchSolveResult r = solver.solve(built.problem);
    ASSERT_TRUE(r) << r.message;
    CommitSolvedGeometry(sketch, built.problem, r);

    const auto* solved = std::get_if<SketchCircle>(&sketch.findEntity(circle)->geometry);
    ASSERT_NE(solved, nullptr);
    EXPECT_NEAR(solved->radiusMm, 10.0, kTol);
    EXPECT_NEAR(solved->center.x, 3.0, kTol) << "Fix did not hold the centre";
    EXPECT_EQ(r.degreesOfFreedom, 0);
}

TEST(SketchConstraintTest, M5_CONS_004_DiameterDrivesTheSameRadiusNotSeparateState) {
    // ADR-M5-002: Diameter is a view of radius, so a Diameter of 30 must give a
    // radius of 15 -- and a Radius and Diameter that disagree must conflict.
    PartDocument document{"CircleDoc"};
    Parameter& diameter = document.addParameter("Diameter", 30.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId circle = sketch.addCircle(Vec2{0, 0}, 7.0);
    sketch.addConstraint(FixConstraint{{circle, SketchSubElement::CenterPoint}});
    sketch.addConstraint(DiameterConstraint{circle, diameter.id()});

    const BuildProblemResult built = BuildSolveProblem(sketch, document.objectRegistry());
    ASSERT_TRUE(built) << built.message;
    GaussNewtonSketchSolver solver;
    SketchSolveResult r = solver.solve(built.problem);
    ASSERT_TRUE(r) << r.message;
    CommitSolvedGeometry(sketch, built.problem, r);

    EXPECT_NEAR(std::get<SketchCircle>(sketch.findEntity(circle)->geometry).radiusMm, 15.0,
                kTol);
}

TEST(SketchConstraintTest, M5_CONS_005_RadiusAndDiameterThatDisagreeConflict) {
    PartDocument document{"CircleDoc"};
    Parameter& radius = document.addParameter("Radius", 10.0, UnitType::Millimeter);
    Parameter& diameter = document.addParameter("Diameter", 30.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId circle = sketch.addCircle(Vec2{0, 0}, 7.0);
    sketch.addConstraint(FixConstraint{{circle, SketchSubElement::CenterPoint}});
    sketch.addConstraint(RadiusConstraint{circle, radius.id()});
    sketch.addConstraint(DiameterConstraint{circle, diameter.id()}); // wants r = 15

    const BuildProblemResult built = BuildSolveProblem(sketch, document.objectRegistry());
    ASSERT_TRUE(built) << built.message;
    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(built.problem);
    EXPECT_FALSE(r) << "r=10 and d=30 should not both be satisfiable";
    EXPECT_EQ(r.status, SketchSolveStatus::Conflicting);
    EXPECT_FALSE(r.offendingConstraints.empty());
}

// --- Validation happens before solving (ADR-M5-005) ------------------------

TEST(SketchConstraintTest, M5_VALID_001_IncompatibleParameterUnitIsRejected) {
    // Spec 32 lists "unit mismatch changes physical geometry" as Critical, and
    // refusing the binding is what makes it unreachable.
    RectangleSketch fx;
    Parameter& mass = fx.document.addParameter("SomeMass", 5.0, UnitType::Kilogram);
    fx.sketch->addConstraint(LengthConstraint{fx.bottom, mass.id()});

    const BuildProblemResult built =
        BuildSolveProblem(*fx.sketch, fx.document.objectRegistry());
    EXPECT_FALSE(built) << "a length bound to a mass Parameter was accepted";
    EXPECT_FALSE(built.message.empty());
}

TEST(SketchConstraintTest, M5_VALID_002_NonFiniteAndNonPositiveDimensionsRejected) {
    for (double bad : {0.0, -5.0, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()}) {
        RectangleSketch fx;
        ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), bad));
        fx.sketch->addConstraint(LengthConstraint{fx.bottom, fx.width->id()});
        const BuildProblemResult built =
            BuildSolveProblem(*fx.sketch, fx.document.objectRegistry());
        EXPECT_FALSE(built) << "dimension " << bad << " was accepted";
    }
}

TEST(SketchConstraintTest, M5_VALID_003_MissingParameterIsRejected) {
    RectangleSketch fx;
    fx.sketch->addConstraint(LengthConstraint{fx.bottom, 999999});
    const BuildProblemResult built =
        BuildSolveProblem(*fx.sketch, fx.document.objectRegistry());
    EXPECT_FALSE(built);
}

TEST(SketchConstraintTest, M5_VALID_004_WrongSubElementIsRejected) {
    // A line has no centre point; naming one must fail rather than silently
    // resolving to its start.
    RectangleSketch fx;
    fx.sketch->addConstraint(FixConstraint{{fx.bottom, SketchSubElement::CenterPoint}});
    const BuildProblemResult built =
        BuildSolveProblem(*fx.sketch, fx.document.objectRegistry());
    EXPECT_FALSE(built) << "a line accepted a CenterPoint reference";
}

// --- Transactional commit (ADR-M5-004) -------------------------------------

TEST(SketchConstraintTest, M5_COMMIT_001_FailedSolveLeavesGeometryUntouched) {
    RectangleSketch fx(100.0, 50.0);
    fx.constrainFully();
    ASSERT_TRUE(fx.solve());
    const Vec2 beforeStart = fx.startOf(fx.bottom);
    const Vec2 beforeEnd = fx.endOf(fx.bottom);

    // Contradict the width: a second Length on the same line, disagreeing.
    Parameter& other = fx.document.addParameter("Other", 120.0, UnitType::Millimeter);
    fx.sketch->addConstraint(LengthConstraint{fx.bottom, other.id()});

    const SketchSolveResult r = fx.solve();
    EXPECT_FALSE(r) << "contradictory lengths reported success";
    EXPECT_EQ(r.status, SketchSolveStatus::Conflicting);

    EXPECT_DOUBLE_EQ(fx.startOf(fx.bottom).x, beforeStart.x)
        << "a failed solve modified the sketch";
    EXPECT_DOUBLE_EQ(fx.endOf(fx.bottom).x, beforeEnd.x);
}

TEST(SketchConstraintTest, M5_COMMIT_002_RecoveryIsDeterministic) {
    RectangleSketch fx(100.0, 50.0);
    fx.constrainFully();
    ASSERT_TRUE(fx.solve());

    Parameter& other = fx.document.addParameter("Other", 120.0, UnitType::Millimeter);
    const SketchConstraintId bad =
        fx.sketch->addConstraint(LengthConstraint{fx.bottom, other.id()});
    ASSERT_FALSE(fx.solve());

    ASSERT_TRUE(fx.sketch->removeConstraint(bad));
    const SketchSolveResult recovered = fx.solve();
    ASSERT_TRUE(recovered) << recovered.message;
    EXPECT_EQ(recovered.status, SketchSolveStatus::Solved);
    EXPECT_NEAR(fx.endOf(fx.bottom).x, 100.0, kTol);
}

// --- Order independence at sketch level (spec 19) --------------------------

TEST(SketchConstraintTest, M5_ORDER_002_ConstraintCreationOrderDoesNotChangeTheResult) {
    RectangleSketch a(100.0, 50.0);
    a.constrainFully();
    ASSERT_TRUE(a.solve());

    // The same constraints, added in a different order.
    RectangleSketch b(100.0, 50.0);
    b.sketch->addConstraint(LengthConstraint{b.right, b.height->id()});
    b.sketch->addConstraint(VerticalConstraint{b.left});
    b.sketch->addConstraint(CoincidentConstraint{
        {b.top, SketchSubElement::EndPoint}, {b.left, SketchSubElement::StartPoint}});
    b.sketch->addConstraint(HorizontalConstraint{b.bottom});
    b.sketch->addConstraint(FixConstraint{{b.bottom, SketchSubElement::StartPoint}});
    b.sketch->addConstraint(CoincidentConstraint{
        {b.left, SketchSubElement::EndPoint}, {b.bottom, SketchSubElement::StartPoint}});
    b.sketch->addConstraint(VerticalConstraint{b.right});
    b.sketch->addConstraint(LengthConstraint{b.bottom, b.width->id()});
    b.sketch->addConstraint(CoincidentConstraint{
        {b.bottom, SketchSubElement::EndPoint}, {b.right, SketchSubElement::StartPoint}});
    b.sketch->addConstraint(HorizontalConstraint{b.top});
    b.sketch->addConstraint(CoincidentConstraint{
        {b.right, SketchSubElement::EndPoint}, {b.top, SketchSubElement::StartPoint}});
    ASSERT_TRUE(b.solve());

    EXPECT_NEAR(a.endOf(a.bottom).x, b.endOf(b.bottom).x, kTol);
    EXPECT_NEAR(a.endOf(a.right).y, b.endOf(b.right).y, kTol);
}

// --- Under-constrained (Gate H) --------------------------------------------

TEST(SketchConstraintTest, M5_DOF_010_AddingConstraintsWalksDofToZero) {
    RectangleSketch fx(100.0, 50.0);
    // Nothing but the four lines: 16 free scalars.
    SketchSolveResult r = fx.solve();
    ASSERT_TRUE(r) << r.message;
    EXPECT_EQ(r.status, SketchSolveStatus::UnderConstrained);
    const int bare = r.degreesOfFreedom;
    EXPECT_EQ(bare, 16);

    fx.constrainFully();
    r = fx.solve();
    ASSERT_TRUE(r) << r.message;
    EXPECT_EQ(r.status, SketchSolveStatus::Solved);
    EXPECT_EQ(r.degreesOfFreedom, 0) << "the fully constrained rectangle is not at DOF 0";
}

} // namespace
