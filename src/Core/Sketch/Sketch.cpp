#include "Core/Sketch/Sketch.h"
#include "Core/Recompute/RecomputeContext.h"
#include "Core/Sketch/SketchSolveSession.h"
#include <algorithm>
#include <variant>
#include <utility>

namespace paramcad {

Sketch::Sketch(std::string name)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)) {}

Sketch::Sketch(ObjectId id, std::string name, SketchFrame frame)
    : id_(RestoreObjectId(id)), name_(std::move(name)), frame_(frame) {}

SketchEntityId Sketch::addEntity(SketchGeometry geometry) {
    if (!IsValidSketchGeometry(geometry)) return kInvalidSketchEntityId;
    const SketchEntityId id = NextSketchEntityId();
    entities_.push_back(SketchEntity{id, std::move(geometry)});
    return id;
}

SketchEntityId Sketch::addPoint(Vec2 position) {
    return addEntity(SketchPoint{position});
}

SketchEntityId Sketch::addLine(Vec2 start, Vec2 end) {
    return addEntity(SketchLine{start, end});
}

SketchEntityId Sketch::addCircle(Vec2 center, double radiusMm) {
    return addEntity(SketchCircle{center, radiusMm});
}

SketchEntityId Sketch::addArc(Vec2 center, double radiusMm, double startAngleRad,
                              double endAngleRad, bool counterClockwise) {
    return addEntity(
        SketchArc{center, radiusMm, startAngleRad, endAngleRad, counterClockwise});
}

bool Sketch::restoreEntity(SketchEntityId id, SketchGeometry geometry) {
    if (id == kInvalidSketchEntityId) return false;
    if (findEntity(id) != nullptr) return false; // duplicate id within this sketch
    entities_.push_back(SketchEntity{RestoreSketchEntityId(id), std::move(geometry)});
    return true;
}

bool Sketch::removeEntity(SketchEntityId id) {
    return removeEntityCascading(id).removed;
}

Sketch::EntityRemoval Sketch::removeEntityCascading(SketchEntityId id) {
    const auto it = std::find_if(entities_.begin(), entities_.end(),
                                 [id](const SketchEntity& e) { return e.id == id; });
    if (it == entities_.end()) return EntityRemoval{};

    // CASCADE (ADR-M5-009). A constraint whose entity is gone has nothing left
    // to constrain; leaving it behind is a dangling reference that the solver
    // would report as InvalidInput on every subsequent recompute, forever, with
    // no way for the user to reach the constraint through the deleted geometry.
    EntityRemoval result;
    result.removed = true;
    result.removedConstraints = constraintsReferencing(id);
    for (SketchConstraintId constraintId : result.removedConstraints) {
        const SketchConstraint* constraint = findConstraint(constraintId);
        if (constraint == nullptr) continue;
        const ObjectId parameterId = BoundParameterId(constraint->data);
        if (parameterId != kInvalidObjectId) result.releasedParameters.push_back(parameterId);
    }
    for (SketchConstraintId constraintId : result.removedConstraints) removeConstraint(constraintId);

    // Erase the entity LAST: constraintsReferencing and findConstraint above
    // read it, and re-finding the iterator here is cheaper than reasoning about
    // whether it survived.
    const auto entity = std::find_if(entities_.begin(), entities_.end(),
                                     [id](const SketchEntity& e) { return e.id == id; });
    entities_.erase(entity);
    return result;
}

const SketchEntity* Sketch::findEntity(SketchEntityId id) const noexcept {
    const auto it = std::find_if(entities_.begin(), entities_.end(),
                                 [id](const SketchEntity& e) { return e.id == id; });
    return it != entities_.end() ? &*it : nullptr;
}



bool Sketch::replaceGeometry(SketchEntityId id, SketchGeometry geometry) {
    for (SketchEntity& entity : entities_) {
        if (entity.id != id) continue;
        entity.geometry = std::move(geometry);
        return true;
    }
    return false;
}

SketchConstraintId Sketch::addConstraint(SketchConstraintData data) {
    // Structural check only: every referenced entity must be in this sketch.
    // Unit and value validation needs the document's Parameters and therefore
    // happens when the solve problem is built.
    for (SketchEntityId referenced : ReferencedEntities(data))
        if (findEntity(referenced) == nullptr) return kInvalidSketchConstraintId;

    const SketchConstraintId id = NextSketchConstraintId();
    constraints_.push_back(SketchConstraint{id, std::move(data)});
    return id;
}

bool Sketch::restoreConstraint(SketchConstraintId id, SketchConstraintData data) {
    if (id == kInvalidSketchConstraintId) return false;
    if (findConstraint(id) != nullptr) return false; // duplicate id within this sketch
    constraints_.push_back(SketchConstraint{RestoreSketchConstraintId(id), std::move(data)});
    return true;
}

