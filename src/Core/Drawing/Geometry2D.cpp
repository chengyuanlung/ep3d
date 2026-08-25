#include "Core/Drawing/Geometry2D.h"

#include <algorithm>
#include <cmath>

namespace paramcad {

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kPi = 3.14159265358979323846;
constexpr double kEpsilon = 1.0e-10;
} // namespace

// =============================================================================
// Matrix2D
// =============================================================================

Vec2 Matrix2D::apply(Vec2 point) const noexcept {
    return Vec2{m00 * point.x + m01 * point.y + m02, m10 * point.x + m11 * point.y + m12};
}

Vec2 Matrix2D::applyDirection(Vec2 direction) const noexcept {
    return Vec2{m00 * direction.x + m01 * direction.y, m10 * direction.x + m11 * direction.y};
}

Matrix2D Matrix2D::then(const Matrix2D& after) const noexcept {
    Matrix2D out;
    out.m00 = after.m00 * m00 + after.m01 * m10;
    out.m01 = after.m00 * m01 + after.m01 * m11;
    out.m02 = after.m00 * m02 + after.m01 * m12 + after.m02;
    out.m10 = after.m10 * m00 + after.m11 * m10;
    out.m11 = after.m10 * m01 + after.m11 * m11;
    out.m12 = after.m10 * m02 + after.m11 * m12 + after.m12;
    return out;
}

double Matrix2D::scaleFactor() const noexcept {
    // The geometric mean of the two axis lengths, which is the square root of
    // |det|. For a uniform transform it is exactly the scale; for a
    // non-uniform one it is the only single number that is not simply wrong,
    // and `isUniform` is how a caller finds out it should not be using it.
    const double determinant = m00 * m11 - m01 * m10;
    return std::sqrt(std::fabs(determinant));
}

bool Matrix2D::isUniform() const noexcept {
    const double xLength = std::sqrt(m00 * m00 + m10 * m10);
    const double yLength = std::sqrt(m01 * m01 + m11 * m11);
    if (!(xLength > kEpsilon) || !(yLength > kEpsilon)) return false;
    // ...and the axes still perpendicular. A shear keeps both lengths and is
    // not uniform, and an arc pushed through one is not an arc any more.
    const double dot = (m00 * m01 + m10 * m11) / (xLength * yLength);
    return std::fabs(xLength - yLength) < 1.0e-9 * std::max(xLength, yLength) &&
           std::fabs(dot) < 1.0e-9;
}

Matrix2D Matrix2D::translation(Vec2 by) noexcept {
    Matrix2D out;
    out.m02 = by.x;
    out.m12 = by.y;
    return out;
}

Matrix2D Matrix2D::rotation(double radians) noexcept {
    Matrix2D out;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    out.m00 = c;
    out.m01 = -s;
    out.m10 = s;
    out.m11 = c;
    return out;
}

Matrix2D Matrix2D::rotationAbout(Vec2 centre, double radians) noexcept {
    return translation(Vec2{-centre.x, -centre.y})
        .then(rotation(radians))
        .then(translation(centre));
}

Matrix2D Matrix2D::scaleAbout(Vec2 centre, double factor) noexcept {
    Matrix2D scale;
    scale.m00 = factor;
    scale.m11 = factor;
    return translation(Vec2{-centre.x, -centre.y}).then(scale).then(translation(centre));
}

Matrix2D Matrix2D::mirror(Vec2 a, Vec2 b) noexcept {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double length = std::sqrt(dx * dx + dy * dy);
    if (!(length > kEpsilon)) return Matrix2D{};
    const double ux = dx / length;
    const double uy = dy / length;
    // Reflection about the line through the origin along u, sandwiched between
    // moving `a` to the origin and back.
    Matrix2D reflect;
    reflect.m00 = ux * ux - uy * uy;
    reflect.m01 = 2.0 * ux * uy;
    reflect.m10 = 2.0 * ux * uy;
    reflect.m11 = uy * uy - ux * ux;
    return translation(Vec2{-a.x, -a.y}).then(reflect).then(translation(a));
}

// =============================================================================
// Box2D
// =============================================================================

void Box2D::grow(Vec2 point) noexcept {
    if (empty) {
        min = point;
        max = point;
        empty = false;
        return;
    }
    min.x = std::min(min.x, point.x);
    min.y = std::min(min.y, point.y);
    max.x = std::max(max.x, point.x);
    max.y = std::max(max.y, point.y);
}

void Box2D::grow(const Box2D& other) noexcept {
    if (other.empty) return;
    grow(other.min);
    grow(other.max);
}

bool Box2D::contains(Vec2 point) const noexcept {
    if (empty) return false;
    return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
}

bool Box2D::insideOf(const Box2D& window) const noexcept {
    if (empty || window.empty) return false;
    return min.x >= window.min.x && max.x <= window.max.x && min.y >= window.min.y &&
           max.y <= window.max.y;
}

bool Box2D::touches(const Box2D& other) const noexcept {
    if (empty || other.empty) return false;
    return !(max.x < other.min.x || min.x > other.max.x || max.y < other.min.y ||
             min.y > other.max.y);
}

