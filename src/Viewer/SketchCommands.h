#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Geometry/MathTypes.h"
#include "Core/Sketch/SketchConstraint.h"
#include "Core/Sketch/SketchTypes.h"
#include "Viewer/SketchCanvas.h"

#include <string>
#include <vector>

namespace paramcad {

class PartDocument;
class Sketch;

// M12.2 -- the half of the sketch UI that TOUCHES A DOCUMENT, still free of Qt.
//
// SketchCanvas.h decides what the user asked for; this applies it. The split is
// not cosmetic: a `SketchEdit` can be built and asserted without a document,
// and everything here can be driven without a widget, so the only thing left
// for a smoke test to check at the screen is what is actually painted.
//
// EVERY mutation goes through PartDocument's facade inside ONE transaction, so
// a command that creates a Parameter AND a constraint is ONE undo step
// (roadmap section 15: one user action = one undo). A rectangle -- four lines
// and eight constraints -- is likewise one step, not twelve.

struct SketchEditOutcome {
    bool applied{false};

    // One line for the status bar. Always populated: silence after a command is
    // indistinguishable from a command that did nothing.
    std::string status;

    // Longer explanation for a tooltip or a message box. Empty on success.
    std::string detail;

    std::vector<SketchEntityId> createdEntities;
    std::vector<SketchConstraintId> createdConstraints;
    // The Parameter a dimension command created, or kInvalidObjectId.
    ObjectId createdParameter{kInvalidObjectId};
};

// Applies one edit. On ANY failure the transaction is aborted, so a partly
// built rectangle never reaches the document.
SketchEditOutcome ApplySketchEdit(PartDocument& document, ObjectId sketchId,
                                  const SketchEdit& edit);

// =============================================================================
// Using projected reference geometry (M17.6, ADR-M17-029)
// =============================================================================

// Turns one projected reference curve into an ORDINARY sketch entity.
//
// The returned edit is an ordinary AddPoint / AddLine / AddCircle / AddArc --
// deliberately not a new edit kind. Everything that already works for drawn
// geometry then works for converted geometry for free: the undo delta, the
// label, the status wording, the transaction that aborts cleanly. A dedicated
// "convert" path would be a second way to create an entity, and the two would
// drift.
//
// It also carries the Fix constraints that keep the DOF readout honest. What
// gets fixed is only what can be fixed WITHOUT redundancy:
//   * a point, and a line's two endpoints -- fully pinned, 4 residuals for a
//     line's 4 degrees of freedom;
//   * a circle's or an arc's CENTRE only. Pinning an arc's centre and both
//     tips would be six residuals against five degrees of freedom, and the
//     solver would rightly report the sketch over-constrained. The radius (and
//     an arc's sweep) stay free, and the returned message says so rather than
//     letting the user discover it from a DOF count.
//
// Refuses, with a reason, when the reference is gone or when an entity with
// the same geometry is already in the sketch -- a duplicate edge lying exactly
// on top of another is how a profile ends up ambiguous, and it is invisible on
// screen.
struct ConvertReferencePlan {
    bool ok{false};
    std::string message; // always set, on success and refusal alike
    SketchEdit edit{};
};

ConvertReferencePlan PlanConvertReference(const Sketch& sketch, SketchReferenceId referenceId);


// =============================================================================
// What the user is shown
// =============================================================================

// Constraint state, in the two channels roadmap A06 requires: never colour
// alone. `badge` is a short text token the widget prints NEXT TO whatever
// colour or icon it also uses.
struct SketchStatusLine {
    enum class Severity { Ok, Info, Warning, Error };

