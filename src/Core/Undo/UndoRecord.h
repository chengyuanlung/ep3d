#pragma once

#include "Core/Assembly/AssemblyStates.h"
#include "Core/Document/ObjectId.h"
#include "Core/Drawing/Annotation.h"
#include "Core/Drawing/DrawingDimension.h"
#include "Core/Drawing/DrawingEntity.h"
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
    int beforeAngle = 0;
    int afterAngle = 0;
};

// --- The frame and the title block (M35) -------------------------------------
//
// ONE DELTA EACH, carrying the WHOLE value on both sides.
//
// A delta per field would be a dozen near-identical types, and the day a
// thirteenth field is added, twelve are right. It also makes a half-restored
// title block impossible -- one that came back with the new width and the old
// rows would draw a box whose lines do not meet, and nothing would say so.
//
// The field list is a handful of short strings, so copying it twice per edit
// costs nothing worth the risk of the alternative.
struct TitleBlockFieldRecord {
    std::string label;
    std::string value;
    int source = 0; // TitleBlockSource, as its underlying value
};

struct TitleBlockEdit {
    std::vector<TitleBlockFieldRecord> before;
    std::vector<TitleBlockFieldRecord> after;
    double beforeWidthMm = 180.0;
    double afterWidthMm = 180.0;
    double beforeRowHeightMm = 8.0;
    double afterRowHeightMm = 8.0;
    bool beforeVisible = true;
    bool afterVisible = true;
};

// --- The parts list (M35.6) --------------------------------------------------
//
// The whole small value, before and after -- the pattern SheetEdit set. The
// ROWS are not in here and never will be: they are counted from the assembly
// on demand, so there is nothing about them to undo.
struct BomExistenceEdit {
    ObjectId tableId = kInvalidObjectId;
    std::string name;
    std::string sourcePath;
    double xMm = 0.0;
    double yMm = 0.0;
    bool addedByTheEdit = false;
};

// M48. A REVISION IS AN ENTRY IN A HISTORY, and undoing one takes the whole
// entry back -- letter included. The letter is stored (see Revision.h), so it
// has to be recorded here too: recomputing it from position on undo is exactly
// the derivation that would rewrite history.
struct RevisionExistenceEdit {
    std::string letter;
    std::string description;
    std::string date;
    std::string by;
    // WHERE in the history, because a revision may be taken out of the middle
    // and has to go back where it was. Appending it on undo would reorder the
    // drawing's history without saying so.
    std::size_t at = 0;
    bool addedByTheEdit = false;
};

// The table that SHOWS that history: a thing on the paper, added, moved,
// deleted -- and holding none of the rows.
struct RevisionTableExistenceEdit {
    ObjectId tableId = kInvalidObjectId;
    std::string name;
    double xMm = 0.0;
    double yMm = 0.0;
    double widthMm = 0.0;
    double rowHeightMm = 0.0;
    bool addedByTheEdit = false;
};

// MOVING IS ITS OWN EDIT, as it is for the parts list. The first cut of M48
// tried to carry both on the existence record, and the move then had no before
// and no after to go back to -- so it was not recorded at all, and dragging the
// table was the one change on this drawing undo could not take back.
struct RevisionTableEdit {
    ObjectId tableId = kInvalidObjectId;
    double beforeXMm = 0.0;
    double beforeYMm = 0.0;
    double afterXMm = 0.0;
    double afterYMm = 0.0;
};

// M39.4. A hole table is a thing on the paper: added, moved, deleted.
//
// It carries the VIEW it belongs to rather than a file path, because that is
// what it is: a table whose own source path could name a different part from
// the view its tags are drawn on is two answers to one question.
struct HoleTableExistenceEdit {
    ObjectId tableId = kInvalidObjectId;
    std::string name;
    ObjectId viewId = kInvalidObjectId;
    double xMm = 0.0;
    double yMm = 0.0;
    double datumXMm = 0.0;
    double datumYMm = 0.0;
    bool addedByTheEdit = false;
};

struct HoleTableEdit {
    ObjectId tableId = kInvalidObjectId;
    double beforeXMm = 0.0;
    double beforeYMm = 0.0;
    double afterXMm = 0.0;
    double afterYMm = 0.0;
    double beforeDatumXMm = 0.0;
    double beforeDatumYMm = 0.0;
    double afterDatumXMm = 0.0;
    double afterDatumYMm = 0.0;
};

