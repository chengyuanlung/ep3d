#pragma once

#include "Core/Assembly/AssemblyStates.h"
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

// M23: an assembly instance came into existence or left it.
//
// Carries the SENTENCE that defines the instance -- which file, which body,
// which frame -- and nothing about geometry, exactly as every other delta
// here does. Notice what is NOT in it: the placement. Moving an instance is a
// FrameTransformEdit, because an instance's placement IS its frame, so undo
// gets the move for free and there is no second way to say where something is.
struct InstanceExistenceEdit {
    ObjectId instanceId = kInvalidObjectId;
    std::string name;
    std::string sourcePath;
    std::string bodyName;
    ObjectId frameId = kInvalidObjectId;
    bool addedByTheEdit = false;
};

// M24: a mate came into existence or left it.
//
// Carries the SENTENCE -- which two instances, which connector on each, what
// kind, what value -- because that is all a mate is. The connector names are
// strings for the same reason the mate holds strings: the connector lives in
// the PART file and is reused by every instance of it (roadmap §21), so there
// is no id in this document to record.
struct MateExistenceEdit {
    ObjectId mateId = kInvalidObjectId;
    std::string name;
    int type = 0; // MateType, as its underlying value, to keep this header light
    ObjectId leadingInstanceId = kInvalidObjectId;
    std::string leadingConnector;
    ObjectId followingInstanceId = kInvalidObjectId;
    std::string followingConnector;
    double value = 0.0;
    bool addedByTheEdit = false;
};

// M24: a mate's one remaining freedom was driven -- the hinge was turned.
//
// Its own delta rather than a ParameterValueEdit, because an assembly has no
// Parameters yet: a mate value is a number on the mate. When assembly
// variables arrive this becomes an expression like any other dimension, and
// the delta will be the parameter's, not this.
struct MateValueEdit {
    ObjectId mateId = kInvalidObjectId;
    // WHICH freedom (M25). A cylindrical mate turns and slides, so "the value"
    // stopped being a single thing the moment there was more than one.
    int component = 0;
    double before = 0.0;
    double after = 0.0;
};

// M25: a motion limit was set or cleared (roadmap §22).
struct MateLimitEdit {
    ObjectId mateId = kInvalidObjectId;
    int component = 0;
    bool beforeEnabled = false;
    double beforeMin = 0.0;
    double beforeMax = 0.0;
    bool afterEnabled = false;
    double afterMin = 0.0;
    double afterMax = 0.0;
};

// M25: a mate was made the one that holds its value through a loop solve, or
// released back to being something the solve may move.
struct MateDrivenEdit {
    ObjectId mateId = kInvalidObjectId;
    bool before = false;
    bool after = false;
};

// M24: an instance was grounded, or let go.
struct InstanceGroundEdit {
    ObjectId instanceId = kInvalidObjectId;
    bool before = false;
    bool after = false;
};

// M26: a named position was captured or deleted (roadmap §49).
//
// It carries the whole pose rather than a reference to one, because undoing a
// delete has to give back what was in it -- and a pose is a snapshot by
// definition, so there is nothing live to point at.
struct NamedPositionExistenceEdit {
    ObjectId positionId = kInvalidObjectId;
    std::string name;
    std::vector<NamedPosition::MateSetting> mates;
    std::vector<NamedPosition::LooseSetting> loose;
    bool addedByTheEdit = false;
};

// M26: an exploded view came into existence or left it.
struct ExplodeViewExistenceEdit {
    ObjectId viewId = kInvalidObjectId;
    std::string name;
    std::vector<ExplodeStep> steps;
    std::size_t previewCut = EvaluationCut::kAll;
    bool addedByTheEdit = false;
};

// M26: an exploded view's steps changed -- added, reordered or deleted.
//
// BOTH WHOLE LISTS, not a diff. §49 says steps can be reordered, and a reorder
// is not expressible as one insertion or one removal; carrying both makes undo
// and redo the same operation with the pair swapped, which is how every other
// order-changing delta in this file works.
struct ExplodeStepsEdit {
    ObjectId viewId = kInvalidObjectId;
    std::vector<ExplodeStep> before;
    std::vector<ExplodeStep> after;
};

// M26: an exploded view's preview position moved. A POSITION, not an edit --
// the same shape as a body's rollback, which is the point of EvaluationCut.
struct ExplodePreviewEdit {
    ObjectId viewId = kInvalidObjectId;
    std::size_t before = EvaluationCut::kAll;
    std::size_t after = EvaluationCut::kAll;
};

// M31: a relation was created or deleted.
//
// It carries the two FREEDOMS rather than two mate ids, because that is what a
// relation is (§20.5) -- and a delta that stored less than the object would be
// an undo that puts back something subtly different.
struct RelationExistenceEdit {
    ObjectId relationId = kInvalidObjectId;
    std::string name;
    int type = 0; // RelationType, as its underlying value, to keep this header light
    ObjectId driverMateId = kInvalidObjectId;
    int driverComponent = 0;
    ObjectId drivenMateId = kInvalidObjectId;
    int drivenComponent = 0;
    double ratio = 1.0;
    bool reversed = false;
    bool addedByTheEdit = false;
};

