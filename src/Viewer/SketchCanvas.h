#pragma once

#include "Core/Geometry/MathTypes.h"
#include "Core/Sketch/SketchConstraint.h"
#include "Core/Sketch/SketchTypes.h"

#include <cstddef>
#include <string>
#include <vector>

namespace paramcad {

class Sketch;

// M12.1 -- the DECIDING half of the 2D sketch canvas, deliberately free of Qt
// AND of OCCT.
//
// This file exists for two reasons, and the second one is the load-bearing one:
//
//  1. M6.14's lesson. A panel once shipped showing ten labels and no values and
//     none of 561 tests could see it, because all of them asked the model. So
//     everything that DECIDES -- where a click lands, what it snaps to, which
//     constraint a selection admits, what the next click will do -- lives where
//     a test can call it directly, and the widget is left with rendering.
//
//  2. The owner's staging decision (2026-08-20): a 2D canvas FIRST, an OCCT
//     overlay on a plane in the 3D view SECOND, sharing one implementation.
//     Everything in this header is expressed in sketch-local (u,v) millimetres
//     and pixels, with no widget type anywhere, so the second consumer needs a
//     different RENDERER and a different event source -- not a second copy of
//     the hit-testing, the snapping or the tool state machine.
//
// Nothing here touches a document. Producing an edit and APPLYING one are
// separate steps: this layer emits a `SketchEdit` value, and SketchCommands.h
// is what puts it through PartDocument's facade inside a transaction.

// =============================================================================
// View transform
// =============================================================================

// Maps sketch (u,v) millimetres to canvas pixels and back.
//
// Sketch +v is UP; pixel +y is DOWN. That flip lives here and nowhere else, so
// no caller has to remember it.
struct CanvasView {
    Vec2 centerMm{0.0, 0.0}; // the sketch point at the centre of the canvas
    double pixelsPerMm{4.0};
    int widthPx{800};
    int heightPx{600};

    Vec2 toPixels(Vec2 sketchMm) const noexcept;
    Vec2 toSketch(Vec2 pixels) const noexcept;

    double toPixelLength(double mm) const noexcept { return mm * pixelsPerMm; }
    double toSketchLength(double px) const noexcept;

    // Zooms about a fixed pixel, so the sketch point under the cursor stays
    // under the cursor. Scale is clamped: an unclamped zoom reaches a scale at
    // which toSketch() returns non-finite coordinates, and the first click
    // afterwards writes them into the document.
    void zoomAt(Vec2 pixels, double factor) noexcept;
    void panByPixels(Vec2 deltaPixels) noexcept;
};

inline constexpr double kMinPixelsPerMm = 0.01;
inline constexpr double kMaxPixelsPerMm = 5000.0;

// A view showing everything in `sketch`, or a default 100 mm-wide view when the
// sketch is empty -- an empty sketch has no extent to fit, and a zero-size fit
// produces the infinite zoom the clamp above exists to prevent.
CanvasView FitView(const Sketch& sketch, int widthPx, int heightPx, double marginPx = 32.0);

// The grid step to draw at this zoom, in mm: a 1-2-5 sequence chosen so the
// spacing on screen stays within a readable band. Never zero.
double GridStepMm(const CanvasView& view) noexcept;

// =============================================================================
// Snapping and picking
// =============================================================================

// What the cursor landed on. ORDER IS PRIORITY: a defined point beats a curve,
// a curve beats the grid, and the grid beats nothing at all.
enum class SnapKind {
    Free,       // nothing within tolerance, or inference suppressed
    Grid,       // rounded to the visible grid
    OnCurve,    // somewhere along a line, circle or arc
    Origin,     // the sketch origin (0,0) -- has no entity, but earns a Fix
    CenterPoint,
    Endpoint
};

const char* SnapKindName(SnapKind kind) noexcept;

struct SnapResult {
    Vec2 point{};        // where the click should be treated as landing, in mm
    SnapKind kind{SnapKind::Free};
    SketchElementRef ref{}; // the element snapped to; invalid for Free/Grid/Origin