struct BomEdit {
    ObjectId tableId = kInvalidObjectId;
    double beforeXMm = 0.0;
    double beforeYMm = 0.0;
    double afterXMm = 0.0;
    double afterYMm = 0.0;
    int beforeDepth = 0;
    int afterDepth = 0;
    double beforeRowHeightMm = 8.0;
    double afterRowHeightMm = 8.0;
    bool beforeGrowsUpward = true;
    bool afterGrowsUpward = true;
    std::vector<int> beforeColumns;
    std::vector<int> afterColumns;
};

// --- Schematic objects (M36) -------------------------------------------------
struct SymbolExistenceEdit {
    ObjectId symbolId = kInvalidObjectId;
    std::string tag;
    std::string symbolName;
    double xMm = 0.0;
    double yMm = 0.0;
    double rotationRad = 0.0;
    bool mirrored = false;
    ObjectId layerId = kInvalidObjectId;
    bool addedByTheEdit = false;
};

// The whole small value, before and after -- the pattern SheetEdit set.
struct SymbolPlacementEdit {
    ObjectId symbolId = kInvalidObjectId;
    double beforeXMm = 0.0;
    double beforeYMm = 0.0;
    double afterXMm = 0.0;
    double afterYMm = 0.0;
    double beforeRotationRad = 0.0;
    double afterRotationRad = 0.0;
    bool beforeMirrored = false;
    bool afterMirrored = false;
    std::string beforeTag;
    std::string afterTag;
};

struct WireExistenceEdit {
    ObjectId wireId = kInvalidObjectId;
    std::vector<double> pointsXY; // flattened, because a delta holds plain values
    std::string label;
    ObjectId layerId = kInvalidObjectId;
    bool addedByTheEdit = false;
};

struct WireEdit {
    ObjectId wireId = kInvalidObjectId;
    std::vector<double> beforePointsXY;
    std::vector<double> afterPointsXY;
    std::string beforeLabel;
    std::string afterLabel;
};

struct SheetFrameEdit {
    double beforeBindingMm = 20.0;
    double afterBindingMm = 20.0;
    double beforeOtherMm = 10.0;
    double afterOtherMm = 10.0;
    double beforeZoneTargetMm = 100.0;
    double afterZoneTargetMm = 100.0;
    bool beforeVisible = true;
    bool afterVisible = true;
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
    // A SECTION'S CUT COMES BACK WITH IT. Without this, undoing the delete of
    // a section view would restore a view with no cut line -- which projects
    // the WHOLE part and looks entirely reasonable.
    bool sectionActive = false;
    double sectionFromXMm = 0.0;
    double sectionFromYMm = 0.0;
    double sectionToXMm = 0.0;
    double sectionToYMm = 0.0;
    int sectionArrowSide = 1;

    // M49. AND A DETAIL'S CIRCLE COMES BACK WITH IT, for the same reason: a
    // detail restored without its circle projects the WHOLE part, at the
    // enlarged scale, and looks like a view somebody put there on purpose.
    bool detailActive = false;
    double detailCentreXMm = 0.0;
    double detailCentreYMm = 0.0;
    double detailRadiusMm = 0.0;
    // M50. And the break, for the same reason again: a broken view restored
    // without it comes back showing the whole three metres of bar.
    // M53. And whether it is a FLAT PATTERN. Restored without it, a blank
    // comes back as a projection of the folded part -- which for a bracket is
    // a rectangle with lines on it either way.
    bool flatPattern = false;
    bool breakActive = false;
    double breakFromMm = 0.0;
    double breakToMm = 0.0;
    bool breakHorizontal = true;
    double breakGapMm = 0.0;

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
    bool showHidden = true;
    bool showTangent = false;
    ObjectId parentViewId = kInvalidObjectId;
    double alignmentOffsetMm = 0.0;
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
    bool beforeShowHidden = true;
    bool afterShowHidden = true;
    bool beforeShowTangent = false;
    bool afterShowTangent = false;
    double beforeAlignmentOffsetMm = 0.0;
    double afterAlignmentOffsetMm = 0.0;
    // WHERE THE KNIFE WENT (M38). Part of the placement rather than a delta of
    // its own: moving a section line is the same kind of edit as moving the
    // view, and splitting them would leave a section whose line came back
    // while its position did not.
    bool beforeSectionActive = false;
    bool afterSectionActive = false;
    double beforeSectionFromXMm = 0.0;
    double beforeSectionFromYMm = 0.0;
    double beforeSectionToXMm = 0.0;
    double beforeSectionToYMm = 0.0;
    int beforeSectionArrowSide = 1;
    double afterSectionFromXMm = 0.0;
    double afterSectionFromYMm = 0.0;
    double afterSectionToXMm = 0.0;
    double afterSectionToYMm = 0.0;
    int afterSectionArrowSide = 1;
    // WHERE THE MAGNIFYING GLASS WENT (M49). Part of the placement for the
    // reason the cut is: moving a detail circle is the same kind of edit as
    // moving the view, and splitting them would leave a detail whose circle
    // came back while its position did not.
    bool beforeDetailActive = false;
    bool afterDetailActive = false;
    double beforeDetailCentreXMm = 0.0;
    double beforeDetailCentreYMm = 0.0;
    double beforeDetailRadiusMm = 0.0;
    double afterDetailCentreXMm = 0.0;
    double afterDetailCentreYMm = 0.0;
    double afterDetailRadiusMm = 0.0;
    // WHERE THE MIDDLE WENT (M50). On the placement for the reason the cut and
    // the circle are: breaking a view is a change to how it lands on the
    // paper, and splitting it off would let the break come back while the
    // position did not.
    bool beforeBreakActive = false;
    bool afterBreakActive = false;
    double beforeBreakFromMm = 0.0;
    double beforeBreakToMm = 0.0;
    bool beforeBreakHorizontal = true;
    double beforeBreakGapMm = 0.0;
    double afterBreakFromMm = 0.0;
    double afterBreakToMm = 0.0;
    bool afterBreakHorizontal = true;
    double afterBreakGapMm = 0.0;
};

