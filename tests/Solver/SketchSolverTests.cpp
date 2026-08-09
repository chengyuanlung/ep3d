// M5-E: the Gauss-Newton solver, tested directly against the neutral problem
// type (spec 19 "Solver").
//
// Every expected value here is an independent analytical result. Spec 20 is
// explicit that production solver helpers must not be used to create expected
// values, and this is the milestone where that matters most: a solver that
// converges to the wrong answer passes any test that only asks "did it
// converge".

#include "Core/Sketch/ISketchSolver.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <random>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;
constexpr double kTol = 1e-6; // mm, generous next to the solver's own 1e-9

SolveVariable PointVar(int entity, SolveVariable::Component c) {
    return SolveVariable{static_cast<SketchEntityId>(entity), SketchSubElement::Whole, c};
}

SolveResidual Fixed(SolveResidual::Kind kind, int var, double target, int constraint = 1) {
    SolveResidual r;
    r.kind = kind;
    r.vars[0] = var;
    r.target = target;
    r.sourceConstraint = static_cast<SketchConstraintId>(constraint);
    return r;
}

SolveResidual Equal(SolveResidual::Kind kind, int a, int b, int constraint = 1) {
    SolveResidual r;
    r.kind = kind;
    r.vars[0] = a;
    r.vars[1] = b;
    r.sourceConstraint = static_cast<SketchConstraintId>(constraint);
    return r;
}

SolveResidual Span(SolveResidual::Kind kind, int au, int av, int bu, int bv, double target,
                   int constraint = 1) {
    SolveResidual r;
    r.kind = kind;
    r.vars[0] = au;
    r.vars[1] = av;
    r.vars[2] = bu;
    r.vars[3] = bv;
    r.target = target;
    r.sourceConstraint = static_cast<SketchConstraintId>(constraint);
    return r;
}

// A rectangle as four corners: variables are P0u P0v P1u P1v P2u P2v P3u P3v.
// Corner 0 fixed at the origin, bottom horizontal, right vertical, and the two
// dimensions. That is the reference configuration of spec 14.
SketchSolveProblem Rectangle(double width, double height, bool fixCorner = true,
                             bool addWidth = true, bool addHeight = true) {
    SketchSolveProblem p;
    for (int i = 0; i < 4; ++i) {
        p.variables.push_back(PointVar(i, SolveVariable::Component::U));
        p.variables.push_back(PointVar(i, SolveVariable::Component::V));
    }
    // A deliberately sloppy start, so convergence is doing real work rather
    // than confirming values it was handed.
    p.initialValues = {1.0, -2.0, 90.0, 7.0, 105.0, 44.0, -3.0, 55.0};

    if (fixCorner) {
        p.residuals.push_back(Fixed(SolveResidual::Kind::FixedU, 0, 0.0, 10));
        p.residuals.push_back(Fixed(SolveResidual::Kind::FixedV, 1, 0.0, 11));
    }
    // bottom P0->P1 horizontal, top P3->P2 horizontal
    p.residuals.push_back(Equal(SolveResidual::Kind::LineHorizontal, 1, 3, 20));
    p.residuals.push_back(Equal(SolveResidual::Kind::LineHorizontal, 7, 5, 21));
    // left P0->P3 vertical, right P1->P2 vertical
    p.residuals.push_back(Equal(SolveResidual::Kind::LineVertical, 0, 6, 30));
    p.residuals.push_back(Equal(SolveResidual::Kind::LineVertical, 2, 4, 31));
    if (addWidth)
        p.residuals.push_back(Span(SolveResidual::Kind::Length, 0, 1, 2, 3, width, 40));
    if (addHeight)
        p.residuals.push_back(Span(SolveResidual::Kind::Length, 2, 3, 4, 5, height, 41));
    return p;
}

// --- Basic convergence ------------------------------------------------------

TEST(SketchSolverTest, M5_SOLVE_001_FixedPointConverges) {
    SketchSolveProblem p;
    p.variables = {PointVar(1, SolveVariable::Component::U),
                   PointVar(1, SolveVariable::Component::V)};
    p.initialValues = {5.0, -3.0};
    p.residuals = {Fixed(SolveResidual::Kind::FixedU, 0, 10.0),
                   Fixed(SolveResidual::Kind::FixedV, 1, 20.0)};

    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    ASSERT_TRUE(r) << r.message;
    EXPECT_EQ(r.status, SketchSolveStatus::Solved);
    EXPECT_EQ(r.degreesOfFreedom, 0);
    EXPECT_NEAR(r.values[0], 10.0, kTol);
    EXPECT_NEAR(r.values[1], 20.0, kTol);
}