    bool hasRef() const noexcept { return ref.entityId != kInvalidSketchEntityId; }
};

// Where a raw cursor position should be treated as landing.
//
// `suppress` is the modifier key's job (roadmap section 4.2 requires an explicit
// suppression modifier AND a status-bar readout of whether it is active): when
// true the raw point is returned untouched and NO reference is reported, so no
// inferred constraint can be generated behind the user's back.
//
// `gridMm <= 0` disables grid snapping without disabling the rest.
SnapResult SnapCursor(const Sketch& sketch, Vec2 rawMm, double toleranceMm, double gridMm,
                      bool suppress);

// Everything within `toleranceMm` of `pickMm`, nearest and most specific first.
// Points come before curves at equal distance: a click on a line's endpoint
// means the endpoint, or no constraint could ever be attached to one.
std::vector<SketchElementRef> HitTest(const Sketch& sketch, Vec2 pickMm, double toleranceMm);

// The single best hit, or an invalid ref.
SketchElementRef PickElement(const Sketch& sketch, Vec2 pickMm, double toleranceMm);

// The projected reference nearest `pickMm`, within `toleranceMm`, or invalid
// (M17.6, ADR-M17-029).
//
// A SEPARATE picker from PickElement, and deliberately so. References are not
// entities: none of them can be constrained, dimensioned, dragged or deleted,
// and a picker that returned one where a SketchElementRef was expected would
// let every one of those be attempted on geometry that has no identity in the
// model. The distance arithmetic is shared with the entity picker, so the two
// always agree about which curve is under the cursor -- it is only the answer
// type that differs.
SketchReferenceId ReferenceAt(const Sketch& sketch, Vec2 pickMm, double toleranceMm);

// Where a reference sits in sketch mm. `ok` is false for a ref this sketch
// cannot resolve -- a Whole reference to a line has no single position, and
// callers must not silently get the origin for one.
Vec2 ResolveElementPoint(const Sketch& sketch, const SketchElementRef& ref, bool* ok);

// True when `ref` names something the solver can treat as a POINT: a Point
// entity, or the start/end/centre of another entity. The distinction decides
// which constraints a selection admits.
bool IsPointRef(const Sketch& sketch, const SketchElementRef& ref) noexcept;
bool IsLineRef(const Sketch& sketch, const SketchElementRef& ref) noexcept;
// Distance from `query` to a piece of sketch geometry, and the nearest point on
// it. Negative when the question has no answer (a query dead on a circle's
// centre, say). Exported so picking, deleting and snapping all measure the same
// way -- an ellipse's distance is SAMPLED, and a second sampler somewhere else
// would pick a different entity than the one under the cursor.
double DistanceToSketchGeometry(const SketchGeometry& geometry, Vec2 query, Vec2* nearest);

// THE one spelling of "point i of this spline": the ends by their own names,
// the rest by index. One place, so the same point cannot be referred to two
// ways and compare unequal to itself.
SketchElementRef SplineRefFor(SketchEntityId id, const SketchSpline& spline, int index);

bool IsCurveRef(const Sketch& sketch, const SketchElementRef& ref) noexcept;

// A SPLINE. Apart from IsCurveRef for the same reason an ellipse is: a spline
// has no centre and no radius, and it is tangent only AT an end, so every
// command that asks "is this a curve" would be offering something a spline
// cannot answer.
bool IsSplineRef(const Sketch& sketch, const SketchElementRef& ref) noexcept;

// An ELLIPSE or a piece of one. Separate from IsCurveRef because the commands
// that ask "is this a curve" all want one radius, and an ellipse has two.
bool IsEllipseRef(const Sketch& sketch, const SketchElementRef& ref) noexcept;

// Anything with a CENTRE -- circle, arc, ellipse, elliptical arc. What
// Concentric needs, and the same set the solve session's `centredSlots`
// accepts. Kept apart from IsCurveRef, which also promises a single radius.
bool IsCentredRef(const Sketch& sketch, const SketchElementRef& ref) noexcept; // circle or arc

// Whether `angleRad` lies on the arc's swept range, following its stored
// direction.
//
// Exported rather than kept private because Trim needs exactly this question:
// a line crosses the CIRCLE through an arc in places the arc itself does not
// reach, and cutting there would trim at a point with nothing drawn on it. Two
// copies of a sweep test would be two chances to disagree about which side of
// a wrap-around an angle falls on.
bool AngleOnArcSweep(const SketchArc& arc, double angleRad) noexcept;

// The POINTS an entity offers, in the sub-element vocabulary the solver
// understands: a line's two ends, a circle's or arc's centre, a Point itself.
//
// The canvas draws these as small handles on whatever is selected, so a user
// who clicks a line can then see -- and click -- the end they actually want to
// measure from. Before that they had to know the handles were there and hit an
// invisible target.
//
// Same list SnapCursor picks from, and deliberately so: a handle the user can
// see but not snap to, or snap to but not see, is worse than none.
std::vector<SketchElementRef> EntityHandles(const Sketch& sketch, SketchEntityId id);

// One line naming a reference, for the status bar and the constraint list:
// "Line #12 start", "Circle #7". Never empty.
std::string DescribeElementRef(const Sketch& sketch, const SketchElementRef& ref);

// =============================================================================
// Tools and edits
// =============================================================================

// The drawing tools. Constraints and dimensions are NOT tools: they act on the
// current selection, so there is one selection model rather than a second
// click-sequence per constraint kind.
// M17.17 adds four ways to place geometry that were only reachable by drawing
// something else and constraining it into shape (ADR-M17-040). Each is a
// different set of CLICKS, not a different kind of entity: a centre rectangle
// is four lines exactly as a corner rectangle is, and the polygon is lines on
// a construction circle. Nothing here adds a type the solver has to learn.
enum class SketchTool {
    Select,
    Point,
    Line,
    Rectangle,
    CenterRectangle,
    Circle,
    ThreePointCircle,
    Arc,
    ThreePointArc,
    TangentArc,
    Ellipse,
    EllipticalArc,
    Spline,
    Polygon,
    Slot
};

const char* SketchToolName(SketchTool tool) noexcept;

// How many points each drawing tool consumes before it produces geometry.
//
// kSplineIsFinishedByHand for the one tool whose count the USER decides: a
// spline takes points until it is told to stop. Anything that compares a
// pending count against this must treat it as "never reached".
inline constexpr int kSplineIsFinishedByHand = -1;
int SketchToolPointCount(SketchTool tool) noexcept;

enum class SketchEditKind {
    None,
    AddPoint,
    AddLine,
    AddCircle,
    AddArc,
    AddRectangle,
    AddCenterRectangle,
    AddThreePointCircle,
    AddThreePointArc,
    AddTangentArc,
    AddEllipse,
    AddEllipticalArc,
    AddSpline,
    AddPolygon,
    AddSlot,
    AddCoincident,
    AddHorizontal,
    AddVertical,
    AddFix,
    // M13 -- roadmap 6.1's "next stage" geometric constraints.
    AddParallel,
    AddPerpendicular,
    AddEqual,
    AddConcentric,
    AddMidpoint,
    AddPointOnObject,
    AddTangent,
    // M17: two points are mirror images across a line. Three selected
    // elements, not two -- the mirror is an input, not a mode.
    AddSymmetric,
    AddDistance,
    // M17 -- the two legs of a point-to-point distance (roadmap 7.1). Never
    // INFERRED from a selection: two points mean the straight-line distance,
    // and guessing which of three meanings a user wanted would be the silent
    // guess section 26 forbids. Reached by their own commands.
    AddHorizontalDistance,
    AddVerticalDistance,
    // M17.25 -- an ellipse's two semi-axes. Two kinds rather than one with a
    // flag, because they are two different commands on the toolbar and two
    // different numbers on the drawing.
    AddMajorAxis,
    AddMinorAxis,
    AddEllipseRotation,
    // M17: the perpendicular distance from a point to a line -- roadmap 7.1's
    // "two parallel lines" case, and what holds an Offset at its distance.
    AddPointLineDistance,
    AddLength,
    AddRadius,
    AddDiameter,
    AddAngle,
    DeleteEntities,
    DeleteConstraints
};

const char* SketchEditKindName(SketchEditKind kind) noexcept;
bool IsDimensionEdit(SketchEditKind kind) noexcept;

// A reference that may point at geometry this edit has not created yet.
//
// Needed because a Rectangle's Horizontal constraints and a chained Line's
// Coincident constraint both refer to entities born in the SAME edit, and an
// index into "the entities this edit creates" is the only way to say so before
// any id exists. It is a position, and this project forbids positions as
// IDENTITY (ADR-M4-004) -- it is not used as one: it is resolved to a real
// SketchEntityId the instant the entity is created, and never stored.
struct PendingRef {
    bool isNew{false};
    std::size_t newEntity{0};   // index into SketchEdit::points-derived entities
    SketchElementRef existing{};
    SketchSubElement subElement{SketchSubElement::Whole};
};

// A constraint the edit adds on its own initiative: a rectangle's four
// horizontal/vertical constraints, the coincidence an inferred snap earned, or
// the Fix a point dropped on the origin earned.
//
// Roadmap section 4.2 is explicit that inference is a CONSTRAINT GENERATOR and
// not drawing-time magnetism: a snap that does not produce a real
// SketchConstraintId makes the DOF readout lie. These are ordinary constraints
// with ordinary ids, listed and deletable like any other (section 6.2).
//
// The origin is the one snap target with no element behind it, so a coincidence
// cannot express it -- Fix is what does. Section 4.2 lists alignment to the
// origin among the inferences a sketcher is expected to make, and ADR-M5-005
// makes it the load-bearing one: without an anchor, a sketch that is fully
// dimensioned in every internal respect still reports two translational DOF.
struct PendingConstraint {
    SketchEditKind kind{SketchEditKind::AddCoincident};
    PendingRef a{};
    PendingRef b{};
};

struct SketchEdit {
    SketchEditKind kind{SketchEditKind::None};

