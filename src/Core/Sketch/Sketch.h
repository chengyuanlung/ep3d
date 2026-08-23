#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Recompute/IRecomputable.h"
#include "Core/Sketch/ISketchSolver.h"
#include "Core/Sketch/SketchConstraint.h"
#include "Core/Kernel/FaceQuery.h"
#include "Core/Sketch/SketchFrame.h"
#include "Core/Sketch/SketchTypes.h"
#include <optional>
#include <string>
#include <utility>
#include <set>
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

    // Whether the last solve pinned down EVERY scalar of this entity (M17.29).
    //
    // The sketch-wide status cannot answer this, and treating it as though it
    // could is what made a fully dimensioned rectangle draw as loose because a
    // spline shared its sketch. A sketch containing a spline never reaches DOF
    // 0 -- no constraint can name an interior point -- so per-sketch colour
    // stops meaning anything the moment there is one.
    //
    // FALSE when the answer is not known: a sketch that has not solved, or one
    // whose solve failed, has measured nothing. That is the safe direction --
    // "not known to be pinned" reads as loose, and claiming pinned would be
    // claiming a measurement nobody made.
    bool isEntityFullyConstrained(SketchEntityId id) const noexcept;
    const std::string& solveMessage() const noexcept { return solveMessage_; }
    const std::vector<SketchConstraintId>& offendingConstraints() const noexcept {
        return offendingConstraints_;
    }
    const std::string& name() const noexcept { return name_; }

    const SketchFrame& frame() const noexcept { return frame_; }
    void setFrame(const SketchFrame& frame) noexcept { frame_ = frame; }

    // --- Tracking a face (M17.14, ADR-M17-036) ------------------------------
    //
    // A sketch made ON A FACE can now NAME that face instead of freezing the
    // plane it happened to be on (which is what ADR-M17-028 had to settle for
    // -- there was no vocabulary for a face that survives a rebuild). With a
    // query set, the plane is re-resolved every recompute: make the pad
    // taller and the sketch rides up with the face it was drawn on.
    //
    // Absent means the M17.5 behaviour, and every sketch made before this: the
    // embedded frame is the whole story and nothing moves it. That is still
    // the right answer for a sketch on a world plane, which has no face to
    // track.
    //
    // The FRAME IS STILL KEPT and still updated in place, so everything that
    // reads a sketch's plane -- the profile builder, the canvas, the 3D
    // wireframe -- goes on reading one thing. The query decides what the frame
    // says; it does not become a second way to ask.
    const std::optional<FaceQuery>& trackedFace() const noexcept { return trackedFace_; }
    ObjectId trackedFaceOwner() const noexcept {
        return trackedFace_.has_value() && trackedFace_->createdBy.has_value()
                   ? *trackedFace_->createdBy
                   : kInvalidObjectId;
    }
    // The last thing re-resolution said, for the tree and the panel. Empty
    // until a recompute has tried.
    const std::string& trackedFaceMessage() const noexcept { return trackedFaceMessage_; }

    // --- Entity creation ---------------------------------------------------
    // Each returns the new entity's stable id, or kInvalidSketchEntityId if the
    // geometry is invalid (IsValidSketchGeometry) -- invalid geometry is never
    // stored, so it cannot reach a profile through this path. The profile
    // validator re-checks anyway, because a restored entity can carry bad data
    // from a hand-edited document that never went through these methods.
    SketchEntityId addPoint(Vec2 position);
    SketchEntityId addLine(Vec2 start, Vec2 end);
    SketchEntityId addCircle(Vec2 center, double radiusMm);
    // MAJOR MUST BE THE LONGER ONE. Refused rather than swapped: the rotation
    // is measured to the major axis, so quietly exchanging them would store the
    // same ellipse turned a quarter turn.
    SketchEntityId addEllipse(Vec2 center, double majorRadiusMm, double minorRadiusMm,
                              double rotationRad);
    // The two params are the ELLIPSE'S OWN parameter, not an angle from the
    // centre -- see SketchEllipticalArc.
    SketchEntityId addEllipticalArc(Vec2 center, double majorRadiusMm, double minorRadiusMm,
                                    double rotationRad, double startParamRad, double endParamRad,
                                    bool counterClockwise);
    // A smooth curve THROUGH `points`, in order. Refused with fewer than
    // kMinSplinePoints, or when two neighbours sit on top of each other.
    SketchEntityId addSpline(std::vector<Vec2> points, bool closed);
    SketchEntityId addArc(Vec2 center, double radiusMm, double startAngleRad,
                          double endAngleRad, bool counterClockwise = true);

    // Restore path (deserialization): keeps the persisted entity id and
    // advances the generator past it. Rejects a duplicate id within this
    // sketch, and -- unlike add* -- accepts geometry the validator would
    // reject, so a document round-trips losslessly and the invalid entity
    // surfaces as a profile diagnostic rather than silently disappearing.
    // --- Support frame (M10, ADR-M10-003) -----------------------------------
    //
    // OPTIONAL. kInvalidObjectId means "use the embedded SketchFrame", which is
    // every sketch written before M10 and every sketch the user has not put on
    // a frame. The embedded frame is not removed and not migrated: it is the
    // FALLBACK, exactly as `SketchFrame`'s own header predicted in M4.
    //
    // A reference by ObjectId (ADR-M4-004), never a pointer. The document owns
    // resolving it -- `PartDocument::effectiveSketchFrame` -- because only the
    // document can compose a frame's world transform, and a Sketch that knew
    // how to reach the document would be a Core layering inversion.
    ObjectId supportFrameId() const noexcept { return supportFrameId_; }

    bool restoreEntity(SketchEntityId id, SketchGeometry geometry);

    // Replaces an entity's geometry, keeping its id.
    //
    // The id is what every constraint on this entity holds, so an in-place edit
    // is the ONLY way to reshape geometry without silently discarding the
    // relationships attached to it. Rejects geometry the validator refuses, and
    // an id this sketch does not own.
    //
    // Does not solve and does not validate against the constraints: whether the
    // new shape is still reachable is the solver's answer to give, and giving
    // it here would be a second, quieter opinion.
    bool setEntityGeometry(SketchEntityId id, SketchGeometry geometry);

    // Marks an entity as construction geometry, or back. False for an id this
    // sketch does not own.
    //
    // A pure flag flip: no geometry moves, no constraint is touched, and the
    // solve is unaffected -- construction geometry is solved exactly like the
    // rest, because a centreline you cannot constrain is useless. What changes
    // is only whether BuildProfile counts its edge.
    bool setEntityConstruction(SketchEntityId id, bool construction);
    bool isConstruction(SketchEntityId id) const noexcept;

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

    // --- Projected reference geometry (M17.6, ADR-M17-029) -----------------
    //
    // See SketchReference. These are a tracing underlay: no variables, no
    // profile edge, no constraints. They are held apart from entities_ so that
    // every loop over entities_ -- solver, profile, serializer, canvas, undo --
    // keeps meaning exactly what it meant before this existed.

    // Adds one projected curve. Rejects geometry the validator refuses, so a
    // degenerate projection cannot become an underlay the user can trace.
    SketchReferenceId addReference(SketchGeometry geometry);

    // Restore path (deserialization): keeps the persisted id, advances the
    // generator past it, and -- like restoreEntity -- accepts geometry the
    // validator would reject so a document round-trips losslessly.
    bool restoreReference(SketchReferenceId id, SketchGeometry geometry);

    const std::vector<SketchReference>& references() const noexcept { return references_; }
    const SketchReference* findReference(SketchReferenceId id) const noexcept;

    // Drops every reference. Used when a sketch's underlay is replaced; there
    // is deliberately no remove-one, because a partial underlay is not a state
    // any command needs and every extra mutator is another thing to keep
    // consistent.
    void clearReferences() noexcept { references_.clear(); }

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

    // Marks a DIMENSIONAL constraint as driven -- it measures instead of
    // driving (M17.19, ADR-M17-042). False for an id this sketch does not own
    // and for a constraint that is not dimensional: "a driven Horizontal"
    // describes nothing, and accepting it would put a flag in the file that no
    // reader could act on.
    bool setConstraintDriven(SketchConstraintId id, bool driven);
    bool isConstraintDriven(SketchConstraintId id) const noexcept;
    const SketchConstraint* findConstraint(SketchConstraintId id) const noexcept;
    const std::vector<SketchConstraint>& constraints() const noexcept { return constraints_; }

    // Constraints referencing an entity, so removing an entity can deal with
    // them deterministically instead of leaving dangling references (spec 17).
    std::vector<SketchConstraintId> constraintsReferencing(SketchEntityId entityId) const;

    // --- Dimension placement (M16) -----------------------------------------
    //
    // Where the USER dragged a dimension's value, in sketch (u,v) millimetres.
    // A constraint with no entry here is placed automatically.
    //
    // WHY THIS IS DOCUMENT STATE AND NOT VIEW STATE, which is the decision that
    // matters: it must survive save and load, and it must be undoable. A user
    // who spends a minute arranging a drawing's dimensions and finds them
    // rearranged on reopen has lost real work -- unlike hide/show or the camera,
    // which roadmap section 15 correctly keeps OUT of the document.
    //
    // WHY IT IS NOT ON THE CONSTRAINT: placement says nothing about what the
    // dimension MEANS. Keeping it beside the constraints instead of inside them
    // leaves all five dimensional structs untouched, and guarantees the solver
    // can never see it -- the problem builder reads `constraints_` and nothing
    // else.
    //
    // A vector, not a map: the order is what the serializer writes, and a
    // std::map's iteration order is an implementation detail to depend on.
    struct DimensionPlacement {
        SketchConstraintId constraintId{kInvalidSketchConstraintId};
        Vec2 labelMm{};
    };

    // False if this sketch has no such constraint, or it is not dimensional --
    // placing a Horizontal has no meaning and silently storing one would leave
    // an entry nothing ever reads and the serializer still has to write.
    bool setDimensionPlacement(SketchConstraintId constraintId, Vec2 labelMm);
    // Back to automatic placement. False if there was nothing to clear.
    bool clearDimensionPlacement(SketchConstraintId constraintId);
    // Null when the dimension is automatically placed.
    const Vec2* dimensionPlacement(SketchConstraintId constraintId) const noexcept;
    const std::vector<DimensionPlacement>& dimensionPlacements() const noexcept {
        return placements_;
    }
    // --- Dimension format (M16) --------------------------------------------
    //
    // How a dimension's value READS: a prefix ("2x "), a suffix (" REF"), and a
    // tolerance. Document state for the same reason placement is -- it is the
    // drawing's content, not the camera.
    //
    // The tolerances are in the dimension's OWN unit: millimetres for a length,
    // radians for an angle, so they need no separate conversion story. Zero on
    // both means no tolerance is shown at all, which is different from an
    // explicit +0/-0 and is why they are not optional numbers.
    struct DimensionFormat {
        SketchConstraintId constraintId{kInvalidSketchConstraintId};
        std::string prefix;
        std::string suffix;
        double plusTolerance{0.0};
        double minusTolerance{0.0};

        bool isDefault() const noexcept {
            return prefix.empty() && suffix.empty() && plusTolerance == 0.0 &&
                   minusTolerance == 0.0;
        }
    };

    // Setting a DEFAULT format clears the entry: an entry that says "nothing
    // special" is one more thing to serialize and to keep in step.
    bool setDimensionFormat(SketchConstraintId constraintId, const DimensionFormat& format);
    const DimensionFormat* dimensionFormat(SketchConstraintId constraintId) const noexcept;
    const std::vector<DimensionFormat>& dimensionFormats() const noexcept { return formats_; }
    void restoreDimensionFormat(const DimensionFormat& format);

    // Restore path (deserialization): no validation against constraint kind,
    // because constraints and placements are loaded in separate passes and the
    // constraint may not be in place yet. A stale entry is dropped by
    // `dropPlacementsWithoutConstraints`, which the loader runs at the end.
    void restoreDimensionPlacement(SketchConstraintId constraintId, Vec2 labelMm);
    void dropPlacementsWithoutConstraints();

