#pragma once

#include "Core/Sketch/SketchConstraint.h"
#include <array>
#include "Core/Sketch/SketchTypes.h"
#include <cstddef>
#include <string>
#include <vector>

namespace paramcad {

// The solver boundary (ADR-M5-003). Everything in this header is Core-side and
// free of Eigen and of any backend type: the problem carries variables and
// residual definitions, the result carries values, a status and a DOF.
//
// Eigen appears in exactly one translation unit behind this interface. Swapping
// in a different backend (PlaneGCS, SolveSpace) means writing a second
// ISketchSolver and changing nothing else -- the same shape as IGeometryKernel
// and its OCCT implementation (ADR-M3-001).

// What a variable in the solve corresponds to. Solver variable INDICES are an
// internal numbering rebuilt on every solve and are never persisted
// (ADR-M5-001) -- this struct is how an index is mapped back to semantics
// afterwards.
struct SolveVariable {
    SketchEntityId entityId{kInvalidSketchEntityId};
    SketchSubElement subElement{SketchSubElement::Whole};
    // Which scalar of that sub-element: u/v for points, or the radius of a
    // circle or arc.
    enum class Component { U, V, Radius } component{Component::U};
};

// One scalar equation the solver must drive to zero. Residuals are supplied as
// definitions rather than callbacks so the problem stays a plain value type
// that can be built, inspected and compared in tests without a solver present.
// "Not measured". A failed solve learns nothing new about the sketch's freedom,
// and 0 is this project's signal for FULLY CONSTRAINED (spec 10, Gate A) -- so
// a sketch whose very first solve failed must not report 0, which is what a
// zero-initialised member did.
inline constexpr int kUnknownDegreesOfFreedom = -1;

struct SolveResidual {
    enum class Kind {
        PointsEqualU,   // a.u - b.u                                    (2 slots)
        PointsEqualV,   // a.v - b.v                                    (2 slots)
        LineHorizontal, // end.v - start.v                              (2 slots)
        LineVertical,   // end.u - start.u                              (2 slots)
        FixedU,         // point.u - target                             (1 slot)
        FixedV,         // point.v - target                             (1 slot)
        Distance,       // |b - a| - target                             (4 slots)
        Length,         // |end - start| - target                       (4 slots)
        Radius,         // radius - target                              (1 slot)
        // wrap(atan2(dvB, duB) - atan2(dvA, duA) - target) into (-pi, pi],
        // where each line's direction is end - start (ADR-M5-006). Needs BOTH
        // components of BOTH endpoints of BOTH lines: 8 slots.
        Angle
    };

    Kind kind{Kind::PointsEqualU};

    // Variable indices into SketchSolveProblem::variables, in the order the
    // residual's formula consumes them; -1 for a slot this kind does not use.
    //
    // EIGHT slots, not four. Four is how many a line-to-line angle needs for
    // each line alone, and an earlier revision -- unable to express the
    // constraint in the four slots it had -- packed only the two lines' v
    // components and evaluated sin(dvB - dvA - target). That subtracts a
    // millimetre difference from a radian target: it is not the angle equation,
    // it converged to a residual of 4e-11, and it reported Solved while
    // producing an angle wrong by up to 260 degrees. The type could not say
    // what the constraint meant, so the code said something else.
    //
    // SlotsRequired() below states each kind's arity, and the solver REJECTS a
    // problem whose residual is missing a slot it needs, so a future kind
    // cannot quietly compute nonsense from whatever happens to be packed.
    static constexpr int kMaxSlots = 8;
    std::array<int, kMaxSlots> vars{{-1, -1, -1, -1, -1, -1, -1, -1}};