    Severity severity{Severity::Info};
    std::string badge;  // "OK", "DOF", "REDUNDANT", "CONFLICT", "FAILED"
    std::string text;   // "Under constrained -- DOF 3"
    std::string detail; // the solver's own message, plus offending constraint ids
};

SketchStatusLine DescribeSketchStatus(const Sketch& sketch);

// One row of the constraint manager (roadmap section 6.3): every constraint in
// the sketch, what it binds, and whether the last solve blamed it.
struct ConstraintRow {
    SketchConstraintId id{kInvalidSketchConstraintId};
    std::string kind;      // "Horizontal", "Diameter"
    std::string elements;  // "Line #12", "Point #4, Line #12 end"
    std::string parameter; // "d1 = 100 mm" for a dimension, empty otherwise
    bool dimensional{false};
    // Named by the last solve as part of a conflicting or redundant set. This
    // is what makes roadmap section 8.2's "which constraints are at fault"
    // answerable instead of "Solver failed".
    bool offending{false};
};

std::vector<ConstraintRow> ConstraintRowsFor(const PartDocument& document, const Sketch& sketch);

// One straight stroke of a dimension, in sketch millimetres.
struct DimensionSegment {
    Vec2 fromMm{};
    Vec2 toMm{};
};

// An arrowhead: where its POINT sits and which way it points.
//
// The size is deliberately absent. Extension and dimension lines belong to the
// drawing and scale with it, but an arrowhead that scaled with the zoom would
// be a speck at 1:10 and swallow the geometry at 50:1 -- so the widget draws
// these at a fixed PIXEL size, and only the widget knows about pixels.
struct DimensionArrow {
    Vec2 tipMm{};
    Vec2 directionMm{}; // unit vector; the arrow points THIS way
};

// The arc of an angular dimension.
struct DimensionArc {
    Vec2 centreMm{};
    double radiusMm{0.0};
    double startRad{0.0};
    double endRad{0.0};
};

// A dimension as it should be DRAWN, in the shape a draughtsman would expect:
// extension lines standing off the geometry, a dimension line between them,
// arrowheads at its ends, and the value sitting on it.
//
// EVERYTHING here is geometry in sketch millimetres, computed in this Qt-free
// layer. The widget strokes what it is given and decides nothing (ADR-M12-001),
// which is also what lets a test assert that a 100 mm length really does get
// two arrows pointing in opposite directions.
struct DimensionAnnotation {
    SketchConstraintId id{kInvalidSketchConstraintId};
    ObjectId parameterId{kInvalidObjectId};
    SketchEditKind kind{SketchEditKind::None};
    std::string text; // "100", "R25", "D50", "45deg"

    // Where the TEXT sits, and how it is turned. The angle is always kept
    // within +/-90 degrees of upright: a dimension whose value reads upside
    // down is a dimension nobody checks.
    Vec2 labelMm{};
    double textAngleRad{0.0};

    // The two extension lines (empty for radial/diametral), then the dimension
    // line itself. Drawn as plain strokes in order.
    std::vector<DimensionSegment> extensionLines;
    std::vector<DimensionSegment> dimensionLines;
    std::vector<DimensionArrow> arrows;

    bool hasArc{false};
    DimensionArc arc{};

    // True when the position came from the USER dragging the value, false when
    // it was computed. The distinction is what stops the auto-layout pass from
    // shoving a dimension somebody deliberately put somewhere.
    bool userPlaced{false};