TEST(SketchSolverTest, M5_SOLVE_002_DistanceConverges) {
    SketchSolveProblem p;
    p.variables = {PointVar(1, SolveVariable::Component::U),
                   PointVar(1, SolveVariable::Component::V),
                   PointVar(2, SolveVariable::Component::U),
                   PointVar(2, SolveVariable::Component::V)};
    p.initialValues = {0.0, 0.0, 3.0, 4.0};
    p.residuals = {Fixed(SolveResidual::Kind::FixedU, 0, 0.0, 1),
                   Fixed(SolveResidual::Kind::FixedV, 1, 0.0, 2),
                   Fixed(SolveResidual::Kind::FixedV, 3, 0.0, 3),
                   Span(SolveResidual::Kind::Distance, 0, 1, 2, 3, 25.0, 4)};

    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    ASSERT_TRUE(r) << r.message;
    // Independent check: the second point is on the u axis at distance 25.
    const double du = r.values[2] - r.values[0];
    const double dv = r.values[3] - r.values[1];
    EXPECT_NEAR(std::sqrt(du * du + dv * dv), 25.0, kTol);
    EXPECT_EQ(r.degreesOfFreedom, 0);
}

// --- Fully constrained rectangle (spec 14) ---------------------------------

TEST(SketchSolverTest, M5_SOLVE_010_FullyConstrainedRectangleHasZeroDof) {
    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(Rectangle(100.0, 50.0));
    ASSERT_TRUE(r) << r.message;
    EXPECT_EQ(r.status, SketchSolveStatus::Solved);
    EXPECT_EQ(r.degreesOfFreedom, 0) << "a fully constrained rectangle must have DOF 0";

    // Corners, computed independently from the constraints rather than from
    // whatever the solver produced.
    EXPECT_NEAR(r.values[0], 0.0, kTol);    // P0.u
    EXPECT_NEAR(r.values[1], 0.0, kTol);    // P0.v
    EXPECT_NEAR(r.values[2], 100.0, kTol);  // P1.u
    EXPECT_NEAR(r.values[3], 0.0, kTol);    // P1.v
    EXPECT_NEAR(r.values[4], 100.0, kTol);  // P2.u
    EXPECT_NEAR(r.values[5], 50.0, kTol);   // P2.v
    EXPECT_NEAR(r.values[6], 0.0, kTol);    // P3.u
    EXPECT_NEAR(r.values[7], 50.0, kTol);   // P3.v
}

TEST(SketchSolverTest, M5_SOLVE_011_WidthChangeMovesOnlyWidth) {
    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(Rectangle(120.0, 50.0));
    ASSERT_TRUE(r) << r.message;
    EXPECT_EQ(r.degreesOfFreedom, 0);
    EXPECT_NEAR(r.values[2], 120.0, kTol) << "width did not reach its target";
    EXPECT_NEAR(r.values[5], 50.0, kTol) << "height changed when only width should have";
}

// --- DOF is measured, not counted (ADR-M5-005, spec 10) --------------------

TEST(SketchSolverTest, M5_DOF_001_MissingDimensionLeavesOneDegreeOfFreedom) {
    GaussNewtonSketchSolver solver;
    // Same rectangle without the height dimension: exactly one freedom left.
    const SketchSolveResult r = solver.solve(Rectangle(100.0, 50.0, true, true, false));
    ASSERT_TRUE(r) << r.message;
    EXPECT_EQ(r.status, SketchSolveStatus::UnderConstrained);
    EXPECT_EQ(r.degreesOfFreedom, 1);
    // Geometry is still finite and valid -- under-constrained is an ordinary
    // editing state, not a failure (ADR-M5-004).
    for (double v : r.values) EXPECT_TRUE(std::isfinite(v));
    EXPECT_NEAR(r.values[2] - r.values[0], 100.0, kTol) << "the width dimension still holds";
}

TEST(SketchSolverTest, M5_DOF_002_UnfixedRectangleHasThreeDegreesOfFreedom) {
    GaussNewtonSketchSolver solver;
    // No Fix: the rectangle can translate in u and v. Rotation is removed by
    // the horizontal/vertical constraints, so 2 remain, not 3.
    const SketchSolveResult r = solver.solve(Rectangle(100.0, 50.0, false));
    ASSERT_TRUE(r) << r.message;
    EXPECT_EQ(r.status, SketchSolveStatus::UnderConstrained);
    EXPECT_EQ(r.degreesOfFreedom, 2) << "an unpinned axis-aligned rectangle has 2 free DOF";
}

TEST(SketchSolverTest, M5_DOF_003_RedundantConstraintDoesNotReduceDofBelowZero) {
    // THE test that separates a measured DOF from `variables - constraints`.
    // Adding a redundant-but-consistent constraint adds a residual row without
    // adding rank; a counting formula would report DOF -1.
    SketchSolveProblem p = Rectangle(100.0, 50.0);
    // The bottom edge is already horizontal via constraint 20; say so again.
    p.residuals.push_back(Equal(SolveResidual::Kind::LineHorizontal, 1, 3, 99));

    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    ASSERT_TRUE(r) << r.message;
    EXPECT_EQ(r.degreesOfFreedom, 0) << "DOF must never go negative";
    EXPECT_EQ(r.status, SketchSolveStatus::OverConstrained)
        << "redundant but consistent constraints are accepted, not rejected";
    EXPECT_NEAR(r.values[2], 100.0, kTol) << "geometry is still committed and correct";
}

