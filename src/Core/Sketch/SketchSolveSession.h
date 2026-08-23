#pragma once

#include "Core/Sketch/ISketchSolver.h"
#include "Core/Sketch/Sketch.h"
#include "Core/Sketch/SketchConstraint.h"
#include <string>
#include <vector>

namespace paramcad {

class ObjectRegistry;

// Translates a Sketch plus its constraints and bound Parameters into the
// solver-neutral problem, and commits a successful result back (M5-D/I).
//
// Kept out of Sketch itself so the sketch stays a plain semantic container: the
// translation needs the ObjectRegistry to resolve Parameters, and giving Sketch
// a registry dependency would make it a document-aware object rather than data.

struct BuildProblemResult {
    SketchSolveProblem problem;
    // Constraints that could not be translated -- unresolved entity or
    // sub-element reference, missing Parameter, incompatible unit, or a
    // non-finite/non-positive dimension. Non-empty means InvalidInput
    // (ADR-M5-005): validation happens BEFORE any solving.
    std::vector<SketchConstraintId> invalidConstraints;

    // DRIVEN dimensions (M17.19, ADR-M17-042): dimensional constraints that
    // MEASURE rather than drive, so no residual was emitted for them.
    //
    // Reported rather than left to the caller to find again by re-testing the
    // flag: two places deciding which constraints are driven is two places
    // that can disagree, and the disagreement would show as a dimension whose
    // number never updates or one that quietly constrains after all.
    std::vector<SketchConstraintId> drivenConstraints;

    std::string message;

    explicit operator bool() const noexcept { return invalidConstraints.empty(); }
};

// Variables are emitted per entity in ascending SketchEntityId order, and
// residuals per constraint in ascending SketchConstraintId order, so the
// problem is a deterministic function of the sketch's semantic content and not
// of its storage order (ADR-M5-001; spec 19 requires order independence, and
// building deterministically is how it is achieved rather than hoped for).
BuildProblemResult BuildSolveProblem(const Sketch& sketch, const ObjectRegistry& registry);

// Writes solved values back into the sketch's entities.
//
// Called ONLY after the caller has checked the result converts to true
// (ADR-M5-004). A failed solve must leave the sketch byte-for-byte unchanged,
// so this function is never reached on the failure path -- that ordering, not a
// flag inside this function, is what makes the commit transactional.
// False if the solved values would produce geometry the sketch's own validator
// rejects -- a line collapsed to zero length by a Coincident between its own
// endpoints, say. Nothing is written in that case: committing state that
// IsValidSketchGeometry refuses means the sketch holds data its own invariant
// forbids, which addEntity has always prevented on the creation path.
bool CommitSolvedGeometry(Sketch& sketch, const SketchSolveProblem& problem,
                          const SketchSolveResult& result);

} // namespace paramcad
