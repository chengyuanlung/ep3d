#include "Core/Sketch/SketchTypes.h"

namespace paramcad {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

bool IsFinite(Vec2 point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

} // namespace

bool IsValidSketchGeometry(const SketchGeometry& geometry) noexcept {
    return std::visit(
        [](const auto& value) -> bool {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, SketchPoint>) {
                return IsFinite(value.position);
            } else if constexpr (std::is_same_v<T, SketchLine>) {
                if (!IsFinite(value.start) || !IsFinite(value.end)) return false;
                // A zero-length line is degenerate: it has no direction, so it
                // can neither be oriented nor contribute an edge to a profile.
                return !SamePoint(value.start, value.end, kSketchToleranceMm);
            } else if constexpr (std::is_same_v<T, SketchCircle>) {
                if (!IsFinite(value.center)) return false;
                return std::isfinite(value.radiusMm) && value.radiusMm >= kSketchToleranceMm;
            } else {
                static_assert(std::is_same_v<T, SketchArc>);
                if (!IsFinite(value.center)) return false;
                if (!std::isfinite(value.radiusMm) || value.radiusMm < kSketchToleranceMm)
                    return false;
                if (!std::isfinite(value.startAngleRad) || !std::isfinite(value.endAngleRad))
                    return false;
                // An arc must span more than nothing and less than a full
                // turn: zero sweep is not an arc, and a full turn is a Circle.
                // Comparing raw angle difference (as an earlier revision did)
                // let a 2*pi sweep through, because 2*pi is comfortably larger
                // than the tolerance -- the check has to be on the NORMALIZED
                // sweep, which is what actually bounds the arc.
                double sweep = value.endAngleRad - value.startAngleRad;
                if (!value.counterClockwise) sweep = -sweep;
                sweep = std::fmod(sweep, kTwoPi);
                if (sweep < 0.0) sweep += kTwoPi;
                return sweep >= kSketchToleranceMm && sweep <= kTwoPi - kSketchToleranceMm;
            }
        },
        geometry);
}

bool HasEndpoints(const SketchGeometry& geometry) noexcept {
    return std::holds_alternative<SketchLine>(geometry) ||
           std::holds_alternative<SketchArc>(geometry);
}

namespace {

Vec2 PointOnArc(const SketchArc& arc, double angleRad) noexcept {
    return Vec2{arc.center.x + arc.radiusMm * std::cos(angleRad),
                arc.center.y + arc.radiusMm * std::sin(angleRad)};
}

} // namespace

Vec2 StartPointOf(const SketchGeometry& geometry) noexcept {
    if (const auto* line = std::get_if<SketchLine>(&geometry)) return line->start;
    if (const auto* arc = std::get_if<SketchArc>(&geometry))
        return PointOnArc(*arc, arc->startAngleRad);
    return Vec2{};
}

Vec2 EndPointOf(const SketchGeometry& geometry) noexcept {
    if (const auto* line = std::get_if<SketchLine>(&geometry)) return line->end;
    if (const auto* arc = std::get_if<SketchArc>(&geometry))
        return PointOnArc(*arc, arc->endAngleRad);
    return Vec2{};
}

namespace {

// Signed sweep from start to end in the arc's own direction, always in
// [0, 2*pi). Normalizing here is what makes the midpoint independent of how
// the caller happened to phrase the angles (e.g. -pi/2 vs 3*pi/2).
double ArcSweep(const SketchArc& arc) noexcept {
    double delta = arc.counterClockwise ? (arc.endAngleRad - arc.startAngleRad)
                                        : (arc.startAngleRad - arc.endAngleRad);
    delta = std::fmod(delta, kTwoPi);
    if (delta < 0.0) delta += kTwoPi;
    return delta;
}

} // namespace

Vec2 MidPointOf(const SketchGeometry& geometry) noexcept {
    if (const auto* line = std::get_if<SketchLine>(&geometry))
        return Vec2{(line->start.x + line->end.x) * 0.5, (line->start.y + line->end.y) * 0.5};
    if (const auto* arc = std::get_if<SketchArc>(&geometry)) {
        const double half = ArcSweep(*arc) * 0.5;
        const double midAngle =
            arc->counterClockwise ? arc->startAngleRad + half : arc->startAngleRad - half;
        return PointOnArc(*arc, midAngle);
    }
    if (const auto* circle = std::get_if<SketchCircle>(&geometry))
        return Vec2{circle->center.x + circle->radiusMm, circle->center.y};
    if (const auto* point = std::get_if<SketchPoint>(&geometry)) return point->position;
    return Vec2{};
}

bool IsSameCurve(const SketchGeometry& a, const SketchGeometry& b, double toleranceMm) noexcept {
    if (a.index() != b.index()) return false; // different curve kinds are never duplicates

    if (const auto* circleA = std::get_if<SketchCircle>(&a)) {
        const auto& circleB = std::get<SketchCircle>(b);
        return SamePoint(circleA->center, circleB.center, toleranceMm) &&
               std::fabs(circleA->radiusMm - circleB.radiusMm) <= toleranceMm;
    }
    if (const auto* pointA = std::get_if<SketchPoint>(&a))
        return SamePoint(pointA->position, std::get<SketchPoint>(b).position, toleranceMm);

    // Lines and arcs: same span in either direction, AND the same path between
    // the endpoints. Without the midpoint check, the two arcs on opposite sides
    // of a chord would compare equal.
    const Vec2 startA = StartPointOf(a), endA = EndPointOf(a);
    const Vec2 startB = StartPointOf(b), endB = EndPointOf(b);
    const bool sameSpan = (SamePoint(startA, startB, toleranceMm) &&
                           SamePoint(endA, endB, toleranceMm)) ||
                          (SamePoint(startA, endB, toleranceMm) &&
                           SamePoint(endA, startB, toleranceMm));
    if (!sameSpan) return false;
    return SamePoint(MidPointOf(a), MidPointOf(b), toleranceMm);
}

} // namespace paramcad