private:
    // PRIVATE with PartDocument as the only caller: setting the support frame
    // must add the frame -> sketch graph edge in the SAME step, or moving the
    // frame would leave the sketch clean and the geometry stale.
    friend class PartDocument;
    void setSupportFrameId(ObjectId frameId) noexcept { supportFrameId_ = frameId; }

    // Same rule, same reason (M17.14): a tracked face means this sketch now
    // depends on the feature that made it, and that graph edge has to be added
    // in the same step -- otherwise the pad moves, the sketch stays clean, and
    // the plane it reports is the one from before the move.
    void setTrackedFace(std::optional<FaceQuery> query) { trackedFace_ = std::move(query); }
    // Written by recompute, which is the only thing that re-resolves.
    void setTrackedFaceResult(SketchFrame frame, std::string message) {
        frame_ = frame;
        trackedFaceMessage_ = std::move(message);
    }
    void setTrackedFaceMessage(std::string message) { trackedFaceMessage_ = std::move(message); }

    ObjectId supportFrameId_ = kInvalidObjectId;


    // Finds the tracked face again and moves the frame onto it. Called at the
    // TOP of recompute -- see there for why the order matters.
    RecomputeResult reresolveTrackedFace(const RecomputeContext& context);

    SketchEntityId addEntity(SketchGeometry geometry);

    ObjectId id_;
    // PRIVATE with PartDocument as the only caller (M17.16, ADR-M17-039).
    //
    // A rename is ONE undo step and must refuse a duplicate; both decisions
    // live in PartDocument::renameObject, and a public setter here would be a
    // way around both. Every other name-writing rule in this file is enforced
    // the same way rather than described in a comment.
    friend class PartDocument;
    void setName(std::string name) { name_ = std::move(name); }

    std::string name_;
    SketchFrame frame_{};
    std::vector<SketchEntity> entities_;
    std::vector<SketchConstraint> constraints_;
    std::vector<SketchReference> references_;
    std::optional<FaceQuery> trackedFace_;
    std::string trackedFaceMessage_;
    std::vector<DimensionPlacement> placements_;
    std::vector<DimensionFormat> formats_;

    // Last solve outcome. Not persisted: solver state is never identity
    // (ADR-M5-001) and a reloaded sketch re-solves before anything reads it.
    // Entities the last solve pinned completely. Absent means "not known to
    // be", which is what an unsolved or failed sketch has to say.
    std::set<ObjectId> fullyConstrainedEntities_;
    SketchSolveStatus solveStatus_{SketchSolveStatus::UnderConstrained};
    int degreesOfFreedom_{kUnknownDegreesOfFreedom};
    std::string solveMessage_;
    std::vector<SketchConstraintId> offendingConstraints_;
};

} // namespace paramcad