    double target{0.0};
    // Which constraint produced this residual, so a failure can name it
    // (spec 25's Gate E requires a useful constraint diagnostic).
    SketchConstraintId sourceConstraint{kInvalidSketchConstraintId};
};

// How many leading slots of SolveResidual::vars a kind actually reads. Declared
// here, next to the enum, so the arity is part of the interface rather than an
// assumption each backend makes privately.
// Which COMPONENT each slot of a kind must name. The arity check alone cannot
// tell a correctly-packed Distance from one packed (a.u, b.u, a.v, b.v): all
// four slots are filled and in range, so it passes -- and then reports Solved
// with a residual of 2.7e-12 while the geometry is wrong by 2.8 mm. That is
// character-for-character the failure that shipped as C1.
//
// Every variable already carries its Component, so the solver can check the
// packing means what the formula assumes rather than trusting the caller.
constexpr SolveVariable::Component SlotComponent(SolveResidual::Kind kind, int slot) noexcept {
    using C = SolveVariable::Component;
    switch (kind) {
        case SolveResidual::Kind::PointsEqualU:
        case SolveResidual::Kind::LineVertical:
        case SolveResidual::Kind::FixedU:
            return C::U;
        case SolveResidual::Kind::PointsEqualV:
        case SolveResidual::Kind::LineHorizontal:
        case SolveResidual::Kind::FixedV:
            return C::V;
        case SolveResidual::Kind::Radius:
            return C::Radius;
        case SolveResidual::Kind::Distance:
        case SolveResidual::Kind::Length:
            // (a.u, a.v, b.u, b.v)
            return (slot % 2 == 0) ? C::U : C::V;
        case SolveResidual::Kind::Angle:
            // (A.start.u, A.start.v, A.end.u, A.end.v, B.start.u, ... )
            return (slot % 2 == 0) ? C::U : C::V;
    }
    return C::U;
}

constexpr int SlotsRequired(SolveResidual::Kind kind) noexcept {
    switch (kind) {
        case SolveResidual::Kind::FixedU:
        case SolveResidual::Kind::FixedV:
        case SolveResidual::Kind::Radius:
            return 1;
        case SolveResidual::Kind::PointsEqualU:
        case SolveResidual::Kind::PointsEqualV:
        case SolveResidual::Kind::LineHorizontal:
        case SolveResidual::Kind::LineVertical:
            return 2;
        case SolveResidual::Kind::Distance:
        case SolveResidual::Kind::Length:
            return 4;
        case SolveResidual::Kind::Angle:
            return 8;
    }
    return 0;
}

struct SketchSolveProblem {
    std::vector<SolveVariable> variables;
    std::vector<double> initialValues; // parallel to variables
    std::vector<SolveResidual> residuals;
};

// Mapping documented in ADR-M5-005 so it can be checked rather than inferred.
enum class SketchSolveStatus {
    Solved,            // converged, DOF == 0
    UnderConstrained,  // converged, DOF > 0
    OverConstrained,   // redundant but CONSISTENT constraints
    Conflicting,       // residuals do not converge; contradictory constraints
    InvalidInput,      // detected before solving: bad reference, parameter or value
    NumericalFailure   // iteration limit, or a non-finite value during iteration
};

const char* SolveStatusName(SketchSolveStatus status) noexcept;

struct SketchSolveResult {
    std::vector<double> values; // parallel to the problem's variables
    SketchSolveStatus status{SketchSolveStatus::InvalidInput};
    // variables - rank(Jacobian), taken numerically at the solved
    // configuration. NEVER variables - residual count (ADR-M5-005).
    // Starts "not measured", not 0: 0 means FULLY CONSTRAINED here, and a
    // failed solve has measured nothing. Failure() leaves it at this value.
    int degreesOfFreedom{kUnknownDegreesOfFreedom};
    int iterations{0};
    double maxResidual{0.0};
    // Constraints implicated in a failure, for diagnostics. Empty on success.
    std::vector<SketchConstraintId> offendingConstraints;
    std::string message;

    explicit operator bool() const noexcept {
        return status == SketchSolveStatus::Solved ||
               status == SketchSolveStatus::UnderConstrained ||
               status == SketchSolveStatus::OverConstrained;
    }
};

class ISketchSolver {
public:
    virtual ~ISketchSolver() = default;

    // Never throws. A problem the solver cannot handle returns a result whose
    // status says why, with `values` left equal to the initial values so a
    // caller that ignores the status still cannot commit garbage -- though the
    // documented contract is that a caller commits nothing unless the result
    // converts to true (ADR-M5-004).
    virtual SketchSolveResult solve(const SketchSolveProblem& problem) = 0;
};

// Numerical policy (ADR-M5-003, spec 12). Documented here, applied by the
// implementation, and never enlarged merely to make a test pass.
inline constexpr double kSolveResidualTolerance = 1e-9;
inline constexpr double kSolveStepTolerance = 1e-12;
inline constexpr int kSolveMaxIterations = 100;
inline constexpr double kSolveRankThreshold = 1e-9;


} // namespace paramcad
