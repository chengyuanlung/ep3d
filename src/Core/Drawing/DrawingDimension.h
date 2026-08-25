#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Drawing/Tolerance.h"
#include "Core/Drawing/Geometry2D.h"

#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

// WHAT A DIMENSION MEASURES BETWEEN (M34).
//
// THE HARD PART OF A DRAWING, and worth stating plainly before the code.
//
// A dimension has to survive the model changing. When a part gets longer, the
// "80" on the drawing must become "120" -- not stay at 80, and not detach. So
// a dimension cannot store the two coordinates it measured; it has to store
// something that can be ASKED AGAIN after the view is reprojected.
//
// THREE KINDS OF ANCHOR, and they are honestly different:
//
//   Free      a coordinate on the paper. Never moves, never dangles, measures
//             whatever it was put on. What a leader or a note needs.
//   Entity    a snap point of something the USER drew. That entity has an
//             ObjectId, so this is exact and stays exact.
//   InView    a point in a view's MODEL space, re-found in the projection
//             after every rebuild.
//
// THE THIRD ONE IS AN APPROXIMATION AND IS LABELLED AS ONE.
//
// A projected curve has no ObjectId -- it is derived, thrown away and rebuilt
// (M32.1) -- so there is nothing exact to point at. What is stored instead is
// the model-space point, and after a rebuild the nearest snap point of the
// projection is adopted if it is within `toleranceMm`.
//
// WHAT THAT BUYS: a hole that moves 2 mm, a face that shifts, a fillet that
// changes radius -- the dimension follows and re-reads.
//
// WHAT IT DOES NOT: a feature that moves FURTHER than the tolerance dangles.
// That is a real limit, and the reason it is acceptable is that dangling is
// LOUD -- the dimension says so, the tree says so, and the number is not
// shown. The failure mode this rules out is the one that matters: a dimension
// that silently re-attaches to a different feature and prints a plausible
// wrong number.
//
// THE PROPER FIX is a topological reference from the kernel -- EP3D already
// has FaceQuery, which answers "which face" as a sentence re-asked every
// rebuild (ADR-M17-036), and edges want the same. That is named work, not a
// footnote, and it is why this file says "approximation" rather than
// pretending.
enum class DimensionAnchorKind { Free, Entity, InView };

std::string_view toString(DimensionAnchorKind kind) noexcept;

// WHAT KIND OF POINT an in-view anchor was put on (M43).
//
// The reason this exists: "the nearest snap point within tolerance" does not
// know that a hole's CENTRE and a corner of the plate are different KINDS of
// thing. Move the part a little and a diameter dimension can re-attach to a
// corner -- which measures a real distance between two real points, prints a
// plausible number, and is not the dimension anybody put there.
//
// Narrowing by role first means a centre only ever re-finds a centre. It is
// not a full topological name and is not claimed as one; what it does is take
// away the two ways an anchor could silently land on something else -- the
// other being ambiguity, which is refused rather than guessed at.
enum class ViewPointRole {
    Corner,   // the end of a straight edge
    Middle,   // the midpoint of a straight edge
    Centre,   // the centre of a circle or arc -- not on the curve at all
    CurveEnd, // where an arc starts or stops
};
std::string_view toString(ViewPointRole role) noexcept;
bool ParseViewPointRole(std::string_view text, ViewPointRole& into) noexcept;

struct DimensionAnchor {
    DimensionAnchorKind kind = DimensionAnchorKind::Free;
    // Free: the point, in SHEET millimetres.
    // Entity: unused.
    // InView: the point, in the view's MODEL millimetres.
    Vec2 at{};
    // Entity: which one, and which of its snap points.
    ObjectId entityId = kInvalidObjectId;
    int snapIndex = 0;
    // InView: which view.
    ObjectId viewId = kInvalidObjectId;
    // InView: WHAT KIND of point this was, so it can only ever re-find that
    // kind. Ignored by the other two kinds of anchor, which name their point
    // exactly rather than looking for it.
    ViewPointRole role = ViewPointRole::Corner;
    // InView: how far the point may have moved and still be the same point.
    // Generous by default -- a dimension that dangles on a 1 mm change is a
    // dimension nobody keeps.
    double toleranceMm = 5.0;

    static DimensionAnchor free(Vec2 sheetMm);
    static DimensionAnchor onEntity(ObjectId entityId, int snapIndex);
    // THE ROLE IS NOT DEFAULTED. Every caller has to say what kind of point it
    // is anchoring, because a default would be "corner" and a diameter
    // dimension quietly anchored to a corner is precisely the failure the role
    // was added to stop.
    static DimensionAnchor inView(ObjectId viewId, Vec2 modelMm, ViewPointRole role,
                                  double toleranceMm = 5.0);
};

