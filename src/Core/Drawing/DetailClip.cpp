#include "Core/Drawing/DetailClip.h"

#include <algorithm>
#include <cmath>

namespace paramcad {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kTiny = 1e-9;

double LengthSquared(Vec2 v) noexcept { return v.x * v.x + v.y * v.y; }

// THE INSIDE TEST, USED EVERYWHERE. Written once so a curve cannot be kept by
// one rule and dropped by another -- which on a boundary case is a detail
// missing exactly the edge the drafter zoomed in to look at.
bool Inside(Vec2 point, Vec2 centre, double radius) noexcept {
    return LengthSquared(Vec2{point.x - centre.x, point.y - centre.y}) <=
           radius * radius + kTiny;
}

// The part of segment a..b inside the circle, as a parameter range, or false
// when none of it is.
bool SegmentInside(Vec2 a, Vec2 b, Vec2 centre, double radius, double& lo, double& hi) {
    const Vec2 d{b.x - a.x, b.y - a.y};
    const Vec2 f{a.x - centre.x, a.y - centre.y};
    const double aa = LengthSquared(d);
    if (aa < kTiny) {
        // A SEGMENT OF NO LENGTH IS A POINT, and it is in or out. Solving the
        // quadratic for it divides by zero, which is how a degenerate edge
        // becomes a NaN and then vanishes without anyone noticing.
        if (!Inside(a, centre, radius)) return false;
        lo = 0.0;
        hi = 1.0;
        return true;
    }
    const double bb = 2.0 * (f.x * d.x + f.y * d.y);
    const double cc = LengthSquared(f) - radius * radius;
    const double disc = bb * bb - 4.0 * aa * cc;
    if (disc <= 0.0) {
        // No crossing: wholly in or wholly out, and the midpoint says which.
        const Vec2 middle{a.x + 0.5 * d.x, a.y + 0.5 * d.y};
        if (!Inside(middle, centre, radius)) return false;
        lo = 0.0;
        hi = 1.0;
        return true;
    }
    const double root = std::sqrt(disc);
    lo = std::max(0.0, (-bb - root) / (2.0 * aa));
    hi = std::min(1.0, (-bb + root) / (2.0 * aa));
    return hi - lo > kTiny;
}

Vec2 At(Vec2 a, Vec2 b, double t) noexcept {
    return Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

// An angular interval, in absolute radians, with lo <= hi.
struct Sweep {
    double lo = 0.0;
    double hi = 0.0;
};

// The arc's own range, as one interval. A full circle is the whole turn --
// which is how OCCT reports one (start == end) and what a naive
// end-minus-start would read as a sweep of nothing.
Sweep SweepOf(const ProjectedArc& arc) {
    if (arc.isFullCircle) return Sweep{arc.startAngle, arc.startAngle + kTwoPi};
    double turn = arc.endAngle - arc.startAngle;
    while (turn <= kTiny) turn += kTwoPi;
    while (turn > kTwoPi) turn -= kTwoPi;
    return Sweep{arc.startAngle, arc.startAngle + turn};
}

ProjectedArc SubArc(const ProjectedArc& arc, double from, double to) {
    ProjectedArc out = arc;
    out.startAngle = from;
    out.endAngle = to;
    // A PIECE OF A CIRCLE IS NOT A CIRCLE. Left true, a trimmed rim would draw
    // and export as the whole ring -- geometry the detail deliberately cropped
    // away, back on the paper.
    out.isFullCircle = to - from >= kTwoPi - kTiny;
    return out;
}

void ClipArc(const ProjectedCurve& curve, const ProjectedArc& arc, Vec2 centre, double radius,
             std::vector<ProjectedCurve>& into) {
    if (arc.radius <= kTiny) return;
    const Vec2 offset{arc.centre.x - centre.x, arc.centre.y - centre.y};
    const double distance = std::sqrt(LengthSquared(offset));
    const Sweep own = SweepOf(arc);

    const auto keepWhole = [&]() {
        // UNCHANGED AND STILL ITSELF -- the rule this file exists for.
        into.push_back(curve);
    };

    if (distance < kTiny) {
        // Concentric: the whole rim is in or out together.
        if (arc.radius <= radius + kTiny) keepWhole();
        return;
    }

    // A point at angle t is inside when cos(t - phi) <= k. Everything about
    // which part of the rim survives is in that one inequality.
    const double k = (radius * radius - distance * distance - arc.radius * arc.radius) /
                     (2.0 * arc.radius * distance);
    if (k >= 1.0) {
        keepWhole();
        return;
    }
    if (k <= -1.0) return;   // no part of the rim reaches inside

    const double phi = std::atan2(offset.y, offset.x);
    const double alpha = std::acos(k);
    // Inside is t - phi in [alpha, 2pi - alpha], repeated every turn. Three
    // copies cover any arc range, because an arc spans at most one turn.
    for (int turn = -1; turn <= 1; ++turn) {
        const double base = phi + static_cast<double>(turn) * kTwoPi;
        const double lo = std::max(own.lo, base + alpha);
        const double hi = std::min(own.hi, base + kTwoPi - alpha);
        if (hi - lo <= kTiny) continue;
        ProjectedCurve piece = curve;
        piece.shape = SubArc(arc, lo, hi);
        into.push_back(std::move(piece));
    }
}

void ClipPoints(const ProjectedCurve& curve, const std::vector<Vec2>& points, Vec2 centre,
                double radius, std::vector<ProjectedCurve>& into) {
    if (points.size() < 2) {
        if (points.size() == 1 && Inside(points.front(), centre, radius)) into.push_back(curve);
        return;
    }
    // KEPT AS ONE RUN WHERE IT IS CONTINUOUS. Emitting a curve per segment
    // would be the same picture made of a hundred pieces, and every later
    // question -- how many curves does this view have, which one did the user
    // pick -- would answer differently for a detail than for the view it came
    // from.
    std::vector<Vec2> run;
    const auto flush = [&]() {
        if (run.size() >= 2) {
            ProjectedCurve piece = curve;
            piece.shape = ProjectedPolyline{run};
            into.push_back(std::move(piece));
        }
        run.clear();
    };
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        double lo = 0.0;
        double hi = 0.0;
        if (!SegmentInside(points[i], points[i + 1], centre, radius, lo, hi)) {
            flush();
            continue;
        }
        const Vec2 from = At(points[i], points[i + 1], lo);
        const Vec2 to = At(points[i], points[i + 1], hi);
        if (run.empty() || LengthSquared(Vec2{run.back().x - from.x, run.back().y - from.y}) >
                               kTiny) {
            flush();
            run.push_back(from);
        }
        run.push_back(to);
        // A piece that stopped short of the segment's end left the circle
        // there, so whatever comes next starts a new run.
        if (hi < 1.0 - kTiny) flush();
    }
    flush();
}

} // namespace

