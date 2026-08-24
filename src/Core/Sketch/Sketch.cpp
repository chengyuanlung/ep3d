#include "Core/Sketch/Sketch.h"

#include <set>
#include "Core/Document/PartDocument.h"
#include <optional>
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Feature/RevolveFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Feature/EdgeDressFeatures.h"
#include "Core/Document/ObjectRegistry.h"
#include "Core/Recompute/RecomputeContext.h"
#include "Core/Sketch/SketchSolveSession.h"
#include <algorithm>
#include <cmath>
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

SketchEntityId Sketch::addEllipse(Vec2 center, double majorRadiusMm, double minorRadiusMm,
                                  double rotationRad) {
    return addEntity(SketchEllipse{center, majorRadiusMm, minorRadiusMm, rotationRad});
}

SketchEntityId Sketch::addEllipticalArc(Vec2 center, double majorRadiusMm, double minorRadiusMm,
                                        double rotationRad, double startParamRad,
                                        double endParamRad, bool counterClockwise) {
    return addEntity(SketchEllipticalArc{center, majorRadiusMm, minorRadiusMm, rotationRad,
                                         startParamRad, endParamRad, counterClockwise});
}

SketchEntityId Sketch::addSpline(std::vector<Vec2> points, bool closed) {
    return addEntity(SketchSpline{std::move(points), closed});
}

bool Sketch::isEntityFullyConstrained(SketchEntityId id) const noexcept {
    return fullyConstrainedEntities_.count(ToObjectId(id)) != 0;
}

bool Sketch::setEntityGeometry(SketchEntityId id, SketchGeometry geometry) {
    if (!IsValidSketchGeometry(geometry)) return false;
    for (SketchEntity& entity : entities_) {
        if (entity.id != id) continue;
        entity.geometry = std::move(geometry);
        // The solve is stale now, exactly as it is after any other edit.
        solveStatus_ = SketchSolveStatus::UnderConstrained;
        return true;
    }
    return false;
}

bool Sketch::setEntityConstruction(SketchEntityId id, bool construction) {
    for (SketchEntity& entity : entities_) {
        if (entity.id != id) continue;
        if (entity.construction == construction) return true;
        entity.construction = construction;
        // NOT a solve trigger. The flag changes what a PROFILE sees, and the
        // profile is rebuilt by whatever consumes it; the sketch's own solution
        // is identical either way.
        return true;
    }
    return false;
}

bool Sketch::isConstruction(SketchEntityId id) const noexcept {
    for (const SketchEntity& entity : entities_)
        if (entity.id == id) return entity.construction;
    return false;
}

bool Sketch::restoreEntity(SketchEntityId id, SketchGeometry geometry) {
    if (id == kInvalidSketchEntityId) return false;
    if (findEntity(id) != nullptr) return false; // duplicate id within this sketch
    entities_.push_back(SketchEntity{RestoreSketchEntityId(id), std::move(geometry)});
    return true;
}

SketchReferenceId Sketch::addReference(SketchGeometry geometry) {
    // Validated like an entity. A zero-length projected edge is not an
    // underlay a user could ever click, and letting it in would give Convert
    // something it must then refuse -- one check here beats a refusal later.
    if (!IsValidSketchGeometry(geometry)) return kInvalidSketchReferenceId;
    const SketchReferenceId id = NextSketchReferenceId();
    references_.push_back(SketchReference{id, std::move(geometry)});
    return id;
}

bool Sketch::restoreReference(SketchReferenceId id, SketchGeometry geometry) {
    if (id == kInvalidSketchReferenceId) return false;
    if (findReference(id) != nullptr) return false; // duplicate id within this sketch
    references_.push_back(SketchReference{RestoreSketchReferenceId(id), std::move(geometry)});
    return true;
}