    // Meaning depends on kind: the point for AddPoint, both ends for AddLine,
    // centre and rim for AddCircle, centre/start/end for AddArc, two opposite
    // corners for AddRectangle. Empty for every non-drawing kind.
    std::vector<Vec2> points;

    // What a constraint, a dimension or a delete applies to.
    std::vector<SketchElementRef> refs;

    // What a DeleteConstraints edit removes.
    //
    // (see also finishPendingSpline, which is how the one open-ended tool is
    // told that the last point has been given)
    std::vector<SketchConstraintId> constraints;

    // Constraints created alongside the geometry (see PendingConstraint).
    std::vector<PendingConstraint> autoConstraints;

    // AddTangent only: which of the two curve-curve tangencies was meant.
    //
    // DECIDED HERE, from the configuration at the moment the user asked for it,
    // and then carried into the constraint as stored state. The solver must
    // never re-derive it (see TangentConstraint): a re-derived branch would let
    // a later edit silently swap the model for its opposite.
    bool tangentInternal{false};

    // OFFSET only: what this geometry is a copy of, and by how much.
    //
    // Carried on the edit rather than expressed as PendingConstraints because
    // the constraints an offset needs (Parallel, Equal, Concentric, Radius,
    // PointLineDistance) are not all two-reference kinds, and one of them binds
    // a Parameter -- which PendingConstraint has no way to say. ApplySketchEdit
    // builds them once the copy has an id.
    SketchEntityId offsetSource{kInvalidSketchEntityId};
    double offsetDistanceMm{0.0};