    bool offending{false};
};

// OFFSET: a parallel copy of a line, or a concentric copy of a circle or arc,
// at `distanceMm` -- with the constraints that say so (roadmap 4.1.1).
//
// The AutoCAD command copies geometry and walks away; a sketch offset that did
// the same would leave the copy free to drift the moment anything solved, and
// the DOF readout would count freedoms the user believes they have spent. What
// makes this an offset rather than a second line that happens to be parallel is
// the constraints, so they are the point of the operation and not a garnish:
//
//   line          Parallel(new, source) + Equal(new, source) +
//                 PointLineDistance(new.start -> source) = distance
//   circle / arc   Concentric(new, source) + Radius(new) = r +/- distance
//
// The line case deliberately leaves ONE degree of freedom -- the copy may still
// slide along its own direction. Pinning it would need a fourth constraint
// expressing "the ends line up", which is a claim the user did not make; an
// offset is about the perpendicular gap, and a sketch that says so honestly is
// better than one that quietly invents an alignment.
//
// `side` picks which way: +1 is the LEFT of the source's start->end direction
// (and outward for a curve), -1 the other. The caller decides from where the
// user clicked, because that is the only place the intent exists.
SketchEdit MakeOffsetEdit(const Sketch& sketch, SketchEntityId sourceId, double distanceMm,
                          double side, std::string* whyNot);

// --- TRIM (M17) --------------------------------------------------------------

// Where `target` crosses `cutter`, as positions ALONG the target line expressed
// as t in [0,1] from its start to its end.
//
// Only points that lie on BOTH entities count: a line crosses the infinite
// circle through an arc in places the arc itself does not reach, and trimming
// to one of those would cut at a point the user cannot see.
std::vector<double> TrimCutsAlongLine(const Sketch& sketch, SketchEntityId targetId,
                                      SketchEntityId cutterId);

// The same question for an ARC target, as positions along its sweep in [0,1]
// from its start angle to its end angle.
//
// Only crossings on the target's OWN sweep count, and only where the cutter
// actually reaches: the circle through an arc goes all the way round, and
// cutting out there would trim at a place with nothing drawn on it.
std::vector<double> TrimCutsAlongArc(const Sketch& sketch, const SketchArc& arc,
                                     SketchEntityId targetId, SketchEntityId cutterId);

// What a trim would do, before anything is changed.
struct TrimPlan {
    bool ok{false};
    std::string why;              // filled when !ok, never empty on refusal
    SketchEntityId target{kInvalidSketchEntityId};
    // The entity as it would be afterwards. A geometry rather than a line since
    // M17: trimming an arc changes its sweep, which is a different shape of
    // answer to moving a line's endpoint.
    SketchGeometry result{};
    // Which end was cut back. Reported so a caller can say so, and so a test
    // can tell "trimmed the right piece" from "trimmed the same length off the
    // wrong end".
    bool trimmedStart{false};
};

// Plans the trim of `targetId` at `pickMm`: the piece of the line containing
// the pick is removed, back to the nearest crossing on either side.
//
// LINES AND ARCS as targets. An arc became possible with ADR-M17-018: its tips
// are solver variables now, so a trimmed arc is held by them.
//
// A CIRCLE is still refused. Trimming one does not shorten it -- it turns it
// into an arc, a change of KIND rather than of extent, and every constraint
// naming its Whole would suddenly be naming something else.
//
// A pick BETWEEN two crossings is also refused: removing a middle piece splits
// one line into two, which is a different operation with a different question
// (which half keeps the constraints?) and it is not answered by guessing.
TrimPlan PlanTrim(const Sketch& sketch, SketchEntityId targetId,
                  const std::vector<SketchEntityId>& cutterIds, Vec2 pickMm);

// Plans the EXTEND of `targetId`: the end nearer `pickMm` is stretched to the
// first place it would meet one of `boundaryIds`.
//
// The same shape as PlanTrim and the same reasons: lines only, in place, and
// the end is chosen by where the user pointed rather than by a rule they would
// have to learn. A TrimPlan is reused because the answer has the same form --
// this line, reshaped, with one end moved.
//
// Crossings BEHIND the chosen end do not count. Extending is growing; a
// boundary the line already passes through is not somewhere to grow to, and
// stretching backwards to reach it would shrink the line while calling itself
// extend.
TrimPlan PlanExtend(const Sketch& sketch, SketchEntityId targetId,
                    const std::vector<SketchEntityId>& boundaryIds, Vec2 pickMm);

// --- TRANSFORM (M17.24) ------------------------------------------------------
//
// Move, rotate or scale what is selected -- and optionally leave a copy behind.
//
// IN PLACE, this only rewrites coordinates. It adds no constraint and removes
// none: the sketch's constraints are still the truth about the shape, and the
// solve that follows is entitled to pull everything back where it was. That is
// not the command failing -- it is the sketch saying it was already pinned --
// and the status says so rather than letting a button look broken.

enum class SketchTransformKind { Move, Rotate, Scale };

struct SketchTransform {
    SketchTransformKind kind{SketchTransformKind::Move};
    Vec2 deltaMm{};        // Move
    double angleRad{0.0};  // Rotate, counter-clockwise about the anchor
    double factor{1.0};    // Scale, about the anchor. Must be positive: a
                           // negative factor is a mirror, and Mirror exists.
    bool keepACopy{false};
};

// `geometry` moved, turned or resized about `anchor`. THE one place the
// arithmetic lives, so the preview, the command and the tests cannot disagree
// about where something lands.
SketchGeometry TransformedGeometry(const SketchGeometry& geometry,
                                   const SketchTransform& transform, Vec2 anchor);

// What a rotation or a scale turns about.
//
// A selected POINT when the selection contains exactly one, because that is a
// user saying where; otherwise the centre of what is selected. Which one was
// used is reported, since the two give visibly different results and a silent
// choice here would look like the command misbehaving.
Vec2 TransformAnchor(const Sketch& sketch, const std::vector<SketchEntityId>& entities,
                     bool* fromSelectedPoint);

struct TransformOutcome {
    bool applied{false};
    std::string status;
    std::vector<SketchEntityId> created; // empty unless keepACopy
    Vec2 anchor{};
    int constraintsCopied{0};
    // Constraints that reach OUT of the selection. A copy cannot have them --
    // it would be pinned to the original's neighbours -- so it does not, and
    // the count is reported. Nothing the user typed is lost: the original keeps
    // every one of them.
    int constraintsLeftBehind{0};
};

TransformOutcome ApplyTransform(PartDocument& document, ObjectId sketchId,
                                const std::vector<SketchEntityId>& entities,
                                const SketchTransform& transform);

// --- SPLIT (M17.23) ----------------------------------------------------------
//
// Cuts one entity at every point where the others cross it, and keeps all the
// pieces. The operation PlanTrim's own comment says it is not: "removing a
// middle piece splits one line into two, which is a different operation with a
// different question (which half keeps the constraints?) and it is not answered
// by guessing."
//
// This is that question, answered explicitly.

// WHAT BECOMES OF A CONSTRAINT when the thing it names is cut in two.
enum class SplitSurvival {
    // The property belongs to every piece: a sub-segment of a horizontal line
    // is horizontal, and a sub-arc has its parent's centre and radius. The
    // constraint is COPIED onto each piece.
    EveryPiece,
    // It names a POINT, so it goes to whichever piece still has that point.
    OwningPiece,
    // It is about the whole EXTENT -- a length, an equality of lengths, a
    // tangency somewhere along it. Splitting changes what it says, and there is
    // no piece it belongs to. The split is REFUSED and this constraint named,
    // because quietly dropping it or quietly copying it are both ways of
    // changing a model the user did not ask to change.
    Refuse
};

// The verdict for ONE constraint, given what is being split.
//
// A single function, exhaustive over the constraint variant, so a new
// constraint kind cannot acquire a default. That matters more here than
// usual: the wrong answer is silent and only shows up when the sketch is next
// dragged.
SplitSurvival SurvivesSplit(const SketchConstraintData& data, SketchEntityId target,
                            const SketchGeometry& targetGeometry);

struct SplitPiece {
    SketchGeometry geometry{};
    // Whether this piece still owns the original's start or end, which is how a
    // constraint naming one of them finds its new home.
    bool keepsStart{false};
    bool keepsEnd{false};
};

// What a split would do, before anything changes.
struct SplitPlan {
    bool ok{false};
    std::string why; // always set on refusal, and it names the obstacle
    SketchEntityId target{kInvalidSketchEntityId};
    // In order along the original, so piece i's end is piece i+1's start.
    std::vector<SplitPiece> pieces;
    // A CIRCLE comes back as a ring of arcs, and the last one closes onto the
    // first. Nothing else does.
    bool closesTheLoop{false};
};

// Where `targetId` would be cut by `cutterIds`, and what the pieces would be.
//
// A crossing AT an end is not a cut: it produces a zero-length piece and
// changes nothing. A circle needs TWO crossings, because one cut does not open
// a closed curve.
SplitPlan PlanSplit(const Sketch& sketch, SketchEntityId targetId,
                    const std::vector<SketchEntityId>& cutterIds);

struct SplitOutcome {
    bool applied{false};
    std::string status;
    std::vector<SketchEntityId> created;
};

// Performs the split: the original goes, the pieces arrive, each piece is
// joined to the next by a Coincident, and every constraint that named the
// original is re-hung per SurvivesSplit.
//
// The joints are what make this a SPLIT rather than "delete it and draw two":
// without them the pieces are unrelated geometry that happens to line up today.
// What is deliberately NOT added is any tie to the CUTTER -- the split point
// following the crossing forever is a further claim, and Point-on-object says
// it when the user wants it.
SplitOutcome ApplySplit(PartDocument& document, ObjectId sketchId, SketchEntityId targetId,
                        const std::vector<SketchEntityId>& cutterIds);

// CHAMFER: cuts the corner where two lines meet and puts a line across it.
//
// The parametric part is what happens to the corner's OWN constraint. Two lines
// that met at a coincident vertex no longer meet, so that Coincident is
// deleted -- it would otherwise be a constraint demanding the two ends occupy
// one point, which is exactly what the chamfer just stopped being true. Two new
// Coincidents put the chamfer line's ends onto the two setbacks instead, so the
// three pieces stay one connected run.
//
// The net effect on the sketch is +2 degrees of freedom, and those two are the
// setbacks. Dimension them and the corner is pinned; leave them and the chamfer
// slides, which is an honest report of what the user has actually said so far.
//
// `distanceA` is measured back along `lineA` from the corner, `distanceB` along
// `lineB`. A corner deeper than either line is long is refused.
struct ChamferOutcome {
    bool applied{false};
    std::string status;
    SketchEntityId created{kInvalidSketchEntityId};
};

ChamferOutcome ApplyChamfer(PartDocument& document, ObjectId sketchId, SketchEntityId lineAId,
                            SketchEntityId lineBId, double distanceA, double distanceB);

// MIRROR: reflected copies of `sourceIds` across `mirrorId`, each held there by
// Symmetric constraints (roadmap 4.1.1).
//
// The copies are not stamps. Every point of every copy is tied to its original
// by a Symmetric constraint, so moving the source -- or the mirror line itself
// -- carries the reflection with it. A mirror that only copied would drift the
// moment anything solved, and the DOF readout would count freedoms the user
// believes the symmetry has spent.
//
// Points, lines, circles AND arcs. Arcs became possible with ADR-M17-018: their
// tips are solver variables now, so a mirrored arc can be held by them.
//
// What ties each kind differs, and each set is chosen to be EXACTLY determined
// rather than merely sufficient -- a redundant-but-consistent set makes the
// solver report over-constrained, and roadmap 8.2 wants that reading to mean
// something:
//
//   point   Symmetric(point, copy)
//   line    Symmetric on BOTH ends
//   circle  Symmetric(centre) + Equal
//   arc     Symmetric on both tips, CROSSED, + Equal -- and NOT the centre,
//           which those five equations have already placed
struct MirrorOutcome {
    bool applied{false};
    std::string status;
    std::vector<SketchEntityId> created;
};

MirrorOutcome ApplyMirror(PartDocument& document, ObjectId sketchId,
                          const std::vector<SketchEntityId>& sourceIds, SketchEntityId mirrorId);

// FILLET: rounds the corner where two lines meet, with an arc of `radiusMm`.
//
// The whole reason this waited for arc-tip variables. A fillet's arc is not a
// decoration dropped in the corner -- its two ends have to STAY on the two
// lines and it has to STAY tangent to both, through every later solve. That is
// four constraints on points an arc did not have until M17:
//
//   Coincident(lineA end, arc tip)      x2
//   Tangent(line, arc)                  x2
//
// plus the two lines trimmed back to the tangent points. Built with the arc's
// angular extent held fixed, the tips would detach from the lines on the first
// solve -- geometry that looks right until something moves, which is the defect
// class this project keeps catching.
//
// Uses the same ChamferOutcome shape: the two commands differ only in what they
// put in the corner.
ChamferOutcome ApplyFillet(PartDocument& document, ObjectId sketchId, SketchEntityId lineAId,
                           SketchEntityId lineBId, double radiusMm);

// The sketch origin as a REAL entity: a Point at (0,0) with a Fix on it.
//
// The canvas has always drawn a marker at the origin and snapped to it, but a
// marker is not a thing -- nothing could select it, so nothing could dimension
// FROM it, which is the one measurement almost every mechanical sketch starts
// with. Making the solver understand a constant point would mean a new residual
// variant for every constraint kind that could name it; an ordinary fixed Point
// gives selection, Distance, Horizontal, the constraint list and serialization
// for free, and the solver never learns a new word.
//
// It is an ORDINARY entity in every other respect -- listed, deletable, and
// restored by undo like anything else. Nothing here is a special case, and that
// is the point.
SketchEdit MakeOriginPointEdit();

// The sketch's origin point, or an invalid id. A Point entity AT (0,0), which
// is what MakeOriginPointEdit leaves behind; the Fix is not part of the test,
// so a user who deleted the Fix still has an origin to measure from.
SketchEntityId FindSketchOrigin(const Sketch& sketch);

// A non-dimensional constraint's badge: the letter or symbol shown on the
// canvas, and the point it hangs off.
//
// Here rather than in the widget for the reason ADR-M12-001 gives, and for one
// more that only showed up when the badges became CLICKABLE: painting and
// hit-testing must agree about where a badge is, and the only way to guarantee
// that is for both to read the same layout. Two copies of "a quarter of the way
// along the line, stacked 15 px apart" drift the first time either is touched,
// and the symptom is a badge that cannot be clicked where it is drawn.
//
// The pixel offsets themselves stay in the widget: a badge is READ, so its box
// is a fixed size on screen and does not scale with the zoom (same argument as
// the arrowheads).
struct ConstraintBadge {
    SketchConstraintId id{kInvalidSketchConstraintId};
    std::string glyph;  // "H", "V", "X", "o", "//", "|_", ...
    Vec2 anchorMm{};    // the badge hangs down and to the right of this
    // Stacking index among badges sharing an anchor, so several constraints on
    // one entity do not print on top of each other.
    int slot{0};
    bool offending{false};
};

// Every constraint that HAS a badge, in the sketch's own order. Dimensional
// kinds are absent -- they are drawn as dimensions, not badges.
std::vector<ConstraintBadge> ConstraintBadgesFor(const Sketch& sketch);

// `pixelsPerMm` lets the layout know how big the value and its arrowheads will
// actually BE, which is what decides whether they fit between the extension
// lines. Pass 0 to skip those decisions -- a caller that only wants the
// measured geometry, such as a test, should not have to invent a zoom.
std::vector<DimensionAnnotation> DimensionAnnotationsFor(const PartDocument& document,
                                                         const Sketch& sketch,
                                                         double pixelsPerMm = 0.0);

// Where a dimension's value would sit if it were NOT user-placed.
//
// The canvas needs this to answer "has the user actually moved it?", and the
// auto-place command needs it to know there is anything to reset.
Vec2 AutomaticDimensionLabel(const PartDocument& document, const Sketch& sketch,
                             SketchConstraintId constraintId, bool* ok);

// The value a dimension shows, with its prefix, suffix and tolerance applied.
//
// Separate from the raw number so the rule lives in ONE place: the canvas, the
// constraint panel and any future drawing all have to agree about what a
// dimension reads, and three copies of the concatenation would not.
std::string FormattedDimensionText(const Sketch& sketch, SketchConstraintId constraintId,
                                   const std::string& bareValue, bool angular);

// The value a dimension's parameter currently holds, formatted for an editor:
// millimetres as a plain number, angles in DEGREES (roadmap section 7 stores
// radians and displays degrees). Empty when the constraint is not dimensional.
std::string DimensionEditText(const PartDocument& document, const Sketch& sketch,
                              SketchConstraintId constraintId);

// Commits a value typed into a dimension editor. Accepts a plain number or an
// expression (M11): a leading '=' or any non-numeric content is treated as an
// expression and goes through PartDocument::setParameterExpression, so `#Width
// / 2` works in a dimension exactly as it does in the property panel.
//
// Angles are typed and displayed in DEGREES and converted here -- the ONE
// conversion site, so no caller has to remember which side of the boundary it
// is on.
SketchEditOutcome CommitDimensionValue(PartDocument& document, const Sketch& sketch,
                                       SketchConstraintId constraintId, const std::string& text);

// Numbers, formatted the way this UI prints them everywhere: up to 4 decimals,
// trailing zeros trimmed, never scientific notation for ordinary magnitudes.
std::string FormatNumber(double value);

} // namespace paramcad