bool Sketch::removeConstraint(SketchConstraintId id) {
    const auto it = std::find_if(constraints_.begin(), constraints_.end(),
                                 [id](const SketchConstraint& c) { return c.id == id; });
    if (it == constraints_.end()) return false;
    constraints_.erase(it);
    return true;
}

const SketchConstraint* Sketch::findConstraint(SketchConstraintId id) const noexcept {
    const auto it = std::find_if(constraints_.begin(), constraints_.end(),
                                 [id](const SketchConstraint& c) { return c.id == id; });
    return it != constraints_.end() ? &*it : nullptr;
}

std::vector<SketchConstraintId> Sketch::constraintsReferencing(SketchEntityId entityId) const {
    std::vector<SketchConstraintId> ids;
    for (const SketchConstraint& constraint : constraints_) {
        for (SketchEntityId referenced : ReferencedEntities(constraint.data)) {
            if (referenced != entityId) continue;
            ids.push_back(constraint.id);
            break;
        }
    }
    return ids;
}



RecomputeResult Sketch::recompute(const RecomputeContext& context) {
    // A sketch with no constraints is ordinary free geometry, not an error --
    // and every M4 document is exactly that, so this path is what keeps them
    // working unchanged.
    if (constraints_.empty()) {
        // A sketch with no ENTITIES has nothing to be free, so it is solved --
        // trivially, but genuinely. Reporting "under-constrained, DOF 0" for it
        // is the self-contradictory reading ADR-M5-012 exists to remove, and an
        // empty sketch still produced it.
        if (entities_.empty()) {
            solveStatus_ = SketchSolveStatus::Solved;
            degreesOfFreedom_ = 0;
            solveMessage_.clear();
            offendingConstraints_.clear();
            return {RecomputeStatus::Success, {}};
        }
        solveStatus_ = SketchSolveStatus::UnderConstrained;
        // The free scalars really are free, so report how many. Hard-setting 0
        // here made every constraint-free sketch -- i.e. every M4 document --
        // read "Under-constrained, DOF 0", which is self-contradictory: 0 is
        // what a FULLY constrained sketch reports. Counting needs no solver,
        // only the variables the problem would have had.
        degreesOfFreedom_ =
            static_cast<int>(BuildSolveProblem(*this, context.registry).problem.variables.size());
        solveMessage_.clear();
        offendingConstraints_.clear();
        return {RecomputeStatus::Success, {}};
    }

    const auto fail = [this](SketchSolveStatus status, std::string message,
                             std::vector<SketchConstraintId> offenders) {
        solveStatus_ = status;
        solveMessage_ = message;
        offendingConstraints_ = std::move(offenders);
        // degreesOfFreedom_ keeps its previous value: a failed solve knows
        // nothing new about the sketch's freedom, and reporting 0 would read as
        // "fully constrained". On the FIRST solve there is no previous value,
        // which is why the member starts at kUnknownDegreesOfFreedom rather
        // than 0 -- it used to start at 0 and produce exactly the reading this
        // comment says to avoid.
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.sketchSolver == nullptr)
        return fail(SketchSolveStatus::InvalidInput, "no sketch solver configured", {});

    // Validation before any solving (ADR-M5-005): unresolved references,
    // missing Parameters, incompatible units and bad dimension values are all
    // InvalidInput, distinct from a solve that ran and failed.
    const BuildProblemResult built = BuildSolveProblem(*this, context.registry);
    if (!built)
        return fail(SketchSolveStatus::InvalidInput,
                    built.message.empty() ? "a constraint is invalid" : built.message,
                    built.invalidConstraints);

    const SketchSolveResult result = context.sketchSolver->solve(built.problem);
    if (!result)
        return fail(result.status,
                    result.message.empty() ? std::string(SolveStatusName(result.status))
                                           : result.message,
                    result.offendingConstraints);

    // Commit only now, after the result is known good (ADR-M5-004). Everything
    // above returns before touching a single coordinate.
    // A commit that refuses is a FAILED solve, not a silent no-op: the solver
    // found values the sketch's own invariant rejects, and reporting Solved
    // over unchanged geometry would be the worst of both.
    if (!CommitSolvedGeometry(*this, built.problem, result))
        return fail(SketchSolveStatus::NumericalFailure,
                    "the solved configuration is not valid sketch geometry", {});
    solveStatus_ = result.status;
    degreesOfFreedom_ = result.degreesOfFreedom;
    solveMessage_ = result.message;
    offendingConstraints_.clear();
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
