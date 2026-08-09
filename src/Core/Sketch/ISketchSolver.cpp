#include "Core/Sketch/ISketchSolver.h"

namespace paramcad {

// Lives in Core, not in the solver backend. The names describe the STATUS ENUM,
// which is Core's, so every Core caller (Sketch::recompute's diagnostic message,
// the UI's status line) can use them whether or not a backend is linked in --
// and a second backend cannot rename Conflicting behind the first one's back.
const char* SolveStatusName(SketchSolveStatus status) noexcept {
    switch (status) {
        case SketchSolveStatus::Solved: return "Solved";
        case SketchSolveStatus::UnderConstrained: return "UnderConstrained";
        case SketchSolveStatus::OverConstrained: return "OverConstrained";
        case SketchSolveStatus::Conflicting: return "Conflicting";
        case SketchSolveStatus::InvalidInput: return "InvalidInput";
        case SketchSolveStatus::NumericalFailure: return "NumericalFailure";
    }
    return "Unknown";
}

} // namespace paramcad