    // A TANGENT ARC only: the endpoint it grows out of.
    //
    // Carried on the edit rather than as a PendingConstraint because the arc
    // cannot even be COMPUTED without it -- its centre is fixed by the
    // direction the existing curve leaves that point in, which the canvas
    // cannot read: completeDrawing has the clicks and the snaps, not the
    // sketch. So ApplySketchEdit, which does have it, builds the geometry AND
    // the two constraints that make it a tangent arc rather than an arc that
    // happens to start there.
    //
    // Invalid when the first click landed on nothing. That is a REFUSAL with a
    // message, not a plain arc: an arc drawn by this tool that is not tangent
    // to anything is not what the user asked for, and would be indisputably
    // worse than being told to click an end.
    SketchElementRef tangentFrom{};

    // A SPLINE only: whether it runs back to its first point.
    //
    // Decided when the curve is finished, not while it is being drawn -- a
    // half-drawn spline is neither, and asking after each click which one the
    // user meant is a question with no answer yet.
    bool splineClosed{false};

    // A DIMENSION EDIT only: where the user put the dimension line.
    //
    // Carried on the edit so that creating a dimension and placing it are ONE
    // undo step (M17.18). Writing the placement after ApplySketchEdit returns
    // made it two: Ctrl+Z moved the dimension back to its automatic position
    // and left it there, which is not what anyone means by undoing a
    // dimension they just placed.
    bool hasDimensionPlacement{false};
    Vec2 dimensionPlacement{};