TEST(SketchSolverTest, M5_DOF_004_RankDeficiencyIsDetectedWhenCountingWouldSayFullyConstrained) {
    // THE discriminating test. The three DOF tests above do not distinguish a
    // measured DOF from `variables - residual count`: two of them happen to
    // have full rank, and the third is masked by the clamp at zero. Verified by
    // temporarily substituting the forbidden formula -- all three still passed.
    //
    // Here the two formulas disagree visibly. Eight variables; eight residuals,
    // one of which is a duplicate Horizontal that adds a row without adding
    // rank; and no height dimension:
    //
    //   counting: 8 - 8 = 0  -> "fully constrained"  (WRONG)
    //   rank:     8 - 7 = 1  -> one free DOF          (CORRECT)
    //
    // The failure mode being guarded is the damaging one: telling a user their
    // sketch is fully constrained when a dimension is still free.
    SketchSolveProblem p = Rectangle(100.0, 50.0, true, true, false);
    ASSERT_EQ(p.variables.size(), 8u);
    ASSERT_EQ(p.residuals.size(), 7u);
    p.residuals.push_back(Equal(SolveResidual::Kind::LineHorizontal, 1, 3, 98)); // duplicate
    ASSERT_EQ(p.residuals.size(), 8u);

    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    ASSERT_TRUE(r) << r.message;

    EXPECT_EQ(r.degreesOfFreedom, 1)
        << "DOF was counted rather than measured: a redundant row hid a real freedom";
    EXPECT_NE(r.status, SketchSolveStatus::Solved)
        << "an under-constrained sketch was reported as fully constrained";
}

TEST(SketchSolverTest, M5_DOF_005_DuplicateConstraintDoesNotChangeReportedDof) {
    // Same rectangle, once clean and once with a redundant row. A measured DOF
    // is identical in both; a counted one differs by exactly the number of
    // duplicates.
    GaussNewtonSketchSolver solver;
    const SketchSolveResult clean =
        solver.solve(Rectangle(100.0, 50.0, true, true, false));
    ASSERT_TRUE(clean) << clean.message;

    SketchSolveProblem withDuplicates = Rectangle(100.0, 50.0, true, true, false);
    withDuplicates.residuals.push_back(Equal(SolveResidual::Kind::LineHorizontal, 1, 3, 96));
    withDuplicates.residuals.push_back(Equal(SolveResidual::Kind::LineVertical, 0, 6, 97));
    const SketchSolveResult redundant = solver.solve(withDuplicates);
    ASSERT_TRUE(redundant) << redundant.message;

    EXPECT_EQ(redundant.degreesOfFreedom, clean.degreesOfFreedom)
        << "adding redundant constraints changed the reported DOF";
}

// --- Conflict (spec 25 Gate E) ---------------------------------------------

TEST(SketchSolverTest, M5_CONFLICT_001_ContradictoryDimensionsReportConflicting) {
    SketchSolveProblem p = Rectangle(100.0, 50.0);
    // The same edge told to be 120 as well as 100.
    p.residuals.push_back(Span(SolveResidual::Kind::Length, 0, 1, 2, 3, 120.0, 77));

    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    EXPECT_FALSE(r) << "a contradictory sketch must not report success";
    EXPECT_EQ(r.status, SketchSolveStatus::Conflicting);
    EXPECT_FALSE(r.message.empty());
    // The diagnostic must name constraints, not just a status -- Gate E asks
    // for something a user can act on.
    EXPECT_FALSE(r.offendingConstraints.empty())
        << "conflict reported without naming any constraint";
}

TEST(SketchSolverTest, M5_CONFLICT_002_NoNonFiniteValueIsEverReturned) {
    SketchSolveProblem p = Rectangle(100.0, 50.0);
    p.residuals.push_back(Span(SolveResidual::Kind::Length, 0, 1, 2, 3, 120.0, 77));

    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    for (double v : r.values)
        EXPECT_TRUE(std::isfinite(v)) << "a failed solve returned a non-finite value";
}

// --- Invalid input, detected before solving --------------------------------

TEST(SketchSolverTest, M5_INVALID_001_NonFiniteInputsRejected) {
    GaussNewtonSketchSolver solver;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    SketchSolveProblem badStart = Rectangle(100.0, 50.0);
    badStart.initialValues[3] = nan;
    EXPECT_EQ(solver.solve(badStart).status, SketchSolveStatus::InvalidInput);

    SketchSolveProblem badTarget = Rectangle(100.0, 50.0);
    badTarget.residuals.back().target = inf;
    EXPECT_EQ(solver.solve(badTarget).status, SketchSolveStatus::InvalidInput);

    SketchSolveProblem empty;
    EXPECT_EQ(solver.solve(empty).status, SketchSolveStatus::InvalidInput);
}

TEST(SketchSolverTest, M5_INVALID_002_NoConstraintsIsUnderConstrainedNotAFailure) {
    SketchSolveProblem p;
    p.variables = {PointVar(1, SolveVariable::Component::U),
                   PointVar(1, SolveVariable::Component::V)};
    p.initialValues = {3.0, 4.0};

    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    EXPECT_TRUE(r) << r.message;
    EXPECT_EQ(r.status, SketchSolveStatus::UnderConstrained);
    EXPECT_EQ(r.degreesOfFreedom, 2);
    EXPECT_DOUBLE_EQ(r.values[0], 3.0) << "an unconstrained point must not move";
}

// --- Determinism and order independence (spec 19/30) -----------------------

