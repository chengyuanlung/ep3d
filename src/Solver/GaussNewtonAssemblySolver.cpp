#include "Solver/GaussNewtonAssemblySolver.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>

namespace paramcad {

namespace {

using Vec = Eigen::VectorXd;
using Mat = Eigen::MatrixXd;

// Angles are radians and lengths are millimetres in the same vector, so one
// absolute tolerance covers both: 1e-9 rad is a nanoradian and 1e-9 mm is a
// picometre. Both are far below anything a CAD model means.
constexpr double kResidualTolerance = 1e-9;
constexpr double kStepTolerance = 1e-12;
constexpr int kMaxIterations = 200;
// Relative to the variable, so a step is meaningful whether the unknown is an
// angle near 1 or a travel near 500.
constexpr double kDifferenceStep = 1e-7;
constexpr double kRankThreshold = 1e-9;

bool AllFinite(const Vec& v) { return v.allFinite(); }

void Evaluate(const AssemblySolveProblem& problem, const Vec& x, Vec& into) {
    problem.evaluate(x.data(), into.data());
}

// Central differences. One-sided would be half the cost and would also make the
// Jacobian wrong to first order exactly where the residual is curving, which
// near a mechanism's dead point is everywhere that matters.
void ComputeJacobian(const AssemblySolveProblem& problem, const Vec& x, Mat& jacobian) {
    const Eigen::Index unknowns = x.size();
    const Eigen::Index rows = jacobian.rows();
    Vec plus(rows);
    Vec minus(rows);
    Vec probe = x;
    for (Eigen::Index i = 0; i < unknowns; ++i) {
        const double step = kDifferenceStep * std::max(1.0, std::fabs(x[i]));
        probe[i] = x[i] + step;
        Evaluate(problem, probe, plus);
        probe[i] = x[i] - step;
        Evaluate(problem, probe, minus);
        probe[i] = x[i];
        jacobian.col(i) = (plus - minus) / (2.0 * step);
    }
}

int NumericalRank(const Mat& jacobian) {
    if (jacobian.rows() == 0 || jacobian.cols() == 0) return 0;
    Eigen::ColPivHouseholderQR<Mat> qr(jacobian);
    qr.setThreshold(kRankThreshold);
    return static_cast<int>(qr.rank());
}

} // namespace


AssemblySolveResult GaussNewtonAssemblySolver::solve(const AssemblySolveProblem& problem) {
    AssemblySolveResult result;
    result.values = problem.initial;

    const Eigen::Index unknowns = static_cast<Eigen::Index>(problem.initial.size());
    const Eigen::Index rows = static_cast<Eigen::Index>(problem.residualCount);
    if (rows == 0) {
        // Nothing to satisfy. Every unknown is free, which is a complete and
        // correct answer rather than a failure.
        result.degreesOfFreedom = static_cast<int>(unknowns);
        return result;
    }
    if (!problem.evaluate) {
        result.status = AssemblySolveStatus::InvalidInput;
        result.message = "the problem carries no residual function";
        return result;
    }

    Vec x = Eigen::Map<const Vec>(problem.initial.data(), unknowns);
    Vec residuals(rows);
    Evaluate(problem, x, residuals);
    if (!AllFinite(residuals)) {
        result.status = AssemblySolveStatus::InvalidInput;
        result.message = "the starting configuration produces a non-finite residual";
        return result;
    }

    Mat jacobian(rows, unknowns);
    double damping = 1e-9;
    int iteration = 0;

    for (; iteration < kMaxIterations; ++iteration) {
        if (residuals.lpNorm<Eigen::Infinity>() <= kResidualTolerance) break;
        if (unknowns == 0) break; // nothing to move; the loop simply does not close

        ComputeJacobian(problem, x, jacobian);
        if (!jacobian.allFinite()) {
            result.status = AssemblySolveStatus::InvalidInput;
            result.message = "the Jacobian became non-finite during iteration";
            result.residualNorm = residuals.lpNorm<Eigen::Infinity>();
            return result;
        }

        const Mat jtj = jacobian.transpose() * jacobian;
        const Vec jtr = jacobian.transpose() * residuals;
        // The SIZE of the normal equations, so the damping below is a fraction
        // of them rather than a number in whatever units this assembly is in.
        const double normalScale = std::max(jtj.diagonal().maxCoeff(), 1e-12);

        bool stepAccepted = false;
        for (int attempt = 0; attempt < 24 && !stepAccepted; ++attempt) {
            Mat damped = jtj;
            damped.diagonal().array() += damping * normalScale;
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
            Vec trial(rows);
            Evaluate(problem, candidate, trial);
            if (!AllFinite(trial)) {
                damping *= 10.0;
                continue;
            }
            if (trial.squaredNorm() < residuals.squaredNorm()) {
                x = candidate;
                residuals = trial;
                damping = std::max(damping * 0.1, 1e-12);
                stepAccepted = true;
                if (step.lpNorm<Eigen::Infinity>() <= kStepTolerance) {
                    iteration = kMaxIterations; // converged in step, not residual
                }
            } else {
                damping *= 10.0;
            }
        }
        if (!stepAccepted) break;
    }

    result.iterations = std::min(iteration, kMaxIterations);
    result.residualNorm = rows > 0 ? residuals.lpNorm<Eigen::Infinity>() : 0.0;
    result.values.assign(x.data(), x.data() + unknowns);

    // MEASURED, not counted. A planar four-bar writes five equations per
    // loop-closing mate and three of them are identically zero at every
    // configuration -- so unknowns-minus-equations would say the mechanism is
    // over-constrained when it turns perfectly well.
    if (unknowns > 0) {
        ComputeJacobian(problem, x, jacobian);
        if (jacobian.allFinite())
            result.degreesOfFreedom = static_cast<int>(unknowns) - NumericalRank(jacobian);
    }

    if (result.residualNorm > kResidualTolerance) {
        // TWO DIFFERENT FAILURES, and telling them apart is the whole value of
        // the message. A search that stalled with a descent direction still
        // available ran out of iterations; one that stalled because every
        // direction makes it worse has found the best configuration there is,
        // and it does not close -- there is no such assembly.
        result.status = iteration >= kMaxIterations ? AssemblySolveStatus::NotConverged
                                                    : AssemblySolveStatus::Contradictory;
    }
    return result;
}

} // namespace paramcad