    // AddPolygon only: how many sides. Carried on the edit rather than read
    // from the model when the edit is applied, so replaying one is not at the
    // mercy of what the toolbar happens to be set to now.
    int polygonSides{6};

    // Undo label and status-bar wording. Never empty for a valid edit.
    std::string label;

    bool valid() const noexcept { return kind != SketchEditKind::None; }
};

// =============================================================================
// The tool state machine
// =============================================================================

// Holds what the user is part-way through and what is selected. Owns no
// document and no widget; a click goes in, an edit may come out.
class SketchCanvasModel {
public:
    SketchTool tool() const noexcept { return tool_; }
    // Switching tools DISCARDS a half-finished shape. Keeping it would let two
    // clicks of a line and one of a circle combine into geometry the user never
    // drew.
    void setTool(SketchTool tool);

    // Esc. Drops a half-finished shape; with nothing pending, drops the
    // selection; with neither, returns to Select. Reports whether anything
    // changed, so a caller knows whether to repaint.
    bool cancel();

    const std::vector<Vec2>& pendingPoints() const noexcept { return pending_; }

    // Whether inference suppression is held (roadmap section 4.2 point 3). The
    // status bar must show this: a user who cannot tell that snapping is off
    // reads its absence as a bug.
    bool suppressInference() const noexcept { return suppressInference_; }
    void setSuppressInference(bool suppress) noexcept { suppressInference_ = suppress; }

    // What the user should do next. Never empty.
    std::string prompt() const;

    // How many sides the Polygon tool draws (M17.17). Held on the model so the
    // prompt can say it before the first click -- "click the centre" tells a
    // user nothing about what they are about to get.
    int polygonSides() const noexcept { return polygonSides_; }
    void setPolygonSides(int sides) noexcept {
        // Three is the fewest that closes; the upper bound is where the sides
        // stop being distinguishable on screen and the constraint count starts
        // costing more than the shape is worth.
        polygonSides_ = sides < 3 ? 3 : (sides > 64 ? 64 : sides);
    }

    // --- Selection ---------------------------------------------------------
    // Clicking TOGGLES and multi-select needs no modifier key. That is roadmap
    // section 13.1's interaction decision, adopted here and applied globally --
    // section 13.1 permits either rule but forbids mixing them.
    const std::vector<SketchElementRef>& selection() const noexcept { return selection_; }
    bool isSelected(const SketchElementRef& ref) const noexcept;
    void toggleSelection(const SketchElementRef& ref);
    void setSelection(std::vector<SketchElementRef> refs);
    bool clearSelection();

    // Select-tool click: picks whatever is within tolerance and toggles it,
    // clearing the selection when nothing is hit. Reports whether the selection
    // changed.
    bool selectAt(const Sketch& sketch, Vec2 pickMm, double toleranceMm);