const SketchReference* Sketch::findReference(SketchReferenceId id) const noexcept {
    for (const SketchReference& reference : references_)
        if (reference.id == id) return &reference;
    return nullptr;
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

bool Sketch::setConstraintDriven(SketchConstraintId id, bool driven) {
    for (SketchConstraint& constraint : constraints_) {
        if (constraint.id != id) continue;
        // A flag only a dimension can carry. Refused rather than ignored, so
        // the caller learns it asked for something meaningless instead of
        // believing it worked.
        if (driven && !IsDimensional(constraint.data)) return false;
        constraint.driven = driven;
        // The solve is invalidated: a constraint that stopped driving changes
        // what the problem IS, not merely what it evaluates to.
        solveStatus_ = SketchSolveStatus::UnderConstrained;
        degreesOfFreedom_ = kUnknownDegreesOfFreedom;
        // ...and nothing is known to be pinned any more. Leaving the old set
        // would paint an edited entity black on the strength of a measurement
        // taken before the edit.
        fullyConstrainedEntities_.clear();
        solveMessage_.clear();
        offendingConstraints_.clear();
        return true;
    }
    return false;
}

bool Sketch::isConstraintDriven(SketchConstraintId id) const noexcept {
    const SketchConstraint* constraint = findConstraint(id);
    return constraint != nullptr && constraint->driven;
}

bool Sketch::removeConstraint(SketchConstraintId id) {
    const auto it = std::find_if(constraints_.begin(), constraints_.end(),
                                 [id](const SketchConstraint& c) { return c.id == id; });
    if (it == constraints_.end()) return false;
    constraints_.erase(it);
    // A placement or a format whose dimension is gone describes nothing.
    // Leaving either would be written to the file and then silently re-attach
    // itself to whatever constraint later reused the id.
    (void)clearDimensionPlacement(id);
    formats_.erase(std::remove_if(formats_.begin(), formats_.end(),
                                  [id](const DimensionFormat& f) {
                                      return f.constraintId == id;
                                  }),
                   formats_.end());
    return true;
}

bool Sketch::setDimensionPlacement(SketchConstraintId constraintId, Vec2 labelMm) {
    const SketchConstraint* constraint = findConstraint(constraintId);
    if (constraint == nullptr) return false;
    if (!IsDimensional(constraint->data)) return false;
    if (!std::isfinite(labelMm.x) || !std::isfinite(labelMm.y)) return false;
    for (DimensionPlacement& placement : placements_) {
        if (placement.constraintId == constraintId) {
            placement.labelMm = labelMm;
            return true;
        }
    }
    placements_.push_back(DimensionPlacement{constraintId, labelMm});
    return true;
}

bool Sketch::clearDimensionPlacement(SketchConstraintId constraintId) {
    const auto it = std::find_if(
        placements_.begin(), placements_.end(),
        [constraintId](const DimensionPlacement& p) { return p.constraintId == constraintId; });
    if (it == placements_.end()) return false;
    placements_.erase(it);
    return true;
}

const Vec2* Sketch::dimensionPlacement(SketchConstraintId constraintId) const noexcept {
    for (const DimensionPlacement& placement : placements_)
        if (placement.constraintId == constraintId) return &placement.labelMm;
    return nullptr;
}

void Sketch::restoreDimensionPlacement(SketchConstraintId constraintId, Vec2 labelMm) {
    for (DimensionPlacement& placement : placements_) {
        if (placement.constraintId == constraintId) {
            placement.labelMm = labelMm;
            return;
        }
    }
    placements_.push_back(DimensionPlacement{constraintId, labelMm});
}

bool Sketch::setDimensionFormat(SketchConstraintId constraintId,
                               const DimensionFormat& format) {
    const SketchConstraint* constraint = findConstraint(constraintId);
    if (constraint == nullptr) return false;
    if (!IsDimensional(constraint->data)) return false;
    if (!std::isfinite(format.plusTolerance) || !std::isfinite(format.minusTolerance))
        return false;

    const auto it = std::find_if(
        formats_.begin(), formats_.end(),
        [constraintId](const DimensionFormat& f) { return f.constraintId == constraintId; });
    if (format.isDefault()) {
        if (it != formats_.end()) formats_.erase(it);
        return true;
    }
    DimensionFormat stored = format;
    stored.constraintId = constraintId;
    if (it != formats_.end()) *it = stored;
    else formats_.push_back(std::move(stored));
    return true;
}

const Sketch::DimensionFormat* Sketch::dimensionFormat(
    SketchConstraintId constraintId) const noexcept {
    for (const DimensionFormat& format : formats_)
        if (format.constraintId == constraintId) return &format;
    return nullptr;
}

void Sketch::restoreDimensionFormat(const DimensionFormat& format) {
    for (DimensionFormat& existing : formats_) {
        if (existing.constraintId == format.constraintId) {
            existing = format;
            return;
        }
    }
    formats_.push_back(format);
}

void Sketch::dropPlacementsWithoutConstraints() {
    const auto orphaned = [this](SketchConstraintId id) {
        const SketchConstraint* constraint = findConstraint(id);
        return constraint == nullptr || !IsDimensional(constraint->data);
    };
    placements_.erase(std::remove_if(placements_.begin(), placements_.end(),
                                     [&orphaned](const DimensionPlacement& placement) {
                                         return orphaned(placement.constraintId);
                                     }),
                      placements_.end());
    formats_.erase(std::remove_if(formats_.begin(), formats_.end(),
                                  [&orphaned](const DimensionFormat& format) {
                                      return orphaned(format.constraintId);
                                  }),
                   formats_.end());
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



RecomputeResult Sketch::reresolveTrackedFace(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        trackedFaceMessage_ = message;
        solveStatus_ = SketchSolveStatus::InvalidInput;
        solveMessage_ = message;
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel to find the tracked face on");
    if (!trackedFace_->createdBy.has_value())
        return fail("a tracked face must name the feature that made it");

    // The solid to look on: the feature the query names. Resolved through the
    // registry by id, never held as a pointer -- the feature is rebuilt on
    // every recompute and a remembered one would be last pass's.
    // Resolved through the registry by id, never held as a pointer -- the
    // feature is rebuilt on every recompute and a remembered one would be last
    // pass's. By CAPABILITY (ISolidFeature), not by naming the five concrete
    // kinds, for the reason ADR-M3-007 gives and ADR-M17-033 had to re-learn:
    // a list of types is a list that goes out of date.
    const ObjectRegistry& registry = context.registry;
    const std::optional<ObjectRegistry::ConstObjectRef> ref =
        registry.find(*trackedFace_->createdBy);
    const ISolidFeature* solid = nullptr;
    if (ref.has_value())
        if (auto* const* recomputable = std::get_if<const IRecomputable*>(&*ref))
            solid = dynamic_cast<const ISolidFeature*>(*recomputable);
    if (solid == nullptr)
        return fail("the feature this sketch's face belongs to is gone");
    if (solid->currentState() != ComputeState::Valid || !solid->currentShape().isValid())
        return fail("the feature this sketch's face belongs to has not built");

    const FaceQueryResult found = context.kernel->resolveFace(solid->currentShape(), *trackedFace_);
    if (!found.ok) {
        // LOUD. A tracked face that cannot be found is the topological-naming
        // failure this whole architecture exists to make visible: the honest
        // answer is that the sketch no longer knows where it is, and the wrong
        // answer -- keeping the old plane and carrying on -- is geometry
        // sitting somewhere the model no longer claims it belongs.
        return fail("this sketch's face could not be found: " + found.message);
    }

    const std::optional<SketchFrame> frame =
        SketchFrame::OnFace(found.face.point, found.face.normal);
    if (!frame) return fail("this sketch's face has no usable plane");

    setTrackedFaceResult(*frame, found.message);
    return {RecomputeStatus::Success, {}};
}

RecomputeResult Sketch::recompute(const RecomputeContext& context) {
    // --- Re-resolve the tracked face FIRST (M17.14, ADR-M17-036) ------------
    //
    // Before solving, because the solve works in (u,v) and the plane is what
    // turns (u,v) into a position: solving first and moving the plane after
    // would produce one recompute's worth of geometry on last recompute's
    // plane, and nothing downstream could tell.
    //
    // A sketch with no tracked face skips all of this and behaves exactly as
    // it did before -- which is every sketch on a world plane, and every
    // sketch made before M17.14.
    if (trackedFace_.has_value()) {
        const RecomputeResult moved = reresolveTrackedFace(context);
        if (moved.status != RecomputeStatus::Success) return moved;
    }

    // A sketch with no constraints is ordinary free geometry, not an error --
    // and every M4 document is exactly that, so this path is what keeps them
    // working unchanged.
    if (constraints_.empty()) {
        // NOTHING IS PINNED without a constraint, and the empty case below
        // reaches Solved without going near the solver -- so the set has to be
        // cleared here rather than only on the solved path.
        fullyConstrainedEntities_.clear();
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
        //
        // ...MINUS THE EQUATIONS THE PROBLEM CARRIES ANYWAY (M18).
        //
        // Not every residual comes from a constraint. An arc's tips, an
        // elliptical arc's tips and a spline handle's tip are each DEFINED by
        // an equation the builder emits whether or not the user constrained
        // anything, and those equations take freedom away exactly as a
        // constraint would. Counting only the variables reported a bare arc as
        // nine free scalars when it has five, and it had done so since arcs
        // grew tips -- invisible until a spline handle made the gap four.
        //
        // The count IS the rank here: every one of these ties owns a tip
        // variable that no other residual mentions, so no two can be multiples
        // of one another. That is a property of how they are built, and
        // M18_HAN_004 holds the builder to it by comparing this number against
        // the one the solver measures from the Jacobian.
        const BuildProblemResult free = BuildSolveProblem(*this, context.registry);
        degreesOfFreedom_ = std::max(0, static_cast<int>(free.problem.variables.size()) -
                                            static_cast<int>(free.problem.residuals.size()));
        solveMessage_.clear();
        offendingConstraints_.clear();
        return {RecomputeStatus::Success, {}};
    }

    const auto fail = [this](SketchSolveStatus status, std::string message,
                             std::vector<SketchConstraintId> offenders) {
        // A FAILED SOLVE HAS MEASURED NOTHING, so it knows nothing about which
        // entities are pinned -- the same reason degreesOfFreedom_ is left
        // alone below rather than set to 0.
        fullyConstrainedEntities_.clear();
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
    // --- DRIVEN dimensions read the result (M17.19, ADR-M17-042) -----------
    //
    // AFTER the commit, because a driven dimension reports what the geometry
    // IS and the geometry is only final now. Measuring before the commit would
    // publish the number from the previous solve, and it would be a plausible
    // number -- the worst kind of wrong.
    //
    // The list comes from the session, which is the one place that decided
    // which constraints were driven. Re-testing the flag here would be a
    // second opinion that can drift from the one the residuals were built on.
    for (SketchConstraintId id : built.drivenConstraints) {
        const SketchConstraint* constraint = findConstraint(id);
        if (constraint == nullptr) continue;
        const ObjectId parameterId = BoundParameterId(constraint->data);
        if (parameterId == kInvalidObjectId) continue;
        const std::optional<double> measured = MeasureConstraint(*this, constraint->data);
        if (!measured.has_value()) continue; // geometry it cannot measure: leave the last value
        // Written through the DOCUMENT, so the parameter's dependents are
        // dirtied by the same machinery an ordinary edit uses -- an expression
        // reading a reference dimension has to see the new number.
        context.part().setDrivenParameterValue(parameterId, *measured);
    }

    solveStatus_ = result.status;
    degreesOfFreedom_ = result.degreesOfFreedom;
    solveMessage_ = result.message;
    offendingConstraints_.clear();

    // --- WHICH entities came out pinned (M17.29) ---------------------------
    //
    // An entity is fully constrained when NONE of its variables is free. Built
    // by elimination rather than by counting, because the number of variables
    // an entity has is a property of its type -- and a spline's is not, which
    // is the case that made a count wrong.
    fullyConstrainedEntities_.clear();
    if (result.variableIsFree.size() == built.problem.variables.size()) {
        std::set<ObjectId> loose;
        for (std::size_t i = 0; i < built.problem.variables.size(); ++i)
            if (result.variableIsFree[i])
                loose.insert(ToObjectId(built.problem.variables[i].entityId));
        for (const SketchEntity& entity : entities_)
            if (loose.count(ToObjectId(entity.id)) == 0)
                fullyConstrainedEntities_.insert(ToObjectId(entity.id));
    }
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
