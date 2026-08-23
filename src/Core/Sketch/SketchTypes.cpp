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
            } else if constexpr (std::is_same_v<T, SketchSpline>) {
                if (value.points.size() < kMinSplinePoints) return false;
                for (const Vec2& point : value.points)
                    if (!IsFinite(point)) return false;
                // NO TWO NEIGHBOURS ON TOP OF EACH OTHER. A zero-length span
                // has no direction, so the curve through it has no tangent
                // there -- and OCCT's interpolator refuses the whole curve
                // rather than the one span, with a message naming nothing.
                for (std::size_t i = 1; i < value.points.size(); ++i)
                    if (SamePoint(value.points[i - 1], value.points[i], kSketchToleranceMm))
                        return false;
                if (value.closed &&
                    SamePoint(value.points.front(), value.points.back(), kSketchToleranceMm))
                    return false;
                // EVERY HANDLE NAMES A POINT THAT EXISTS, and points somewhere.
                //
                // This is the ONE place the handle map's invariant is enforced,
                // and it is enforced rather than tested: every mutation path in
                // this program commits through IsValidSketchGeometry, so a
                // handle keyed past the end of the point list cannot be stored
                // -- which is what makes a sparse map safe where a parallel
                // array would not be.
                //
                // A ZERO-LENGTH tangent says nothing about direction, so it is
                // not a handle; it is the absence of one, spelled wrongly.
                for (const auto& [index, tangent] : value.handles) {
                    if (index < 0 || index >= static_cast<int>(value.points.size())) return false;
                    if (!IsFinite(tangent)) return false;
                    if (std::hypot(tangent.x, tangent.y) < kSketchToleranceMm) return false;
                }
                return true;
            } else if constexpr (std::is_same_v<T, SketchEllipse>) {
                if (!IsFinite(value.center)) return false;
                if (!std::isfinite(value.majorRadiusMm) || !std::isfinite(value.minorRadiusMm))
                    return false;
                if (!std::isfinite(value.rotationRad)) return false;
                if (value.minorRadiusMm < kSketchToleranceMm) return false;
                // MAJOR IS THE LONGER ONE. The rotation is measured to it, so an
                // ellipse stored the other way round is the same shape turned a
                // quarter turn -- a difference no later check would notice.
                return value.majorRadiusMm >= value.minorRadiusMm;
            } else if constexpr (std::is_same_v<T, SketchEllipticalArc>) {
                if (!IsFinite(value.center)) return false;
                if (!std::isfinite(value.majorRadiusMm) || !std::isfinite(value.minorRadiusMm))
                    return false;
                if (!std::isfinite(value.rotationRad)) return false;
                if (value.minorRadiusMm < kSketchToleranceMm) return false;
                if (value.majorRadiusMm < value.minorRadiusMm) return false;
                if (!std::isfinite(value.startParamRad) || !std::isfinite(value.endParamRad))
                    return false;
                double sweep = value.endParamRad - value.startParamRad;
                if (!value.counterClockwise) sweep = -sweep;
                sweep = std::fmod(sweep, kTwoPi);
                if (sweep < 0.0) sweep += kTwoPi;
                return sweep >= kSketchToleranceMm && sweep <= kTwoPi - kSketchToleranceMm;
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
    if (const auto* spline = std::get_if<SketchSpline>(&geometry))
        // A CLOSED spline is a loop on its own, like a circle: it has no ends
        // for a profile to chain through.
        return !spline->closed;
    return std::holds_alternative<SketchLine>(geometry) ||
           std::holds_alternative<SketchArc>(geometry) ||
           std::holds_alternative<SketchEllipticalArc>(geometry);
}