TEST(SketchSolverTest, M5_DETERMINISM_001_RepeatedSolvesAgreeExactly) {
    GaussNewtonSketchSolver solver;
    const SketchSolveResult first = solver.solve(Rectangle(100.0, 50.0));
    ASSERT_TRUE(first) << first.message;
    for (int i = 0; i < 20; ++i) {
        const SketchSolveResult again = solver.solve(Rectangle(100.0, 50.0));
        ASSERT_TRUE(again);
        for (std::size_t v = 0; v < first.values.size(); ++v)
            EXPECT_DOUBLE_EQ(again.values[v], first.values[v]) << "solve is not deterministic";
    }
}

TEST(SketchSolverTest, M5_DETERMINISM_002_NoDriftOver100ResolvesFromPreviousResult) {
    // Spec 30: repeat solve 100x for drift. Feeding each result back as the
    // next initial configuration is the case where drift would accumulate.
    GaussNewtonSketchSolver solver;
    SketchSolveProblem p = Rectangle(100.0, 50.0);
    SketchSolveResult r = solver.solve(p);
    ASSERT_TRUE(r) << r.message;
    const std::vector<double> first = r.values;

    for (int i = 0; i < 100; ++i) {
        p.initialValues = r.values;
        r = solver.solve(p);
        ASSERT_TRUE(r) << "solve " << i << " failed: " << r.message;
    }
    for (std::size_t v = 0; v < first.size(); ++v)
        EXPECT_NEAR(r.values[v], first[v], 1e-9) << "value " << v << " drifted";
}

TEST(SketchSolverTest, M5_ORDER_001_ResidualOrderDoesNotChangeTheAnswer) {
    GaussNewtonSketchSolver solver;
    const SketchSolveResult reference = solver.solve(Rectangle(100.0, 50.0));
    ASSERT_TRUE(reference) << reference.message;

    std::mt19937 rng(12345);
    for (int trial = 0; trial < 10; ++trial) {
        SketchSolveProblem shuffled = Rectangle(100.0, 50.0);
        std::shuffle(shuffled.residuals.begin(), shuffled.residuals.end(), rng);
        const SketchSolveResult r = solver.solve(shuffled);
        ASSERT_TRUE(r) << r.message;
        EXPECT_EQ(r.degreesOfFreedom, reference.degreesOfFreedom);
        for (std::size_t v = 0; v < reference.values.size(); ++v)
            EXPECT_NEAR(r.values[v], reference.values[v], kTol)
                << "constraint order changed the solved geometry";
    }
}

// --- Angle convention (ADR-M5-006) -----------------------------------------
//
// These replace two tests that could not fail. The old M5_ANGLE_001 built four
// bare scalars -- no lines, no geometry -- and finished with
// `EXPECT_NEAR(std::sin(b - a - kPi/2.0), 0.0, kTol)`, recomputing the solver's
// OWN residual formula and checking the solver had driven it to zero. It
// asserted convergence, not an angle, and it stayed green while the residual
// compared millimetres to radians. The rule these obey now: measure the angle
// from the SOLVED COORDINATES with atan2, never by re-evaluating the residual.

// Two real lines with a shared, pinned corner. lineA is pinned along +u so the
// answer is unambiguous; lineB keeps its length and is free to rotate.
SketchSolveProblem AnglePairProblem(double target, double lineAAngleRad = 0.0) {
    SketchSolveProblem p;
    // vars 0..3: lineA start(u,v), end(u,v);  vars 4..7: lineB start(u,v), end(u,v)
    for (int entity = 1; entity <= 2; ++entity) {
        p.variables.push_back(PointVar(entity, SolveVariable::Component::U));
        p.variables.push_back(PointVar(entity, SolveVariable::Component::V));
        p.variables.push_back(PointVar(entity, SolveVariable::Component::U));
        p.variables.push_back(PointVar(entity, SolveVariable::Component::V));
    }
    const double ax = 10.0 * std::cos(lineAAngleRad);
    const double ay = 10.0 * std::sin(lineAAngleRad);
    // lineB starts somewhere unhelpful, so a solver that does nothing fails.
    p.initialValues = {0.0, 0.0, ax, ay, 0.0, 0.0, 7.0, 1.0};

    // Pin lineA completely and pin lineB's start at the origin.
    p.residuals = {Fixed(SolveResidual::Kind::FixedU, 0, 0.0, 1),
                   Fixed(SolveResidual::Kind::FixedV, 1, 0.0, 2),
                   Fixed(SolveResidual::Kind::FixedU, 2, ax, 3),
                   Fixed(SolveResidual::Kind::FixedV, 3, ay, 4),
                   Fixed(SolveResidual::Kind::FixedU, 4, 0.0, 5),
                   Fixed(SolveResidual::Kind::FixedV, 5, 0.0, 6),
                   // lineB keeps a fixed length, so only its direction is free.
                   Span(SolveResidual::Kind::Length, 4, 5, 6, 7, 10.0, 7)};

    SolveResidual angle;
    angle.kind = SolveResidual::Kind::Angle;
    for (int slot = 0; slot < 8; ++slot) angle.vars[static_cast<std::size_t>(slot)] = slot;
    angle.target = target;
    angle.sourceConstraint = static_cast<SketchConstraintId>(8);
    p.residuals.push_back(angle);
    return p;
}