// M31: a relation's ratio or its direction was changed.
//
// BOTH IN ONE DELTA, because they are one edit from the user's side -- a gear
// ratio typed as a negative number is a ratio AND a direction, and splitting
// them would make undo walk back through a state that was never on screen.
struct RelationValueEdit {
    ObjectId relationId = kInvalidObjectId;
    double beforeRatio = 1.0;
    double afterRatio = 1.0;
    bool beforeReversed = false;
    bool afterReversed = false;
};

// --- Drawing (M32) -----------------------------------------------------------
//
// THE WHOLE SMALL VALUE, BEFORE AND AFTER.
//
// A sheet, a layer's properties and a view's placement are each a handful of
// fields that a user changes one dialog at a time. A delta per FIELD would be
// six near-identical types whose only difference is which member they carry --
// and the day a field is added, five of them are right and the new one is
// missing from somebody's switch. Carrying the whole value makes a partial
// restore impossible to write.
//
// This is only safe because these values are SMALL and OWN nothing. A delta
// that carried a body this way would be a copy of the geometry.

struct SheetEdit {
    // SheetSize and SheetOrientation as their underlying values, to keep this
    // header free of the drawing headers -- the same reason MateExistenceEdit
    // carries an int for its type.
    int beforeSize = 0;
    int afterSize = 0;
    int beforeOrientation = 0;
    int afterOrientation = 0;
    int beforeScaleNumerator = 1;
    int beforeScaleDenominator = 1;
    int afterScaleNumerator = 1;
    int afterScaleDenominator = 1;
    double beforeWidthMm = 0.0;
    double beforeHeightMm = 0.0;
    double afterWidthMm = 0.0;
    double afterHeightMm = 0.0;
};

struct LayerExistenceEdit {
    ObjectId layerId = kInvalidObjectId;
    std::string name;
    int color = 7;
    std::string linetype;
    bool on = true;
    bool frozen = false;
    bool locked = false;
    int lineweight = -3;
    bool addedByTheEdit = false;
};

struct LayerPropertyEdit {
    ObjectId layerId = kInvalidObjectId;
    int beforeColor = 7;
    int afterColor = 7;
    std::string beforeLinetype;
    std::string afterLinetype;
    bool beforeOn = true;
    bool afterOn = true;
    bool beforeFrozen = false;
    bool afterFrozen = false;
    bool beforeLocked = false;
    bool afterLocked = false;
    int beforeLineweight = -3;
    int afterLineweight = -3;
};

struct CurrentLayerEdit {
    ObjectId before = kInvalidObjectId;
    ObjectId after = kInvalidObjectId;
};

struct LinetypeExistenceEdit {
    ObjectId linetypeId = kInvalidObjectId;
    std::string name;
    std::string description;
    std::vector<double> pattern;
    bool addedByTheEdit = false;
};

struct DrawingViewExistenceEdit {
    ObjectId viewId = kInvalidObjectId;
    std::string name;
    std::string sourcePath;
    std::string bodyName;
    int direction = 0;
    double positionXMm = 0.0;
    double positionYMm = 0.0;
    int scaleNumerator = 1;
    int scaleDenominator = 1;
    bool ownScale = false;
    bool addedByTheEdit = false;
};

// Where a view sits, which way it looks and what it is drawn at -- the whole
// placement, for the reason stated above.
struct DrawingViewPlacementEdit {
    ObjectId viewId = kInvalidObjectId;
    double beforeXMm = 0.0;
    double beforeYMm = 0.0;
    double afterXMm = 0.0;
    double afterYMm = 0.0;
    int beforeDirection = 0;
    int afterDirection = 0;
    int beforeScaleNumerator = 1;
    int beforeScaleDenominator = 1;
    int afterScaleNumerator = 1;
    int afterScaleDenominator = 1;
    bool beforeOwnScale = false;
    bool afterOwnScale = false;
};

using UndoDelta =
    std::variant<ParameterValueEdit, FeatureExistenceEdit, SuppressionEdit, RollbackEdit,
                 ParameterExistenceEdit, FrameExistenceEdit, FrameTransformEdit,
                 FrameParentEdit, SketchSupportEdit, ConnectorExistenceEdit,
                 SketchEntityExistenceEdit, SketchConstraintExistenceEdit,
                 SketchDimensionPlacementEdit, SketchDimensionFormatEdit,
                 SketchEntityConstructionEdit, SketchEntityGeometryEdit, ObjectNameEdit,
                 SketchConstraintDrivenEdit, InstanceExistenceEdit, MateExistenceEdit,
                 MateValueEdit, InstanceGroundEdit, MateLimitEdit, MateDrivenEdit,
                 NamedPositionExistenceEdit, ExplodeViewExistenceEdit,
                 ExplodeStepsEdit, ExplodePreviewEdit, RelationExistenceEdit,
                 RelationValueEdit, SheetEdit, LayerExistenceEdit, LayerPropertyEdit,
                 CurrentLayerEdit, LinetypeExistenceEdit, DrawingViewExistenceEdit,
                 DrawingViewPlacementEdit>;

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