namespace {

// One HERMITE span, from `from` to `to`, leaving along `leaving` and arriving
// along `arriving`. `u` runs 0..1 across the span.
//
// This replaced a Catmull-Rom span (M18), and the two are THE SAME CURVE when
// the tangents are the Catmull-Rom ones -- Catmull-Rom is precisely Hermite
// with m_i = (p[i+1] - p[i-1])/2. Multiplying out the four basis polynomials
// against that substitution gives the halved Catmull-Rom basis coefficient for
// coefficient; M18_HAN_001 checks it numerically rather than on the strength of
// that paragraph.
//
// The generalisation is what lets a single point carry its own tangent while
// every other point keeps the default, which is what a handle is.
Vec2 Hermite(Vec2 from, Vec2 leaving, Vec2 to, Vec2 arriving, double u) noexcept {
    const double u2 = u * u;
    const double u3 = u2 * u;
    const double h00 = 2.0 * u3 - 3.0 * u2 + 1.0;
    const double h10 = u3 - 2.0 * u2 + u;
    const double h01 = -2.0 * u3 + 3.0 * u2;
    const double h11 = u3 - u2;
    return Vec2{h00 * from.x + h10 * leaving.x + h01 * to.x + h11 * arriving.x,
                h00 * from.y + h10 * leaving.y + h01 * to.y + h11 * arriving.y};
}

// The point `index` positions along, wrapping for a closed spline and
// REFLECTING at the ends of an open one.
//
// Reflecting rather than clamping: clamping makes the first and last spans
// straighten out, which reads as a spline that goes limp at its ends. The
// reflection p1 + (p1 - p2) gives the end span the same curvature as its
// neighbour, which is what a drawn spline looks like.
Vec2 SplinePointAt(const SketchSpline& spline, int index) noexcept {
    const int count = static_cast<int>(spline.points.size());
    if (count == 0) return Vec2{};
    if (spline.closed) {
        int wrapped = index % count;
        if (wrapped < 0) wrapped += count;
        return spline.points[static_cast<std::size_t>(wrapped)];
    }
    if (index < 0) {
        const Vec2 first = spline.points.front();
        const Vec2 second = spline.points[static_cast<std::size_t>(count > 1 ? 1 : 0)];
        return Vec2{2.0 * first.x - second.x, 2.0 * first.y - second.y};
    }
    if (index >= count) {
        const Vec2 last = spline.points.back();
        const Vec2 before = spline.points[static_cast<std::size_t>(count > 1 ? count - 2 : 0)];
        return Vec2{2.0 * last.x - before.x, 2.0 * last.y - before.y};
    }
    return spline.points[static_cast<std::size_t>(index)];
}

} // namespace

Vec2 SplineTangentAt(const SketchSpline& spline, int index) noexcept {
    const int count = static_cast<int>(spline.points.size());
    if (count == 0) return Vec2{};
    // THE HANDLE WINS where there is one. Only a real index has a handle: the
    // virtual neighbours either side of an open spline are reflections, not
    // points the user can put a handle on.
    if (index >= 0 && index < count) {
        const auto found = spline.handles.find(index);
        if (found != spline.handles.end()) return found->second;
    }
    // ...and otherwise the Catmull-Rom default. Through SplinePointAt, so the
    // reflection at the ends is stated once: it is what makes the tangent at
    // the first point come out as exactly p1 - p0 (ADR-M18-001).
    const Vec2 before = SplinePointAt(spline, index - 1);
    const Vec2 after = SplinePointAt(spline, index + 1);
    return Vec2{0.5 * (after.x - before.x), 0.5 * (after.y - before.y)};
}

namespace {

// One span of the curve, span `span` at parameter `u`, whichever tangents its
// two ends turn out to have.
Vec2 SplineSpanAt(const SketchSpline& spline, int span, double u) noexcept {
    return Hermite(SplinePointAt(spline, span), SplineTangentAt(spline, span),
                   SplinePointAt(spline, span + 1), SplineTangentAt(spline, span + 1), u);
}

} // namespace

