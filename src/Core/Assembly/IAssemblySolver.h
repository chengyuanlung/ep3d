#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

// THE ASSEMBLY SOLVER BOUNDARY (M25, ADR-M25-003).
//
// The same shape as ISketchSolver and IGeometryKernel: Core states the
// problem, one translation unit behind this interface knows about Eigen, and
// swapping the backend means writing a second implementation and changing
// nothing else.
//
// It differs from ISketchSolver in one deliberate way, and the reason matters.
// A sketch residual is a small algebraic KIND -- "these two points have the
// same u" -- so ISketchSolver takes a list of definitions and stays a plain
// value type. An assembly's residual is not like that: it is "walk the whole
// kinematic chain from the ground, composing transforms, and see how far the
// loop misses by". There is no small algebra to enumerate; the chain IS the
// equation. So the problem carries a callback, and what lives behind this
// interface is a general least-squares solver rather than a CAD-aware one.
//
// That is a real trade and it is worth naming: the problem can no longer be
// built and compared in a test without a solver present, and a residual cannot
// be inspected for what it MEANS. What is bought is that the mate model stays
// in one place (MateResiduals) instead of being restated as solver kinds.
struct AssemblySolveProblem {
    // Where the search starts. The current mate values, which for an assembly
    // being dragged are already close to the answer -- so a good seed is not a
    // convenience here, it is what picks WHICH of a mechanism's several valid
    // configurations comes back.
    std::vector<double> initial;
    std::size_t residualCount = 0;
    // Writes `residualCount` numbers, all of which the solve drives to zero.
    std::function<void(const double* values, double* residuals)> evaluate;
};

enum class AssemblySolveStatus {
    Solved,        // residuals within tolerance
    NotConverged,  // ran out of iterations still far from zero
    Contradictory, // converged to a residual that is not zero: no such assembly
    InvalidInput,  // a non-finite residual at the seed
};

// Inline, because Core states the problem AND reports the failure: a
// definition living in the solver library would make Core unable to say what
// went wrong without linking a backend it is defined not to need.
inline std::string_view toString(AssemblySolveStatus status) noexcept {
    switch (status) {
        case AssemblySolveStatus::Solved: return "solved";
        case AssemblySolveStatus::NotConverged: return "ran out of iterations";
        case AssemblySolveStatus::Contradictory: return "found no configuration that closes it";
        case AssemblySolveStatus::InvalidInput: return "could not start";
    }
    return "solved";
}

struct AssemblySolveResult {
    AssemblySolveStatus status = AssemblySolveStatus::Solved;
    std::vector<double> values;
    double residualNorm = 0.0;
    int iterations = 0;
    // Unknowns minus the rank of the Jacobian: how many freedoms the closed
    // loop actually leaves. MEASURED, not counted from unknowns-minus-equations
    // -- a redundant-but-consistent equation adds a row without adding rank,
    // and a planar four-bar has three of those (the out-of-plane residuals are
    // identically zero at every configuration).
    int degreesOfFreedom = 0;
    std::string message;

    explicit operator bool() const noexcept { return status == AssemblySolveStatus::Solved; }
};

class IAssemblySolver {
public:
    virtual ~IAssemblySolver() = default;
    virtual AssemblySolveResult solve(const AssemblySolveProblem& problem) = 0;
};

} // namespace paramcad