// --- Authored drawing geometry (M33) -----------------------------------------
//
// THE SHAPE ITSELF, not a description of the change.
//
// A drawn line has no parameters and no constraints: its coordinates ARE the
// object. So an edit to one is a before-and-after of the whole shape, exactly
// as SketchEntityGeometryEdit is for the same reason -- and a delta that
// carried "moved by (3, 4)" would have to be re-derived on every undo, which
// is where a rounding difference turns a there-and-back into a drift.
//
// `DrawShape` is a variant of small structs, so copying one is cheap. That is
// the same thing that made the whole-value SheetEdit safe.
struct DrawingEntityExistenceEdit {
    ObjectId entityId = kInvalidObjectId;
    DrawShape shape;
    ObjectId layerId = kInvalidObjectId;
    int color = 256;
    std::string linetype;
    int lineweight = -1;
    bool addedByTheEdit = false;
};

struct DrawingEntityShapeEdit {
    ObjectId entityId = kInvalidObjectId;
    DrawShape before;
    DrawShape after;
};

struct DrawingEntityPropertyEdit {
    ObjectId entityId = kInvalidObjectId;
    ObjectId beforeLayerId = kInvalidObjectId;
    ObjectId afterLayerId = kInvalidObjectId;
    int beforeColor = 256;
    int afterColor = 256;
    std::string beforeLinetype;
    std::string afterLinetype;
    int beforeLineweight = -1;
    int afterLineweight = -1;
};

// --- Dimensions (M34) --------------------------------------------------------
// M41. A surface finish, a feature control frame or a datum: added, moved,
// deleted. ONE delta for all three, because they are one object with three
// bodies -- three deltas would be three places to forget a field the day a
// fourth symbol arrives.
// M44. A page of the drawing, added or deleted. Its paper, frame and title
// block are NOT in the delta: a page that is deleted has nothing on it (the
// document refuses otherwise), so what comes back is a blank page of the same
// name, which is what was taken away.
struct SheetPageExistenceEdit {
    ObjectId pageId = kInvalidObjectId;
    std::string name;
    bool addedByTheEdit = false;
};

struct AnnotationExistenceEdit {
    ObjectId annotationId = kInvalidObjectId;
    AnnotationBody body;
    DimensionAnchor anchor;
    double xMm = 0.0;
    double yMm = 0.0;
    ObjectId layerId = kInvalidObjectId;
    bool addedByTheEdit = false;
};

struct AnnotationEdit {
    ObjectId annotationId = kInvalidObjectId;
    AnnotationBody beforeBody;
    AnnotationBody afterBody;
    double beforeXMm = 0.0;
    double beforeYMm = 0.0;
    double afterXMm = 0.0;
    double afterYMm = 0.0;
};

struct DimensionExistenceEdit {
    ObjectId dimensionId = kInvalidObjectId;
    int kind = 0;
    DimensionAnchor first;
    DimensionAnchor second;
    int direction = 0;
    double lineXMm = 0.0;
    double lineYMm = 0.0;
    ObjectId styleId = kInvalidObjectId;
    ObjectId layerId = kInvalidObjectId;
    std::string textOverride;
    bool addedByTheEdit = false;
};