std::vector<Vec2> SampleSpline(const SketchSpline& spline, int samplesPerSpan) {
    std::vector<Vec2> out;
    const int count = static_cast<int>(spline.points.size());
    if (count == 0) return out;
    if (count == 1) {
        out.push_back(spline.points.front());
        return out;
    }
    const int steps = samplesPerSpan < 1 ? 1 : samplesPerSpan;
    const int spans = spline.closed ? count : count - 1;
    out.reserve(static_cast<std::size_t>(spans) * steps + 1);
    for (int span = 0; span < spans; ++span) {
        for (int step = 0; step < steps; ++step)
            out.push_back(SplineSpanAt(spline, span, static_cast<double>(step) / steps));
    }
    // THE LAST POINT, exactly. An open spline has to END where it was told to,
    // and a sampler that stopped one step short would leave a gap a profile
    // could not close.
    if (!spline.closed) out.push_back(spline.points.back());
    return out;
}

Vec2 PointOnSpline(const SketchSpline& spline, double t) {
    const int count = static_cast<int>(spline.points.size());
    if (count == 0) return Vec2{};
    if (count == 1) return spline.points.front();
    const int spans = spline.closed ? count : count - 1;
    double where = t * spans;
    if (where < 0.0) where = 0.0;
    if (where > spans) where = spans;
    int span = static_cast<int>(where);
    if (span >= spans) span = spans - 1;
    return SplineSpanAt(spline, span, where - span);
}

Vec2 PointOnEllipse(Vec2 centre, double majorRadiusMm, double minorRadiusMm, double rotationRad,
                    double paramRad) noexcept {
    const double c = std::cos(rotationRad);
    const double s = std::sin(rotationRad);
    const double au = majorRadiusMm * std::cos(paramRad);
    const double bv = minorRadiusMm * std::sin(paramRad);
    return Vec2{centre.x + au * c - bv * s, centre.y + au * s + bv * c};
}

double EllipseParamOf(Vec2 centre, double majorRadiusMm, double minorRadiusMm, double rotationRad,
                      Vec2 p) noexcept {
    const double c = std::cos(rotationRad);
    const double s = std::sin(rotationRad);
    const double du = p.x - centre.x;
    const double dv = p.y - centre.y;
    // Into the ellipse's own frame, then UNSTRETCH each axis: dividing by the
    // radii turns the ellipse into the unit circle, and on a circle the
    // parameter IS the angle. Taking atan2 of the raw offset instead -- the
    // obvious-looking move -- gives an answer that is right at the four axis
    // points and wrong everywhere between them.
    const double x = (du * c + dv * s) / (majorRadiusMm > 0.0 ? majorRadiusMm : 1.0);
    const double y = (-du * s + dv * c) / (minorRadiusMm > 0.0 ? minorRadiusMm : 1.0);
    return std::atan2(y, x);
}

namespace {

Vec2 PointOnArc(const SketchArc& arc, double angleRad) noexcept {
    return Vec2{arc.center.x + arc.radiusMm * std::cos(angleRad),
                arc.center.y + arc.radiusMm * std::sin(angleRad)};
}

} // namespace

Vec2 StartPointOf(const SketchGeometry& geometry) noexcept {
    if (const auto* spline = std::get_if<SketchSpline>(&geometry))
        return spline->points.empty() ? Vec2{} : spline->points.front();
    if (const auto* line = std::get_if<SketchLine>(&geometry)) return line->start;
    if (const auto* arc = std::get_if<SketchArc>(&geometry))
        return PointOnArc(*arc, arc->startAngleRad);
    if (const auto* ellipse = std::get_if<SketchEllipticalArc>(&geometry))
        return PointOnEllipse(ellipse->center, ellipse->majorRadiusMm, ellipse->minorRadiusMm,
                              ellipse->rotationRad, ellipse->startParamRad);
    return Vec2{};
}