// Compares two angles AS ANGLES: 0 and 2*pi are the same direction, and a
// plain EXPECT_NEAR on normalized values calls them 6.28 apart whenever the
// solver lands a hair below the wrap. The property under test is the geometry,
// not which representative of the equivalence class came back.
void ExpectAngleNear(double measured, double expected, double tolerance,
                     const std::string& context) {
    const double difference = std::remainder(measured - expected, 2.0 * kPi);
    EXPECT_NEAR(difference, 0.0, tolerance)
        << context << ": measured " << measured * 180.0 / kPi << " deg, expected "
        << expected * 180.0 / kPi << " deg";
}

// The angle actually present in the solved coordinates. Deliberately NOT the
// residual's formula.
double MeasuredAngle(const std::vector<double>& v) {
    const double a = std::atan2(v[3] - v[1], v[2] - v[0]);
    const double b = std::atan2(v[7] - v[5], v[6] - v[4]);
    double d = b - a;
    while (d < 0.0) d += 2.0 * kPi;
    while (d >= 2.0 * kPi) d -= 2.0 * kPi;
    return d;
}

TEST(SketchSolverTest, M5_ANGLE_001_SolvedGeometryCarriesTheRequestedAngle) {
    for (double degrees : {30.0, 45.0, 90.0, 120.0, 210.0, 300.0}) {
        const double target = degrees * kPi / 180.0;
        GaussNewtonSketchSolver solver;
        const SketchSolveResult r = solver.solve(AnglePairProblem(target));
        ASSERT_TRUE(r) << degrees << " deg: " << r.message;
        ExpectAngleNear(MeasuredAngle(r.values), target, 1e-6,
                        "asked for " + std::to_string(degrees) + " deg");
    }
}

TEST(SketchSolverTest, M5_ANGLE_002_AngleIsDirectedNotModuloPi) {
    // ADR-M5-006 justifies a DIRECTED angle precisely so a 60-degree corner can
    // be told from a 120-degree one. A sin()-based residual has period pi and
    // cannot: it would satisfy 30 degrees with 210. This is the test that
    // formulation fails.
    for (double degrees : {30.0, 60.0}) {
        const double target = degrees * kPi / 180.0;
        SketchSolveProblem p = AnglePairProblem(target);
        // START NEAR THE SUPPLEMENTARY ROOT. The default fixture starts line B
        // at about 8 degrees, close enough that Gauss-Newton picks the correct
        // root of a sin() residual anyway -- so this test PASSED against the
        // very formulation it is named for rejecting. Starting near
        // target + 180 makes the wrong root the nearer one, which is what
        // forces the residual to be directed rather than merely convergent.
        const double decoy = target + kPi - 0.05;
        p.initialValues[6] = 10.0 * std::cos(decoy);
        p.initialValues[7] = 10.0 * std::sin(decoy);
        GaussNewtonSketchSolver solver;
        const SketchSolveResult r = solver.solve(p);
        ASSERT_TRUE(r) << r.message;
        const double measured = MeasuredAngle(r.values) * 180.0 / kPi;
        EXPECT_NEAR(measured, degrees, 1e-4);
        EXPECT_GT(std::fabs(measured - (degrees + 180.0)), 1.0)
            << "solved to the supplementary angle: " << measured;
    }
}

TEST(SketchSolverTest, M5_ANGLE_003_AngleIsRotationInvariant) {
    // Rotating the reference line must rotate the answer with it. The broken
    // v-components-only residual was not rotation invariant, which is by itself
    // proof that it was not measuring an angle.
    const double target = 45.0 * kPi / 180.0;
    for (double referenceDeg : {0.0, 30.0, 90.0, 137.0, 200.0}) {
        GaussNewtonSketchSolver solver;
        const SketchSolveResult r =
            solver.solve(AnglePairProblem(target, referenceDeg * kPi / 180.0));
        ASSERT_TRUE(r) << referenceDeg << ": " << r.message;
        ExpectAngleNear(MeasuredAngle(r.values), target, 1e-6,
                        "reference line at " + std::to_string(referenceDeg) + " deg");
    }
}

TEST(SketchSolverTest, M5_ANGLE_004_AlreadySatisfiedAngleLeavesGeometryAlone) {
    // lineA along +u, lineB along +v: exactly 90 degrees already. Asking for 90
    // must move nothing. The broken residual stretched lineB by ~1 mm here.
    SketchSolveProblem p = AnglePairProblem(kPi / 2.0);
    p.initialValues = {0.0, 0.0, 10.0, 0.0, 0.0, 0.0, 0.0, 10.0};
    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    ASSERT_TRUE(r) << r.message;
    for (std::size_t i = 0; i < p.initialValues.size(); ++i)
        EXPECT_NEAR(r.values[i], p.initialValues[i], 1e-9)
            << "variable " << i << " moved on an already-satisfied angle";
}

