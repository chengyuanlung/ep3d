#pragma once

#include "Core/Geometry/MathTypes.h"

#include <optional>
#include <vector>

namespace paramcad {

// PLANE GEOMETRY FOR THE DRAWING SHEET (M33).
//
// Ported from EasyCad's `EasyCad.Core.Geometry` -- Vec2 arithmetic, a 2D affine
// transform, a bounding box and the handful of intersections a drafting
// package actually needs. That code was written once, proved against a working
// AutoCAD-alike, and its edge cases are already decided; what crosses the line
// is those decisions, not the VB.
//
// WHY THIS IS NOT THE SKETCH'S GEOMETRY. A sketch is SOLVED: its coordinates
// are outputs of a constraint system, and nothing may move a point except the
// solver. A drawing's authored geometry is DRAWN: the user's click is the
// coordinate, and a move is a move. Sharing a representation between them
// would mean every operation here had to ask which of the two it was dealing
// with -- and the day one forgot, a drag would quietly edit a constrained
// sketch or a solve would quietly move a drawn line.
//
// WHAT IS SHARED is Vec2 and Transform3D's arithmetic where it applies. This
// header adds only what is two-dimensional and only what has a caller.

// 2D affine, row-major, as EasyCad's Matrix3 was:
//
//   | m00 m01 m02 |
//   | m10 m11 m12 |
//   |  0   0   1  |
//
// Kept 2D rather than borrowing Transform3D: a drawing has no third dimension
// to be wrong about, and a 3D transform on a sheet would let a Z creep in that
// nothing downstream could draw.
struct Matrix2D {
    double m00 = 1.0, m01 = 0.0, m02 = 0.0;
    double m10 = 0.0, m11 = 1.0, m12 = 0.0;

    Vec2 apply(Vec2 point) const noexcept;
    // The direction only -- no translation. What a tangent or a normal needs.
    Vec2 applyDirection(Vec2 direction) const noexcept;
    Matrix2D then(const Matrix2D& after) const noexcept;

    // How much this scales lengths. A radius has to be scaled by it, and a
    // NON-UNIFORM transform has no single answer -- so this reports the one it
    // would use and `isUniform` says whether to believe it.
    double scaleFactor() const noexcept;
    bool isUniform() const noexcept;

    static Matrix2D translation(Vec2 by) noexcept;
    static Matrix2D rotation(double radians) noexcept;
    static Matrix2D rotationAbout(Vec2 centre, double radians) noexcept;
    static Matrix2D scaleAbout(Vec2 centre, double factor) noexcept;
    // A MIRROR, about the line through `a` and `b`. Reflection is the one
    // common transform that flips orientation, which is why an arc has to be
    // told about it rather than just having its points moved.
    static Matrix2D mirror(Vec2 a, Vec2 b) noexcept;
};

struct Box2D {
    Vec2 min{0.0, 0.0};
    Vec2 max{0.0, 0.0};
    bool empty = true;

    void grow(Vec2 point) noexcept;
    void grow(const Box2D& other) noexcept;
    double width() const noexcept { return empty ? 0.0 : max.x - min.x; }
    double height() const noexcept { return empty ? 0.0 : max.y - min.y; }
    bool contains(Vec2 point) const noexcept;
    // Fully inside `window`? The AutoCAD "window" selection, as against
    // "crossing" -- two different rules, and a package that had only one of
    // them would surprise every user who has met the other.
    bool insideOf(const Box2D& window) const noexcept;
    bool touches(const Box2D& other) const noexcept;
};

// --- The handful of intersections a drafting package needs -------------------
//
// Every one of these was in EasyCad and every one has a caller here: TRIM and
// EXTEND ask where two things cross, FILLET asks for the tangent point, and
// osnap's INTERSECTION mode asks all of them.

double DistancePointToSegment(Vec2 point, Vec2 a, Vec2 b) noexcept;
Vec2 ClosestPointOnSegment(Vec2 point, Vec2 a, Vec2 b) noexcept;

// `asSegments` false treats both as INFINITE lines, which is what EXTEND
// needs -- it asks where a line WOULD meet a boundary it does not yet reach.
std::optional<Vec2> LineLineIntersection(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2,
                                         bool asSegments) noexcept;

std::vector<Vec2> LineCircleIntersection(Vec2 a, Vec2 b, Vec2 centre, double radius,
                                         bool asSegment) noexcept;

std::vector<Vec2> CircleCircleIntersection(Vec2 centreA, double radiusA, Vec2 centreB,
                                           double radiusB) noexcept;

// Is `angle` inside the sweep from `start` to `end`, counter-clockwise? The
// question every arc operation reduces to, answered once -- EasyCad had it in
// one place for the same reason and it is where its arc trimming got right
// what a per-call comparison would have got wrong at the wrap.
bool AngleWithinArc(double angle, double startAngle, double endAngle) noexcept;

// Normalised to [0, 2*pi).
double NormaliseAngle(double angle) noexcept;

// The circle through three points, or nothing when they are collinear. What
// the 3-point ARC command is.
struct CircleThroughPoints {
    Vec2 centre{};
    double radius = 0.0;
};
std::optional<CircleThroughPoints> CircleFrom3Points(Vec2 a, Vec2 b, Vec2 c) noexcept;

// A BULGE is AutoCAD's way of putting an arc in a polyline: the tangent of a
// quarter of the included angle, signed for direction. Kept because it is what
// DXF stores, and converting on the way in and out would make a round trip
// lossy at every vertex.
struct BulgeArc {
    Vec2 centre{};
    double radius = 0.0;
    double startAngle = 0.0;
    double endAngle = 0.0;
    bool counterClockwise = true;
};
std::optional<BulgeArc> ArcFromBulge(Vec2 from, Vec2 to, double bulge) noexcept;

// Points along a circular arc, at most `chordToleranceMm` from the true curve.
// What FLATTEN gives a hatch, a boolean or a plotter that cannot draw arcs.
std::vector<Vec2> TessellateArc(Vec2 centre, double radius, double startAngle, double endAngle,
                                double chordToleranceMm);

} // namespace paramcad