    // --- Input -------------------------------------------------------------
    // A click at a snapped position, for a DRAWING tool. Feeds the shape and
    // returns a valid edit on the click that completes it; returns nothing
    // while the shape is still incomplete, and nothing at all under Select
    // (use selectAt for that).
    //
    // Chaining: Line keeps its end point as the next line's start, so a polyline
    // is drawn without re-picking. Esc ends the chain.
    SketchEdit click(const SnapResult& snap);

    // THE SPLINE'S "that was the last point". Returns an invalid edit unless a
    // spline is being drawn and has enough points to be one -- so a caller can
    // offer it on every double-click without first asking what tool is active.
    //
    // A spline that has been closed by clicking its own first point comes back
    // closed; anything else comes back open.
    SketchEdit finishPendingSpline();

    // Told to the model AFTER an edit has been applied, so a chained line can
    // reference the previous segment BY ID rather than by coordinate. Pass the
    // entities the edit actually created; an empty list (the apply failed)
    // leaves the chain geometrically joined and semantically free, which is the
    // honest outcome -- it must never invent a reference.
    void afterApply(const std::vector<SketchEntityId>& createdEntities);

    // --- Selection-driven commands -----------------------------------------
    // Each returns an invalid edit and fills `whyNot` when the selection does
    // not admit the command. `whyNot` is never left empty on refusal: "nothing
    // happened" with no reason is the failure mode roadmap section 8 is about.

    // Handles every non-dimensional constraint command, including the seven
    // M13 added. `whyNot` is filled on every refusal and names the element at
    // fault, never just "cannot".
    SketchEdit requestConstraint(const Sketch& sketch, SketchEditKind kind,
                                 std::string* whyNot) const;

    // True for a kind requestConstraint understands. Lets the shell build its
    // command list from ONE place instead of a second hand-kept list that can
    // drift out of step with what is actually implemented.
    static bool IsConstraintCommand(SketchEditKind kind) noexcept;

    // Infers the dimension type from the selection (roadmap section 7.1):
    //   one line              -> Length
    //   one circle            -> Diameter
    //   one arc               -> Radius
    //   two points            -> Distance
    //   two lines             -> Angle
    // Pass an explicit kind to override the inference -- the only genuinely
    // ambiguous case is Radius vs Diameter on the same curve.
    SketchEdit requestDimension(const Sketch& sketch, SketchEditKind explicitKind,
                                std::string* whyNot) const;

    SketchEdit requestDelete(const Sketch& sketch, std::string* whyNot) const;

private:
    SketchEdit completeDrawing(const SnapResult& snap);

    // What one pending click snapped to. `ref` is invalid unless the snap named
    // an element; `atOrigin` records the one snap that names nothing and still
    // has to become a constraint. The two are mutually exclusive by
    // construction: SnapCursor only reaches the origin after no defined point
    // was within tolerance.
    struct PendingSnap {
        SketchElementRef ref{};
        bool atOrigin{false};
    };

    SketchTool tool_{SketchTool::Select};
    std::vector<Vec2> pending_;
    // The snaps behind `pending_`, parallel to it, so a completed shape can turn
    // the snaps that produced it into real constraints.
    std::vector<PendingSnap> pendingSnaps_;
    std::vector<SketchElementRef> selection_;
    bool suppressInference_{false};
    // Set when a completed line left a chain start behind whose reference is
    // only knowable once the line has an id. Cleared by afterApply().
    bool chainFromCreatedEntity_{false};
    // A SPLINE was closed by clicking its own first point. Cleared when the
    // curve is finished, so the next one starts open.
    bool closePendingSpline_{false};
    // How near "the same point" is when closing a spline by clicking its start.
    // Generous next to kSketchToleranceMm: this is a CURSOR landing on a point,
    // not two coordinates being compared.
    double toleranceForClosing_{0.5};
    int polygonSides_{6};
};

} // namespace paramcad
