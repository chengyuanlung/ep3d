#pragma once

#include "Core/Sketch/ISketchSolver.h"

namespace paramcad {

// Project-owned Gauss-Newton solver with Levenberg-Marquardt damping
// (ADR-M5-003). Eigen is used for dense linear algebra and, critically, for the
// rank-revealing decomposition that makes DOF a measured quantity rather than
// `variables - constraint count` (ADR-M5-005, spec 10).
//
// This header is deliberately free of Eigen: the include lives in the .cpp, so
// nothing that merely constructs a solver acquires an Eigen dependency, and the
// replacement boundary stays exactly `ISketchSolver`.
class GaussNewtonSketchSolver final : public ISketchSolver {
public:
    SketchSolveResult solve(const SketchSolveProblem& problem) override;
};

} // namespace paramcad