TEST(SketchSolverTest, M5_ANGLE_005_TargetsNearZeroAndPiConverge) {
    // The numerically delicate cases spec 30 names explicitly -- now checked
    // geometrically, not merely for convergence and finiteness.
    for (double target : {1e-4, kPi - 1e-4, kPi, kPi + 1e-4, 2.0 * kPi - 1e-4}) {
        GaussNewtonSketchSolver solver;
        const SketchSolveResult r = solver.solve(AnglePairProblem(target));
        ASSERT_TRUE(r) << "angle target " << target << " failed: " << r.message;
        for (double v : r.values) EXPECT_TRUE(std::isfinite(v));
        ExpectAngleNear(MeasuredAngle(r.values), target, 1e-5,
                        "target " + std::to_string(target));
    }
}

TEST(SketchSolverTest, M5_ANGLE_006_UnderPackedAngleResidualIsRejected) {
    // The guard that makes the original defect unrepresentable: an Angle
    // residual carrying fewer variables than its equation reads must be
    // refused, not evaluated against index -1.
    SketchSolveProblem p = AnglePairProblem(kPi / 2.0);
    p.residuals.back().vars[7] = -1;
    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    EXPECT_FALSE(r);
    EXPECT_EQ(r.status, SketchSolveStatus::InvalidInput);
}

// --- Regressions for the M5 RE-review -----------------------------------------

TEST(SketchSolverTest, M5_REV2_001_AngleStartingAtTheHalfTurnStillConverges) {
    // ADR-M5-006 claimed the wrap point was "a measure-zero starting
    // configuration the solve converges through from either side". Measured, it
    // stalled at iteration 1 and reported "did not converge within the
    // iteration limit" -- after refusing to take a single step. Two lines drawn
    // exactly parallel or antiparallel is an everyday result of axis snapping.
    struct Case { double target; double start; };
    for (const Case& c : {Case{kPi, 0.0}, Case{0.0, kPi}, Case{kPi / 2.0, 3.0 * kPi / 2.0},
                          Case{kPi / 6.0, kPi / 6.0 + kPi}}) {
        SketchSolveProblem p = AnglePairProblem(c.target);
        // Place line B exactly a half-turn away from where the target wants it.
        p.initialValues[6] = 10.0 * std::cos(c.start);
        p.initialValues[7] = 10.0 * std::sin(c.start);
        GaussNewtonSketchSolver solver;
        const SketchSolveResult r = solver.solve(p);
        ASSERT_TRUE(r) << "target " << c.target << " from " << c.start << ": " << r.message;
        ExpectAngleNear(MeasuredAngle(r.values), c.target, 1e-5,
                        "half-turn start " + std::to_string(c.start));
    }
}

TEST(SketchSolverTest, M5_REV3_001_HalfTurnConvergesAtEveryScaleAndOffset) {
    // The scale-blind trap. A previous fix rotated the starting guess off the
    // antipode using a FIXED 1e-6 rad guard and a FIXED 1e-4 rad nudge, while
    // the residual's usable angular resolution is 1e-7*|coordinate|/length --
    // scale-DEPENDENT. It improved 131 of 760 grid cases and BROKE 42, turning
    // correct Solved results into false "did not converge" failures for short
    // lines far from the sketch origin.
    //
    // Fixing the Jacobian instead of dodging the point removes the tuning
    // constants, so this sweeps the space that exposed the trap: short and long
    // lines, near and far from the origin, exactly at and just off the antipode.
    for (double length : {10.0, 1.0, 0.1, 0.01, 0.001}) {
        for (double origin : {0.0, 10.0, 100.0, 1000.0}) {
            for (double offset : {0.0, 1e-9, -3e-9, 1e-7, 3e-7, 1e-5, 1e-3}) {
                const double target = kPi / 2.0;
                SketchSolveProblem p;
                for (int entity = 1; entity <= 2; ++entity)
                    for (int k = 0; k < 2; ++k) {
                        p.variables.push_back(PointVar(entity, SolveVariable::Component::U));
                        p.variables.push_back(PointVar(entity, SolveVariable::Component::V));
                    }
                // Line A along +u; line B a half-turn from where the target
                // wants it, plus the offset under test.
                const double armAngle = target + kPi + offset;
                p.initialValues = {origin, origin, origin + 10.0, origin,
                                   origin, origin,
                                   origin + length * std::cos(armAngle),
                                   origin + length * std::sin(armAngle)};
                p.residuals = {Fixed(SolveResidual::Kind::FixedU, 0, origin, 1),
                               Fixed(SolveResidual::Kind::FixedV, 1, origin, 2),
                               Fixed(SolveResidual::Kind::FixedU, 2, origin + 10.0, 3),
                               Fixed(SolveResidual::Kind::FixedV, 3, origin, 4),
                               Fixed(SolveResidual::Kind::FixedU, 4, origin, 5),
                               Fixed(SolveResidual::Kind::FixedV, 5, origin, 6),
                               Span(SolveResidual::Kind::Length, 4, 5, 6, 7, length, 7)};
                SolveResidual angle;
                angle.kind = SolveResidual::Kind::Angle;
                for (int slot = 0; slot < 8; ++slot)
                    angle.vars[static_cast<std::size_t>(slot)] = slot;
                angle.target = target;
                angle.sourceConstraint = static_cast<SketchConstraintId>(8);
                p.residuals.push_back(angle);

                GaussNewtonSketchSolver solver;
                const SketchSolveResult r = solver.solve(p);
                const std::string context = "length " + std::to_string(length) + " origin " +
                                            std::to_string(origin) + " offset " +
                                            std::to_string(offset);
                ASSERT_TRUE(r) << context << ": " << r.message;
                ExpectAngleNear(MeasuredAngle(r.values), target, 1e-5, context);
            }
        }
    }
}

