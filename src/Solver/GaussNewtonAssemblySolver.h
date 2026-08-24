#pragma once

#include "Core/Assembly/IAssemblySolver.h"

namespace paramcad {

// Levenberg-Marquardt least squares with a NUMERICAL Jacobian (M25).
//
// Numerical rather than analytic, and that is a decision rather than a
// shortcut: the residual is a walk over the whole kinematic chain, so an
// analytic Jacobian would be a second derivation of the chain that has to
// agree with the first -- the exact defect class this project keeps removing.
// Assemblies have a handful of unknowns, so the cost of differencing is
// nothing, and being right without a second source of truth is everything.
//
// The damping is what makes the ordinary case work rather than a refinement:
// an under-constrained mechanism is rank-deficient BY CONSTRUCTION, and plain
// Gauss-Newton diverges on those. Same reasoning, same shape, as
// GaussNewtonSketchSolver.
class GaussNewtonAssemblySolver final : public IAssemblySolver {
public:
    AssemblySolveResult solve(const AssemblySolveProblem& problem) override;
};

} // namespace paramcad
