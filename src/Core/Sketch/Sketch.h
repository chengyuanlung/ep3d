#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Recompute/IRecomputable.h"
#include "Core/Sketch/ISketchSolver.h"
#include "Core/Sketch/SketchConstraint.h"
#include "Core/Sketch/SketchFrame.h"
#include "Core/Sketch/SketchTypes.h"
#include <string>
#include <vector>

namespace paramcad {

// A 2D sketch: a support plane plus a set of stably-identified entities in
// sketch-local (u,v) millimetres (ADR-M4-001/002).
//
// A Sketch is a document object with an ObjectId, registered in ObjectRegistry
// and participating in the dependency graph. In M4 it was a DIRTY SOURCE like
// Parameter and Material, because it had no derived state of its own. Since M5
// it does: solved entity coordinates are derived from its constraints and their
// bound Parameters, so it is an IRecomputable whose recompute() solves. Editing
// it still dirties dependent PadFeatures exactly as before, and a sketch with
// no constraints still behaves as free geometry.
//
// Profile validation remains a pure function run inside PadFeature::recompute
// and is still deliberately not cached here (ADR-M4-005): what M5 adds is
// solved geometry, not a cached profile.
//
// Entities are sub-objects, NOT registered in ObjectRegistry: nothing outside
// their Sketch references an individual entity in M4.
class Sketch final : public IRecomputable {
public:
    explicit Sketch(std::string name);
    // Restore constructor (deserialization): keeps the persisted id and frame.
    Sketch(ObjectId id, std::string name, SketchFrame frame);

    ObjectId id() const noexcept override { return id_; }

    // Solves the sketch: builds the neutral problem from this sketch's
    // constraints and the document's Parameters, solves through the injected
    // ISketchSolver, and commits ONLY on success (ADR-M5-004).
    //
    // A sketch with no constraints succeeds without solving -- it is ordinary
    // free geometry, not an error, and M4 documents that never carried a
    // constraint must keep working unchanged.
    RecomputeResult recompute(const RecomputeContext& context) override;

    // Result of the last solve, for the UI and for downstream diagnostics
    // (spec 18). Reset to a not-yet-solved state whenever constraints change.
    SketchSolveStatus solveStatus() const noexcept { return solveStatus_; }
    int degreesOfFreedom() const noexcept { return degreesOfFreedom_; }
    const std::string& solveMessage() const noexcept { return solveMessage_; }
    const std::vector<SketchConstraintId>& offendingConstraints() const noexcept {
        return offendingConstraints_;
    }
    const std::string& name() const noexcept { return name_; }

    const SketchFrame& frame() const noexcept { return frame_; }
    void setFrame(const SketchFrame& frame) noexcept { frame_ = frame; }

    // --- Entity creation ---------------------------------------------------
    // Each returns the new entity's stable id, or kInvalidSketchEntityId if the
    // geometry is invalid (IsValidSketchGeometry) -- invalid geometry is never
    // stored, so it cannot reach a profile through this path. The profile
    // validator re-checks anyway, because a restored entity can carry bad data
    // from a hand-edited document that never went through these methods.
    SketchEntityId addPoint(Vec2 position);
    SketchEntityId addLine(Vec2 start, Vec2 end);
    SketchEntityId addCircle(Vec2 center, double radiusMm);
    SketchEntityId addArc(Vec2 center, double radiusMm, double startAngleRad,
                          double endAngleRad, bool counterClockwise = true);

    // Restore path (deserialization): keeps the persisted entity id and
    // advances the generator past it. Rejects a duplicate id within this
    // sketch, and -- unlike add* -- accepts geometry the validator would
    // reject, so a document round-trips losslessly and the invalid entity
    // surfaces as a profile diagnostic rather than silently disappearing.
    bool restoreEntity(SketchEntityId id, SketchGeometry geometry);

    // Removes an entity AND every constraint referencing it (ADR-M5-009): a
    // constraint whose geometry is gone has nothing to constrain, and leaving
    // it would be a dangling reference no UI could reach. False if this sketch
    // owns no entity with that id.
    //
    // Prefer PartDocument::removeSketchEntity: this overload cannot drop the
    // graph edges of the Parameters the cascaded constraints released.
    bool removeEntity(SketchEntityId id);

    struct EntityRemoval {
        bool removed = false;
        std::vector<SketchConstraintId> removedConstraints;
        // Parameters the removed constraints were bound to, so the caller can
        // re-evaluate the sketch's graph edges. May contain duplicates and may
        // list a Parameter another surviving constraint still binds -- the
        // caller re-checks rather than trusting this as a removal list.
        std::vector<ObjectId> releasedParameters;
    };
    EntityRemoval removeEntityCascading(SketchEntityId id);

    // Lookup is always by id, never by position (ADR-M4-001): removal does not
    // renumber anything, and no caller may treat an index as identity.
    const SketchEntity* findEntity(SketchEntityId id) const noexcept;

    const std::vector<SketchEntity>& entities() const noexcept { return entities_; }

    // Replaces an entity's geometry, keeping its identity. This is how solved
    // coordinates are written back (M5): the entity is the same entity, so its
    // SketchEntityId must not change and no constraint referencing it may be
    // disturbed. False if no entity has that id.
    bool replaceGeometry(SketchEntityId id, SketchGeometry geometry);

    // --- Constraints (M5, ADR-M5-001) --------------------------------------
    // Sub-objects of this sketch, identified by SketchConstraintId, never by
    // position. Adding, removing or reordering a constraint must not disturb
    // any other constraint's identity or any entity's.

    // Returns the new constraint's stable id, or kInvalidSketchConstraintId if
    // the data is structurally unusable (a reference to an entity this sketch
    // does not own). Semantic validation -- parameter units, dimension values,
    // sub-element applicability -- happens when the solve problem is built,
    // because it needs the document's Parameters.
    SketchConstraintId addConstraint(SketchConstraintData data);

    // Restore path (deserialization): keeps the persisted id and advances the
    // generator past it. Rejects a duplicate id within this sketch.
    bool restoreConstraint(SketchConstraintId id, SketchConstraintData data);

    bool removeConstraint(SketchConstraintId id);
    const SketchConstraint* findConstraint(SketchConstraintId id) const noexcept;
    const std::vector<SketchConstraint>& constraints() const noexcept { return constraints_; }

    // Constraints referencing an entity, so removing an entity can deal with
    // them deterministically instead of leaving dangling references (spec 17).
    std::vector<SketchConstraintId> constraintsReferencing(SketchEntityId entityId) const;

private:
    SketchEntityId addEntity(SketchGeometry geometry);

    ObjectId id_;
    std::string name_;
    SketchFrame frame_{};
    std::vector<SketchEntity> entities_;
    std::vector<SketchConstraint> constraints_;

    // Last solve outcome. Not persisted: solver state is never identity
    // (ADR-M5-001) and a reloaded sketch re-solves before anything reads it.
    SketchSolveStatus solveStatus_{SketchSolveStatus::UnderConstrained};
    int degreesOfFreedom_{kUnknownDegreesOfFreedom};
    std::string solveMessage_;
    std::vector<SketchConstraintId> offendingConstraints_;
};

} // namespace paramcad