// WHICH MEASUREMENT. Not a style -- a different QUESTION about the geometry.
//
//   Linear     the distance between two points, along a given direction. What
//              DIMLINEAR and DIMALIGNED both are: the difference is which
//              direction, so they are one kind with a direction rather than
//              two kinds that share their code.
//   Radius     of a circle or arc.
//   Diameter   of the same, times two, drawn differently and prefixed.
//   Angular    between two lines, in degrees.
enum class DimensionKind { Linear, Radius, Diameter, Angular };

std::string_view toString(DimensionKind kind) noexcept;

// The direction a LINEAR dimension measures along.
//
//   Aligned      along the line between the two points -- the true distance.
//   Horizontal   the X component only.
//   Vertical     the Y component only.
//
// AutoCAD makes these three commands; they are one measurement with three
// projections, and keeping them one kind is what stops the three drifting
// apart in how they draw their extension lines.
enum class LinearDirection { Aligned, Horizontal, Vertical };

std::string_view toString(LinearDirection direction) noexcept;

class DrawingDimension {
public:
    DrawingDimension(DimensionKind kind, DimensionAnchor first, DimensionAnchor second,
                     Vec2 linePositionMm, ObjectId styleId, ObjectId layerId);
    DrawingDimension(ObjectId id, DimensionKind kind, DimensionAnchor first,
                     DimensionAnchor second, Vec2 linePositionMm, ObjectId styleId,
                     ObjectId layerId);

    ObjectId id() const noexcept { return id_; }
    static std::string_view typeName() noexcept { return "Dimension"; }

    DimensionKind kind() const noexcept { return kind_; }
    const DimensionAnchor& first() const noexcept { return first_; }
    const DimensionAnchor& second() const noexcept { return second_; }
    void setFirst(DimensionAnchor anchor) { first_ = anchor; }
    void setSecond(DimensionAnchor anchor) { second_ = anchor; }

    LinearDirection direction() const noexcept { return direction_; }
    void setDirection(LinearDirection direction) noexcept { direction_ = direction; }

    // Where the dimension LINE sits, in sheet millimetres. The user drags this;
    // it is not derived, because where a dimension is readable is a judgement
    // no rule makes well.
    Vec2 linePositionMm() const noexcept { return linePositionMm_; }
    void setLinePositionMm(Vec2 at) noexcept { linePositionMm_ = at; }

    ObjectId styleId() const noexcept { return styleId_; }
    void setStyleId(ObjectId styleId) noexcept { styleId_ = styleId; }
    ObjectId layerId() const noexcept { return layerId_; }
    void setLayerId(ObjectId layerId) noexcept { layerId_ = layerId; }

    // AN OVERRIDE, when the drafter has to say something the measurement does
    // not -- "2x", "TYP", a theoretical dimension in a box. EMPTY means "use
    // the measurement", which is the case that must stay the default: a
    // drawing full of typed-in numbers is a drawing that stops tracking its
    // model, and that is the failure this whole block exists to prevent.
    // WHAT THIS SIZE IS ALLOWED TO BE (M37).
    //
    // A dimension without one is incomplete on anything that has to fit: "25"
    // on a bore says the size and nothing about how close, so the machinist
    // telephones. A FIT keeps its code and derives its numbers, so correcting
    // the table corrects every drawing already made.
    const DimensionTolerance& tolerance() const noexcept { return tolerance_; }
    void setTolerance(DimensionTolerance tolerance) { tolerance_ = std::move(tolerance); }

    const std::string& textOverride() const noexcept { return textOverride_; }
    void setTextOverride(std::string text) { textOverride_ = std::move(text); }

private:
    ObjectId id_;
    DimensionKind kind_;
    DimensionAnchor first_;
    DimensionAnchor second_;
    LinearDirection direction_ = LinearDirection::Aligned;
    Vec2 linePositionMm_{};
    ObjectId styleId_ = kInvalidObjectId;
    ObjectId layerId_ = kInvalidObjectId;
    std::string textOverride_;
    DimensionTolerance tolerance_;
};

// WHAT A DIMENSION CURRENTLY READS.
//
// Resolved, not stored: the anchors are asked again every time. `ok` false
// means an anchor could not be found -- the dimension is DANGLING, and the
// value must not be shown.
struct DimensionMeasurement {
    bool ok = false;
    std::string why;      // set when it is not ok, and only then
    double valueMm = 0.0; // in MODEL millimetres: the size of the part
    Vec2 firstMm{};       // both ends, in SHEET millimetres, for drawing
    Vec2 secondMm{};
};

} // namespace paramcad