Vec2 EndPointOf(const SketchGeometry& geometry) noexcept {
    if (const auto* spline = std::get_if<SketchSpline>(&geometry))
        return spline->points.empty() ? Vec2{} : spline->points.back();
    if (const auto* line = std::get_if<SketchLine>(&geometry)) return line->end;
    if (const auto* arc = std::get_if<SketchArc>(&geometry))
        return PointOnArc(*arc, arc->endAngleRad);
    if (const auto* ellipse = std::get_if<SketchEllipticalArc>(&geometry))
        return PointOnEllipse(ellipse->center, ellipse->majorRadiusMm, ellipse->minorRadiusMm,
                              ellipse->rotationRad, ellipse->endParamRad);
    return Vec2{};
}

namespace {

// Signed sweep from start to end in the arc's own direction, always in
// [0, 2*pi). Normalizing here is what makes the midpoint independent of how
// the caller happened to phrase the angles (e.g. -pi/2 vs 3*pi/2).
double EllipseSweep(const SketchEllipticalArc& arc) noexcept {
    double delta = arc.counterClockwise ? (arc.endParamRad - arc.startParamRad)
                                        : (arc.startParamRad - arc.endParamRad);
    delta = std::fmod(delta, kTwoPi);
    if (delta < 0.0) delta += kTwoPi;
    return delta;
}

double ArcSweep(const SketchArc& arc) noexcept {
    double delta = arc.counterClockwise ? (arc.endAngleRad - arc.startAngleRad)
                                        : (arc.startAngleRad - arc.endAngleRad);
    delta = std::fmod(delta, kTwoPi);
    if (delta < 0.0) delta += kTwoPi;
    return delta;
}

} // namespace

Vec2 MidPointOf(const SketchGeometry& geometry) noexcept {
    if (const auto* spline = std::get_if<SketchSpline>(&geometry))
        return PointOnSpline(*spline, 0.5);
    if (const auto* line = std::get_if<SketchLine>(&geometry))
        return Vec2{(line->start.x + line->end.x) * 0.5, (line->start.y + line->end.y) * 0.5};
    if (const auto* arc = std::get_if<SketchArc>(&geometry)) {
        const double half = ArcSweep(*arc) * 0.5;
        const double midAngle =
            arc->counterClockwise ? arc->startAngleRad + half : arc->startAngleRad - half;
        return PointOnArc(*arc, midAngle);
    }
    if (const auto* ellipse = std::get_if<SketchEllipticalArc>(&geometry)) {
        const double half = EllipseSweep(*ellipse) * 0.5;
        const double mid = ellipse->counterClockwise ? ellipse->startParamRad + half
                                                     : ellipse->startParamRad - half;
        return PointOnEllipse(ellipse->center, ellipse->majorRadiusMm, ellipse->minorRadiusMm,
                              ellipse->rotationRad, mid);
    }
    if (const auto* circle = std::get_if<SketchCircle>(&geometry))
        return Vec2{circle->center.x + circle->radiusMm, circle->center.y};
    if (const auto* full = std::get_if<SketchEllipse>(&geometry))
        return PointOnEllipse(full->center, full->majorRadiusMm, full->minorRadiusMm,
                              full->rotationRad, 0.0);
    if (const auto* point = std::get_if<SketchPoint>(&geometry)) return point->position;
    return Vec2{};
}

bool IsResolvableRef(const SketchGeometry& geometry, SketchSubElement part,
                     int index) noexcept {
    if (part == SketchSubElement::SplineHandle) {
        // ONLY A POINT THAT HAS ONE. A handle that is not there has no tip, and
        // answering with the point itself would be a constraint on a place the
        // caller did not name -- and one that looks satisfied from the start.
        const auto* spline = std::get_if<SketchSpline>(&geometry);
        if (spline == nullptr) return false;
        return spline->handles.find(index) != spline->handles.end();
    }
    if (part == SketchSubElement::SplinePoint) {
        const auto* spline = std::get_if<SketchSpline>(&geometry);
        if (spline == nullptr) return false;
        // INTERIOR ONLY. A closed spline has no ends to exclude, so every one
        // of its points is nameable this way; an open one keeps StartPoint and
        // EndPoint as the names for its first and last.
        const int count = static_cast<int>(spline->points.size());
        if (index < 0 || index >= count) return false;
        if (spline->closed) return true;
        return index > 0 && index < count - 1;
    }
    return PointOfSubElement(geometry, part, index).has_value() ||
           part == SketchSubElement::Whole;
}

