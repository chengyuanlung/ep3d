// M13 -- the seven geometric constraints of roadmap 6.1's "next stage",
// solved for real.
//
// Every test here starts from geometry that does NOT satisfy the constraint and
// asserts the relationship afterwards. That is deliberate: a sketch drawn
// already-correct passes whether or not the residual means anything, and this
// project has shipped a residual that converged to 4e-11 while measuring the
// wrong quantity (ADR-M5-006). Starting off-relation is what makes "Solved" say
// something.

#include "Core/Document/PartDocument.h"
#include "Core/Sketch/SketchSolveSession.h"
#include "Solver/GaussNewtonSketchSolver.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kTol = 1e-6;

struct Fixture {
    PartDocument document{"GeometricDoc"};
    Sketch* sketch = nullptr;

    Fixture() { sketch = &document.addSketch("Sketch001"); }

    SketchEntityId line(Vec2 a, Vec2 b) { return sketch->addLine(a, b); }
    SketchEntityId circle(Vec2 c, double r) { return sketch->addCircle(c, r); }
    SketchEntityId point(Vec2 p) { return sketch->addPoint(p); }

    SketchConstraintId add(SketchConstraintData data) {
        return document.addSketchConstraint(sketch->id(), std::move(data));
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

    const SketchLine& lineOf(SketchEntityId id) const {
        return *std::get_if<SketchLine>(&sketch->findEntity(id)->geometry);
    }
    const SketchCircle& circleOf(SketchEntityId id) const {
        return *std::get_if<SketchCircle>(&sketch->findEntity(id)->geometry);
    }
    Vec2 pointOf(SketchEntityId id) const {
        return std::get_if<SketchPoint>(&sketch->findEntity(id)->geometry)->position;
    }
};

Vec2 Direction(const SketchLine& line) {
    return Vec2{line.end.x - line.start.x, line.end.y - line.start.y};
}
double Norm(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
double Cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
double Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
double Distance(Vec2 a, Vec2 b) { return Norm(Vec2{a.x - b.x, a.y - b.y}); }

// Perpendicular distance from `p` to the infinite line through the segment.
double DistanceToLine(Vec2 p, const SketchLine& line) {
    const Vec2 d = Direction(line);
    const Vec2 r{p.x - line.start.x, p.y - line.start.y};
    return std::fabs(Cross(r, d)) / Norm(d);
}

} // namespace

// =============================================================================
// Parallel and Perpendicular
// =============================================================================

TEST(SketchGeometricConstraintTest, M13_PAR_001_TwoSkewLinesBecomeParallel) {
    Fixture fx;
    const SketchEntityId a = fx.line(Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId b = fx.line(Vec2{0, 40}, Vec2{90, 70}); // clearly not parallel
    ASSERT_NE(fx.add(ParallelConstraint{a, b}), kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;

    const Vec2 dirA = Direction(fx.lineOf(a));
    const Vec2 dirB = Direction(fx.lineOf(b));
    // The NORMALISED cross product, which is what the residual drives to zero.
    // Checking the raw cross product would let a solver that shrank one line to
    // nothing pass.
    EXPECT_NEAR(Cross(dirA, dirB) / (Norm(dirA) * Norm(dirB)), 0.0, kTol);
    EXPECT_GT(Norm(dirA), 1.0);
    EXPECT_GT(Norm(dirB), 1.0);
}

TEST(SketchGeometricConstraintTest, M13_PAR_002_ParallelAcceptsAntiParallelToo) {
    Fixture fx;
    // Drawn in OPPOSITE directions and 20 degrees apart. Parallel is about
    // direction up to sign -- an Angle constraint of 0 would refuse this pair,
    // and that difference is the reason Parallel is not just Angle(0).
    const SketchEntityId a = fx.line(Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId b = fx.line(Vec2{100, 40}, Vec2{6, 6});
    ASSERT_NE(fx.add(ParallelConstraint{a, b}), kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;
    const Vec2 dirA = Direction(fx.lineOf(a));
    const Vec2 dirB = Direction(fx.lineOf(b));
    EXPECT_NEAR(Cross(dirA, dirB) / (Norm(dirA) * Norm(dirB)), 0.0, kTol);
    // ...and it settled on the ANTI-parallel branch, the one it started near,
    // rather than flipping the line end for end.
    EXPECT_LT(Dot(dirA, dirB), 0.0);
}

TEST(SketchGeometricConstraintTest, M13_PERP_001_TwoShallowLinesBecomePerpendicular) {
    Fixture fx;
    const SketchEntityId a = fx.line(Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId b = fx.line(Vec2{10, 5}, Vec2{80, 30}); // ~20 degrees, not 90
    ASSERT_NE(fx.add(PerpendicularConstraint{a, b}), kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;

    const Vec2 dirA = Direction(fx.lineOf(a));
    const Vec2 dirB = Direction(fx.lineOf(b));
    EXPECT_NEAR(Dot(dirA, dirB) / (Norm(dirA) * Norm(dirB)), 0.0, kTol);
    EXPECT_GT(Norm(dirB), 1.0);
}

TEST(SketchGeometricConstraintTest, M13_PAR_003_ParallelRemovesExactlyOneDegreeOfFreedom) {
    Fixture fx;
    const SketchEntityId a = fx.line(Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId b = fx.line(Vec2{0, 40}, Vec2{90, 70});

    const SketchSolveResult before = fx.solve();
    ASSERT_TRUE(before) << before.message;
    const int dofBefore = before.degreesOfFreedom;

    ASSERT_NE(fx.add(ParallelConstraint{a, b}), kInvalidSketchConstraintId);
    const SketchSolveResult after = fx.solve();
    ASSERT_TRUE(after) << after.message;

    // DOF is MEASURED from the Jacobian's rank (ADR-M5-005), so this is a real
    // check that the residual is independent and non-degenerate at the
    // solution -- a residual that is identically zero would remove nothing.
    EXPECT_EQ(after.degreesOfFreedom, dofBefore - 1);
}

// =============================================================================
// Equal
// =============================================================================

TEST(SketchGeometricConstraintTest, M13_EQ_001_TwoLinesBecomeTheSameLength) {
    Fixture fx;
    const SketchEntityId a = fx.line(Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId b = fx.line(Vec2{0, 40}, Vec2{30, 40});
    ASSERT_NE(fx.add(EqualConstraint{a, b}), kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;
    EXPECT_NEAR(Norm(Direction(fx.lineOf(a))), Norm(Direction(fx.lineOf(b))), kTol);
}

TEST(SketchGeometricConstraintTest, M13_EQ_002_TwoCurvesBecomeTheSameRadius) {
    Fixture fx;
    const SketchEntityId a = fx.circle(Vec2{0, 0}, 25.0);
    const SketchEntityId b = fx.circle(Vec2{100, 0}, 8.0);
    ASSERT_NE(fx.add(EqualConstraint{a, b}), kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;
    EXPECT_NEAR(fx.circleOf(a).radiusMm, fx.circleOf(b).radiusMm, kTol);
    // And it did not collapse both to zero to satisfy the equation.
    EXPECT_GT(fx.circleOf(a).radiusMm, 1.0);
}

TEST(SketchGeometricConstraintTest, M13_EQ_003_ALineAndACurveAreRefusedNotEquated) {
    Fixture fx;
    const SketchEntityId a = fx.line(Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId b = fx.circle(Vec2{0, 40}, 8.0);
    const SketchConstraintId id = fx.add(EqualConstraint{a, b});
    ASSERT_NE(id, kInvalidSketchConstraintId); // structurally fine; semantically not

    const SketchSolveResult result = fx.solve();
    EXPECT_EQ(result.status, SketchSolveStatus::InvalidInput);
    // Named, not merely refused: roadmap 8.2's locatability requirement.
    ASSERT_FALSE(result.offendingConstraints.empty());
    EXPECT_EQ(result.offendingConstraints.front(), id);
    EXPECT_NE(result.message.find("two lines or two curves"), std::string::npos)
        << result.message;
}

// =============================================================================
// Concentric
// =============================================================================

TEST(SketchGeometricConstraintTest, M13_CONC_001_TwoCirclesShareACentre) {
    Fixture fx;
    const SketchEntityId a = fx.circle(Vec2{0, 0}, 25.0);
    const SketchEntityId b = fx.circle(Vec2{60, 30}, 10.0);
    ASSERT_NE(fx.add(ConcentricConstraint{a, b}), kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;
    EXPECT_NEAR(Distance(fx.circleOf(a).center, fx.circleOf(b).center), 0.0, kTol);
    // Concentric says nothing about SIZE. A version that reused the Equal
    // residual by mistake would also have equalised these.
    EXPECT_NEAR(fx.circleOf(a).radiusMm, 25.0, kTol);
    EXPECT_NEAR(fx.circleOf(b).radiusMm, 10.0, kTol);
}

TEST(SketchGeometricConstraintTest, M13_CONC_002_ALineIsRefused) {
    Fixture fx;
    const SketchEntityId a = fx.circle(Vec2{0, 0}, 25.0);
    const SketchEntityId b = fx.line(Vec2{0, 40}, Vec2{90, 70});
    const SketchConstraintId id = fx.add(ConcentricConstraint{a, b});
    const SketchSolveResult result = fx.solve();
    EXPECT_EQ(result.status, SketchSolveStatus::InvalidInput);
    ASSERT_FALSE(result.offendingConstraints.empty());
    EXPECT_EQ(result.offendingConstraints.front(), id);
}

// =============================================================================
// Midpoint
// =============================================================================

TEST(SketchGeometricConstraintTest, M13_MID_001_APointMovesToTheLinesMidpoint) {
    Fixture fx;
    const SketchEntityId l = fx.line(Vec2{0, 0}, Vec2{100, 40});
    const SketchEntityId p = fx.point(Vec2{5, 90});
    // The LINE is pinned, so the only way to satisfy this is to move the point
    // -- otherwise the solver could satisfy it by dragging the line to the
    // point, and the test would pass without the point ever moving.
    ASSERT_NE(fx.add(FixConstraint{{l, SketchSubElement::StartPoint}}),
              kInvalidSketchConstraintId);
    ASSERT_NE(fx.add(FixConstraint{{l, SketchSubElement::EndPoint}}),
              kInvalidSketchConstraintId);
    ASSERT_NE(fx.add(MidpointConstraint{{p, SketchSubElement::Whole}, l}),
              kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;
    EXPECT_NEAR(fx.pointOf(p).x, 50.0, kTol);
    EXPECT_NEAR(fx.pointOf(p).y, 20.0, kTol);
}

TEST(SketchGeometricConstraintTest, M13_MID_002_TheMidpointIsNotMerelyOnTheLine) {
    Fixture fx;
    const SketchEntityId l = fx.line(Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId p = fx.point(Vec2{90, 30});
    ASSERT_NE(fx.add(FixConstraint{{l, SketchSubElement::StartPoint}}),
              kInvalidSketchConstraintId);
    ASSERT_NE(fx.add(FixConstraint{{l, SketchSubElement::EndPoint}}),
              kInvalidSketchConstraintId);
    ASSERT_NE(fx.add(MidpointConstraint{{p, SketchSubElement::Whole}, l}),
              kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;
    // Started at u = 90, which is ON the line's span. A point-on-line residual
    // would have been satisfied by dropping straight down to (90, 0).
    EXPECT_NEAR(fx.pointOf(p).x, 50.0, kTol);
}

// =============================================================================
// Point on object
// =============================================================================

TEST(SketchGeometricConstraintTest, M13_ON_001_APointLandsOnALine) {
    Fixture fx;
    const SketchEntityId l = fx.line(Vec2{0, 0}, Vec2{100, 40});
    const SketchEntityId p = fx.point(Vec2{20, 80});
    ASSERT_NE(fx.add(FixConstraint{{l, SketchSubElement::StartPoint}}),
              kInvalidSketchConstraintId);
    ASSERT_NE(fx.add(FixConstraint{{l, SketchSubElement::EndPoint}}),
              kInvalidSketchConstraintId);
    ASSERT_NE(fx.add(PointOnObjectConstraint{{p, SketchSubElement::Whole}, l}),
              kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;
    EXPECT_NEAR(DistanceToLine(fx.pointOf(p), fx.lineOf(l)), 0.0, kTol);
    // It slid ALONG the line rather than being pinned to one spot: the
    // constraint removes one freedom, not two.
    EXPECT_EQ(result.status, SketchSolveStatus::UnderConstrained);
}

TEST(SketchGeometricConstraintTest, M13_ON_002_APointLandsOnACirclesRim) {
    Fixture fx;
    const SketchEntityId c = fx.circle(Vec2{0, 0}, 30.0);
    const SketchEntityId p = fx.point(Vec2{80, 10});
    ASSERT_NE(fx.add(FixConstraint{{c, SketchSubElement::CenterPoint}}),
              kInvalidSketchConstraintId);
    ASSERT_NE(fx.add(PointOnObjectConstraint{{p, SketchSubElement::Whole}, c}),
              kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;
    EXPECT_NEAR(Distance(fx.pointOf(p), fx.circleOf(c).center), fx.circleOf(c).radiusMm, kTol);
}

// =============================================================================
// Tangent
// =============================================================================

TEST(SketchGeometricConstraintTest, M13_TAN_001_ALineBecomesTangentToACircle) {
    Fixture fx;
    const SketchEntityId c = fx.circle(Vec2{0, 0}, 30.0);
    // A line that currently CUTS the circle: its distance from the centre is
    // 10 mm, well inside r = 30.
    const SketchEntityId l = fx.line(Vec2{-80, 10}, Vec2{80, 10});
    ASSERT_NE(fx.add(FixConstraint{{c, SketchSubElement::CenterPoint}}),
              kInvalidSketchConstraintId);
    ASSERT_NE(fx.add(TangentConstraint{l, c, false}), kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;
    EXPECT_NEAR(DistanceToLine(fx.circleOf(c).center, fx.lineOf(l)), fx.circleOf(c).radiusMm,
                kTol);
}

TEST(SketchGeometricConstraintTest, M13_TAN_002_TwoCirclesTouchFromOutside) {
    Fixture fx;
    const SketchEntityId a = fx.circle(Vec2{0, 0}, 30.0);
    const SketchEntityId b = fx.circle(Vec2{40, 0}, 20.0); // overlapping: 40 < 50
    ASSERT_NE(fx.add(FixConstraint{{a, SketchSubElement::CenterPoint}}),
              kInvalidSketchConstraintId);
    ASSERT_NE(fx.add(TangentConstraint{a, b, /*internal=*/false}), kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;
    const double centres = Distance(fx.circleOf(a).center, fx.circleOf(b).center);
    EXPECT_NEAR(centres, fx.circleOf(a).radiusMm + fx.circleOf(b).radiusMm, kTol);
    // Neither circle was collapsed to nothing to satisfy the equation, and
    // they really are OUTSIDE one another rather than nested.
    EXPECT_GT(fx.circleOf(a).radiusMm, 1.0);
    EXPECT_GT(fx.circleOf(b).radiusMm, 1.0);
    EXPECT_GT(centres, std::fabs(fx.circleOf(a).radiusMm - fx.circleOf(b).radiusMm));
}

TEST(SketchGeometricConstraintTest, M13_TAN_003_TwoCirclesTouchFromInside) {
    Fixture fx;
    const SketchEntityId a = fx.circle(Vec2{0, 0}, 30.0);
    const SketchEntityId b = fx.circle(Vec2{4, 0}, 12.0); // nested, not yet touching
    ASSERT_NE(fx.add(FixConstraint{{a, SketchSubElement::CenterPoint}}),
              kInvalidSketchConstraintId);
    ASSERT_NE(fx.add(TangentConstraint{a, b, /*internal=*/true}), kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;
    const double centres = Distance(fx.circleOf(a).center, fx.circleOf(b).center);
    EXPECT_NEAR(centres, std::fabs(fx.circleOf(a).radiusMm - fx.circleOf(b).radiusMm), kTol);
    // The INNER branch, not the outer one: the two are different models, and
    // the stored flag is what decides which was meant.
    EXPECT_LT(centres, fx.circleOf(a).radiusMm + fx.circleOf(b).radiusMm);
}

TEST(SketchGeometricConstraintTest, M13_TAN_004_TwoLinesAreRefused) {
    Fixture fx;
    const SketchEntityId a = fx.line(Vec2{0, 0}, Vec2{100, 0});
    const SketchEntityId b = fx.line(Vec2{0, 40}, Vec2{90, 70});
    const SketchConstraintId id = fx.add(TangentConstraint{a, b, false});

    const SketchSolveResult result = fx.solve();
    EXPECT_EQ(result.status, SketchSolveStatus::InvalidInput);
    ASSERT_FALSE(result.offendingConstraints.empty());
    EXPECT_EQ(result.offendingConstraints.front(), id);
    EXPECT_NE(result.message.find("a line and a curve"), std::string::npos) << result.message;
}

// --- M17.21: tangency at a point the sketch already knows --------------------

TEST(SketchGeometricConstraintTest, M17_TAN_010_TangencyAtAnEndHoldsTHATEndSquare) {
    // The line's END is pinned on the circle by a coincidence, and the tangency
    // says the touch is THERE. What that must produce is a right angle at that
    // end -- and the sketch starts bent, so "Solved" means the residual pulled.
    Fixture fx;
    const SketchEntityId c = fx.circle(Vec2{0, 0}, 10.0);
    const SketchEntityId l = fx.line(Vec2{30, 20}, Vec2{7, 7});
    ASSERT_NE(fx.add(PointOnObjectConstraint{SketchElementRef{l, SketchSubElement::EndPoint}, c}),
              kInvalidSketchConstraintId);
    ASSERT_NE(fx.add(TangentConstraint{l, c, false, SketchSubElement::EndPoint}),
              kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;

    const SketchLine& line = fx.lineOf(l);
    const SketchCircle& circle = fx.circleOf(c);
    // On the circle...
    EXPECT_NEAR(Distance(line.end, circle.center), circle.radiusMm, kTol);
    // ...and square to the radius drawn THERE, which is the whole claim. The
    // distance form is satisfied by any grazing line and would leave the end
    // free to slide round the rim.
    const Vec2 radial{line.end.x - circle.center.x, line.end.y - circle.center.y};
    EXPECT_NEAR(Dot(Direction(line), radial) / (Norm(Direction(line)) * Norm(radial)), 0.0, kTol);
}

TEST(SketchGeometricConstraintTest, M17_TAN_011_ItHoldsTheEndItNAMESAndNotTheOtherOne) {
    // Both ends sit on the circle -- a chord. Only ONE of them is named, and
    // only that one may come out square. If the packing read the wrong end this
    // would still solve and still look tangent, at the wrong corner.
    Fixture fx;
    const SketchEntityId c = fx.circle(Vec2{0, 0}, 10.0);
    const SketchEntityId l = fx.line(Vec2{-9, 4}, Vec2{9, 4});
    ASSERT_NE(
        fx.add(PointOnObjectConstraint{SketchElementRef{l, SketchSubElement::StartPoint}, c}),
        kInvalidSketchConstraintId);
    ASSERT_NE(fx.add(TangentConstraint{l, c, false, SketchSubElement::StartPoint}),
              kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;

    const SketchLine& line = fx.lineOf(l);
    const SketchCircle& circle = fx.circleOf(c);
    const Vec2 atStart{line.start.x - circle.center.x, line.start.y - circle.center.y};
    EXPECT_NEAR(Distance(line.start, circle.center), circle.radiusMm, kTol);
    EXPECT_NEAR(Dot(Direction(line), atStart) / (Norm(Direction(line)) * Norm(atStart)), 0.0,
                kTol);
    // The far end is NOT on the circle any more: a tangent line touches once.
    EXPECT_GT(std::fabs(Distance(line.end, circle.center) - circle.radiusMm), 1.0);
}

TEST(SketchGeometricConstraintTest, M17_TAN_012_ACIRCLEHasNoEndToTouchAt) {
    // A curve pair CAN be pinned (M17.22) -- but only where the first of the
    // two has an end for the point to be. A circle is closed. Refused rather
    // than quietly solved as the centre-distance form, which would put the
    // sketch straight back in the rank-deficient case and say nothing about it.
    Fixture fx;
    const SketchEntityId a = fx.circle(Vec2{0, 0}, 10.0);
    const SketchEntityId b = fx.circle(Vec2{40, 0}, 6.0);
    const SketchConstraintId id =
        fx.add(TangentConstraint{a, b, false, SketchSubElement::StartPoint});

    const SketchSolveResult result = fx.solve();
    EXPECT_EQ(result.status, SketchSolveStatus::InvalidInput);
    ASSERT_FALSE(result.offendingConstraints.empty());
    EXPECT_EQ(result.offendingConstraints.front(), id);
    EXPECT_NE(result.message.find("not a circle"), std::string::npos) << result.message;
}

TEST(SketchGeometricConstraintTest, M17_TAN_012b_TheNamedEndMustBeOnTheFIRSTEntity) {
    // `at` names an end of `a`. Given the curve first and the line second there
    // is nothing to apply it to -- and applying it to the line anyway would
    // hold an end the caller never named, which solves and comes out wrong at
    // the other corner.
    Fixture fx;
    const SketchEntityId c = fx.circle(Vec2{0, 0}, 10.0);
    const SketchEntityId l = fx.line(Vec2{30, 20}, Vec2{7, 7});
    const SketchConstraintId id =
        fx.add(TangentConstraint{c, l, false, SketchSubElement::EndPoint});

    const SketchSolveResult result = fx.solve();
    EXPECT_EQ(result.status, SketchSolveStatus::InvalidInput);
    ASSERT_FALSE(result.offendingConstraints.empty());
    EXPECT_EQ(result.offendingConstraints.front(), id);
    EXPECT_NE(result.message.find("FIRST entity"), std::string::npos) << result.message;
}

TEST(SketchGeometricConstraintTest, M17_TAN_013_ACENTREIsNotAPlaceToTouch) {
    // Whole means "not known" and start/end name the two ends. CenterPoint is
    // neither, and a line has no centre to be tangent at anyway -- so it is a
    // refusal with a reason, not a fourth silent behaviour.
    Fixture fx;
    const SketchEntityId c = fx.circle(Vec2{0, 0}, 10.0);
    const SketchEntityId l = fx.line(Vec2{30, 20}, Vec2{7, 7});
    const SketchConstraintId id =
        fx.add(TangentConstraint{l, c, false, SketchSubElement::CenterPoint});

    const SketchSolveResult result = fx.solve();
    EXPECT_EQ(result.status, SketchSolveStatus::InvalidInput);
    ASSERT_FALSE(result.offendingConstraints.empty());
    EXPECT_EQ(result.offendingConstraints.front(), id);
    EXPECT_NE(result.message.find("start or end point"), std::string::npos) << result.message;
}

TEST(SketchGeometricConstraintTest, M17_TAN_014_AnUnpinnedTangencyStillMeansWhatItAlwaysDid) {
    // Whole is not a new behaviour: every tangency written before M17.21 means
    // a line free to slide, and this is the same sketch M13_TAN_001 solves.
    // Left explicit so a future change to the packing cannot quietly move the
    // old meaning along with the new one.
    Fixture fx;
    const SketchEntityId c = fx.circle(Vec2{0, 0}, 10.0);
    const SketchEntityId l = fx.line(Vec2{-40, 25}, Vec2{40, 25});
    ASSERT_NE(fx.add(TangentConstraint{l, c, false, SketchSubElement::Whole}),
              kInvalidSketchConstraintId);

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;
    EXPECT_NEAR(DistanceToLine(fx.circleOf(c).center, fx.lineOf(l)), fx.circleOf(c).radiusMm,
                kTol);
}

// =============================================================================
// Degenerate and self-referential input
// =============================================================================

TEST(SketchGeometricConstraintTest, M13_REJ_001_SelfReferenceIsRefusedForEveryPairKind) {
    // Each of these would otherwise contribute a residual that is identically
    // zero -- always satisfied, constraining nothing, and silently making the
    // DOF report one fewer freedom than the sketch really has. A constraint
    // that lies about the DOF is worse than one that is refused.
    enum class Which { Parallel, Perpendicular, Equal, Concentric, Tangent };
    for (const Which which : {Which::Parallel, Which::Perpendicular, Which::Equal,
                              Which::Concentric, Which::Tangent}) {
        Fixture fx;
        const SketchEntityId l = fx.line(Vec2{0, 0}, Vec2{100, 0});
        const SketchEntityId c = fx.circle(Vec2{0, 40}, 10.0);

        SketchConstraintData data{};
        switch (which) {
        case Which::Parallel: data = ParallelConstraint{l, l}; break;
        case Which::Perpendicular: data = PerpendicularConstraint{l, l}; break;
        case Which::Equal: data = EqualConstraint{l, l}; break;
        case Which::Concentric: data = ConcentricConstraint{c, c}; break;
        case Which::Tangent: data = TangentConstraint{c, c, false}; break;
        }
        const char* name = ConstraintKindName(data);

        const SketchConstraintId id = fx.add(data);
        ASSERT_NE(id, kInvalidSketchConstraintId) << name;
        const SketchSolveResult result = fx.solve();
        EXPECT_EQ(result.status, SketchSolveStatus::InvalidInput) << name;
        ASSERT_FALSE(result.offendingConstraints.empty()) << name;
        EXPECT_EQ(result.offendingConstraints.front(), id) << name;
    }
}

TEST(SketchGeometricConstraintTest, M13_REJ_002_AZeroLengthLineIsRefusedByTheDirectionKinds) {
    Fixture fx;
    const SketchEntityId good = fx.line(Vec2{0, 0}, Vec2{100, 0});
    // restoreEntity deliberately does NOT validate, so a hand-edited file can
    // carry this; the check has to live where the constraint is used.
    const SketchEntityId degenerate = NextSketchEntityId();
    ASSERT_TRUE(fx.sketch->restoreEntity(degenerate, SketchLine{Vec2{5, 5}, Vec2{5, 5}}));

    const SketchConstraintId id = fx.add(ParallelConstraint{good, degenerate});
    ASSERT_NE(id, kInvalidSketchConstraintId);
    const SketchSolveResult result = fx.solve();
    EXPECT_EQ(result.status, SketchSolveStatus::InvalidInput);
    ASSERT_FALSE(result.offendingConstraints.empty());
    EXPECT_EQ(result.offendingConstraints.front(), id);
}

// =============================================================================
// Working together with what M5 already had
// =============================================================================

TEST(SketchGeometricConstraintTest, M13_MIX_001_AParallelogramReachesFullyConstrained) {
    Fixture fx;
    // Four sloppy sides. Parallel + Equal + one Angle-free anchor is a
    // parallelogram: the shape the seven constraints exist to make expressible
    // without naming a single dimension twice.
    const SketchEntityId bottom = fx.line(Vec2{0, 0}, Vec2{95, 6});
    const SketchEntityId right = fx.line(Vec2{95, 6}, Vec2{110, 48});
    const SketchEntityId top = fx.line(Vec2{110, 48}, Vec2{12, 44});
    const SketchEntityId left = fx.line(Vec2{12, 44}, Vec2{0, 0});

    fx.add(CoincidentConstraint{{bottom, SketchSubElement::EndPoint},
                                {right, SketchSubElement::StartPoint}});
    fx.add(CoincidentConstraint{{right, SketchSubElement::EndPoint},
                                {top, SketchSubElement::StartPoint}});
    fx.add(CoincidentConstraint{{top, SketchSubElement::EndPoint},
                                {left, SketchSubElement::StartPoint}});
    fx.add(CoincidentConstraint{{left, SketchSubElement::EndPoint},
                                {bottom, SketchSubElement::StartPoint}});
    fx.add(HorizontalConstraint{bottom});
    fx.add(ParallelConstraint{bottom, top});
    fx.add(ParallelConstraint{right, left});
    fx.add(EqualConstraint{bottom, top});
    fx.add(EqualConstraint{right, left});
    fx.add(FixConstraint{{bottom, SketchSubElement::StartPoint}});

    const SketchSolveResult result = fx.solve();
    ASSERT_TRUE(result) << result.message;

    const SketchLine& b = fx.lineOf(bottom);
    const SketchLine& t = fx.lineOf(top);
    const SketchLine& r = fx.lineOf(right);
    const SketchLine& l = fx.lineOf(left);
    EXPECT_NEAR(b.start.y, b.end.y, kTol);                       // horizontal
    EXPECT_NEAR(Norm(Direction(b)), Norm(Direction(t)), kTol);   // equal
    EXPECT_NEAR(Norm(Direction(r)), Norm(Direction(l)), kTol);
    EXPECT_NEAR(Cross(Direction(b), Direction(t)) / (Norm(Direction(b)) * Norm(Direction(t))),
                0.0, kTol);
    EXPECT_NEAR(Cross(Direction(r), Direction(l)) / (Norm(Direction(r)) * Norm(Direction(l))),
                0.0, kTol);
    // Still free to change size and skew -- a parallelogram named by relations
    // alone is deliberately not fully constrained.
    EXPECT_EQ(result.status, SketchSolveStatus::UnderConstrained);
    EXPECT_GT(result.degreesOfFreedom, 0);
}

// --- M18: A SPLINE'S END IS TANGENT TO ITS NEIGHBOUR -------------------------
//
// The whole family rests on one claim: for the uniform Catmull-Rom with
// REFLECTED ends that Core evaluates, the direction at the first point is
// exactly p1 - p0. M18_TAN_001 checks the claim itself; the rest check that the
// constraints built on it hold.

TEST(SketchGeometricConstraintTest, M18_TAN_001_TheCHORDIsTheEndTangentExactly) {
    // Not an approximation, and worth pinning: every spline tangency below
    // constrains the CHORD, so if the curve did not actually leave along it,
    // all of them would be holding the wrong thing while converging happily.
    //
    // Measured by sampling, which is what the canvas draws and what the profile
    // hands the kernel -- so this is the direction the user can see.
    const SketchSpline spline{{Vec2{10, 5}, Vec2{40, 25}, Vec2{75, 0}, Vec2{110, 30}}, false};
    const Vec2 chordStart{spline.points[1].x - spline.points[0].x,
                          spline.points[1].y - spline.points[0].y};
    const Vec2 chordEnd{spline.points[3].x - spline.points[2].x,
                        spline.points[3].y - spline.points[2].y};

    const double h = 1e-6;
    const int spans = static_cast<int>(spline.points.size()) - 1;
    const Vec2 atStart = PointOnSpline(spline, 0.0);
    const Vec2 justAfter = PointOnSpline(spline, h / spans);
    const Vec2 atEnd = PointOnSpline(spline, 1.0);
    const Vec2 justBefore = PointOnSpline(spline, 1.0 - h / spans);

    const auto sameDirection = [](Vec2 a, Vec2 b) {
        const double la = std::hypot(a.x, a.y);
        const double lb = std::hypot(b.x, b.y);
        return (a.x * b.y - a.y * b.x) / (la * lb);
    };
    EXPECT_NEAR(sameDirection(Vec2{justAfter.x - atStart.x, justAfter.y - atStart.y}, chordStart),
                0.0, 1e-6);
    EXPECT_NEAR(sameDirection(Vec2{atEnd.x - justBefore.x, atEnd.y - justBefore.y}, chordEnd),
                0.0, 1e-6);
}

TEST(SketchGeometricConstraintTest, M18_TAN_002_ASplinesEndLeavesAlongTheLineItMeets) {
    // The gap M18 closed: tangency refused a spline outright, so a spline could
    // never join anything without a visible kink -- which is what kept it out
    // of every profile.
    PartDocument document{"TanDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline =
        sketch.addSpline({Vec2{100, 40}, Vec2{70, 33}, Vec2{40, 10}}, false);
    const SketchEntityId line = sketch.addLine(Vec2{100, 40}, Vec2{160, 40});

    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  CoincidentConstraint{SketchElementRef{spline, SketchSubElement::StartPoint},
                                       SketchElementRef{line, SketchSubElement::StartPoint}}),
              kInvalidSketchConstraintId);
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  TangentConstraint{spline, line, false, SketchSubElement::StartPoint}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const auto& solved = std::get<SketchSpline>(sketch.findEntity(spline)->geometry);
    const auto& solvedLine = std::get<SketchLine>(sketch.findEntity(line)->geometry);
    const Vec2 chord{solved.points[1].x - solved.points[0].x,
                     solved.points[1].y - solved.points[0].y};
    const Vec2 along{solvedLine.end.x - solvedLine.start.x, solvedLine.end.y - solvedLine.start.y};
    const double cross = chord.x * along.y - chord.y * along.x;
    EXPECT_NEAR(cross / (std::hypot(chord.x, chord.y) * std::hypot(along.x, along.y)), 0.0, 1e-6)
        << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_TAN_003_ASplinesEndLeavesAtARightAngleToTheRadius) {
    // Against a circle the equation is the perpendicularity one, not a distance
    // -- the same lesson as ADR-M17-044. The touch point is already pinned by
    // the coincidence, so a distance form would sit at its minimum and hold
    // nothing.
    PartDocument document{"TanDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId circle = sketch.addCircle(Vec2{0, 0}, 50.0);
    const SketchEntityId spline =
        sketch.addSpline({Vec2{50, 0}, Vec2{70, 18}, Vec2{100, 5}}, false);

    // ON the circle, not coincident WITH it: a circle's Whole is not a point.
    // This is what pins the touch point, which is what makes the perpendicular
    // form the right equation.
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  PointOnObjectConstraint{SketchElementRef{spline, SketchSubElement::StartPoint},
                                          circle}),
              kInvalidSketchConstraintId);
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  TangentConstraint{spline, circle, false, SketchSubElement::StartPoint}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const auto& solved = std::get<SketchSpline>(sketch.findEntity(spline)->geometry);
    const auto& solvedCircle = std::get<SketchCircle>(sketch.findEntity(circle)->geometry);
    const Vec2 chord{solved.points[1].x - solved.points[0].x,
                     solved.points[1].y - solved.points[0].y};
    const Vec2 radius{solved.points[0].x - solvedCircle.center.x,
                      solved.points[0].y - solvedCircle.center.y};
    const double dot = chord.x * radius.x + chord.y * radius.y;
    EXPECT_NEAR(dot / (std::hypot(chord.x, chord.y) * std::hypot(radius.x, radius.y)), 0.0, 1e-6)
        << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_TAN_004_TwoSplinesCanMeetSmoothly) {
    // Two splines joined end to end. The near end of the second is chosen from
    // where the two currently are -- there is no other way to say it, since
    // `at` names an end of the FIRST -- so the pick is made once, here.
    PartDocument document{"TanDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId a = sketch.addSpline({Vec2{10, 10}, Vec2{40, 25}, Vec2{70, 30}}, false);
    const SketchEntityId b = sketch.addSpline({Vec2{70, 30}, Vec2{95, 5}, Vec2{130, 0}}, false);

    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  CoincidentConstraint{SketchElementRef{a, SketchSubElement::EndPoint},
                                       SketchElementRef{b, SketchSubElement::StartPoint}}),
              kInvalidSketchConstraintId);
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(), TangentConstraint{a, b, false, SketchSubElement::EndPoint}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const auto& sa = std::get<SketchSpline>(sketch.findEntity(a)->geometry);
    const auto& sb = std::get<SketchSpline>(sketch.findEntity(b)->geometry);
    const Vec2 inbound{sa.points[2].x - sa.points[1].x, sa.points[2].y - sa.points[1].y};
    const Vec2 outbound{sb.points[1].x - sb.points[0].x, sb.points[1].y - sb.points[0].y};
    const double cross = inbound.x * outbound.y - inbound.y * outbound.x;
    EXPECT_NEAR(cross / (std::hypot(inbound.x, inbound.y) * std::hypot(outbound.x, outbound.y)),
                0.0, 1e-6)
        << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_TAN_005_ATangencyThatDoesNotSayWHICHEndIsREFUSED) {
    // A spline has a different tangent at every point along it, so "this spline
    // is tangent to that line" does not name an equation. Guessing an end would
    // hold a point the caller never chose.
    PartDocument document{"TanDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline =
        sketch.addSpline({Vec2{10, 10}, Vec2{40, 25}, Vec2{70, 30}}, false);
    const SketchEntityId line = sketch.addLine(Vec2{0, 60}, Vec2{80, 60});

    document.addSketchConstraint(sketch.id(),
                                 TangentConstraint{spline, line, false, SketchSubElement::Whole});

    EXPECT_FALSE(document.recompute().success);
    EXPECT_NE(sketch.solveMessage().find("which end"), std::string::npos) << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_TAN_006_ACLOSEDSplineHasNoEndToBeTangentAt) {
    // The same reason a circle is refused by TangentCurvesAtPoint: no ends.
    PartDocument document{"TanDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline =
        sketch.addSpline({Vec2{10, 10}, Vec2{40, 25}, Vec2{70, 30}, Vec2{40, -5}}, true);
    const SketchEntityId line = sketch.addLine(Vec2{0, 60}, Vec2{80, 60});

    document.addSketchConstraint(
        sketch.id(), TangentConstraint{spline, line, false, SketchSubElement::StartPoint});

    EXPECT_FALSE(document.recompute().success);
    EXPECT_NE(sketch.solveMessage().find("closed spline"), std::string::npos)
        << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_TAN_007_TheSplineMustComeFIRST) {
    // `at` names an end of `a` everywhere else in this constraint, and a
    // tangency whose end belonged to the OTHER entity would silently apply the
    // named end to the line -- holding a point nobody chose. Refused instead.
    PartDocument document{"TanDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline =
        sketch.addSpline({Vec2{10, 10}, Vec2{40, 25}, Vec2{70, 30}}, false);
    const SketchEntityId line = sketch.addLine(Vec2{0, 60}, Vec2{80, 60});

    document.addSketchConstraint(
        sketch.id(), TangentConstraint{line, spline, false, SketchSubElement::StartPoint});

    EXPECT_FALSE(document.recompute().success);
    EXPECT_NE(sketch.solveMessage().find("FIRST"), std::string::npos) << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_TAN_008_ASplineOfTwoPointsIsALineAndStillHasADirection) {
    // The smallest spline that has a direction at all. One point fewer and the
    // chord does not exist -- that is the branch that must refuse rather than
    // read past the end of the point list.
    PartDocument document{"TanDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline = sketch.addSpline({Vec2{100, 40}, Vec2{60, 20}}, false);
    const SketchEntityId line = sketch.addLine(Vec2{100, 40}, Vec2{160, 40});

    document.addSketchConstraint(
        sketch.id(),
        CoincidentConstraint{SketchElementRef{spline, SketchSubElement::StartPoint},
                             SketchElementRef{line, SketchSubElement::StartPoint}});
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  TangentConstraint{spline, line, false, SketchSubElement::StartPoint}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const auto& solved = std::get<SketchSpline>(sketch.findEntity(spline)->geometry);
    const auto& solvedLine = std::get<SketchLine>(sketch.findEntity(line)->geometry);
    const Vec2 chord{solved.points[1].x - solved.points[0].x,
                     solved.points[1].y - solved.points[0].y};
    const Vec2 along{solvedLine.end.x - solvedLine.start.x, solvedLine.end.y - solvedLine.start.y};
    const double cross = chord.x * along.y - chord.y * along.x;
    EXPECT_NEAR(cross / (std::hypot(chord.x, chord.y) * std::hypot(along.x, along.y)), 0.0, 1e-6)
        << sketch.solveMessage();
}

// --- M18: A LINE TANGENT TO AN ELLIPSE ---------------------------------------

namespace {

// The distance from an ellipse's centre to the line, and the distance from the
// centre to the tangent line with that same normal. Tangency is the two being
// equal, and BOTH are computed here from the raw formula rather than from the
// solver's own residual -- deriving the expectation from the thing under test
// would check nothing (ADR-M4-003).
void ExpectTangentToEllipse(const SketchLine& line, const SketchEllipse& oval,
                            const std::string& why) {
    const double du = line.end.x - line.start.x;
    const double dv = line.end.y - line.start.y;
    const double length = std::hypot(du, dv);
    ASSERT_GT(length, 1e-9) << why;
    const double nu = -dv / length;
    const double nv = du / length;
    const double h = (oval.center.x - line.start.x) * nu + (oval.center.y - line.start.y) * nv;

    const double cosR = std::cos(oval.rotationRad);
    const double sinR = std::sin(oval.rotationRad);
    const double nMajor = nu * cosR + nv * sinR;
    const double nMinor = -nu * sinR + nv * cosR;
    const double reach = std::sqrt(oval.majorRadiusMm * oval.majorRadiusMm * nMajor * nMajor +
                                   oval.minorRadiusMm * oval.minorRadiusMm * nMinor * nMinor);

    EXPECT_NEAR(std::fabs(h), reach, 1e-6) << why;
}

} // namespace

TEST(SketchGeometricConstraintTest, M18_ELL_001_ALineIsPulledOntoAnEllipse) {
    // Starts CUTTING the ellipse, so a residual that measured nothing would
    // leave it cutting. Rotated deliberately: on an axis-aligned ellipse the
    // rotation terms drop out and a residual that ignored them would pass.
    PartDocument document{"EllDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId oval = sketch.addEllipse(Vec2{0, 0}, 60.0, 25.0, 0.6);
    const SketchEntityId line = sketch.addLine(Vec2{-80, 20}, Vec2{80, 20});

    ASSERT_NE(document.addSketchConstraint(sketch.id(), TangentConstraint{line, oval}),
              kInvalidSketchConstraintId);
    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();

    ExpectTangentToEllipse(std::get<SketchLine>(sketch.findEntity(line)->geometry),
                           std::get<SketchEllipse>(sketch.findEntity(oval)->geometry),
                           sketch.solveMessage());
}

TEST(SketchGeometricConstraintTest, M18_ELL_002_TheELLIPSEMayBeTheOneThatMoves) {
    // Same equation, the other way round: pin the line and let the ellipse
    // shrink onto it. A packing that had the two entities' roles crossed would
    // still converge here, so the line is FIXED and the assertion is about
    // where the ellipse ended up.
    PartDocument document{"EllDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId oval = sketch.addEllipse(Vec2{0, 0}, 60.0, 40.0, 0.0);
    const SketchEntityId line = sketch.addLine(Vec2{-80, 30}, Vec2{80, 30});

    document.addSketchConstraint(sketch.id(),
                                 FixConstraint{SketchElementRef{line,
                                                                SketchSubElement::StartPoint}});
    document.addSketchConstraint(sketch.id(),
                                 FixConstraint{SketchElementRef{line,
                                                                SketchSubElement::EndPoint}});
    ASSERT_NE(document.addSketchConstraint(sketch.id(), TangentConstraint{oval, line}),
              kInvalidSketchConstraintId);
    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();

    const auto& solvedLine = std::get<SketchLine>(sketch.findEntity(line)->geometry);
    EXPECT_NEAR(solvedLine.start.y, 30.0, 1e-9) << "the line was fixed and should not have moved";
    ExpectTangentToEllipse(solvedLine,
                           std::get<SketchEllipse>(sketch.findEntity(oval)->geometry),
                           sketch.solveMessage());
}

TEST(SketchGeometricConstraintTest, M18_ELL_003_AnEllipticalARCIsTangentTheSameWay) {
    // A piece of an ellipse has the same axes, so the same equation holds -- and
    // the slots come from the same fields, which is what this checks.
    PartDocument document{"EllDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId piece =
        sketch.addEllipticalArc(Vec2{0, 0}, 50.0, 20.0, 0.3, 0.0, 3.0, true);
    const SketchEntityId line = sketch.addLine(Vec2{-70, 10}, Vec2{70, 10});

    ASSERT_NE(document.addSketchConstraint(sketch.id(), TangentConstraint{line, piece}),
              kInvalidSketchConstraintId);
    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();

    const auto& arc = std::get<SketchEllipticalArc>(sketch.findEntity(piece)->geometry);
    const SketchEllipse asFull{arc.center, arc.majorRadiusMm, arc.minorRadiusMm, arc.rotationRad};
    ExpectTangentToEllipse(std::get<SketchLine>(sketch.findEntity(line)->geometry), asFull,
                           sketch.solveMessage());
}

TEST(SketchGeometricConstraintTest, M18_ELL_004_ACircleAndAnEllipseAreREFUSED) {
    // The honest answer, and it names what is missing. An approximation here
    // would be a tangency that converges and does not touch.
    PartDocument document{"EllDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId oval = sketch.addEllipse(Vec2{0, 0}, 60.0, 25.0, 0.0);
    const SketchEntityId circle = sketch.addCircle(Vec2{120, 0}, 30.0);

    document.addSketchConstraint(sketch.id(), TangentConstraint{oval, circle});

    EXPECT_FALSE(document.recompute().success);
    EXPECT_NE(sketch.solveMessage().find("touch point"), std::string::npos)
        << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_ELL_005_TangencyToAnEllipseCannotBePinnedAtAnEnd) {
    // The closed form has no contact point in it, so naming an end would
    // promise something the equation cannot hold. Refused rather than silently
    // ignoring the `at` the caller gave.
    PartDocument document{"EllDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId oval = sketch.addEllipse(Vec2{0, 0}, 60.0, 25.0, 0.0);
    const SketchEntityId line = sketch.addLine(Vec2{-80, 20}, Vec2{80, 20});

    document.addSketchConstraint(
        sketch.id(), TangentConstraint{line, oval, false, SketchSubElement::StartPoint});

    EXPECT_FALSE(document.recompute().success);
    EXPECT_NE(sketch.solveMessage().find("pinned at an end"), std::string::npos)
        << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_ELL_006_TheROTATIONIsPartOfTheEquation) {
    // Turning the ellipse under a FIXED line has to move the line's tangency
    // out of true -- which is only so if the residual reads the rotation. A
    // version that ignored it would report Solved at both angles.
    //
    // Built as two separate documents rather than one re-solved, so neither
    // answer can be the other's starting point.
    const auto tangentDistanceFor = [](double rotation) {
        PartDocument document{"EllDoc"};
        GaussNewtonSketchSolver solver;
        document.setSketchSolver(&solver);
        Sketch& sketch = document.addSketch("Sketch001");
        const SketchEntityId oval = sketch.addEllipse(Vec2{0, 0}, 60.0, 20.0, rotation);
        const SketchEntityId line = sketch.addLine(Vec2{-80, 15}, Vec2{80, 15});
        // The ellipse is held completely, so only the line can move.
        Parameter& major = document.addParameter("A" , 60.0, UnitType::Millimeter);
        Parameter& minor = document.addParameter("B", 20.0, UnitType::Millimeter);
        Parameter& turn = document.addParameter("R", rotation, UnitType::Radian);
        // The field is `minor`, so false drives the MAJOR axis. Getting these
        // the wrong way round makes the major radius smaller than the minor
        // one, which the sketch's own invariant rejects at commit time.
        document.addSketchConstraint(sketch.id(), EllipseAxisConstraint{oval, major.id(), false});
        document.addSketchConstraint(sketch.id(), EllipseAxisConstraint{oval, minor.id(), true});
        document.addSketchConstraint(sketch.id(), EllipseRotationConstraint{oval, turn.id()});
        document.addSketchConstraint(
            sketch.id(), FixConstraint{SketchElementRef{oval, SketchSubElement::CenterPoint}});
        document.addSketchConstraint(sketch.id(), HorizontalConstraint{line});
        // A LENGTH, or nothing stops the line collapsing to a point -- and a
        // line with no direction has no normal, so the tangency would be
        // measuring nothing.
        Parameter& span = document.addParameter("L", 160.0, UnitType::Millimeter);
        document.addSketchConstraint(sketch.id(), LengthConstraint{line, span.id()});
        document.addSketchConstraint(sketch.id(), TangentConstraint{line, oval});
        EXPECT_TRUE(document.recompute().success) << sketch.solveMessage();
        return std::get<SketchLine>(sketch.findEntity(line)->geometry).start.y;
    };

    const double flat = tangentDistanceFor(0.0);
    const double turned = tangentDistanceFor(1.0);
    // Flat, the horizontal tangent sits at the minor radius, 20. Turned by one
    // radian it has to sit further out -- the exact value is the formula the
    // helper above checks; what matters here is that it MOVED.
    EXPECT_NEAR(flat, 20.0, 1e-6);
    EXPECT_GT(turned, 25.0);
}

TEST(SketchGeometricConstraintTest, M18_TAN_009_TheENDUsesITSOwnNeighbourNotTheStarts) {
    // The end branch, exercised where it CANNOT be confused with the start.
    //
    // Every other test here uses a three-point spline, and on three points the
    // start's neighbour (index 1) and the end's neighbour (index count - 2) are
    // the same point -- so a version that always took index 1 passed all of
    // them. Four points is the smallest spline on which the two differ, and
    // that is the whole reason this test exists.
    PartDocument document{"TanDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const std::vector<Vec2> drawn{Vec2{0, 0}, Vec2{30, 40}, Vec2{70, 10}, Vec2{110, 35}};
    const SketchEntityId spline = sketch.addSpline(drawn, false);
    const SketchEntityId line = sketch.addLine(Vec2{110, 35}, Vec2{170, 35});

    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  CoincidentConstraint{SketchElementRef{spline, SketchSubElement::EndPoint},
                                       SketchElementRef{line, SketchSubElement::StartPoint}}),
              kInvalidSketchConstraintId);
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  TangentConstraint{spline, line, false, SketchSubElement::EndPoint}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const auto& solved = std::get<SketchSpline>(sketch.findEntity(spline)->geometry);
    const auto& solvedLine = std::get<SketchLine>(sketch.findEntity(line)->geometry);
    const Vec2 along{solvedLine.end.x - solvedLine.start.x, solvedLine.end.y - solvedLine.start.y};

    const auto sineWith = [&](Vec2 chord) {
        return (chord.x * along.y - chord.y * along.x) /
               (std::hypot(chord.x, chord.y) * std::hypot(along.x, along.y));
    };
    const Vec2 endChord{solved.points[3].x - solved.points[2].x,
                        solved.points[3].y - solved.points[2].y};
    const Vec2 startChord{solved.points[1].x - solved.points[0].x,
                          solved.points[1].y - solved.points[0].y};
    EXPECT_NEAR(sineWith(endChord), 0.0, 1e-6) << sketch.solveMessage();
    // And the START is NOT parallel to it, which is what makes the assertion
    // above about the end rather than about splines in general.
    EXPECT_GT(std::fabs(sineWith(startChord)), 0.1) << sketch.solveMessage();
}

// --- M18: SPLINE TANGENT HANDLES ---------------------------------------------

TEST(SketchGeometricConstraintTest, M18_HAN_001_HermiteIsCatmullRomWhenNobodySetAHandle) {
    // The substitution the whole change rests on. The evaluator was rewritten
    // from a Catmull-Rom span to a Hermite one so that a single point could
    // carry its own tangent -- and Catmull-Rom IS Hermite with
    // m_i = (p[i+1] - p[i-1])/2, so a spline with no handles must be the SAME
    // CURVE it was before, not merely a similar one.
    //
    // Checked against the Catmull-Rom basis written out here by hand, not
    // against the old code, which is gone.
    const SketchSpline spline{{Vec2{10, 5}, Vec2{40, 25}, Vec2{75, 0}, Vec2{110, 30}}, false};
    const auto catmullRom = [](double a, double b, double c, double d, double u) {
        const double u2 = u * u;
        const double u3 = u2 * u;
        return 0.5 * ((2.0 * b) + (-a + c) * u + (2.0 * a - 5.0 * b + 4.0 * c - d) * u2 +
                      (-a + 3.0 * b - 3.0 * c + d) * u3);
    };
    // Span 1, between points 1 and 2, whose neighbours are both real points --
    // so this compares the basis alone, with no end reflection involved.
    for (int step = 0; step <= 10; ++step) {
        const double u = step / 10.0;
        const Vec2 got = PointOnSpline(spline, (1.0 + u) / 3.0);
        const double x = catmullRom(spline.points[0].x, spline.points[1].x, spline.points[2].x,
                                    spline.points[3].x, u);
        const double y = catmullRom(spline.points[0].y, spline.points[1].y, spline.points[2].y,
                                    spline.points[3].y, u);
        EXPECT_NEAR(got.x, x, 1e-9) << "u = " << u;
        EXPECT_NEAR(got.y, y, 1e-9) << "u = " << u;
    }
}

TEST(SketchGeometricConstraintTest, M18_HAN_002_AHandleCHANGESTheCurveItLeavesAlong) {
    // What a handle is for. Without this the whole feature could be inert and
    // every other test here would still pass.
    SketchSpline spline{{Vec2{0, 0}, Vec2{50, 50}, Vec2{100, 0}}, false};
    const Vec2 before = PointOnSpline(spline, 0.25);
    spline.handles[0] = Vec2{0, 60}; // leave straight UP instead of along the chord
    const Vec2 after = PointOnSpline(spline, 0.25);

    EXPECT_GT(std::hypot(after.x - before.x, after.y - before.y), 1.0);
    // ...and it really does leave upwards now.
    const Vec2 justAfterStart = PointOnSpline(spline, 1e-6);
    EXPECT_GT(justAfterStart.y - 0.0, std::fabs(justAfterStart.x - 0.0));
}

TEST(SketchGeometricConstraintTest, M18_HAN_003_TheHandlesLENGTHMattersNotJustItsDirection) {
    // A handle is better than another point precisely because it says how HARD
    // the curve is pulled before it turns. A version that normalised the vector
    // would draw the same curve for both of these.
    SketchSpline gentle{{Vec2{0, 0}, Vec2{50, 50}, Vec2{100, 0}}, false};
    gentle.handles[0] = Vec2{20, 0};
    SketchSpline firm = gentle;
    firm.handles[0] = Vec2{90, 0};

    const Vec2 a = PointOnSpline(gentle, 0.25);
    const Vec2 b = PointOnSpline(firm, 0.25);
    EXPECT_GT(std::hypot(b.x - a.x, b.y - a.y), 1.0);
    // ALONG the handle, which here is +u. Both handles point the same way, so
    // the difference between them is entirely in how FAR the curve is carried
    // before it turns -- and asserting anything about v would be asserting a
    // number the basis makes identical either way.
    EXPECT_GT(b.x, a.x + 5.0);
}

TEST(SketchGeometricConstraintTest, M18_HAN_004_AHandleCostsTWODegreesOfFreedom) {
    // Four variables -- the tangent and the tip -- less the two equations that
    // tie the tip to its point. A handle is two new numbers, and the readout
    // has to say so.
    //
    // This also holds the no-constraint DOF shortcut to what the solver would
    // measure: that path counts variables and subtracts the residuals the
    // builder emits anyway, on the strength of those ties being independent.
    // Here a real constraint forces the solver's own rank computation, and the
    // two answers have to agree.
    PartDocument document{"HandleDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    SketchSpline geometry{{Vec2{10, 10}, Vec2{50, 50}, Vec2{100, 10}}, false};
    geometry.handles[1] = Vec2{30, 0};
    const SketchEntityId spline = sketch.addSpline(geometry.points, geometry.closed);
    ASSERT_TRUE(document.setSketchEntityGeometry(sketch.id(), spline, geometry));

    // One Fix, so the solver runs rather than the count-only path.
    document.addSketchConstraint(
        sketch.id(), FixConstraint{SketchElementRef{spline, SketchSubElement::StartPoint}});

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    // Six for the points, two for the handle, less the two the Fix takes.
    EXPECT_EQ(sketch.degreesOfFreedom(), 6) << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_HAN_005_AnUnconstrainedHandleIsFreeAndMOVES) {
    // WHAT THE TIE DOES AND WHAT IT DOES NOT.
    //
    // It makes the tip mean p + m, so a constraint on the tip is a constraint
    // on the tangent. It does NOT nail the tangent to its point: a handle is
    // two degrees of freedom like everything else here, so when a dimension
    // moves the point, the solver may satisfy the tie by moving the tip, by
    // changing the tangent, or -- taking a least-norm step -- by doing half of
    // each. It does half of each, and this test says so rather than wishing
    // otherwise.
    //
    // A tangent that must survive its point moving is one the user constrains.
    // M18_HAN_005b is that, and it is the answer to anyone surprised by this.
    PartDocument document{"HandleDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    SketchSpline geometry{{Vec2{10, 10}, Vec2{50, 50}, Vec2{100, 10}}, false};
    geometry.handles[1] = Vec2{30, 0};
    const SketchEntityId spline = sketch.addSpline(geometry.points, geometry.closed);
    ASSERT_TRUE(document.setSketchEntityGeometry(sketch.id(), spline, geometry));

    Parameter& lift = document.addParameter("Y", 80.0, UnitType::Millimeter);
    document.addSketchConstraint(
        sketch.id(), FixConstraint{SketchElementRef{spline, SketchSubElement::StartPoint}});
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  VerticalDistanceConstraint{
                      SketchElementRef{spline, SketchSubElement::StartPoint},
                      SketchElementRef{spline, SketchSubElement::SplinePoint, 1}, lift.id()}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const auto& solved = std::get<SketchSpline>(sketch.findEntity(spline)->geometry);
    EXPECT_NEAR(solved.points[1].y - solved.points[0].y, 80.0, 1e-6) << sketch.solveMessage();
    ASSERT_NE(solved.handles.find(1), solved.handles.end());
    // The point rose 40 mm. Half of that went into the tip and half into the
    // tangent, which is the least-norm split -- so the tangent tilted by 20 and
    // NOT by 40, and not by nothing.
    EXPECT_NEAR(solved.handles.at(1).x, 30.0, 1e-6) << sketch.solveMessage();
    EXPECT_NEAR(std::fabs(solved.handles.at(1).y), 20.0, 1e-3) << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_HAN_005b_AConstrainedHandleIsHELDWhileItsPointMoves) {
    // The other half of M18_HAN_005, and the reason the tip is a point at all.
    //
    // Saying "this tangent is horizontal" is an ordinary vertical distance of
    // zero between the point and its handle's tip -- no residual was written
    // for the purpose. With it in place the point can be dimensioned anywhere
    // and the tangent stays where the user put it.
    PartDocument document{"HandleDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    SketchSpline geometry{{Vec2{10, 10}, Vec2{50, 50}, Vec2{100, 10}}, false};
    geometry.handles[1] = Vec2{30, 0};
    const SketchEntityId spline = sketch.addSpline(geometry.points, geometry.closed);
    ASSERT_TRUE(document.setSketchEntityGeometry(sketch.id(), spline, geometry));

    Parameter& lift = document.addParameter("Y", 80.0, UnitType::Millimeter);
    Parameter& level = document.addParameter("Flat", 0.0, UnitType::Millimeter);
    document.addSketchConstraint(
        sketch.id(), FixConstraint{SketchElementRef{spline, SketchSubElement::StartPoint}});
    document.addSketchConstraint(
        sketch.id(),
        VerticalDistanceConstraint{SketchElementRef{spline, SketchSubElement::SplinePoint, 1},
                                   SketchElementRef{spline, SketchSubElement::SplineHandle, 1},
                                   level.id()});
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  VerticalDistanceConstraint{
                      SketchElementRef{spline, SketchSubElement::StartPoint},
                      SketchElementRef{spline, SketchSubElement::SplinePoint, 1}, lift.id()}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const auto& solved = std::get<SketchSpline>(sketch.findEntity(spline)->geometry);
    EXPECT_NEAR(solved.points[1].y - solved.points[0].y, 80.0, 1e-6) << sketch.solveMessage();
    ASSERT_NE(solved.handles.find(1), solved.handles.end());
    EXPECT_NEAR(solved.handles.at(1).y, 0.0, 1e-6) << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_HAN_006_AHandleCanBeCONSTRAINEDLikeAnyPoint) {
    // Why the tip is a variable at all. It is a point, so a Horizontal between
    // it and its own point makes the tangent horizontal -- with no residual
    // written for the purpose, exactly as an arc's tips buy every point
    // constraint for an arc.
    PartDocument document{"HandleDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    SketchSpline geometry{{Vec2{10, 10}, Vec2{50, 50}, Vec2{100, 10}}, false};
    geometry.handles[1] = Vec2{30, 25}; // deliberately NOT horizontal
    const SketchEntityId spline = sketch.addSpline(geometry.points, geometry.closed);
    ASSERT_TRUE(document.setSketchEntityGeometry(sketch.id(), spline, geometry));

    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  VerticalDistanceConstraint{
                      SketchElementRef{spline, SketchSubElement::SplinePoint, 1},
                      SketchElementRef{spline, SketchSubElement::SplineHandle, 1},
                      document.addParameter("Zero", 0.0, UnitType::Millimeter).id()}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const auto& solved = std::get<SketchSpline>(sketch.findEntity(spline)->geometry);
    ASSERT_NE(solved.handles.find(1), solved.handles.end());
    EXPECT_NEAR(solved.handles.at(1).y, 0.0, 1e-6) << sketch.solveMessage();
    // ...and it did not simply collapse: the handle still points somewhere.
    EXPECT_GT(std::fabs(solved.handles.at(1).x), 1.0) << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_HAN_007_AnEndsTangencyUsesTheHANDLENotTheChord) {
    // ADR-M18-001 said the end tangent is the chord; a handle overrides that,
    // and the tangency constraint has to follow the same rule the evaluator
    // does. Reading the chord here while the curve left along a handle would be
    // a tangency that converges on a direction the shape does not have.
    PartDocument document{"HandleDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    SketchSpline geometry{{Vec2{100, 40}, Vec2{70, 33}, Vec2{40, 10}}, false};
    geometry.handles[0] = Vec2{-20, -35}; // nothing like the chord
    const SketchEntityId spline = sketch.addSpline(geometry.points, geometry.closed);
    ASSERT_TRUE(document.setSketchEntityGeometry(sketch.id(), spline, geometry));
    const SketchEntityId line = sketch.addLine(Vec2{100, 40}, Vec2{160, 40});

    document.addSketchConstraint(
        sketch.id(),
        CoincidentConstraint{SketchElementRef{spline, SketchSubElement::StartPoint},
                             SketchElementRef{line, SketchSubElement::StartPoint}});
    ASSERT_NE(document.addSketchConstraint(
                  sketch.id(),
                  TangentConstraint{spline, line, false, SketchSubElement::StartPoint}),
              kInvalidSketchConstraintId);

    ASSERT_TRUE(document.recompute().success) << sketch.solveMessage();
    const auto& solved = std::get<SketchSpline>(sketch.findEntity(spline)->geometry);
    const auto& solvedLine = std::get<SketchLine>(sketch.findEntity(line)->geometry);
    const Vec2 along{solvedLine.end.x - solvedLine.start.x, solvedLine.end.y - solvedLine.start.y};
    const auto sineWith = [&](Vec2 direction) {
        return (direction.x * along.y - direction.y * along.x) /
               (std::hypot(direction.x, direction.y) * std::hypot(along.x, along.y));
    };
    ASSERT_NE(solved.handles.find(0), solved.handles.end());
    // THE HANDLE is what became parallel to the line...
    EXPECT_NEAR(sineWith(solved.handles.at(0)), 0.0, 1e-6) << sketch.solveMessage();
    // ...and the chord did NOT, which is what makes that assertion about the
    // handle rather than about the spline in general.
    const Vec2 chord{solved.points[1].x - solved.points[0].x,
                     solved.points[1].y - solved.points[0].y};
    EXPECT_GT(std::fabs(sineWith(chord)), 0.05) << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_HAN_008_AHandleOnAPointThatHasNoneIsREFUSED) {
    // Answering with the point itself would be a constraint on a place the
    // caller never named -- and one that looks satisfied from the start,
    // because a zero-length tangent is parallel to everything.
    PartDocument document{"HandleDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId spline =
        sketch.addSpline({Vec2{10, 10}, Vec2{50, 50}, Vec2{100, 10}}, false);
    const SketchEntityId point = sketch.addPoint(Vec2{5, 5});

    document.addSketchConstraint(
        sketch.id(),
        CoincidentConstraint{SketchElementRef{point, SketchSubElement::Whole},
                             SketchElementRef{spline, SketchSubElement::SplineHandle, 1}});

    EXPECT_FALSE(document.recompute().success) << sketch.solveMessage();
}

TEST(SketchGeometricConstraintTest, M18_HAN_009_AHandleKeyedPastTheEndIsNOTVALIDGeometry) {
    // The one place the sparse map's invariant is enforced. A handle naming a
    // point that does not exist is what a parallel array would have made
    // impossible by construction, and what a map has to be told.
    SketchSpline geometry{{Vec2{10, 10}, Vec2{50, 50}, Vec2{100, 10}}, false};
    geometry.handles[7] = Vec2{30, 0};
    EXPECT_FALSE(IsValidSketchGeometry(geometry));

    SketchSpline zeroLength{{Vec2{10, 10}, Vec2{50, 50}, Vec2{100, 10}}, false};
    zeroLength.handles[1] = Vec2{0, 0};
    // A tangent with no length says nothing about direction: that is the
    // ABSENCE of a handle, spelled wrongly.
    EXPECT_FALSE(IsValidSketchGeometry(zeroLength));
}

TEST(SketchGeometricConstraintTest, M18_HAN_010_ACONSTRAINTFREESketchStillCountsItsTieEquations) {
    // The DOF readout has TWO implementations and they have to agree.
    //
    // A sketch with no constraints never reaches the solver at all: that path
    // counts the variables the problem would have had. But not every residual
    // comes from a constraint -- an arc's tips, an elliptical arc's tips and a
    // spline handle's tip are each DEFINED by an equation the builder emits
    // regardless, and those take freedom away exactly as a constraint would.
    //
    // Counting only the variables reported a bare arc as nine free scalars when
    // it has five, and had done so since arcs grew tips. It was invisible until
    // a handle made the gap four.
    PartDocument document{"FreeDoc"};
    GaussNewtonSketchSolver solver;
    document.setSketchSolver(&solver);

    Sketch& arcOnly = document.addSketch("Arc");
    arcOnly.addArc(Vec2{0, 0}, 40.0, 0.0, 1.0, true);
    ASSERT_TRUE(document.recompute().success) << arcOnly.solveMessage();
    // Centre, radius and two angles. The four tip variables are paid for by the
    // four equations that place them.
    EXPECT_EQ(arcOnly.degreesOfFreedom(), 5) << arcOnly.solveMessage();

    Sketch& plain = document.addSketch("Spline");
    plain.addSpline({Vec2{0, 0}, Vec2{40, 30}, Vec2{90, 10}}, false);
    ASSERT_TRUE(document.recompute().success) << plain.solveMessage();
    EXPECT_EQ(plain.degreesOfFreedom(), 6) << plain.solveMessage();

    Sketch& handled = document.addSketch("Handled");
    SketchSpline geometry{{Vec2{0, 0}, Vec2{40, 30}, Vec2{90, 10}}, false};
    geometry.handles[1] = Vec2{20, 0};
    const SketchEntityId spline = handled.addSpline(geometry.points, geometry.closed);
    ASSERT_TRUE(document.setSketchEntityGeometry(handled.id(), spline, geometry));
    ASSERT_TRUE(document.recompute().success) << handled.solveMessage();
    // Six for the points and TWO for the handle -- four variables less the two
    // equations that tie the tip to its point.
    EXPECT_EQ(handled.degreesOfFreedom(), 8) << handled.solveMessage();
}