// The whole small value, before and after -- the pattern SheetEdit set, for
// the same reason: a delta per field would be five near-identical types and
// the day a field is added, four are right.
struct DimensionEdit {
    ObjectId dimensionId = kInvalidObjectId;
    int beforeDirection = 0;
    int afterDirection = 0;
    double beforeXMm = 0.0;
    double beforeYMm = 0.0;
    double afterXMm = 0.0;
    double afterYMm = 0.0;
    ObjectId beforeStyleId = kInvalidObjectId;
    ObjectId afterStyleId = kInvalidObjectId;
    std::string beforeText;
    std::string afterText;
};

// The whole small value, before and after -- the pattern SheetEdit set.
struct DimensionToleranceEdit {
    ObjectId dimensionId = kInvalidObjectId;
    int beforeKind = 0;
    int afterKind = 0;
    double beforeUpperMm = 0.0;
    double afterUpperMm = 0.0;
    double beforeLowerMm = 0.0;
    double afterLowerMm = 0.0;
    std::string beforeFitCode;
    std::string afterFitCode;
    int beforeDecimals = -1;
    int afterDecimals = -1;
};

// WHAT UNMARKED SIZES MEAN, which is the SHEET's business and not any one
// dimension's -- exactly like the projection angle, and for the same reason: a
// drawing whose dimensions answered to two different general classes is one no
// reader can use.
struct GeneralToleranceEdit {
    int before = 0;
    int after = 0;
};

// M51. WHAT A PART IS MADE OF, when it is made of sheet.
//
// The whole setting, both sides, because the three fields are one decision: a
// thickness without its material has no K factor and a material without a
// thickness has no minimum radius. Half of this restored is a part whose flat
// pattern is computed from a number nobody chose.
struct SheetMetalSettingEdit {
    bool beforeIsSheet = false;
    bool afterIsSheet = false;
    double beforeThicknessMm = 0.0;
    double afterThicknessMm = 0.0;
    int beforeMaterial = 0;
    int afterMaterial = 0;
    double beforeBendRadiusMm = 0.0;
    double afterBendRadiusMm = 0.0;
};

struct DimensionStyleExistenceEdit {
    ObjectId styleId = kInvalidObjectId;
    std::string name;
    bool addedByTheEdit = false;
};

// A STYLE IS ITS OWN VALUE, so the delta carries the whole of it. Editing one
// changes how every dimension using it is drawn, and a half-restored style
// would leave a drawing whose text is one size and whose arrows are another.
struct DimensionStyleEdit {
    ObjectId styleId = kInvalidObjectId;
    double beforeTextHeightMm = 3.5;
    double afterTextHeightMm = 3.5;
    double beforeArrowSizeMm = 3.5;
    double afterArrowSizeMm = 3.5;
    double beforeTextGapMm = 0.8;
    double afterTextGapMm = 0.8;
    double beforeExtensionGapMm = 1.5;
    double afterExtensionGapMm = 1.5;
    double beforeExtensionOvershootMm = 2.0;
    double afterExtensionOvershootMm = 2.0;
    int beforeDecimals = 2;
    int afterDecimals = 2;
    std::string beforeSuffix;
    std::string afterSuffix;
    double beforeOverallScale = 1.0;
    double afterOverallScale = 1.0;
};

struct CurrentDimensionStyleEdit {
    ObjectId before = kInvalidObjectId;
    ObjectId after = kInvalidObjectId;
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
                 DrawingViewPlacementEdit, DrawingEntityExistenceEdit,
                 DrawingEntityShapeEdit, DrawingEntityPropertyEdit, DimensionExistenceEdit,
                 DimensionEdit, DimensionStyleExistenceEdit, DimensionStyleEdit,
                 CurrentDimensionStyleEdit, TitleBlockEdit, SheetFrameEdit,
                 BomExistenceEdit, BomEdit, SymbolExistenceEdit, SymbolPlacementEdit,
                 WireExistenceEdit, WireEdit, DimensionToleranceEdit,
                 GeneralToleranceEdit, SheetMetalSettingEdit, HoleTableExistenceEdit,
                 HoleTableEdit,
                 RevisionExistenceEdit, RevisionTableExistenceEdit, RevisionTableEdit,
                 AnnotationExistenceEdit, AnnotationEdit, SheetPageExistenceEdit>;

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
