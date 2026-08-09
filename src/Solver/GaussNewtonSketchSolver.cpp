#include "Solver/GaussNewtonSketchSolver.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace paramcad {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Angle wrapped into [-pi, pi].
double WrapToPi(double radians) noexcept { return std::remainder(radians, 2.0 * kPi); }

// Whether a residual's VALUE lives on a circle. Only the derivative of such a
// residual needs special handling, and only inside ComputeJacobian.
constexpr bool IsAngular(SolveResidual::Kind kind) noexcept {
    return kind == SolveResidual::Kind::Angle;
}

// Below this, two points are "the same point" as far as a distance residual's
// derivative is concerned.
constexpr double kDegenerateSeparationMm = 1e-12;


using Vec = Eigen::VectorXd;
using Mat = Eigen::MatrixXd;

bool AllFinite(const Vec& v) noexcept {
    for (int i = 0; i < v.size(); ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

double ValueAt(const Vec& x, int index) noexcept {
    return index >= 0 && index < x.size() ? x[index] : 0.0;
}

// One residual's value at the given configuration.
//
// NOTE: the comment that stood here described `sin(actual - target)` for Angle,
// which ADR-M5-006 superseded -- and it went on describing it after the code
// changed. On a project that has already shipped a Critical because two
// comments described an algorithm the code did not implement, a comment left
// describing the REMOVED algorithm is the same hazard pointing the other way.
// Angle is now the wrapped angular difference; see the Angle case below.
double Evaluate(const SolveResidual& r, const Vec& x) {
    switch (r.kind) {
        case SolveResidual::Kind::PointsEqualU:
        case SolveResidual::Kind::PointsEqualV:
        case SolveResidual::Kind::LineHorizontal:
        case SolveResidual::Kind::LineVertical:
            return ValueAt(x, r.vars[1]) - ValueAt(x, r.vars[0]);

        case SolveResidual::Kind::FixedU:
        case SolveResidual::Kind::FixedV:
            return ValueAt(x, r.vars[0]) - r.target;

        case SolveResidual::Kind::Radius:
            return ValueAt(x, r.vars[0]) - r.target;

        case SolveResidual::Kind::Distance:
        case SolveResidual::Kind::Length: {
            const double du = ValueAt(x, r.vars[2]) - ValueAt(x, r.vars[0]);
            const double dv = ValueAt(x, r.vars[3]) - ValueAt(x, r.vars[1]);
            return std::sqrt(du * du + dv * dv) - r.target;
        }

        case SolveResidual::Kind::Angle: {
            // Slots: A.start.u, A.start.v, A.end.u, A.end.v,
            //        B.start.u, B.start.v, B.end.u, B.end.v.
            const double duA = ValueAt(x, r.vars[2]) - ValueAt(x, r.vars[0]);
            const double dvA = ValueAt(x, r.vars[3]) - ValueAt(x, r.vars[1]);
            const double duB = ValueAt(x, r.vars[6]) - ValueAt(x, r.vars[4]);
            const double dvB = ValueAt(x, r.vars[7]) - ValueAt(x, r.vars[5]);

            // The WRAPPED angular difference, not sin(). sin() is smooth
            // everywhere, which is why an earlier revision chose it -- but its
            // period is pi, so it cannot tell 30 degrees from 210, and telling
            // those apart is the entire justification ADR-M5-006 gives for a
            // DIRECTED angle. A residual that is smooth but measures the wrong
            // thing is worse than one with a single removable discontinuity.
            //
            // wrap() is smooth except exactly at a half-turn from the target,
            // where it jumps between +pi and -pi. That point is a measure-zero
            // starting configuration and the solve converges from either side
            // of it; in exchange the residual is zero exactly at the requested
            // angle (mod 2*pi) and has derivative 1 there, which is what makes
            // the rank -- and therefore the DOF -- come out right.
            // std::remainder, not a while-loop. `while (d > kPi) d -= 2*kPi`
            // is unbounded, and DimensionValueValid accepts ANY finite angle:
            // an Angle Parameter of 1e300 rad made recompute() never return,
            // because 2*pi is below the ULP of 1e300. 1e9 rad took 21.8 s.
            // remainder is O(1) and exact for every finite input.
            // The TARGET is wrapped first, then subtracted. Subtracting the raw
            // value and wrapping the whole expression loses the geometry
            // entirely once the target is large: at 1e9 rad the ULP of the
            // target exceeds the angular signal, the residual stops depending
            // on the coordinates, and the solver reported "constraints are
            // contradictory: no configuration satisfies them" for a system that
            // solves perfectly when the mathematically identical pre-wrapped
            // target is used. ADR-M5-014's own rule -- a false accusation of
            // contradiction is the damaging kind of wrong answer -- applies to
            // the very fix that replaced the unbounded-loop hang.
            return WrapToPi(std::atan2(dvB, duB) - std::atan2(dvA, duA) -
                            WrapToPi(r.target));
        }
    }
    return 0.0;
}

void EvaluateAll(const std::vector<SolveResidual>& residuals, const Vec& x, Vec& out) {
    for (std::size_t i = 0; i < residuals.size(); ++i)
        out[static_cast<int>(i)] = Evaluate(residuals[i], x);
}

// Jacobian by central differences.
//
// Analytic derivatives would be faster, but this milestone's systems are tiny
// (a constrained rectangle is 8 variables) and a hand-derived Jacobian is
// exactly the kind of code where a sign error produces a solver that converges
// to the wrong answer while every test that checks "did it converge" passes.
// Central differences are second-order accurate and cannot disagree with the
// residuals they are differentiating.
void ComputeJacobian(const std::vector<SolveResidual>& residuals, const Vec& x, Mat& jacobian) {
    Vec probe = x;
    Vec plus(static_cast<int>(residuals.size()));
    Vec minus(static_cast<int>(residuals.size()));

    for (int j = 0; j < x.size(); ++j) {
        // Step scaled to the variable's magnitude so it stays meaningful for
        // both a 0.001 mm coordinate and a 1000 mm one.
        const double step = 1e-7 * std::max(1.0, std::fabs(x[j]));
        probe[j] = x[j] + step;
        EvaluateAll(residuals, probe, plus);
        probe[j] = x[j] - step;
        EvaluateAll(residuals, probe, minus);
        probe[j] = x[j];

        for (std::size_t i = 0; i < residuals.size(); ++i) {
            const int row = static_cast<int>(i);
            double difference = plus[row] - minus[row];
            // An angular residual's VALUE jumps by 2*pi across the wrap point;
            // its GRADIENT does not. Differencing two independently-wrapped
            // values straddling that point yields a Jacobian entry of about
            // 2*pi/(2h) -- enormous and wrong -- so Levenberg-Marquardt rejects
            // every step and the solve stalls at iteration 1.
            //
            // Wrapping the DIFFERENCE instead recovers the true derivative: the
            // step is tiny, so the real change is tiny, and any 2*pi in it came
            // from the wrap rather than from the geometry.
            //
            // This replaces a nudge that rotated the starting guess off the
            // antipode. That nudge used a fixed 1e-6 rad guard band and a fixed
            // 1e-4 rad rotation, while the residual's usable angular resolution
            // is 1e-7*|coordinate|/length -- SCALE-DEPENDENT. Over a 760-case
            // grid it improved 131 configurations and BROKE 42, turning correct
            // Solved results into false "did not converge" failures for short
            // lines far from the sketch origin. Fixing the derivative instead
            // of dodging the point needs no tuning constant at all.
            if (IsAngular(residuals[i].kind)) difference = WrapToPi(difference);
            jacobian(row, j) = difference / (2.0 * step);
        }
    }
}

// Numerical rank via a rank-revealing decomposition with an explicit threshold.
// This is what makes DOF a measurement rather than `variables - constraints`
// (ADR-M5-005): a redundant-but-consistent constraint adds a row without adding
// rank, and a doubly-constrained direction shows up as rank deficiency.
int NumericalRank(const Mat& jacobian) {
    if (jacobian.rows() == 0 || jacobian.cols() == 0) return 0;
    Eigen::ColPivHouseholderQR<Mat> qr(jacobian);
    qr.setThreshold(kSolveRankThreshold);
    return static_cast<int>(qr.rank());
}

// Constraints whose residuals are still violated, for diagnostics. Reported in
// ascending id order so the message is deterministic.
std::vector<SketchConstraintId> OffendersOf(const std::vector<SolveResidual>& residuals,
                                            const Vec& values) {
    std::set<ObjectId> ids;
    for (std::size_t i = 0; i < residuals.size(); ++i) {
        if (std::fabs(values[static_cast<int>(i)]) <= kSolveResidualTolerance) continue;
        if (residuals[i].sourceConstraint == kInvalidSketchConstraintId) continue;
        ids.insert(ToObjectId(residuals[i].sourceConstraint));
    }
    std::vector<SketchConstraintId> out;
    out.reserve(ids.size());
    for (ObjectId id : ids) out.push_back(static_cast<SketchConstraintId>(id));
    return out;
}

SketchSolveResult Failure(const SketchSolveProblem& problem, SketchSolveStatus status,
                          std::string message) {
    SketchSolveResult result;
    // Values left at the initial configuration: a caller that ignores the
    // status still cannot commit garbage, though the contract is that nothing
    // is committed unless the result converts to true (ADR-M5-004).
    result.values = problem.initialValues;
    result.status = status;
    result.message = std::move(message);
    return result;
}

} // namespace

SketchSolveResult GaussNewtonSketchSolver::solve(const SketchSolveProblem& problem) {
    const int variableCount = static_cast<int>(problem.variables.size());
    const int residualCount = static_cast<int>(problem.residuals.size());

    if (variableCount == 0)
        return Failure(problem, SketchSolveStatus::InvalidInput, "no variables to solve for");
    if (problem.initialValues.size() != problem.variables.size())
        return Failure(problem, SketchSolveStatus::InvalidInput,
                       "initial value count does not match variable count");

    Vec x(variableCount);
    for (int i = 0; i < variableCount; ++i) {
        x[i] = problem.initialValues[static_cast<std::size_t>(i)];
        if (!std::isfinite(x[i]))
            return Failure(problem, SketchSolveStatus::InvalidInput,
                           "initial configuration contains a non-finite value");
    }

    for (const SolveResidual& residual : problem.residuals) {
        if (!std::isfinite(residual.target))
            return Failure(problem, SketchSolveStatus::InvalidInput,
                           "a constraint carries a non-finite target value");

        // Every slot the kind's formula reads must actually be filled, and must
        // name a real variable. Without this, a residual packed with fewer
        // variables than its formula consumes reads index -1, silently
        // evaluates something that is not the constraint, and converges to a
        // wrong answer while reporting Solved -- which is exactly how the Angle
        // residual shipped broken and passed 444 tests.
        const int required = SlotsRequired(residual.kind);
        for (int slot = 0; slot < required; ++slot) {
            const int index = residual.vars[static_cast<std::size_t>(slot)];
            if (index < 0 || index >= variableCount)
                return Failure(problem, SketchSolveStatus::InvalidInput,
                               "a constraint is missing a variable its equation needs");

            // ...and the variable must be the KIND of scalar the formula reads.
            // Arity alone let a mis-ORDERED Distance through -- all four slots
            // filled and in range -- which then reported Solved with a tiny
            // residual and geometry wrong by millimetres.
            if (problem.variables[static_cast<std::size_t>(index)].component !=
                SlotComponent(residual.kind, slot))
                return Failure(problem, SketchSolveStatus::InvalidInput,
                               "a constraint's variables are packed in the wrong order");
        }
    }

    // EVERYTHING BELOW INDEXES x[] BY SLOT, so it must come AFTER the guard.
    //
    // The degeneracy nudge was originally placed above it and read/wrote
    // `x[r.vars[2]]` raw. An under-packed Distance -- exactly what the guard
    // exists to refuse -- was dereferenced first: an assertion abort in Debug,
    // and a silent out-of-bounds read AND WRITE in Release, through an
    // interface whose header promises it never throws. The fix for one finding
    // re-opened the hole another finding's guard had just closed. Order is the
    // whole protection here, so it is stated rather than left to reading order.
    // Nudge a Distance/Length whose two points START exactly coincident.
    //
    // Its residual is sqrt(du^2 + dv^2) - target, whose central difference at
    // du = dv = 0 is |+h| - |-h| = 0: the entire Jacobian row is zeros, there
    // is no descent direction, and the rank test then reported CONFLICTING
    // ("no configuration satisfies them") for a system with an INFINITE
    // solution set -- any point at the requested distance will do. A false
    // accusation of contradiction is the damaging kind of wrong answer,
    // because no constraint is actually in conflict and the user has nothing
    // to act on. Two coincident points arrive by ordinary means: snapping,
    // duplication, a collapsed edit, an imported file.
    //
    // This perturbs the solver's STARTING GUESS, never stored geometry -- the
    // initial values are the solver's input, and any direction is as good as
    // any other for a configuration that constrains none.
    for (const SolveResidual& r : problem.residuals) {
        if (r.kind != SolveResidual::Kind::Distance && r.kind != SolveResidual::Kind::Length)
            continue;
        const double du = x[r.vars[2]] - x[r.vars[0]];
        const double dv = x[r.vars[3]] - x[r.vars[1]];
        if (std::hypot(du, dv) > kDegenerateSeparationMm) continue;
        const double nudge = std::max(kDegenerateSeparationMm, 1e-6 * std::fabs(r.target));
        x[r.vars[2]] += nudge; // break the tie along +u; direction is arbitrary
    }

    Vec residuals(residualCount);
    Mat jacobian(residualCount, variableCount);
    EvaluateAll(problem.residuals, x, residuals);
    if (!AllFinite(residuals))
        return Failure(problem, SketchSolveStatus::InvalidInput,
                       "the initial configuration produces a non-finite residual");

    double damping = 1e-6;
    int iteration = 0;
    bool stopEarly = false; // step size below tolerance, or no step accepted

    // Levenberg-Marquardt: Gauss-Newton with a damping term that grows when a
    // step is rejected. Plain Gauss-Newton diverges on rank-deficient systems,
    // which under-constrained sketches are BY CONSTRUCTION -- so the damping is
    // not a refinement here, it is what makes the ordinary case work.
    for (; iteration < kSolveMaxIterations; ++iteration) {
        if (residuals.lpNorm<Eigen::Infinity>() <= kSolveResidualTolerance) break;

        ComputeJacobian(problem.residuals, x, jacobian);
        if (!jacobian.allFinite())
            return Failure(problem, SketchSolveStatus::NumericalFailure,
                           "the Jacobian became non-finite during iteration");

        const Mat jtj = jacobian.transpose() * jacobian;
        const Vec jtr = jacobian.transpose() * residuals;

        bool stepAccepted = false;
        for (int attempt = 0; attempt < 12 && !stepAccepted; ++attempt) {
            Mat damped = jtj;
            damped.diagonal().array() += damping;
            const Vec step = damped.ldlt().solve(-jtr);
            if (!AllFinite(step)) {
                damping *= 10.0;
                continue;
            }

            const Vec candidate = x + step;
            if (!AllFinite(candidate)) {
                damping *= 10.0;
                continue;
            }

            Vec candidateResiduals(residualCount);
            EvaluateAll(problem.residuals, candidate, candidateResiduals);
            if (!AllFinite(candidateResiduals)) {
                damping *= 10.0;
                continue;
            }

            if (candidateResiduals.squaredNorm() <= residuals.squaredNorm()) {
                x = candidate;
                residuals = candidateResiduals;
                damping = std::max(damping / 10.0, 1e-12);
                stepAccepted = true;
                // Stop, but do not LIE about how far we got. Setting
                // iteration = kSolveMaxIterations to break out made a solve
                // that settled in 3 steps report 100, and made the failure
                // message blame an iteration limit the solver never reached.
                if (step.lpNorm<Eigen::Infinity>() <= kSolveStepTolerance) stopEarly = true;
            } else {
                damping *= 10.0;
            }
        }

        // Counted BEFORE the early-stop breaks: a solve that completed one
        // accepted step reported 0 iterations because these exits skipped the
        // increment the loop header performs.
        ++iteration;
        if (!stepAccepted) {
            stopEarly = true; // no damping value improved the residual
            break;
        }
        if (stopEarly) break;
    }

    SketchSolveResult result;
    result.values.assign(x.data(), x.data() + x.size());
    result.iterations = std::min(iteration, kSolveMaxIterations);
    const bool hitIterationLimit = !stopEarly && iteration >= kSolveMaxIterations;
    result.maxResidual = residuals.lpNorm<Eigen::Infinity>();

    if (!AllFinite(x))
        return Failure(problem, SketchSolveStatus::NumericalFailure,
                       "the solved configuration contains a non-finite value");

    // Rank at the SOLVED configuration, which is where the DOF question is
    // actually being asked.
    ComputeJacobian(problem.residuals, x, jacobian);
    const int rank = NumericalRank(jacobian);
    result.degreesOfFreedom = std::max(0, variableCount - rank);

    const bool converged = result.maxResidual <= kSolveResidualTolerance;
    const bool redundant = rank < residualCount;

    if (!converged) {
        // The DOF measured above describes the configuration the solver stopped
        // at, which is not a solution -- so it says nothing about the sketch's
        // freedom, and 0 would read as "fully constrained". The header promises
        // a failed result carries kUnknownDegreesOfFreedom; only the Failure()
        // paths honoured it, and this one did not.
        result.degreesOfFreedom = kUnknownDegreesOfFreedom;
        // Did not converge. Rank-deficient in the direction of the violation
        // means contradictory constraints; otherwise the solver simply failed
        // to get there. Borderline cases resolve toward Conflicting, because
        // reporting a real conflict as benign is the more damaging error
        // (ADR-M5-005).
        result.status =
            redundant ? SketchSolveStatus::Conflicting : SketchSolveStatus::NumericalFailure;
        result.offendingConstraints = OffendersOf(problem.residuals, residuals);
        // Say which one actually happened. Reporting an iteration limit for a
        // solver that stalled after one step sends the reader looking for a
        // slow solve that never occurred.
        result.message =
            redundant ? "constraints are contradictory: no configuration satisfies them"
                      : (hitIterationLimit
                             ? "the solver did not converge within the iteration limit"
                             : "the solver could not improve on its last step");
        return result;
    }

    // DOF OUTRANKS REDUNDANCY. A sketch that still has free degrees is
    // under-constrained, whatever else is true of it. Reporting OverConstrained
    // here -- as an earlier revision did, because the redundancy branch came
    // first -- told the user "too many constraints" about a sketch that needs
    // MORE, and hid the free degrees behind a status that implies none remain.
    // Redundancy only becomes the headline once there is no freedom left.
    if (result.degreesOfFreedom > 0) {
        result.status = SketchSolveStatus::UnderConstrained;
        if (redundant)
            result.message = "under-constrained, and some constraints are redundant";
        return result;
    }

    if (redundant) {
        // Converged despite redundant rows: the extra constraints agree with
        // the others. Accepted and committed, not rejected -- rejecting would
        // make ordinary modelling fail for no user-visible reason (ADR-M5-005).
        result.status = SketchSolveStatus::OverConstrained;
        result.message = "constraints are redundant but consistent";
        return result;
    }

    result.status = SketchSolveStatus::Solved;
    return result;
}

} // namespace paramcad