// =============================================================================
// Intersections
// =============================================================================

Vec2 ClosestPointOnSegment(Vec2 point, Vec2 a, Vec2 b) noexcept {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double lengthSquared = dx * dx + dy * dy;
    if (!(lengthSquared > kEpsilon)) return a;
    double t = ((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSquared;
    t = std::clamp(t, 0.0, 1.0);
    return Vec2{a.x + t * dx, a.y + t * dy};
}

double DistancePointToSegment(Vec2 point, Vec2 a, Vec2 b) noexcept {
    const Vec2 near = ClosestPointOnSegment(point, a, b);
    return std::hypot(point.x - near.x, point.y - near.y);
}

std::optional<Vec2> LineLineIntersection(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2,
                                         bool asSegments) noexcept {
    const double ax = a2.x - a1.x;
    const double ay = a2.y - a1.y;
    const double bx = b2.x - b1.x;
    const double by = b2.y - b1.y;
    const double denominator = ax * by - ay * bx;
    // PARALLEL IS NO INTERSECTION, including collinear. Two collinear segments
    // share infinitely many points and no single one of them is "the"
    // intersection -- returning one would be a guess that TRIM would then cut
    // at.
    if (std::fabs(denominator) < kEpsilon) return std::nullopt;

    const double t = ((b1.x - a1.x) * by - (b1.y - a1.y) * bx) / denominator;
    const double u = ((b1.x - a1.x) * ay - (b1.y - a1.y) * ax) / denominator;
    if (asSegments) {
        constexpr double kOn = 1.0e-9;
        if (t < -kOn || t > 1.0 + kOn || u < -kOn || u > 1.0 + kOn) return std::nullopt;
    }
    return Vec2{a1.x + t * ax, a1.y + t * ay};
}

std::vector<Vec2> LineCircleIntersection(Vec2 a, Vec2 b, Vec2 centre, double radius,
                                         bool asSegment) noexcept {
    std::vector<Vec2> hits;
    if (!(radius > kEpsilon)) return hits;
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double lengthSquared = dx * dx + dy * dy;
    if (!(lengthSquared > kEpsilon)) return hits;

    const double fx = a.x - centre.x;
    const double fy = a.y - centre.y;
    const double halfB = fx * dx + fy * dy;
    const double c = fx * fx + fy * fy - radius * radius;
    const double discriminant = halfB * halfB - lengthSquared * c;
    if (discriminant < 0.0) return hits;

    const double root = std::sqrt(discriminant);
    for (const double sign : {-1.0, 1.0}) {
        const double t = (-halfB + sign * root) / lengthSquared;
        if (asSegment && (t < -1.0e-9 || t > 1.0 + 1.0e-9)) continue;
        hits.push_back(Vec2{a.x + t * dx, a.y + t * dy});
        // A TANGENT TOUCHES ONCE. Reporting it twice would make TRIM cut the
        // same place twice and leave a zero-length piece behind.
        if (root < kEpsilon) break;
    }
    return hits;
}

std::vector<Vec2> CircleCircleIntersection(Vec2 centreA, double radiusA, Vec2 centreB,
                                           double radiusB) noexcept {
    std::vector<Vec2> hits;
    const double dx = centreB.x - centreA.x;
    const double dy = centreB.y - centreA.y;
    const double distance = std::hypot(dx, dy);
    if (!(distance > kEpsilon)) return hits; // concentric: none, or all of them
    if (distance > radiusA + radiusB + kEpsilon) return hits;
    if (distance < std::fabs(radiusA - radiusB) - kEpsilon) return hits;

    const double a = (radiusA * radiusA - radiusB * radiusB + distance * distance) /
                     (2.0 * distance);
    const double hSquared = radiusA * radiusA - a * a;
    const double h = hSquared > 0.0 ? std::sqrt(hSquared) : 0.0;
    const Vec2 middle{centreA.x + a * dx / distance, centreA.y + a * dy / distance};
    if (h < kEpsilon) {
        hits.push_back(middle);
        return hits;
    }
    hits.push_back(Vec2{middle.x + h * dy / distance, middle.y - h * dx / distance});
    hits.push_back(Vec2{middle.x - h * dy / distance, middle.y + h * dx / distance});
    return hits;
}

double NormaliseAngle(double angle) noexcept {
    double out = std::fmod(angle, kTwoPi);
    if (out < 0.0) out += kTwoPi;
    return out;
}

bool AngleWithinArc(double angle, double startAngle, double endAngle) noexcept {
    // MEASURED FROM THE START, so the wrap at 2*pi is handled once instead of
    // at every call site. That wrap is where a per-call comparison gets an arc
    // crossing zero wrong, and it gets it wrong SILENTLY -- the trim cuts the
    // piece that was meant to stay.
    const double sweep = NormaliseAngle(endAngle - startAngle);
    const double toIt = NormaliseAngle(angle - startAngle);
    constexpr double kOn = 1.0e-9;
    // A full circle: every angle is inside it.
    if (sweep < kOn) return true;
    return toIt <= sweep + kOn;
}

std::optional<CircleThroughPoints> CircleFrom3Points(Vec2 a, Vec2 b, Vec2 c) noexcept {
    const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    // COLLINEAR HAS NO CIRCLE. Returning a huge one would let a 3-point arc
    // through three points in a row draw something the user cannot see and
    // cannot select.
    if (std::fabs(area) < kEpsilon) return std::nullopt;

    const double aSquared = a.x * a.x + a.y * a.y;
    const double bSquared = b.x * b.x + b.y * b.y;
    const double cSquared = c.x * c.x + c.y * c.y;
    const double ux =
        (aSquared * (b.y - c.y) + bSquared * (c.y - a.y) + cSquared * (a.y - b.y)) / (2.0 * area);
    const double uy =
        (aSquared * (c.x - b.x) + bSquared * (a.x - c.x) + cSquared * (b.x - a.x)) / (2.0 * area);
    CircleThroughPoints out;
    out.centre = Vec2{ux, uy};
    out.radius = std::hypot(a.x - ux, a.y - uy);
    return out;
}

std::optional<BulgeArc> ArcFromBulge(Vec2 from, Vec2 to, double bulge) noexcept {
    // FROM THE SAGITTA, not from the included angle.
    //
    // The first version worked backwards from bulge = tan(includedAngle / 4),
    // built a centre by offsetting the chord's midpoint, and chose the side
    // with a sign it reasoned about in prose. It put a bulge of +1 on the
    // WRONG SIDE -- a half circle that should rise 5 above a chord of 10 dipped
    // 5 below it -- and the reasoning read perfectly well.
    //
    // The definition everybody actually computes with leaves no room for that:
    //
    //     bulge = 2 * sagitta / chord, signed
    //
    // so the arc's midpoint is the chord's midpoint pushed out by
    // bulge * chord/2 along the chord's LEFT normal. Three points then name
    // exactly one circle, and CircleFrom3Points already knows how to find it.
    // There is no side left to choose and no sign left to get wrong.
    if (std::fabs(bulge) < kEpsilon) return std::nullopt; // straight: not an arc
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double chord = std::hypot(dx, dy);
    if (!(chord > kEpsilon)) return std::nullopt;

    // The left normal of the direction of travel: +X rotated a quarter turn
    // counter-clockwise is +Y, so a positive bulge rises above a chord drawn
    // left to right.
    const double nx = -dy / chord;
    const double ny = dx / chord;
    const double sagitta = bulge * chord / 2.0;
    const Vec2 middle{(from.x + to.x) / 2.0 + nx * sagitta,
                      (from.y + to.y) / 2.0 + ny * sagitta};

    const std::optional<CircleThroughPoints> circle = CircleFrom3Points(from, middle, to);
    if (!circle.has_value()) return std::nullopt;

    BulgeArc out;
    out.centre = circle->centre;
    out.radius = circle->radius;
    // WHICH WAY IT IS TRAVELLED, read off the three points rather than off the
    // bulge's sign -- they are the same fact, and taking it from the geometry
    // means it cannot disagree with the geometry.
    const double cross = (middle.x - from.x) * (to.y - middle.y) -
                         (middle.y - from.y) * (to.x - middle.x);
    out.counterClockwise = cross > 0.0;

    const double fromAngle = std::atan2(from.y - out.centre.y, from.x - out.centre.x);
    const double toAngle = std::atan2(to.y - out.centre.y, to.x - out.centre.x);
    // ALWAYS STORED COUNTER-CLOCKWISE, so everything downstream has one
    // convention; the traversal direction lives in the flag.
    out.startAngle = NormaliseAngle(out.counterClockwise ? fromAngle : toAngle);
    out.endAngle = NormaliseAngle(out.counterClockwise ? toAngle : fromAngle);
    return out;
}

std::vector<Vec2> TessellateArc(Vec2 centre, double radius, double startAngle, double endAngle,
                                double chordToleranceMm) {
    std::vector<Vec2> points;
    if (!(radius > kEpsilon)) return points;
    double sweep = NormaliseAngle(endAngle - startAngle);
    if (sweep < 1.0e-12) sweep = kTwoPi; // start == end means a full circle

    // HOW MANY SEGMENTS THE TOLERANCE ASKS FOR, not a fixed count. A 2 mm
    // fillet and a 2 m arc need very different numbers, and a fixed count
    // makes one of them a polygon and the other a waste.
    const double tolerance = std::max(1.0e-6, chordToleranceMm);
    double perSegment = kPi;
    if (tolerance < radius) perSegment = 2.0 * std::acos(1.0 - tolerance / radius);
    const int segments =
        std::max(2, static_cast<int>(std::ceil(sweep / std::max(1.0e-6, perSegment))));
    points.reserve(static_cast<std::size_t>(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
        const double angle = startAngle + sweep * (static_cast<double>(i) / segments);
        points.push_back(Vec2{centre.x + radius * std::cos(angle),
                              centre.y + radius * std::sin(angle)});
    }
    return points;
}

} // namespace paramcad
