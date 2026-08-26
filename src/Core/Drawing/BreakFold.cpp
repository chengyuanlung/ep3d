#include "Core/Drawing/BreakFold.h"

#include "Core/Text/NumberText.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <variant>

namespace paramcad {

namespace {

constexpr double kTiny = 1e-9;

} // namespace

std::string WhyBreakRefused(const BreakSpan& span, double extentFromMm, double extentToMm) {
    if (!span.active) return {};
    // A BREAK THAT REMOVES NOTHING IS NOT A BREAK. It draws the symbols, says
    // material was taken out, and takes none -- so the reader believes the
    // part is longer than it is drawn when it is exactly as drawn.
    if (span.toMm - span.fromMm <= kTiny)
        return "a break has to remove something -- its far end is not past its near one";
    if (span.gapMm < 0.0) return "a break cannot leave less than no gap between the halves";
    if (extentToMm - extentFromMm <= kTiny) return {};   // nothing projected yet to check

    // AND IT HAS TO BE OVER THE PART. A break past the end removes no material
    // and draws its symbols across empty paper -- which reads as a part that
    // continues off the sheet.
    if (span.toMm <= extentFromMm + kTiny || span.fromMm >= extentToMm - kTiny)
        return "this break is not over the part -- it runs from " + ShortNumber(span.fromMm) +
               " to " + ShortNumber(span.toMm) + " and the part reaches from " +
               ShortNumber(extentFromMm) + " to " + ShortNumber(extentToMm);
    // A BREAK THAT SWALLOWS THE WHOLE PART leaves two ends and no middle,
    // which is a view of nothing with a symbol at each side.
    if (span.fromMm <= extentFromMm + kTiny && span.toMm >= extentToMm - kTiny)
        return "this break covers the whole part, so there would be nothing left to draw";
    return {};
}

double RemovedMm(const BreakSpan& span) noexcept {
    if (!span.usable()) return 0.0;
    return span.toMm - span.fromMm;
}

double FoldAlongMm(double alongMm, const BreakSpan& span) noexcept {
    if (!span.usable()) return alongMm;
    if (alongMm <= span.fromMm) return alongMm;
    // THE SEAM. Everything the break took out lands on one line, and nothing
    // is drawn there -- the curves that crossed it were trimmed at the lips.
    if (alongMm < span.toMm) return span.fromMm;
    return alongMm - (span.toMm - span.fromMm) + span.gapMm;
}

double UnfoldAlongMm(double foldedMm, const BreakSpan& span) noexcept {
    if (!span.usable()) return foldedMm;
    if (foldedMm <= span.fromMm) return foldedMm;
    // INSIDE THE GAP is the seam read back, and it cannot be told from either
    // lip. Answering with the near one is the choice that keeps unfolding
    // monotonic; nothing anchors there, because nothing is drawn there.
    if (foldedMm < span.fromMm + span.gapMm) return span.fromMm;
    return foldedMm + (span.toMm - span.fromMm) - span.gapMm;
}

Vec2 FoldPointMm(Vec2 modelMm, const BreakSpan& span) noexcept {
    if (!span.usable()) return modelMm;
    if (span.horizontal) return Vec2{FoldAlongMm(modelMm.x, span), modelMm.y};
    return Vec2{modelMm.x, FoldAlongMm(modelMm.y, span)};
}

Vec2 UnfoldPointMm(Vec2 foldedMm, const BreakSpan& span) noexcept {
    if (!span.usable()) return foldedMm;
    if (span.horizontal) return Vec2{UnfoldAlongMm(foldedMm.x, span), foldedMm.y};
    return Vec2{foldedMm.x, UnfoldAlongMm(foldedMm.y, span)};
}


namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kHalfPi = 1.5707963267948966192313216916398;

double AlongOf(Vec2 point, const BreakSpan& span) noexcept {
    return span.horizontal ? point.x : point.y;
}

// Is this point on material the break did NOT take out?
bool Outside(Vec2 point, const BreakSpan& span) noexcept {
    const double along = AlongOf(point, span);
    return along <= span.fromMm + kTiny || along >= span.toMm - kTiny;
}

Vec2 Lerp(Vec2 a, Vec2 b, double t) noexcept {
    return Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

// Where a..b crosses `limit` along the break axis, or false when it does not.
bool CrossingAt(Vec2 a, Vec2 b, double limit, const BreakSpan& span, double& t) noexcept {
    const double from = AlongOf(a, span);
    const double to = AlongOf(b, span);
    const double run = to - from;
    if (std::fabs(run) < kTiny) return false;
    t = (limit - from) / run;
    return t > kTiny && t < 1.0 - kTiny;
}

void SplitSegment(Vec2 a, Vec2 b, const BreakSpan& span,
                  const std::function<void(Vec2, Vec2)>& emit) {
    // The two lips, in the order this segment meets them.
    double t1 = 0.0;
    double t2 = 0.0;
    const bool hitsNear = CrossingAt(a, b, span.fromMm, span, t1);
    const bool hitsFar = CrossingAt(a, b, span.toMm, span, t2);
    if (!hitsNear && !hitsFar) {
        // Wholly one side or wholly inside. The midpoint decides which, and a
        // segment buried in the removed span is dropped -- that is the
        // material the break took out.
        if (Outside(Lerp(a, b, 0.5), span)) emit(a, b);
        return;
    }
    double lo = std::min(hitsNear ? t1 : 1.0, hitsFar ? t2 : 1.0);
    double hi = std::max(hitsNear ? t1 : 0.0, hitsFar ? t2 : 0.0);
    if (!hitsNear) lo = 0.0;
    if (!hitsFar) hi = 1.0;
    if (lo > kTiny && Outside(Lerp(a, b, lo * 0.5), span)) emit(a, Lerp(a, b, lo));
    if (hi < 1.0 - kTiny && Outside(Lerp(a, b, (hi + 1.0) * 0.5), span))
        emit(Lerp(a, b, hi), b);
}

// The arc's own range as one interval, exactly as the detail crop reads it.
void ArcSweep(const ProjectedArc& arc, double& lo, double& hi) noexcept {
    if (arc.isFullCircle) {
        lo = arc.startAngle;
        hi = arc.startAngle + kTwoPi;
        return;
    }
    double turn = arc.endAngle - arc.startAngle;
    while (turn <= kTiny) turn += kTwoPi;
    while (turn > kTwoPi) turn -= kTwoPi;
    lo = arc.startAngle;
    hi = arc.startAngle + turn;
}

void EmitSubArc(const ProjectedCurve& curve, const ProjectedArc& arc, double from, double to,
                std::vector<ProjectedCurve>& out) {
    if (to - from <= kTiny) return;
    ProjectedArc piece = arc;
    piece.startAngle = from;
    piece.endAngle = to;
    piece.isFullCircle = to - from >= kTwoPi - kTiny;
    ProjectedCurve made = curve;
    made.shape = piece;
    out.push_back(std::move(made));
}

// Along the break axis an arc reads centre + radius * cos(theta - phase):
// phase is zero across and a quarter turn upright, which is what lets one
// piece of trigonometry answer for both axes instead of two that must agree.
void SplitArc(const ProjectedCurve& curve, const ProjectedArc& arc, const BreakSpan& span,
              std::vector<ProjectedCurve>& out) {
    if (arc.radius <= kTiny) return;
    const double phase = span.horizontal ? 0.0 : kHalfPi;
    const double centre = span.horizontal ? arc.centre.x : arc.centre.y;
    double lo = 0.0;
    double hi = 0.0;
    ArcSweep(arc, lo, hi);

    const double kNear = (span.fromMm - centre) / arc.radius;   // cos <= kNear
    const double kFar = (span.toMm - centre) / arc.radius;      // cos >= kFar

    const auto keepWhere = [&](double angleLo, double angleHi) {
        for (int turn = -1; turn <= 1; ++turn) {
            const double base = static_cast<double>(turn) * kTwoPi;
            EmitSubArc(curve, arc, std::max(lo, angleLo + base), std::min(hi, angleHi + base),
                       out);
        }
    };

    // Below the near lip.
    if (kNear >= 1.0) {
        EmitSubArc(curve, arc, lo, hi, out);
    } else if (kNear > -1.0) {
        const double alpha = std::acos(kNear);
        keepWhere(phase + alpha, phase + kTwoPi - alpha);
    }
    // Above the far lip.
    if (kFar <= -1.0) {
        // The whole rim is past the far lip -- but it was already emitted
        // whole above if it was also below the near one, and the two cannot
        // both be true for a usable span.
        if (kNear < 1.0) EmitSubArc(curve, arc, lo, hi, out);
    } else if (kFar < 1.0) {
        const double beta = std::acos(kFar);
        keepWhere(phase - beta, phase + beta);
    }
}

} // namespace

std::vector<ProjectedCurve> SplitAtBreak(const std::vector<ProjectedCurve>& curves,
                                         const BreakSpan& span) {
    if (!span.usable()) return curves;
    std::vector<ProjectedCurve> out;
    out.reserve(curves.size());
    for (const ProjectedCurve& curve : curves) {
        if (const auto* line = std::get_if<ProjectedLine>(&curve.shape)) {
            SplitSegment(line->a, line->b, span, [&](Vec2 a, Vec2 b) {
                ProjectedCurve piece = curve;
                piece.shape = ProjectedLine{a, b};
                out.push_back(std::move(piece));
            });
        } else if (const auto* arc = std::get_if<ProjectedArc>(&curve.shape)) {
            SplitArc(curve, *arc, span, out);
        } else if (const auto* polyline = std::get_if<ProjectedPolyline>(&curve.shape)) {
            // KEPT AS RUNS, not one curve per segment -- the same rule the
            // detail crop follows, and for the same reason.
            std::vector<Vec2> run;
            const auto flush = [&]() {
                if (run.size() >= 2) {
                    ProjectedCurve piece = curve;
                    piece.shape = ProjectedPolyline{run};
                    out.push_back(std::move(piece));
                }
                run.clear();
            };
            for (std::size_t i = 0; i + 1 < polyline->points.size(); ++i) {
                bool any = false;
                SplitSegment(polyline->points[i], polyline->points[i + 1], span,
                             [&](Vec2 a, Vec2 b) {
                                 any = true;
                                 if (run.empty() ||
                                     std::fabs(AlongOf(run.back(), span) - AlongOf(a, span)) >
                                         kTiny) {
                                     flush();
                                     run.push_back(a);
                                 }
                                 run.push_back(b);
                             });
                if (!any) flush();
            }
            flush();
        }
    }
    return out;
}

} // namespace paramcad