std::optional<Vec2> PointOfSubElement(const SketchGeometry& geometry, SketchSubElement part,
                                      int index) noexcept {
    if (part == SketchSubElement::SplineHandle) {
        const auto* spline = std::get_if<SketchSpline>(&geometry);
        if (spline == nullptr) return std::nullopt;
        if (index < 0 || index >= static_cast<int>(spline->points.size())) return std::nullopt;
        const auto found = spline->handles.find(index);
        if (found == spline->handles.end()) return std::nullopt;
        // THE TIP: where the handle's end sits, which is the point plus the
        // tangent. One place, so the canvas draws the grab target exactly where
        // the solver's tip variable is.
        const Vec2 base = spline->points[static_cast<std::size_t>(index)];
        return Vec2{base.x + found->second.x, base.y + found->second.y};
    }
    if (part == SketchSubElement::SplinePoint) {
        const auto* spline = std::get_if<SketchSpline>(&geometry);
        if (spline == nullptr) return std::nullopt;
        if (index < 0 || index >= static_cast<int>(spline->points.size())) return std::nullopt;
        return spline->points[static_cast<std::size_t>(index)];
    }
    if (part == SketchSubElement::StartPoint) {
        if (!HasEndpoints(geometry)) return std::nullopt;
        return StartPointOf(geometry);
    }
    if (part == SketchSubElement::EndPoint) {
        if (!HasEndpoints(geometry)) return std::nullopt;
        return EndPointOf(geometry);
    }
    if (part == SketchSubElement::CenterPoint) {
        if (const auto* circle = std::get_if<SketchCircle>(&geometry)) return circle->center;
        if (const auto* arc = std::get_if<SketchArc>(&geometry)) return arc->center;
        if (const auto* full = std::get_if<SketchEllipse>(&geometry)) return full->center;
        if (const auto* piece = std::get_if<SketchEllipticalArc>(&geometry))
            return piece->center;
        return std::nullopt;
    }
    if (part == SketchSubElement::Whole) {
        if (const auto* point = std::get_if<SketchPoint>(&geometry)) return point->position;
        return std::nullopt;
    }
    return std::nullopt;
}