TEST(SketchSolverTest, M5_REV3_002_AHugeAngleTargetTerminates) {
    // `while (d > kPi) d -= 2*kPi` is unbounded, and any finite angle is
    // accepted as a dimension: 1e300 rad made recompute() never return, because
    // 2*pi is below the ULP of 1e300. 1e9 rad took 21.8 seconds. std::remainder
    // is O(1) for every finite input. If this test hangs, the loop is back.
    for (double target : {1e9, 1e15, 1e300}) {
        SketchSolveProblem p = AnglePairProblem(target);
        GaussNewtonSketchSolver solver;
        const SketchSolveResult r = solver.solve(p); // must simply return
        for (double v : r.values) EXPECT_TRUE(std::isfinite(v));
    }
}

TEST(SketchSolverTest, M5_REV2_002_UnderPackedDistanceIsRejectedNotDereferenced) {
    // The degeneracy nudge was placed ABOVE the slot guard and indexed x[] raw,
    // so an under-packed Distance -- exactly what the guard refuses -- was
    // dereferenced first: an assertion abort in Debug and an out-of-bounds read
    // AND WRITE in Release. A fix for one finding re-opened the hole another
    // finding's guard had just closed. This runs the path that crashed.
    SketchSolveProblem p;
    p.variables = {PointVar(1, SolveVariable::Component::U),
                   PointVar(1, SolveVariable::Component::V),
                   PointVar(2, SolveVariable::Component::U),
                   PointVar(2, SolveVariable::Component::V)};
    p.initialValues = {0.0, 0.0, 0.0, 0.0}; // degenerate, so the nudge would fire
    SolveResidual bad = Span(SolveResidual::Kind::Distance, 0, 1, 2, 3, 10.0, 1);
    bad.vars[2] = -1; // the slot the nudge read and wrote
    p.residuals = {bad};

    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    EXPECT_FALSE(r);
    EXPECT_EQ(r.status, SketchSolveStatus::InvalidInput);
}

TEST(SketchSolverTest, M5_REV2_003_AFailedResultDoesNotClaimZeroDegreesOfFreedom) {
    // 0 means FULLY CONSTRAINED. A failed solve has measured nothing, and the
    // result type used to hand out 0 regardless.
    SketchSolveProblem p;
    p.variables = {PointVar(1, SolveVariable::Component::U)};
    p.initialValues = {std::numeric_limits<double>::quiet_NaN()};
    p.residuals = {Fixed(SolveResidual::Kind::FixedU, 0, 1.0, 1)};

    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.degreesOfFreedom, kUnknownDegreesOfFreedom);
}

// --- Previously-deferred findings, now closed --------------------------------

TEST(SketchSolverTest, M5_DEF_001_MisorderedResidualSlotsAreRejected) {
    // The guard was arity-only: a Distance packed (a.u, b.u, a.v, b.v) instead
    // of (a.u, a.v, b.u, b.v) filled all four slots in range, passed, and then
    // reported Solved with a residual of 2.7e-12 while the geometry was wrong
    // by millimetres -- character-for-character the C1 failure mode. Every
    // variable already carries its Component, so the packing can be checked
    // against what the formula assumes.
    SketchSolveProblem p;
    p.variables = {PointVar(1, SolveVariable::Component::U),
                   PointVar(1, SolveVariable::Component::V),
                   PointVar(2, SolveVariable::Component::U),
                   PointVar(2, SolveVariable::Component::V)};
    p.initialValues = {0.0, 0.0, 6.0, 4.0};
    p.residuals = {Fixed(SolveResidual::Kind::FixedU, 0, 0.0, 1),
                   Fixed(SolveResidual::Kind::FixedV, 1, 0.0, 2),
                   // Slots 1 and 2 swapped: u where v belongs and vice versa.
                   Span(SolveResidual::Kind::Distance, 0, 2, 1, 3, 10.0, 3)};

    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    EXPECT_FALSE(r) << "a mis-packed Distance was accepted";
    EXPECT_EQ(r.status, SketchSolveStatus::InvalidInput);

    // The correctly packed version of the same problem still solves.
    p.residuals.back() = Span(SolveResidual::Kind::Distance, 0, 1, 2, 3, 10.0, 3);
    const SketchSolveResult good = solver.solve(p);
    ASSERT_TRUE(good) << good.message;
    EXPECT_NEAR(std::hypot(good.values[2] - good.values[0], good.values[3] - good.values[1]),
                10.0, 1e-6);
}