bool InsideCircle(Vec2 point, Vec2 centreMm, double radiusMm) noexcept {
    return Inside(point, centreMm, radiusMm);
}

std::vector<ProjectedCurve> ClipToCircle(const std::vector<ProjectedCurve>& curves,
                                         Vec2 centreMm, double radiusMm) {
    std::vector<ProjectedCurve> out;
    if (radiusMm <= kTiny) return out;
    out.reserve(curves.size());
    for (const ProjectedCurve& curve : curves) {
        if (const auto* line = std::get_if<ProjectedLine>(&curve.shape)) {
            double lo = 0.0;
            double hi = 0.0;
            if (!SegmentInside(line->a, line->b, centreMm, radiusMm, lo, hi)) continue;
            if (lo <= kTiny && hi >= 1.0 - kTiny) {
                // WHOLE, AND HANDED BACK RATHER THAN REBUILT. The mutation
                // gate confirms this branch is not load-bearing -- rebuilding
                // the line from t = 0 and t = 1 gives the same line, to well
                // inside anything a drawing can tell apart. It is here so an
                // untouched curve costs nothing, not to keep an answer right.
                out.push_back(curve);
                continue;
            }
            ProjectedCurve piece = curve;
            piece.shape = ProjectedLine{At(line->a, line->b, lo), At(line->a, line->b, hi)};
            out.push_back(std::move(piece));
        } else if (const auto* arc = std::get_if<ProjectedArc>(&curve.shape)) {
            ClipArc(curve, *arc, centreMm, radiusMm, out);
        } else if (const auto* polyline = std::get_if<ProjectedPolyline>(&curve.shape)) {
            ClipPoints(curve, polyline->points, centreMm, radiusMm, out);
        }
    }
    return out;
}

} // namespace paramcad
