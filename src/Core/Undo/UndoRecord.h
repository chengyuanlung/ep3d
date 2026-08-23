#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/FeatureSnapshot.h"
#include "Core/Geometry/MathTypes.h"
#include "Core/Sketch/SketchConstraint.h"
#include "Core/Sketch/SketchTypes.h"

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace paramcad {

// M9.1 -- the undo history's unit of change.
//
// ADR-M9-001's rule, stated where the type is: **an undo record is a SEMANTIC
// DELTA over the Core model.** ObjectIds, parameter values, feature snapshots.
// Never a serialized OCCT shape, never a kernel handle, never a whole-document
// snapshot taken because it was easier.
//
// That rule is not decoration. A snapshot-based undo stack would pin kernel
// memory for the length of the session, and -- worse -- would let a redo
// resurrect GEOMETRY rather than re-derive it, so an undo/redo pair could show
// a shape the current parameters no longer produce. Everything below is
// re-executed through the ordinary document facade, and the geometry is
// whatever the recompute engine builds from it.

// A Parameter's value and/or expression changed.
struct ParameterValueEdit {
    ObjectId parameterId = kInvalidObjectId;
    double before = 0.0;
    double after = 0.0;
    std::string expressionBefore;
    std::string expressionAfter;
};

// A feature came into existence or left it. `index` is the position the
// feature occupied in `Body::features()`, recorded because feature ORDER is
// load-bearing: the loader requires a consumer's base to appear earlier in the
// array, and `validateSaveable` enforces the same rule at save time. Restoring
// a middle feature at the end would produce a document that cannot be saved.
//
// The index is a position and this project forbids positions as IDENTITY
// (ADR-M4-004). It is not used as one: the feature is identified by
// `snapshot.id` throughout, and the index only says where to put it back.
struct FeatureExistenceEdit {
    ObjectId bodyId = kInvalidObjectId;
    std::size_t index = 0;
    FeatureSnapshot snapshot;
    // true  -> the edit ADDED this feature (undo removes it, redo re-adds)
    // false -> the edit REMOVED it (undo restores it, redo removes it again)
    bool addedByTheEdit = false;
};

// A feature was suppressed or unsuppressed (M9.3). A state, not an edit to the
// model: nothing about the feature's references changes, so the delta is one
// bool either side.
struct SuppressionEdit {
    ObjectId featureId = kInvalidObjectId;
    bool before = false;
    bool after = false;
};

// A body's rollback position moved (M9.4). Also a state and not an edit -- the
// features it hides are untouched -- but it is undoable for the same reason
// every other view-changing command is: the user can get back to where they
// were without remembering where that was.
struct RollbackEdit {
    ObjectId bodyId = kInvalidObjectId;
    std::size_t before = 0;
    std::size_t after = 0;
};

// A Parameter came into existence or left it (M9.5). Needed the moment the
// shell could CREATE a feature: a creation command adds a Parameter and a
// Feature together, and an undo that removed only the feature would leave an
// orphan Parameter behind every time -- visible in the tree, saved to the file,
// and impossible to explain.
struct ParameterExistenceEdit {
    ObjectId parameterId = kInvalidObjectId;
    std::string name;
    double value = 0.0;
    int unit = 0; // UnitType, stored as its underlying value to keep this header light
    std::string expression;
    bool addedByTheEdit = false;
};

// M10: frames are first-class, so every edit to one is a step the user can take
// back. A frame carries no geometry of its own, so each delta is small.
struct FrameExistenceEdit {
    ObjectId frameId = kInvalidObjectId;
    std::string name;
    ObjectId parentFrameId = kInvalidObjectId;
    Transform3D localTransform{};
    bool addedByTheEdit = false;
};

struct FrameTransformEdit {
    ObjectId frameId = kInvalidObjectId;
    Transform3D before{};
    Transform3D after{};
};

struct FrameParentEdit {
    ObjectId frameId = kInvalidObjectId;
    ObjectId before = kInvalidObjectId;
    ObjectId after = kInvalidObjectId;
};

// A sketch was put on a frame, moved to another, or taken off one (M10.2).
struct SketchSupportEdit {
    ObjectId sketchId = kInvalidObjectId;
    ObjectId before = kInvalidObjectId;
    ObjectId after = kInvalidObjectId;
};

// A connector was created or removed (M10.3). `role` and `owner` are stored as
// their underlying integers so this header stays free of the Connector type --
// the undo layer needs the VALUES, not the class.
struct ConnectorExistenceEdit {
    ObjectId connectorId = kInvalidObjectId;
    std::string name;
    int role = 0;
    ObjectId frameId = kInvalidObjectId;
    int owner = 0;
    bool addedByTheEdit = false;
};

// M12: sketch GEOMETRY and CONSTRAINTS are undoable.
//
// Until M12 the undo stack covered parameters, features, frames and connectors
// but NOT the contents of a sketch -- which was invisible while sketches could
// only be built in code or imported wholesale, and became the first thing a
// user hits the moment there is a mouse-driven drawing tool. Drawing a line and
// pressing Ctrl+Z did nothing.
//
// Both deltas carry the DEFINITION, not a handle: an undone entity is restored
// through `Sketch::restoreEntity` with its persisted SketchEntityId, so the id
// a constraint refers to survives an undo/redo round trip. Restoring geometry
// under a NEW id would silently orphan every constraint on it (A03).
struct SketchEntityExistenceEdit {
    ObjectId sketchId = kInvalidObjectId;
    SketchEntityId entityId{kInvalidSketchEntityId};
    SketchGeometry geometry{};
    // Construction geometry can now be created in ONE step (M17.17), so the
    // flag has to be part of what "this entity existed" means -- an undo that
    // restored the polygon's circumscribed circle as a real edge would put a
    // curve nobody drew into the solid.
    bool construction = false;
    bool addedByTheEdit = false;
};

// Removing an ENTITY cascades into its constraints (ADR-M5-009), so a delete
// records one of these per cascaded constraint ALONGSIDE the entity delta. They
// are pushed BEFORE the entity's own delta, because deltas undo in reverse
// order and a constraint cannot be restored onto an entity that is not back
// yet.
struct SketchConstraintExistenceEdit {
    ObjectId sketchId = kInvalidObjectId;
    SketchConstraintId constraintId{kInvalidSketchConstraintId};
    SketchConstraintData data{};
    bool addedByTheEdit = false;
};

// M16: a dimension's value was dragged to a new place, or put back on
// automatic. `hasBefore` / `hasAfter` distinguish "was at this point" from
// "was automatic" -- a bare Vec2 pair cannot, and undoing a first-ever drag has
// to restore AUTOMATIC rather than some coordinate that was never chosen.
struct SketchDimensionPlacementEdit {
    ObjectId sketchId = kInvalidObjectId;
    SketchConstraintId constraintId{kInvalidSketchConstraintId};
    bool hasBefore = false;
    Vec2 before{};
    bool hasAfter = false;
    Vec2 after{};
};

// M16: a dimension's prefix, suffix or tolerance changed. Carries both whole
// formats rather than a diff, because the four fields are edited together in
// one dialog and reversing them one at a time would be a stranger history than
// the user's own action.
struct SketchDimensionFormatEdit {
    ObjectId sketchId = kInvalidObjectId;
    SketchConstraintId constraintId{kInvalidSketchConstraintId};
    std::string beforePrefix;
    std::string beforeSuffix;
    double beforePlus = 0.0;
    double beforeMinus = 0.0;
    std::string afterPrefix;
    std::string afterSuffix;
    double afterPlus = 0.0;
    double afterMinus = 0.0;
};

// M17: an entity's GEOMETRY was edited in place -- a trim, an extend, a
// chamfer's setback.
//
// Carries both whole geometries rather than a delta, because the shapes are
// variants and "the start point moved" cannot describe a circle becoming an
// arc. Both halves are stored so undo and redo are the same operation with the
// fields swapped.
//
// The entity keeps its ID, and that is the entire point: every constraint on it
// stays attached and keeps meaning what it meant. Trimming by delete-and-
// recreate would silently drop them all (ADR-M5-009 cascades a delete), which
// is how a sketch stops saying what the user drew.
struct SketchEntityGeometryEdit {
    ObjectId sketchId = kInvalidObjectId;
    SketchEntityId entityId{kInvalidSketchEntityId};
    SketchGeometry before{};
    SketchGeometry after{};
};

// M17: an entity became construction geometry, or stopped being it.
//
// One delta per entity rather than a list, so a mixed selection undoes exactly
// as it was -- a single delta carrying "these five became construction" cannot
// restore the two that already were.
struct SketchEntityConstructionEdit {
    ObjectId sketchId = kInvalidObjectId;
    SketchEntityId entityId{kInvalidSketchEntityId};
    bool before = false;
    bool after = false;
};

// A dimension changed between DRIVING and MEASURING (M17.19, ADR-M17-042).
struct SketchConstraintDrivenEdit {
    ObjectId sketchId = kInvalidObjectId;
    SketchConstraintId constraintId{kInvalidSketchConstraintId};
    bool before = false;
    bool after = false;
};

// An object was RENAMED (M17.16, ADR-M17-039).
//
// Works for anything the tree shows a name for -- a sketch, a feature, a
// parameter, a body, the material -- which is why it holds a plain ObjectId
// rather than one of the per-kind ids: the name is the one property all of
// them have in common, and a delta per kind would be five copies of one idea.
//
// Both strings, so undo and redo are the same operation with the pair swapped.
struct ObjectNameEdit {
    ObjectId objectId = kInvalidObjectId;
    std::string before;
    std::string after;
};

using UndoDelta =
    std::variant<ParameterValueEdit, FeatureExistenceEdit, SuppressionEdit, RollbackEdit,
                 ParameterExistenceEdit, FrameExistenceEdit, FrameTransformEdit,
                 FrameParentEdit, SketchSupportEdit, ConnectorExistenceEdit,
                 SketchEntityExistenceEdit, SketchConstraintExistenceEdit,
                 SketchDimensionPlacementEdit, SketchDimensionFormatEdit,
                 SketchEntityConstructionEdit, SketchEntityGeometryEdit, ObjectNameEdit,
                 SketchConstraintDrivenEdit>;

// One atomic user-visible operation. Deltas are applied in order and undone in
// reverse order, so a transaction that changed three things comes back exactly
// as it was.
//
// `label` is not cosmetic: M9 spec section 4 requires that a user can tell what
// an undo will undo, and a stack of unnamed steps cannot answer that.
struct UndoRecord {
    std::string label;
    std::vector<UndoDelta> deltas;
};

} // namespace paramcad