bool SameSketchGeometryValue(const SketchGeometry& a, const SketchGeometry& b) noexcept {
    if (a.index() != b.index()) return false;
    // The indices agree, so both hold the same alternative and every std::get
    // below is safe by construction -- which is the property the two get_if
    // chains this replaced could not state and did not have.
    return std::visit(
        [&b](const auto& first) -> bool {
            using T = std::decay_t<decltype(first)>;
            const auto& second = std::get<T>(b);
            const auto same = [](Vec2 p, Vec2 q) { return p.x == q.x && p.y == q.y; };
            if constexpr (std::is_same_v<T, SketchPoint>) {
                return same(first.position, second.position);
            } else if constexpr (std::is_same_v<T, SketchLine>) {
                return same(first.start, second.start) && same(first.end, second.end);
            } else if constexpr (std::is_same_v<T, SketchCircle>) {
                return same(first.center, second.center) && first.radiusMm == second.radiusMm;
            } else if constexpr (std::is_same_v<T, SketchArc>) {
                return same(first.center, second.center) && first.radiusMm == second.radiusMm &&
                       first.startAngleRad == second.startAngleRad &&
                       first.endAngleRad == second.endAngleRad &&
                       first.counterClockwise == second.counterClockwise;
            } else if constexpr (std::is_same_v<T, SketchEllipse>) {
                return same(first.center, second.center) &&
                       first.majorRadiusMm == second.majorRadiusMm &&
                       first.minorRadiusMm == second.minorRadiusMm &&
                       first.rotationRad == second.rotationRad;
            } else if constexpr (std::is_same_v<T, SketchEllipticalArc>) {
                return same(first.center, second.center) &&
                       first.majorRadiusMm == second.majorRadiusMm &&
                       first.minorRadiusMm == second.minorRadiusMm &&
                       first.rotationRad == second.rotationRad &&
                       first.startParamRad == second.startParamRad &&
                       first.endParamRad == second.endParamRad &&
                       first.counterClockwise == second.counterClockwise;
            } else {
                static_assert(std::is_same_v<T, SketchSpline>);
                if (first.closed != second.closed) return false;
                if (first.points.size() != second.points.size()) return false;
                for (std::size_t i = 0; i < first.points.size(); ++i)
                    if (!same(first.points[i], second.points[i])) return false;
                // THE HANDLES TOO (M18). This is the ONE comparison the whole
                // program uses to decide whether a solve changed anything, so
                // leaving them out would make a solve that moved only a
                // tangent look like a solve that changed nothing -- and the
                // new shape would never be committed.
                if (first.handles.size() != second.handles.size()) return false;
                for (const auto& [index, tangent] : first.handles) {
                    const auto found = second.handles.find(index);
                    if (found == second.handles.end()) return false;
                    if (!same(tangent, found->second)) return false;
                }
                return true;
            }
        },
        a);
}

bool IsSameCurve(const SketchGeometry& a, const SketchGeometry& b, double toleranceMm) noexcept {
    if (a.index() != b.index()) return false; // different curve kinds are never duplicates

    if (const auto* splineA = std::get_if<SketchSpline>(&a)) {
        const auto& splineB = std::get<SketchSpline>(b);
        // TWO CURVES WITH DIFFERENT HANDLES ARE DIFFERENT CURVES, even through
        // the same points -- that is the whole point of a handle. Compared
        // before the positions because it is the cheap half.
        if (splineA->handles.size() != splineB.handles.size()) return false;
        // POINT FOR POINT, and in EITHER direction -- a spline drawn backwards
        // is the same curve, exactly as a line drawn backwards is. Comparing
        // only forwards would let a reversed duplicate through, and two curves
        // lying on top of each other make a profile ambiguous.
        if (splineA->closed != splineB.closed) return false;
        if (splineA->points.size() != splineB.points.size()) return false;
        bool forward = true;
        bool backward = true;
        const std::size_t n = splineA->points.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (!SamePoint(splineA->points[i], splineB.points[i], toleranceMm)) forward = false;
            if (!SamePoint(splineA->points[i], splineB.points[n - 1 - i], toleranceMm))
                backward = false;
        }
        return forward || backward;
    }

    if (const auto* ellipseA = std::get_if<SketchEllipse>(&a)) {
        const auto& ellipseB = std::get<SketchEllipse>(b);
        // NOT its endpoints -- it has none. Two ellipses are the same when
        // their centres, both radii and their orientation agree. The rotation
        // is compared MODULO A HALF TURN, because an ellipse turned 180 degrees
        // is the same ellipse and refusing the duplicate is the whole point.
        if (!SamePoint(ellipseA->center, ellipseB.center, toleranceMm)) return false;
        if (std::fabs(ellipseA->majorRadiusMm - ellipseB.majorRadiusMm) > toleranceMm)
            return false;
        if (std::fabs(ellipseA->minorRadiusMm - ellipseB.minorRadiusMm) > toleranceMm)
            return false;
        double turn = std::fmod(ellipseA->rotationRad - ellipseB.rotationRad, kTwoPi / 2.0);
        if (turn < 0.0) turn += kTwoPi / 2.0;
        return turn <= toleranceMm || turn >= kTwoPi / 2.0 - toleranceMm;
    }

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