TEST(SketchSolverTest, M5_DEF_002_IterationCountAndMessageTellTheTruth) {
    // `iteration = kSolveMaxIterations` was used to break out of the loop, so a
    // solve that settled in a few steps reported 100 -- and the failure message
    // blamed an iteration limit the solver had never reached.
    GaussNewtonSketchSolver solver;
    const SketchSolveResult quick = solver.solve(AnglePairProblem(kPi / 4.0));
    ASSERT_TRUE(quick) << quick.message;
    EXPECT_LT(quick.iterations, kSolveMaxIterations)
        << "a solve that converged quickly reported the iteration limit";
    EXPECT_GT(quick.iterations, 0);
}

// --- Round-4 review regressions ----------------------------------------------

TEST(SketchSolverTest, M5_REV4_001_ALargeAngleTargetIsNotCalledContradictory) {
    // WrapToPi(a - b - target) subtracted the RAW target, so at 1e9 rad the
    // target's ULP swamped the angular signal, the residual stopped depending
    // on the geometry, and the solver reported "constraints are contradictory:
    // no configuration satisfies them" -- for a system that solves perfectly
    // with the mathematically identical pre-wrapped target. The round-3 fix
    // turned a hang into a wrong answer instead of a right one.
    const double base = kPi / 3.0;
    for (double turns : {1.0, 1e3, 1e6, 1e8, 1e9}) {
        const double target = base + turns * 2.0 * kPi;
        GaussNewtonSketchSolver solver;
        const SketchSolveResult r = solver.solve(AnglePairProblem(target));
        ASSERT_TRUE(r) << "target " << target << ": " << r.message;
        EXPECT_NE(r.status, SketchSolveStatus::Conflicting)
            << "a solvable angle was called contradictory at target " << target;
        ExpectAngleNear(MeasuredAngle(r.values), base, 1e-4,
                        "target " + std::to_string(target));
    }
}

TEST(SketchSolverTest, M5_REV4_002_ANonConvergedResultReportsAnUnmeasuredDof) {
    // Round 3 fixed this on the non-converged path and round 4 found it had no
    // test: deleting the assignment left the whole suite green. 0 means FULLY
    // CONSTRAINED, and a solver that stopped short has measured nothing.
    SketchSolveProblem p = AnglePairProblem(kPi / 2.0);
    // Two disagreeing angle targets on the same pair: converges nowhere.
    SolveResidual clash = p.residuals.back();
    clash.target = kPi / 4.0;
    clash.sourceConstraint = static_cast<SketchConstraintId>(9);
    p.residuals.push_back(clash);

    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    ASSERT_FALSE(r) << "the fixture must not converge";
    EXPECT_EQ(r.degreesOfFreedom, kUnknownDegreesOfFreedom);
}

TEST(SketchSolverTest, M5_REV4_003_AContradictionIsNotBlamedOnTheIterationLimit) {
    // "did not converge within the iteration limit" was reported for a solver
    // that had refused to take a single step.
    //
    // SCOPE, stated because it is narrower than the fix: this covers the
    // RANK-DEFICIENT non-convergence branch, which reports a contradiction.
    // The other branch -- full-rank non-convergence, where the choice between
    // "iteration limit" and "could not improve on its last step" is actually
    // made -- has NO deterministic fixture I could build: a full-rank residual
    // system that reliably stalls is exactly what a working solver does not
    // produce on demand. A mutation hard-coding the iteration-limit message
    // therefore survives, and that gap is recorded in the review document
    // rather than papered over by renaming this test.
    SketchSolveProblem p = AnglePairProblem(kPi / 2.0);
    SolveResidual clash = p.residuals.back();
    clash.target = kPi / 4.0;
    clash.sourceConstraint = static_cast<SketchConstraintId>(9);
    p.residuals.push_back(clash);

    GaussNewtonSketchSolver solver;
    const SketchSolveResult r = solver.solve(p);
    ASSERT_FALSE(r);
    EXPECT_FALSE(r.message.empty());

    // Asserted UNCONDITIONALLY. The first draft only checked the message when
    // `iterations < kSolveMaxIterations`, so a fixture that ran to the limit
    // skipped the assertion entirely -- and a mutation that hard-coded the
    // iteration-limit message left it green. A test that opts out of its own
    // subject is not a test.
    ASSERT_LT(r.iterations, kSolveMaxIterations)
        << "this fixture must stop EARLY for the message to be checkable";
    EXPECT_EQ(r.status, SketchSolveStatus::Conflicting);
    EXPECT_EQ(r.message.find("iteration limit"), std::string::npos)
        << "blamed the iteration limit after " << r.iterations << " iterations: "
        << r.message;
    EXPECT_NE(r.message.find("contradictory"), std::string::npos) << r.message;
}

// --- Scale (spec 30: very small / large reasonable geometry) ---------------

TEST(SketchSolverTest, M5_SCALE_001_SolvesAcrossFourOrdersOfMagnitude) {
    GaussNewtonSketchSolver solver;
    for (double width : {0.1, 1.0, 100.0, 1000.0}) {
        const SketchSolveResult r = solver.solve(Rectangle(width, width / 2.0));
        ASSERT_TRUE(r) << "width " << width << " failed: " << r.message;
        EXPECT_EQ(r.degreesOfFreedom, 0);
        EXPECT_NEAR(r.values[2], width, width * 1e-9)
            << "width " << width << " did not converge to its target";
    }
}

} // namespace
