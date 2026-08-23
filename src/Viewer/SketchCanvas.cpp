#include "Viewer/SketchCanvas.h"

#include "Core/Sketch/Sketch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <variant>

namespace paramcad {
namespace {

constexpr double kEps = 1e-9;

double Length(Vec2 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y); }
Vec2 Sub(Vec2 a, Vec2 b) noexcept { return Vec2{a.x - b.x, a.y - b.y}; }
double Distance(Vec2 a, Vec2 b) noexcept { return Length(Sub(a, b)); }

bool IsFinite(Vec2 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y); }

Vec2 ArcPoint(const SketchArc& arc, double angleRad) noexcept {
    return Vec2{arc.center.x + arc.radiusMm * std::cos(angleRad),
                arc.center.y + arc.radiusMm * std::sin(angleRad)};
}

// M_PI is not guaranteed by the standard, and MSVC only defines it with
// _USE_MATH_DEFINES, which this project does not set. One definition, here.
constexpr double kPi = 3.14159265358979323846;

// Normalises to [0, 2pi).
double WrapPositive(double angle) noexcept {
    const double twoPi = 2.0 * kPi;
    double wrapped = std::fmod(angle, twoPi);
    if (wrapped < 0.0) wrapped += twoPi;
    return wrapped;
}

// True when `angle` lies on the arc, following its stored direction.
bool AngleOnArc(const SketchArc& arc, double angle) noexcept {
    const double from = WrapPositive(arc.startAngleRad);
    const double to = WrapPositive(arc.endAngleRad);
    const double a = WrapPositive(angle);
    const double span = arc.counterClockwise ? WrapPositive(to - from) : WrapPositive(from - to);
    const double offset = arc.counterClockwise ? WrapPositive(a - from) : WrapPositive(from - a);
    return offset <= span + 1e-12;
}

// Nearest point on a segment, and how far away it is.
double DistanceToSegment(Vec2 p, Vec2 a, Vec2 b, Vec2* nearest) noexcept {
    const Vec2 ab = Sub(b, a);
    const double lengthSquared = ab.x * ab.x + ab.y * ab.y;
    double t = 0.0;
    if (lengthSquared > kEps) {
        t = ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / lengthSquared;
        t = std::clamp(t, 0.0, 1.0);
    }
    const Vec2 point{a.x + ab.x * t, a.y + ab.y * t};
    if (nearest != nullptr) *nearest = point;
    return Distance(p, point);
}

// One snap or hit candidate, before ranking.
struct Candidate {
    SketchElementRef ref{};
    Vec2 point{};
    double distance{0.0};
    bool isPoint{false};
};

// Every POINT this sketch offers, in the sub-element vocabulary the SOLVER
// understands (SketchSolveSession): a Point entity is its Whole, a line has
// StartPoint/EndPoint, a circle or arc has CenterPoint -- and NOTHING else.
//
// An arc's two tip positions are deliberately absent. They are real places on
// screen, but the solver has no variable for them (an arc contributes centre
// and radius only), so a constraint attached to one would either be refused or,
// worse, resolve to the radius. Snapping still reaches them through the
// on-curve path, which produces a position and no reference.
void CollectPointCandidates(const Sketch& sketch, Vec2 query, std::vector<Candidate>& out) {
    for (const SketchEntity& entity : sketch.entities()) {
        if (const auto* point = std::get_if<SketchPoint>(&entity.geometry)) {
            out.push_back({SketchElementRef{entity.id, SketchSubElement::Whole}, point->position,
                           Distance(query, point->position), true});
        } else if (const auto* line = std::get_if<SketchLine>(&entity.geometry)) {
            out.push_back({SketchElementRef{entity.id, SketchSubElement::StartPoint}, line->start,
                           Distance(query, line->start), true});
            out.push_back({SketchElementRef{entity.id, SketchSubElement::EndPoint}, line->end,
                           Distance(query, line->end), true});
        } else if (const auto* circle = std::get_if<SketchCircle>(&entity.geometry)) {
            out.push_back({SketchElementRef{entity.id, SketchSubElement::CenterPoint},
                           circle->center, Distance(query, circle->center), true});
        } else if (const auto* arc = std::get_if<SketchArc>(&entity.geometry)) {
            out.push_back({SketchElementRef{entity.id, SketchSubElement::CenterPoint}, arc->center,
                           Distance(query, arc->center), true});
        } else if (const auto* full = std::get_if<SketchEllipse>(&entity.geometry)) {
            out.push_back({SketchElementRef{entity.id, SketchSubElement::CenterPoint},
                           full->center, Distance(query, full->center), true});
        } else if (const auto* piece = std::get_if<SketchEllipticalArc>(&entity.geometry)) {
            out.push_back({SketchElementRef{entity.id, SketchSubElement::CenterPoint},
                           piece->center, Distance(query, piece->center), true});
        }
        // A SPLINE'S POINTS, every one of them. The two ends are references a
        // constraint can hold; the interior ones are positions and nothing
        // more, and they are offered anyway -- a user drawing to a spline aims
        // at the points they clicked, whether or not the solver can name them.
        if (const auto* spline = std::get_if<SketchSpline>(&entity.geometry)) {
            // EVERY POINT IS NAMEABLE NOW (M17.30) -- the ends by their own
            // names, the rest by index. Before this they were positions a
            // cursor could aim at and nothing could constrain.
            for (std::size_t i = 0; i < spline->points.size(); ++i)
                out.push_back(Candidate{SplineRefFor(entity.id, *spline, static_cast<int>(i)),
                                        spline->points[i],
                                        Distance(query, spline->points[i]), true});
        }

        // ...and an ELLIPTICAL ARC's two ends, which are ordinary snappable
        // points like any other curve's. They are added through the shared
        // StartPointOf/EndPointOf rather than recomputed, so the snap lands
        // where the solver's tip variables are.
        if (std::holds_alternative<SketchEllipticalArc>(entity.geometry) ||
            std::holds_alternative<SketchArc>(entity.geometry)) {
            const Vec2 tipStart = StartPointOf(entity.geometry);
            const Vec2 tipEnd = EndPointOf(entity.geometry);
            out.push_back({SketchElementRef{entity.id, SketchSubElement::StartPoint}, tipStart,
                           Distance(query, tipStart), true});
            out.push_back({SketchElementRef{entity.id, SketchSubElement::EndPoint}, tipEnd,
                           Distance(query, tipEnd), true});
        }
    }
}

// Every CURVE, as a Whole reference plus the nearest point on it.

void CollectCurveCandidates(const Sketch& sketch, Vec2 query, std::vector<Candidate>& out) {
    for (const SketchEntity& entity : sketch.entities()) {
        if (std::holds_alternative<SketchPoint>(entity.geometry)) continue; // a point is not a curve
        Vec2 nearest{};
        const double distance = DistanceToSketchGeometry(entity.geometry, query, &nearest);
        if (distance < 0.0) continue; // no answer for this query (dead centre)
        out.push_back({SketchElementRef{entity.id, SketchSubElement::Whole}, nearest, distance,
                       false});
    }
}

// Centre and radius of a circle OR an arc. The tangent command needs both to
// decide which of the two curve-curve tangencies the user meant.
bool CurveCentreAndRadius(const Sketch& sketch, SketchEntityId id, Vec2* centre,
                          double* radius) noexcept {
    const SketchEntity* entity = sketch.findEntity(id);
    if (entity == nullptr) return false;
    if (const auto* circle = std::get_if<SketchCircle>(&entity->geometry)) {
        *centre = circle->center;
        *radius = circle->radiusMm;
        return true;
    }
    if (const auto* arc = std::get_if<SketchArc>(&entity->geometry)) {
        *centre = arc->center;
        *radius = arc->radiusMm;
        return true;
    }
    return false;
}

bool SameRef(const SketchElementRef& a, const SketchElementRef& b) noexcept {
    // THE INDEX COUNTS TOO (M17.30). Left out, every interior point of a spline
    // compares equal to every other, so selecting one would deselect another
    // and a constraint would silently name the wrong point.
    return a.entityId == b.entityId && a.subElement == b.subElement && a.index == b.index;
}

const SketchEntity* EntityOf(const Sketch& sketch, const SketchElementRef& ref) noexcept {
    return sketch.findEntity(ref.entityId);
}

std::string IdText(SketchEntityId id) {
    return "#" + std::to_string(static_cast<unsigned long long>(ToObjectId(id)));
}

std::string SubElementText(SketchSubElement part, int index) {
    switch (part) {
    case SketchSubElement::StartPoint: return " start";
    case SketchSubElement::EndPoint: return " end";
    case SketchSubElement::CenterPoint: return " centre";
    // The index is part of WHICH POINT, so it is part of the name: "Spline1
    // point 3" and "Spline1 point 4" are different things and a description
    // that called both "Spline1 point" would be unusable in a constraint list.
    case SketchSubElement::SplinePoint: return " point " + std::to_string(index);
    case SketchSubElement::Whole: break;
    }
    return "";
}

// A 1-2-5 step at or above `raw`.
double NiceStep(double raw) noexcept {
    if (!(raw > 0.0) || !std::isfinite(raw)) return 1.0;
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    const double normalised = raw / magnitude;
    double step = 10.0;
    if (normalised <= 1.0) step = 1.0;
    else if (normalised <= 2.0) step = 2.0;
    else if (normalised <= 5.0) step = 5.0;
    return step * magnitude;
}

} // namespace

// THE ONE spelling of "point i of this spline".
//
// Called wherever a point is turned into a reference -- the picker, the
// handles, the script parser -- so the rule that index 0 is StartPoint lives in
// one place. Written twice, it would eventually be written differently, and
// the same point would compare unequal to itself.
SketchElementRef SplineRefFor(SketchEntityId id, const SketchSpline& spline, int index) {
    const int count = static_cast<int>(spline.points.size());
    if (index < 0 || index >= count) return SketchElementRef{};
    if (!spline.closed && index == 0) return SketchElementRef{id, SketchSubElement::StartPoint, 0};
    if (!spline.closed && index == count - 1)
        return SketchElementRef{id, SketchSubElement::EndPoint, 0};
    return SketchElementRef{id, SketchSubElement::SplinePoint, index};
}

// Distance from `query` to a curve, and the nearest point on it.
//
// Extracted from CollectCurveCandidates when projected reference geometry
// needed the SAME answer (M17.6): a reference is not an entity, so it cannot
// go through the entity hit-test, but a user clicking one expects it to pick
// exactly the way a drawn curve does. Two copies of this arithmetic would be
// two pickers that disagree about which edge is under the cursor.
//
// Returns a NEGATIVE distance when the query has no meaningful answer -- dead
// on a circle's centre, where every point on the rim is equally near, and the
// centre candidate is the right pick anyway.
double DistanceToSketchGeometry(const SketchGeometry& geometry, Vec2 query, Vec2* nearest) {
    const auto give = [nearest](Vec2 point, double distance) {
        if (nearest != nullptr) *nearest = point;
        return distance;
    };
    if (const auto* point = std::get_if<SketchPoint>(&geometry))
        return give(point->position, Distance(query, point->position));
    if (const auto* line = std::get_if<SketchLine>(&geometry)) {
        Vec2 hit{};
        const double distance = DistanceToSegment(query, line->start, line->end, &hit);
        return give(hit, distance);
    }
    if (const auto* circle = std::get_if<SketchCircle>(&geometry)) {
        const Vec2 radial = Sub(query, circle->center);
        const double length = Length(radial);
        if (length <= kEps) return -1.0;
        const Vec2 hit{circle->center.x + radial.x / length * circle->radiusMm,
                       circle->center.y + radial.y / length * circle->radiusMm};
        return give(hit, std::abs(length - circle->radiusMm));
    }
    if (const auto* arc = std::get_if<SketchArc>(&geometry)) {
        const Vec2 radial = Sub(query, arc->center);
        const double length = Length(radial);
        if (length <= kEps) return -1.0;
        const double angle = std::atan2(radial.y, radial.x);
        if (AngleOnArc(*arc, angle))
            return give(ArcPoint(*arc, angle), std::abs(length - arc->radiusMm));
        // Off the swept range: the nearest point is whichever tip is closer,
        // not the projection onto the full circle.
        const Vec2 startPoint = ArcPoint(*arc, arc->startAngleRad);
        const Vec2 endPoint = ArcPoint(*arc, arc->endAngleRad);
        const double toStart = Distance(query, startPoint);
        const double toEnd = Distance(query, endPoint);
        return toStart <= toEnd ? give(startPoint, toStart) : give(endPoint, toEnd);
    }
    if (const auto* spline = std::get_if<SketchSpline>(&geometry)) {
        // ALONG THE SAMPLED POLYLINE, using the shared sampler -- the same
        // curve the canvas draws and the same one the profile's winding test
        // walks. Measuring against the raw points instead would pick a spline
        // the cursor is nowhere near whenever the curve bulges away from them.
        const std::vector<Vec2> sampled = SampleSpline(*spline, 8);
        if (sampled.size() < 2) return -1.0;
        Vec2 best{};
        double bestDistance = -1.0;
        for (std::size_t i = 1; i < sampled.size(); ++i) {
            Vec2 hit{};
            const double d = DistanceToSegment(query, sampled[i - 1], sampled[i], &hit);
            if (bestDistance < 0.0 || d < bestDistance) {
                bestDistance = d;
                best = hit;
            }
        }
        if (spline->closed) {
            Vec2 hit{};
            const double d = DistanceToSegment(query, sampled.back(), sampled.front(), &hit);
            if (d < bestDistance) {
                bestDistance = d;
                best = hit;
            }
        }
        if (bestDistance < 0.0) return -1.0;
        return give(best, bestDistance);
    }
    if (std::holds_alternative<SketchEllipse>(geometry) ||
        std::holds_alternative<SketchEllipticalArc>(geometry)) {
        // NEAREST BY SAMPLING, and it is worth saying why rather than looking
        // like laziness: the true nearest point on an ellipse is the root of a
        // quartic, and this answer is used for picking and for the snap
        // readout -- both of which are already quantised to a few pixels. A
        // hundred samples put the answer within a thousandth of the curve's
        // own extent, and every one of them is EXACT on the curve, so a snap
        // still lands on the ellipse rather than near it.
        const auto sample = [&geometry](double t) {
            if (const auto* full = std::get_if<SketchEllipse>(&geometry))
                return PointOnEllipse(full->center, full->majorRadiusMm, full->minorRadiusMm,
                                      full->rotationRad, t);
            const auto& piece = std::get<SketchEllipticalArc>(geometry);
            return PointOnEllipse(piece.center, piece.majorRadiusMm, piece.minorRadiusMm,
                                  piece.rotationRad, t);
        };
        constexpr int kSamples = 240;
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        double from = 0.0;
        double sweep = kTwoPi;
        if (const auto* piece = std::get_if<SketchEllipticalArc>(&geometry)) {
            from = piece->startParamRad;
            sweep = piece->endParamRad - piece->startParamRad;
            if (piece->counterClockwise) {
                while (sweep < 0.0) sweep += kTwoPi;
            } else {
                while (sweep > 0.0) sweep -= kTwoPi;
            }
        }
        Vec2 best{};
        double bestDistance = -1.0;
        for (int i = 0; i <= kSamples; ++i) {
            const Vec2 at = sample(from + sweep * (static_cast<double>(i) / kSamples));
            const double d = Distance(query, at);
            if (bestDistance < 0.0 || d < bestDistance) {
                bestDistance = d;
                best = at;
            }
        }
        if (bestDistance < 0.0) return -1.0;
        return give(best, bestDistance);
    }
    return -1.0;
}


// =============================================================================
// CanvasView
// =============================================================================

Vec2 CanvasView::toPixels(Vec2 sketchMm) const noexcept {
    return Vec2{static_cast<double>(widthPx) * 0.5 + (sketchMm.x - centerMm.x) * pixelsPerMm,
                static_cast<double>(heightPx) * 0.5 - (sketchMm.y - centerMm.y) * pixelsPerMm};
}

Vec2 CanvasView::toSketch(Vec2 pixels) const noexcept {
    const double scale = pixelsPerMm > kEps ? pixelsPerMm : kEps;
    return Vec2{centerMm.x + (pixels.x - static_cast<double>(widthPx) * 0.5) / scale,
                centerMm.y - (pixels.y - static_cast<double>(heightPx) * 0.5) / scale};
}

double CanvasView::toSketchLength(double px) const noexcept {
    const double scale = pixelsPerMm > kEps ? pixelsPerMm : kEps;
    return px / scale;
}

void CanvasView::zoomAt(Vec2 pixels, double factor) noexcept {
    if (!std::isfinite(factor) || factor <= 0.0) return;
    const Vec2 before = toSketch(pixels);
    pixelsPerMm = std::clamp(pixelsPerMm * factor, kMinPixelsPerMm, kMaxPixelsPerMm);
    const Vec2 after = toSketch(pixels);
    // The clamp can make this a no-op, and that is the point: the alternative
    // is a scale at which toSketch() returns values a click then writes into
    // the document.
    centerMm.x += before.x - after.x;
    centerMm.y += before.y - after.y;
}

void CanvasView::panByPixels(Vec2 deltaPixels) noexcept {
    const double scale = pixelsPerMm > kEps ? pixelsPerMm : kEps;
    centerMm.x -= deltaPixels.x / scale;
    centerMm.y += deltaPixels.y / scale;
}

CanvasView FitView(const Sketch& sketch, int widthPx, int heightPx, double marginPx) {
    CanvasView view;
    view.widthPx = std::max(widthPx, 1);
    view.heightPx = std::max(heightPx, 1);

    // The ORIGIN is always in the box. It is drawn, it is what an unconstrained
    // sketch is anchored to, and a fit that could scroll it off screen would
    // hide the one landmark a user has.
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    const auto include = [&](Vec2 p) {
        if (!IsFinite(p)) return;
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    };
    for (const SketchEntity& entity : sketch.entities()) {
        if (const auto* point = std::get_if<SketchPoint>(&entity.geometry)) {
            include(point->position);
        } else if (const auto* line = std::get_if<SketchLine>(&entity.geometry)) {
            include(line->start);
            include(line->end);
        } else if (const auto* circle = std::get_if<SketchCircle>(&entity.geometry)) {
            include(Vec2{circle->center.x - circle->radiusMm, circle->center.y - circle->radiusMm});
            include(Vec2{circle->center.x + circle->radiusMm, circle->center.y + circle->radiusMm});
        } else if (const auto* arc = std::get_if<SketchArc>(&entity.geometry)) {
            include(Vec2{arc->center.x - arc->radiusMm, arc->center.y - arc->radiusMm});
            include(Vec2{arc->center.x + arc->radiusMm, arc->center.y + arc->radiusMm});
        } else if (const auto* spline = std::get_if<SketchSpline>(&entity.geometry)) {
            // THE SAMPLED CURVE, not just its points: a spline bulges past
            // them, and a fit that clipped the bulge would hide the very part
            // of the shape the user is looking at.
            for (const Vec2& at : SampleSpline(*spline, 8)) include(at);
        } else if (std::holds_alternative<SketchEllipse>(entity.geometry) ||
                   std::holds_alternative<SketchEllipticalArc>(entity.geometry)) {
            // THE MAJOR RADIUS BOTH WAYS, which is a box that certainly
            // contains the ellipse whatever its rotation. Rotating the true
            // half-extents would be tighter and would also be a second place
            // for the rotation convention to live; a fit that is slightly loose
            // is not a defect, and one that clips is.
            double major = 0.0;
            Vec2 centre{};
            if (const auto* full = std::get_if<SketchEllipse>(&entity.geometry)) {
                major = full->majorRadiusMm;
                centre = full->center;
            } else {
                const auto& piece = std::get<SketchEllipticalArc>(entity.geometry);
                major = piece.majorRadiusMm;
                centre = piece.center;
            }
            include(Vec2{centre.x - major, centre.y - major});
            include(Vec2{centre.x + major, centre.y + major});
        }
    }

    double spanX = maxX - minX;
    double spanY = maxY - minY;
    if (spanX < 1.0 && spanY < 1.0) {
        // An empty (or dot-sized) sketch has no extent to fit. 100 mm across is
        // a working default; deriving a scale from a zero span is how a fit
        // produces the infinite zoom the clamp exists to catch.
        spanX = 100.0;
        spanY = 100.0;
        minX = -50.0;
        minY = -50.0;
    }
    spanX = std::max(spanX, 1.0);
    spanY = std::max(spanY, 1.0);

    const double usableX = std::max(static_cast<double>(view.widthPx) - 2.0 * marginPx, 1.0);
    const double usableY = std::max(static_cast<double>(view.heightPx) - 2.0 * marginPx, 1.0);
    view.pixelsPerMm =
        std::clamp(std::min(usableX / spanX, usableY / spanY), kMinPixelsPerMm, kMaxPixelsPerMm);
    view.centerMm = Vec2{minX + spanX * 0.5, minY + spanY * 0.5};
    return view;
}

double GridStepMm(const CanvasView& view) noexcept {
    // Aim for roughly 40 px between grid lines, then round to 1-2-5.
    const double scale = view.pixelsPerMm > kEps ? view.pixelsPerMm : kEps;
    return NiceStep(40.0 / scale);
}

// =============================================================================
// Snapping and picking
// =============================================================================

const char* SnapKindName(SnapKind kind) noexcept {
    switch (kind) {
    case SnapKind::Free: return "free";
    case SnapKind::Grid: return "grid";
    case SnapKind::OnCurve: return "on curve";
    case SnapKind::Origin: return "origin";
    case SnapKind::CenterPoint: return "centre";
    case SnapKind::Endpoint: return "endpoint";
    }
    return "free";
}

SnapResult SnapCursor(const Sketch& sketch, Vec2 rawMm, double toleranceMm, double gridMm,
                      bool suppress) {
    SnapResult result;
    result.point = rawMm;
    result.kind = SnapKind::Free;
    if (suppress || !IsFinite(rawMm)) return result;

    const double tolerance = std::max(toleranceMm, 0.0);

    // 1. Defined points win. A snap that produces a REFERENCE is the only kind
    //    that can become a constraint (roadmap 4.2), so it outranks everything
    //    that produces only a position.
    std::vector<Candidate> points;
    CollectPointCandidates(sketch, rawMm, points);
    const Candidate* bestPoint = nullptr;
    for (const Candidate& candidate : points) {
        if (candidate.distance > tolerance) continue;
        if (bestPoint == nullptr || candidate.distance < bestPoint->distance)
            bestPoint = &candidate;
    }
    if (bestPoint != nullptr) {
        result.point = bestPoint->point;
        result.ref = bestPoint->ref;
        result.kind = bestPoint->ref.subElement == SketchSubElement::CenterPoint
                          ? SnapKind::CenterPoint
                          : SnapKind::Endpoint;
        return result;
    }

    // 2. The origin. It has no entity, so it can anchor nothing -- but a Fix on
    //    a point the user placed there is the ordinary way to anchor a sketch,
    //    and landing exactly on (0,0) is what makes that possible.
    if (Distance(rawMm, Vec2{0.0, 0.0}) <= tolerance) {
        result.point = Vec2{0.0, 0.0};
        result.kind = SnapKind::Origin;
        return result;
    }

    // 3. Somewhere along a curve. Reports a POSITION and no reference: EP3D has
    //    no point-on-object constraint yet (todo 2.1), and reporting a Whole
    //    reference here would let an inferred constraint resolve to a circle's
    //    RADIUS variable rather than to a point on it.
    std::vector<Candidate> curves;
    CollectCurveCandidates(sketch, rawMm, curves);
    const Candidate* bestCurve = nullptr;
    for (const Candidate& candidate : curves) {
        if (candidate.distance > tolerance) continue;
        if (bestCurve == nullptr || candidate.distance < bestCurve->distance)
            bestCurve = &candidate;
    }
    if (bestCurve != nullptr) {
        result.point = bestCurve->point;
        result.kind = SnapKind::OnCurve;
        return result;
    }

    // 4. The grid.
    if (gridMm > kEps) {
        const Vec2 snapped{std::round(rawMm.x / gridMm) * gridMm,
                           std::round(rawMm.y / gridMm) * gridMm};
        if (Distance(rawMm, snapped) <= tolerance) {
            result.point = snapped;
            result.kind = SnapKind::Grid;
            return result;
        }
    }
    return result;
}

std::vector<SketchElementRef> HitTest(const Sketch& sketch, Vec2 pickMm, double toleranceMm) {
    std::vector<SketchElementRef> hits;
    if (!IsFinite(pickMm)) return hits;
    const double tolerance = std::max(toleranceMm, 0.0);

    std::vector<Candidate> points;
    CollectPointCandidates(sketch, pickMm, points);
    std::vector<Candidate> curves;
    CollectCurveCandidates(sketch, pickMm, curves);

    const auto within = [&](std::vector<Candidate>& candidates) {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [&](const Candidate& c) { return c.distance > tolerance; }),
                         candidates.end());
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const Candidate& a, const Candidate& b) {
                             return a.distance < b.distance;
                         });
    };
    within(points);
    within(curves);

    // Points before curves, unconditionally. A click on a line's endpoint means
    // the endpoint: without this, every coincident and every fix would need the
    // user to find a pixel that is on the endpoint and off the line.
    for (const Candidate& candidate : points) hits.push_back(candidate.ref);
    for (const Candidate& candidate : curves) {
        // A Point entity's Whole is already in `points`; do not offer it twice.
        const bool duplicate =
            std::any_of(hits.begin(), hits.end(),
                        [&](const SketchElementRef& ref) { return SameRef(ref, candidate.ref); });
        if (!duplicate) hits.push_back(candidate.ref);
    }
    return hits;
}

SketchReferenceId ReferenceAt(const Sketch& sketch, Vec2 pickMm, double toleranceMm) {
    if (!IsFinite(pickMm)) return kInvalidSketchReferenceId;
    const double tolerance = std::max(toleranceMm, 0.0);
    // POINTS WIN, exactly as they do in the entity hit-test. A projected
    // vertex sits ON the two edges that meet there, so without this rule the
    // corner of a face could never be converted -- every click there would
    // land on an edge that is precisely as near.
    SketchReferenceId bestPoint = kInvalidSketchReferenceId;
    double bestPointDistance = 0.0;
    SketchReferenceId bestCurve = kInvalidSketchReferenceId;
    double bestCurveDistance = 0.0;
    for (const SketchReference& reference : sketch.references()) {
        const double distance = DistanceToSketchGeometry(reference.geometry, pickMm, nullptr);
        if (distance < 0.0 || distance > tolerance) continue;
        const bool isPoint = std::holds_alternative<SketchPoint>(reference.geometry);
        SketchReferenceId& slot = isPoint ? bestPoint : bestCurve;
        double& held = isPoint ? bestPointDistance : bestCurveDistance;
        if (slot != kInvalidSketchReferenceId && distance >= held) continue;
        slot = reference.id;
        held = distance;
    }
    return bestPoint != kInvalidSketchReferenceId ? bestPoint : bestCurve;
}

SketchElementRef PickElement(const Sketch& sketch, Vec2 pickMm, double toleranceMm) {
    const std::vector<SketchElementRef> hits = HitTest(sketch, pickMm, toleranceMm);
    return hits.empty() ? SketchElementRef{} : hits.front();
}

Vec2 ResolveElementPoint(const Sketch& sketch, const SketchElementRef& ref, bool* ok) {
    const auto fail = [&]() {
        if (ok != nullptr) *ok = false;
        return Vec2{0.0, 0.0};
    };
    const SketchEntity* entity = EntityOf(sketch, ref);
    if (entity == nullptr) return fail();
    if (ok != nullptr) *ok = true;

    if (const auto* point = std::get_if<SketchPoint>(&entity->geometry)) {
        if (ref.subElement != SketchSubElement::Whole) return fail();
        return point->position;
    }
    if (const auto* line = std::get_if<SketchLine>(&entity->geometry)) {
        if (ref.subElement == SketchSubElement::StartPoint) return line->start;
        if (ref.subElement == SketchSubElement::EndPoint) return line->end;
        if (ref.subElement == SketchSubElement::Whole)
            return Vec2{(line->start.x + line->end.x) * 0.5, (line->start.y + line->end.y) * 0.5};
        return fail();
    }
    if (const auto* circle = std::get_if<SketchCircle>(&entity->geometry)) {
        if (ref.subElement == SketchSubElement::CenterPoint ||
            ref.subElement == SketchSubElement::Whole)
            return circle->center;
        return fail();
    }
    if (const auto* arc = std::get_if<SketchArc>(&entity->geometry)) {
        if (ref.subElement == SketchSubElement::CenterPoint ||
            ref.subElement == SketchSubElement::Whole)
            return arc->center;
        if (ref.subElement == SketchSubElement::StartPoint)
            return StartPointOf(entity->geometry);
        if (ref.subElement == SketchSubElement::EndPoint) return EndPointOf(entity->geometry);
        return fail();
    }
    if (const auto* spline = std::get_if<SketchSpline>(&entity->geometry)) {
        if (spline->points.empty()) return fail();
        // WHOLE is the middle OF THE CURVE, not the middle of the point list --
        // it is where a label or a badge goes, and it has to be on the shape.
        if (ref.subElement == SketchSubElement::Whole) return PointOnSpline(*spline, 0.5);
        const std::optional<Vec2> at =
            PointOfSubElement(entity->geometry, ref.subElement, ref.index);
        if (!at) return fail();
        return *at;
    }
    if (const auto* full = std::get_if<SketchEllipse>(&entity->geometry)) {
        if (ref.subElement == SketchSubElement::CenterPoint ||
            ref.subElement == SketchSubElement::Whole)
            return full->center;
        return fail();
    }
    if (const auto* piece = std::get_if<SketchEllipticalArc>(&entity->geometry)) {
        if (ref.subElement == SketchSubElement::CenterPoint ||
            ref.subElement == SketchSubElement::Whole)
            return piece->center;
        if (ref.subElement == SketchSubElement::StartPoint)
            return StartPointOf(entity->geometry);
        if (ref.subElement == SketchSubElement::EndPoint) return EndPointOf(entity->geometry);
        return fail();
    }
    return fail();
}

bool IsPointRef(const Sketch& sketch, const SketchElementRef& ref) noexcept {
    const SketchEntity* entity = EntityOf(sketch, ref);
    if (entity == nullptr) return false;
    if (std::holds_alternative<SketchPoint>(entity->geometry))
        return ref.subElement == SketchSubElement::Whole;
    if (std::holds_alternative<SketchLine>(entity->geometry))
        return ref.subElement == SketchSubElement::StartPoint ||
               ref.subElement == SketchSubElement::EndPoint;
    // AN ARC'S TIPS ARE VARIABLES since M17 (ArcTipU/V), and an elliptical
    // arc's are too (EllipseTipU/V) -- so they are points a constraint can
    // hold, not merely positions on screen. The comment that used to stand here
    // said otherwise and had been wrong since ADR-M17-018.
    if (const auto* spline = std::get_if<SketchSpline>(&entity->geometry))
        // EVERY point, through the one resolvability rule -- so what the UI
        // offers and what the solver accepts cannot come apart.
        return IsResolvableRef(entity->geometry, ref.subElement, ref.index) &&
               ref.subElement != SketchSubElement::Whole && spline->points.size() > 0;
    if (std::holds_alternative<SketchArc>(entity->geometry) ||
        std::holds_alternative<SketchEllipticalArc>(entity->geometry))
        return ref.subElement == SketchSubElement::CenterPoint ||
               ref.subElement == SketchSubElement::StartPoint ||
               ref.subElement == SketchSubElement::EndPoint;
    // A closed curve has only its centre.
    return ref.subElement == SketchSubElement::CenterPoint;
}

bool IsLineRef(const Sketch& sketch, const SketchElementRef& ref) noexcept {
    const SketchEntity* entity = EntityOf(sketch, ref);
    return entity != nullptr && std::holds_alternative<SketchLine>(entity->geometry);
}

bool IsCurveRef(const Sketch& sketch, const SketchElementRef& ref) noexcept {
    const SketchEntity* entity = EntityOf(sketch, ref);
    if (entity == nullptr) return false;
    // A CIRCLE OR AN ARC -- deliberately NOT an ellipse.
    //
    // Every caller of this asks it in order to offer something that needs ONE
    // radius: a radius dimension, a tangency, the two curve-curve branches.
    // None of those has an answer for an ellipse yet, and widening this would
    // let the commands be offered and then refused deep in the solve session,
    // where the message names a constraint id rather than the thing the user
    // clicked. IsEllipseRef below is how the ellipse's own commands ask.
    return std::holds_alternative<SketchCircle>(entity->geometry) ||
           std::holds_alternative<SketchArc>(entity->geometry);
}

bool IsEllipseRef(const Sketch& sketch, const SketchElementRef& ref) noexcept {
    const SketchEntity* entity = EntityOf(sketch, ref);
    if (entity == nullptr) return false;
    return std::holds_alternative<SketchEllipse>(entity->geometry) ||
           std::holds_alternative<SketchEllipticalArc>(entity->geometry);
}

bool IsSplineRef(const Sketch& sketch, const SketchElementRef& ref) noexcept {
    const SketchEntity* entity = EntityOf(sketch, ref);
    return entity != nullptr && std::holds_alternative<SketchSpline>(entity->geometry);
}

bool IsCentredRef(const Sketch& sketch, const SketchElementRef& ref) noexcept {
    // ANYTHING WITH A CENTRE, which is what Concentric actually needs -- and
    // exactly what the solve session's own `centredSlots` accepts.
    //
    // Concentric asked IsCurveRef, which deliberately excludes an ellipse
    // because every OTHER caller of it wants one radius. So the command refused
    // a constraint the solver has supported since M17.25, and the message told
    // the user their ellipse was "not a circle or an arc" -- which is true and
    // is not the reason. Found by the first script that tried it.
    return IsCurveRef(sketch, ref) || IsEllipseRef(sketch, ref);
}

bool AngleOnArcSweep(const SketchArc& arc, double angleRad) noexcept {
    return AngleOnArc(arc, angleRad);
}

std::vector<SketchElementRef> EntityHandles(const Sketch& sketch, SketchEntityId id) {
    std::vector<SketchElementRef> handles;
    const SketchEntity* entity = sketch.findEntity(id);
    if (entity == nullptr) return handles;
    if (std::holds_alternative<SketchPoint>(entity->geometry)) {
        handles.push_back({id, SketchSubElement::Whole});
    } else if (std::holds_alternative<SketchLine>(entity->geometry)) {
        handles.push_back({id, SketchSubElement::StartPoint});
        handles.push_back({id, SketchSubElement::EndPoint});
    } else if (const auto* spline = std::get_if<SketchSpline>(&entity->geometry)) {
        // EVERY POINT gets a handle now: a handle the user cannot grab is a
        // constraint they cannot make, and until M17.30 that was five of the
        // seven points on a spline.
        for (int i = 0; i < static_cast<int>(spline->points.size()); ++i)
            handles.push_back(SplineRefFor(id, *spline, i));
    } else if (std::holds_alternative<SketchArc>(entity->geometry) ||
               std::holds_alternative<SketchEllipticalArc>(entity->geometry)) {
        // THE CENTRE AND BOTH TIPS. The comment that used to stand here said an
        // arc's tips were places on screen the solver had no variable for --
        // true when it was written, and false since ADR-M17-018 made them
        // variables. A handle the user cannot grab is a constraint they cannot
        // make.
        handles.push_back({id, SketchSubElement::CenterPoint});
        handles.push_back({id, SketchSubElement::StartPoint});
        handles.push_back({id, SketchSubElement::EndPoint});
    } else {
        // A closed curve offers its CENTRE and nothing else.
        handles.push_back({id, SketchSubElement::CenterPoint});
    }
    return handles;
}

std::string DescribeElementRef(const Sketch& sketch, const SketchElementRef& ref) {
    const SketchEntity* entity = EntityOf(sketch, ref);
    if (entity == nullptr) return "unknown element";
    std::string kind = "entity";
    if (std::holds_alternative<SketchPoint>(entity->geometry)) kind = "Point";
    else if (std::holds_alternative<SketchLine>(entity->geometry)) kind = "Line";
    else if (std::holds_alternative<SketchCircle>(entity->geometry)) kind = "Circle";
    else if (std::holds_alternative<SketchArc>(entity->geometry)) kind = "Arc";
    else if (std::holds_alternative<SketchEllipse>(entity->geometry)) kind = "Ellipse";
    else if (std::holds_alternative<SketchEllipticalArc>(entity->geometry))
        kind = "Elliptical arc";
    else if (std::holds_alternative<SketchSpline>(entity->geometry)) kind = "Spline";
    return kind + " " + IdText(ref.entityId) + SubElementText(ref.subElement, ref.index);
}

// =============================================================================
// Tools and edits
// =============================================================================

const char* SketchToolName(SketchTool tool) noexcept {
    switch (tool) {
    case SketchTool::Select: return "Select";
    case SketchTool::Point: return "Point";
    case SketchTool::Line: return "Line";
    case SketchTool::Rectangle: return "Rectangle";
    case SketchTool::CenterRectangle: return "Center Rectangle";
    case SketchTool::Circle: return "Circle";
    case SketchTool::ThreePointCircle: return "3-Point Circle";
    case SketchTool::Arc: return "Arc";
    case SketchTool::ThreePointArc: return "3-Point Arc";
    case SketchTool::TangentArc: return "Tangent Arc";
    case SketchTool::Ellipse: return "Ellipse";
    case SketchTool::EllipticalArc: return "Elliptical Arc";
    case SketchTool::Spline: return "Spline";
    case SketchTool::Polygon: return "Polygon";
    case SketchTool::Slot: return "Slot";
    }
    return "Select";
}

int SketchToolPointCount(SketchTool tool) noexcept {
    switch (tool) {
    case SketchTool::Select: return 0;
    case SketchTool::Point: return 1;
    case SketchTool::Line: return 2;
    case SketchTool::Rectangle: return 2;
    case SketchTool::CenterRectangle: return 2; // centre, then a corner
    case SketchTool::Circle: return 2;
    case SketchTool::ThreePointCircle: return 3; // three points on the rim
    case SketchTool::Arc: return 3;
    case SketchTool::ThreePointArc: return 3; // both tips, then a point on it
    case SketchTool::TangentArc: return 2;    // the end to grow from, then where to stop
    case SketchTool::Ellipse: return 3;      // centre, the major end, then the width
    case SketchTool::EllipticalArc: return 4; // ...and then where to stop
    case SketchTool::Spline: return kSplineIsFinishedByHand;
    case SketchTool::Polygon: return 2;       // centre, then a vertex
    case SketchTool::Slot: return 3;          // both centres, then the width
    }
    return 0;
}

const char* SketchEditKindName(SketchEditKind kind) noexcept {
    switch (kind) {
    case SketchEditKind::None: return "Nothing";
    case SketchEditKind::AddPoint: return "Point";
    case SketchEditKind::AddLine: return "Line";
    case SketchEditKind::AddCircle: return "Circle";
    case SketchEditKind::AddArc: return "Arc";
    case SketchEditKind::AddRectangle: return "Rectangle";
    case SketchEditKind::AddCenterRectangle: return "Centre rectangle";
    case SketchEditKind::AddThreePointCircle: return "Circle through 3 points";
    case SketchEditKind::AddThreePointArc: return "Arc through 3 points";
    case SketchEditKind::AddTangentArc: return "Tangent arc";
    case SketchEditKind::AddEllipse: return "Ellipse";
    case SketchEditKind::AddEllipticalArc: return "Elliptical arc";
    case SketchEditKind::AddSpline: return "Spline";
    case SketchEditKind::AddPolygon: return "Polygon";
    case SketchEditKind::AddSlot: return "Slot";
    case SketchEditKind::AddCoincident: return "Coincident";
    case SketchEditKind::AddHorizontal: return "Horizontal";
    case SketchEditKind::AddVertical: return "Vertical";
    case SketchEditKind::AddFix: return "Fix";
    case SketchEditKind::AddDistance: return "Distance";
    case SketchEditKind::AddHorizontalDistance: return "HorizontalDistance";
    case SketchEditKind::AddVerticalDistance: return "VerticalDistance";
    case SketchEditKind::AddPointLineDistance: return "PointLineDistance";
    case SketchEditKind::AddLength: return "Length";
    case SketchEditKind::AddRadius: return "Radius";
    case SketchEditKind::AddDiameter: return "Diameter";
    case SketchEditKind::AddMajorAxis: return "Major axis";
    case SketchEditKind::AddMinorAxis: return "Minor axis";
    case SketchEditKind::AddEllipseRotation: return "Ellipse angle";
    case SketchEditKind::AddAngle: return "Angle";
    case SketchEditKind::AddParallel: return "Parallel";
    case SketchEditKind::AddPerpendicular: return "Perpendicular";
    case SketchEditKind::AddEqual: return "Equal";
    case SketchEditKind::AddConcentric: return "Concentric";
    case SketchEditKind::AddMidpoint: return "Midpoint";
    case SketchEditKind::AddPointOnObject: return "Point on object";
    case SketchEditKind::AddTangent: return "Tangent";
    case SketchEditKind::AddSymmetric: return "Symmetric";
    case SketchEditKind::DeleteEntities: return "Delete geometry";
    case SketchEditKind::DeleteConstraints: return "Delete constraint";
    }
    return "Nothing";
}

bool IsDimensionEdit(SketchEditKind kind) noexcept {
    return kind == SketchEditKind::AddDistance ||
           kind == SketchEditKind::AddHorizontalDistance ||
           kind == SketchEditKind::AddVerticalDistance ||
           kind == SketchEditKind::AddPointLineDistance || kind == SketchEditKind::AddLength ||
           kind == SketchEditKind::AddRadius || kind == SketchEditKind::AddDiameter ||
           kind == SketchEditKind::AddMajorAxis || kind == SketchEditKind::AddMinorAxis ||
           kind == SketchEditKind::AddEllipseRotation ||
           kind == SketchEditKind::AddAngle;
}

// =============================================================================
// SketchCanvasModel
// =============================================================================

void SketchCanvasModel::setTool(SketchTool tool) {
    tool_ = tool;
    pending_.clear();
    pendingSnaps_.clear();
    chainFromCreatedEntity_ = false;
}

bool SketchCanvasModel::cancel() {
    // ONE press leaves the tool, dropping whatever was half-drawn with it.
    //
    // It used to take two -- the first dropped the shape and stayed in the
    // tool, the second returned to Select -- on the theory that a user who
    // abandoned one line probably wanted to draw another. In practice Esc means
    // "stop", and a user who pressed it once and then clicked to select
    // something drew a stray line instead. Drawing another line is one click on
    // the tool; recovering from geometry you did not mean to create is not.
    //
    // Ending a polyline chain is unaffected: the chain lives in `pending_`, so
    // it goes with the same press.
    if (tool_ != SketchTool::Select) {
        pending_.clear();
        pendingSnaps_.clear();
        chainFromCreatedEntity_ = false;
        tool_ = SketchTool::Select;
        return true;
    }
    if (!pending_.empty()) {
        pending_.clear();
        pendingSnaps_.clear();
        chainFromCreatedEntity_ = false;
        return true;
    }
    if (!selection_.empty()) {
        selection_.clear();
        return true;
    }
    return false;
}

std::string SketchCanvasModel::prompt() const {
    const std::string suppressed = suppressInference_ ? "  [inference suppressed]" : "";
    const int needed = SketchToolPointCount(tool_);
    const int have = static_cast<int>(pending_.size());
    switch (tool_) {
    case SketchTool::Select:
        return selection_.empty()
                   ? "Select: click geometry to select it; click again to deselect" + suppressed
                   : "Select: " + std::to_string(selection_.size()) +
                         " selected -- apply a constraint or a dimension" + suppressed;
    case SketchTool::Point:
        return "Point: click to place" + suppressed;
    case SketchTool::Line:
        return (have == 0 ? "Line: click the start point"
                          : "Line: click the end point (Esc ends the chain)") +
               suppressed;
    case SketchTool::Rectangle:
        return (have == 0 ? "Rectangle: click the first corner"
                          : "Rectangle: click the opposite corner") +
               suppressed;
    case SketchTool::Circle:
        return (have == 0 ? "Circle: click the centre" : "Circle: click a point on the rim") +
               suppressed;
    case SketchTool::Arc:
        if (have == 0) return "Arc: click the centre" + suppressed;
        if (have == 1) return "Arc: click the start point" + suppressed;
        return "Arc: click the end point (swept counter-clockwise)" + suppressed;
    case SketchTool::CenterRectangle:
        return (have == 0 ? "Centre rectangle: click the centre"
                          : "Centre rectangle: click a corner") +
               suppressed;
    case SketchTool::ThreePointCircle:
        if (have == 0) return "3-point circle: click the first point on it" + suppressed;
        if (have == 1) return "3-point circle: click the second point" + suppressed;
        return "3-point circle: click the third point" + suppressed;
    case SketchTool::ThreePointArc:
        if (have == 0) return "3-point arc: click one end" + suppressed;
        if (have == 1) return "3-point arc: click the other end" + suppressed;
        return "3-point arc: click a point the arc passes through" + suppressed;
    case SketchTool::TangentArc:
        // The first prompt says what has to be under the cursor, because that
        // is the one thing this tool cannot do without and the one thing a
        // user cannot guess.
        return (have == 0 ? "Tangent arc: click the free END of a line or arc"
                          : "Tangent arc: click where it should stop") +
               suppressed;
    case SketchTool::Ellipse:
        if (have == 0) return "Ellipse: click the centre" + suppressed;
        if (have == 1) return "Ellipse: click the end of the LONG axis" + suppressed;
        return "Ellipse: click to set the width" + suppressed;
    case SketchTool::Spline:
        if (have == 0) return "Spline: click the first point it goes through" + suppressed;
        if (have == 1) return "Spline: click the next point" + suppressed;
        return "Spline: click the next point, double-click to finish, or click the first "
               "point again to close it" +
               suppressed;
    case SketchTool::EllipticalArc:
        if (have == 0) return "Elliptical arc: click the centre" + suppressed;
        if (have == 1) return "Elliptical arc: click the end of the LONG axis" + suppressed;
        if (have == 2) return "Elliptical arc: click to set the width" + suppressed;
        return "Elliptical arc: click where it should stop (it starts at the long axis)" +
               suppressed;
    case SketchTool::Polygon:
        return (have == 0 ? "Polygon (" + std::to_string(polygonSides_) +
                                " sides): click the centre"
                          : "Polygon: click a corner") +
               suppressed;
    case SketchTool::Slot:
        if (have == 0) return "Slot: click the centre of one end" + suppressed;
        if (have == 1) return "Slot: click the centre of the other end" + suppressed;
        return "Slot: click to set the width" + suppressed;
    }
    (void)needed;
    return "Ready";
}

bool SketchCanvasModel::isSelected(const SketchElementRef& ref) const noexcept {
    return std::any_of(selection_.begin(), selection_.end(),
                       [&](const SketchElementRef& held) { return SameRef(held, ref); });
}

void SketchCanvasModel::toggleSelection(const SketchElementRef& ref) {
    if (ref.entityId == kInvalidSketchEntityId) return;
    for (auto it = selection_.begin(); it != selection_.end(); ++it) {
        if (SameRef(*it, ref)) {
            selection_.erase(it);
            return;
        }
    }
    selection_.push_back(ref);
}

void SketchCanvasModel::setSelection(std::vector<SketchElementRef> refs) {
    selection_ = std::move(refs);
}

bool SketchCanvasModel::clearSelection() {
    if (selection_.empty()) return false;
    selection_.clear();
    return true;
}

bool SketchCanvasModel::selectAt(const Sketch& sketch, Vec2 pickMm, double toleranceMm) {
    const SketchElementRef hit = PickElement(sketch, pickMm, toleranceMm);
    if (hit.entityId == kInvalidSketchEntityId) return clearSelection();

    // Clicking a HANDLE of something already selected NARROWS the selection to
    // that handle instead of adding to it.
    //
    // This is the click-the-line-then-click-its-end flow, and without the rule
    // it does not work: the line stays selected alongside the endpoint, so the
    // selection is one line plus one point, and Dimension refuses it as
    // uninterpretable (roadmap 7.1 has no entry for that pair). The user's
    // second click is a refinement of the first -- "this END of that line" --
    // not a second thing to measure.
    //
    // It does NOT break the no-modifier multi-select rule (roadmap 13.1): the
    // whole entity was never a separate pick from its own handle, and picking a
    // handle of something ELSE still adds, as it always did.
    if (hit.subElement != SketchSubElement::Whole) {
        const SketchElementRef whole{hit.entityId, SketchSubElement::Whole};
        for (auto it = selection_.begin(); it != selection_.end(); ++it) {
            if (!SameRef(*it, whole)) continue;
            *it = hit;
            return true;
        }
    }

    toggleSelection(hit);
    return true;
}

SketchEdit SketchCanvasModel::click(const SnapResult& snap) {
    SketchEdit edit;
    if (tool_ == SketchTool::Select) return edit;
    if (!IsFinite(snap.point)) return edit;

    pending_.push_back(snap.point);
    // A snapped-to reference is recorded only when it is one the solver can
    // constrain. Everything else contributes a position and nothing more --
    // except the origin, which names no element and still has to be remembered,
    // because a point dropped there earns a Fix rather than a coincidence.
    PendingSnap record;
    if (snap.hasRef()) record.ref = snap.ref;
    record.atOrigin = snap.kind == SnapKind::Origin;
    pendingSnaps_.push_back(record);

    // CLOSING BY CLICKING THE FIRST POINT AGAIN, which is how every sketcher
    // closes a chain -- and it has to be checked before the count, because a
    // spline has no count to reach.
    if (tool_ == SketchTool::Spline && pending_.size() > 2 &&
        SamePoint(pending_.front(), snap.point, toleranceForClosing_)) {
        pending_.pop_back();      // the closing click is the first point, not a new one
        pendingSnaps_.pop_back();
        closePendingSpline_ = true;
        return finishPendingSpline();
    }
    if (SketchToolPointCount(tool_) == kSplineIsFinishedByHand) return edit;
    if (static_cast<int>(pending_.size()) < SketchToolPointCount(tool_)) return edit;
    return completeDrawing(snap);
}

SketchEdit SketchCanvasModel::finishPendingSpline() {
    SketchEdit edit;
    if (tool_ != SketchTool::Spline) return edit;
    // TOO FEW POINTS IS NOT A SPLINE, and it is not an error either -- a
    // double-click after one point is a user changing their mind. The pending
    // points are kept so the next click continues the same curve.
    if (pending_.size() < kMinSplinePoints) return edit;

    edit.kind = SketchEditKind::AddSpline;
    edit.points = pending_;
    edit.splineClosed = closePendingSpline_;
    edit.label = "Add spline";
    // EVERY CLICKED POINT that landed on something earns a coincidence -- but
    // only the two ENDS can have one, because only they are nameable. The
    // interior points snapped for aim, and saying so is better than pretending
    // a constraint was made.
    const auto joinIfSnapped = [&](std::size_t index, SketchSubElement part) {
        if (index >= pendingSnaps_.size()) return;
        const SketchElementRef& existing = pendingSnaps_[index].ref;
        if (existing.entityId == kInvalidSketchEntityId) return;
        PendingConstraint constraint;
        constraint.kind = SketchEditKind::AddCoincident;
        constraint.a = PendingRef{true, 0, SketchElementRef{}, part};
        constraint.b = PendingRef{false, 0, existing, existing.subElement};
        edit.autoConstraints.push_back(constraint);
    };
    if (!closePendingSpline_) {
        joinIfSnapped(0, SketchSubElement::StartPoint);
        joinIfSnapped(pending_.size() - 1, SketchSubElement::EndPoint);
        if (!pendingSnaps_.empty() && pendingSnaps_.front().atOrigin) {
            PendingConstraint fix;
            fix.kind = SketchEditKind::AddFix;
            fix.a = PendingRef{true, 0, SketchElementRef{}, SketchSubElement::StartPoint};
            edit.autoConstraints.push_back(fix);
        }
    }

    pending_.clear();
    pendingSnaps_.clear();
    closePendingSpline_ = false;
    return edit;
}

SketchEdit SketchCanvasModel::completeDrawing(const SnapResult& snap) {
    (void)snap;
    SketchEdit edit;

    // Turns the reference behind pending point `index` into a coincidence with
    // the sub-element `part` of the entity this edit creates at `newEntity`.
    const auto inferCoincidence = [&](std::size_t index, std::size_t newEntity,
                                      SketchSubElement part) {
        if (index >= pendingSnaps_.size()) return;
        const SketchElementRef& existing = pendingSnaps_[index].ref;
        if (existing.entityId == kInvalidSketchEntityId) return;
        PendingConstraint constraint;
        constraint.kind = SketchEditKind::AddCoincident;
        constraint.a = PendingRef{true, newEntity, SketchElementRef{}, part};
        constraint.b = PendingRef{false, 0, existing, existing.subElement};
        edit.autoConstraints.push_back(constraint);
    };

    // The same idea for the origin, which is the one snap target a coincidence
    // cannot express: there is no origin ENTITY to be coincident with, so the
    // relationship "this point is at (0,0)" is spelled Fix.
    //
    // Only points the geometry actually OWNS are eligible. An arc's third click
    // is projected onto the radius the first two clicks fixed, so it is not
    // where the user clicked; a Fix there would pin the projection and claim it
    // was the origin. Passing only the sub-elements that reproduce the click
    // exactly is what keeps the constraint honest.
    const auto inferOriginFix = [&](std::size_t index, std::size_t newEntity,
                                    SketchSubElement part) {
        if (index >= pendingSnaps_.size()) return;
        if (!pendingSnaps_[index].atOrigin) return;
        PendingConstraint constraint;
        constraint.kind = SketchEditKind::AddFix;
        constraint.a = PendingRef{true, newEntity, SketchElementRef{}, part};
        edit.autoConstraints.push_back(constraint);
    };

    const auto finish = [&]() {
        pending_.clear();
        pendingSnaps_.clear();
    };

    switch (tool_) {
    case SketchTool::Point: {
        edit.kind = SketchEditKind::AddPoint;
        edit.points = pending_;
        edit.label = "Add point";
        inferCoincidence(0, 0, SketchSubElement::Whole);
        inferOriginFix(0, 0, SketchSubElement::Whole);
        finish();
        return edit;
    }
    case SketchTool::Line: {
        if (Distance(pending_[0], pending_[1]) <= kSketchToleranceMm) {
            // A zero-length line is refused by the sketch anyway; dropping the
            // offending click here means the user simply clicks again instead
            // of watching a command fail for a reason nothing explains.
            pending_.pop_back();
            pendingSnaps_.pop_back();
            return edit;
        }
        edit.kind = SketchEditKind::AddLine;
        edit.points = pending_;
        edit.label = "Add line";
        inferCoincidence(0, 0, SketchSubElement::StartPoint);
        inferCoincidence(1, 0, SketchSubElement::EndPoint);
        inferOriginFix(0, 0, SketchSubElement::StartPoint);
        inferOriginFix(1, 0, SketchSubElement::EndPoint);
        // Chaining: the end becomes the next line's start. The reference for it
        // is not known until the line has been APPLIED and has an id, which is
        // what afterApply() supplies.
        //
        // The carried-over snap is deliberately NOT marked as being at the
        // origin even when it is: this end has just been Fixed, and the next
        // segment reaches it through the coincidence afterApply() sets up. A
        // second Fix on the far side of that coincidence would be a redundant
        // constraint the user never asked for, which is precisely what 8.2
        // requires a sketch to be able to distinguish from a real conflict --
        // so it is better not to manufacture one.
        const Vec2 last = pending_[1];
        finish();
        pending_.push_back(last);
        pendingSnaps_.push_back(PendingSnap{});
        chainFromCreatedEntity_ = true;
        return edit;
    }
    case SketchTool::Rectangle: {
        const Vec2 a = pending_[0];
        const Vec2 b = pending_[1];
        if (std::abs(a.x - b.x) <= kSketchToleranceMm ||
            std::abs(a.y - b.y) <= kSketchToleranceMm) {
            pending_.pop_back();
            pendingSnaps_.pop_back();
            return edit;
        }
        edit.kind = SketchEditKind::AddRectangle;
        edit.points = pending_;
        edit.label = "Add rectangle";
        // Four ordinary lines plus constraints, NOT a rectangle topology
        // (roadmap 4.1). The constraints are what make it a rectangle, and they
        // are listed and deletable like any others.
        const auto whole = [](std::size_t index, SketchSubElement part) {
            return PendingRef{true, index, SketchElementRef{}, part};
        };
        for (std::size_t i = 0; i < 4; ++i) {
            PendingConstraint orientation;
            orientation.kind =
                (i % 2 == 0) ? SketchEditKind::AddHorizontal : SketchEditKind::AddVertical;
            orientation.a = whole(i, SketchSubElement::Whole);
            edit.autoConstraints.push_back(orientation);
        }
        for (std::size_t i = 0; i < 4; ++i) {
            PendingConstraint corner;
            corner.kind = SketchEditKind::AddCoincident;
            corner.a = whole(i, SketchSubElement::EndPoint);
            corner.b = whole((i + 1) % 4, SketchSubElement::StartPoint);
            edit.autoConstraints.push_back(corner);
        }
        // The two clicked corners, mapped to the sides that own them:
        // ApplySketchEdit lays the corners out as {a, (b.x,a.y), b, (a.x,b.y)}
        // and runs side i from corner i to corner i+1, so the FIRST click is
        // side 0's start and the SECOND is side 2's. Only these two are
        // eligible -- the other two corners are derived, and the user never put
        // a cursor on them.
        //
        // A corner dropped on an existing point earns a coincidence for the
        // same reason every other tool's snap does (4.2): a rectangle that
        // merely STARTS at another line's endpoint comes apart the first time
        // either is dragged, and the DOF readout counts a freedom the user
        // believes they have already spent.
        inferCoincidence(0, 0, SketchSubElement::StartPoint);
        inferCoincidence(1, 2, SketchSubElement::StartPoint);
        inferOriginFix(0, 0, SketchSubElement::StartPoint);
        inferOriginFix(1, 2, SketchSubElement::StartPoint);
        finish();
        return edit;
    }
    case SketchTool::Circle: {
        const double radius = Distance(pending_[0], pending_[1]);
        if (radius <= kSketchToleranceMm) {
            pending_.pop_back();
            pendingSnaps_.pop_back();
            return edit;
        }
        edit.kind = SketchEditKind::AddCircle;
        edit.points = pending_;
        edit.label = "Add circle";
        inferCoincidence(0, 0, SketchSubElement::CenterPoint);
        // The centre only. A circle has no start or end point, so the rim click
        // is a radius and not a point anything could be fixed at.
        inferOriginFix(0, 0, SketchSubElement::CenterPoint);
        finish();
        return edit;
    }
    case SketchTool::Arc: {
        const double radius = Distance(pending_[0], pending_[1]);
        if (radius <= kSketchToleranceMm) {
            pending_.pop_back();
            pendingSnaps_.pop_back();
            return edit;
        }
        edit.kind = SketchEditKind::AddArc;
        edit.points = pending_;
        edit.label = "Add arc";
        inferCoincidence(0, 0, SketchSubElement::CenterPoint);
        // The centre only, for the reason inferOriginFix documents: the second
        // click sets the radius and the third contributes only an angle, so
        // neither tip is guaranteed to sit where the cursor was.
        inferOriginFix(0, 0, SketchSubElement::CenterPoint);
        finish();
        return edit;
    }
    case SketchTool::CenterRectangle: {
        // Centre, then a corner. The rectangle it makes is EXACTLY the one the
        // corner tool makes -- four lines and the same constraints -- because
        // the centre is a way of placing it, not a property of it. Saying so
        // matters: a user who expects the centre to stay put while the
        // rectangle is resized is expecting a constraint that is not there,
        // and there is no centreline to hang one on yet.
        const Vec2 centre = pending_[0];
        const Vec2 corner = pending_[1];
        const double halfWidth = std::abs(corner.x - centre.x);
        const double halfHeight = std::abs(corner.y - centre.y);
        if (halfWidth <= kSketchToleranceMm || halfHeight <= kSketchToleranceMm) {
            pending_.pop_back();
            pendingSnaps_.pop_back();
            return edit;
        }
        edit.kind = SketchEditKind::AddCenterRectangle;
        edit.points = pending_;
        edit.label = "Add centre rectangle";
        const auto whole = [](std::size_t index, SketchSubElement part) {
            return PendingRef{true, index, SketchElementRef{}, part};
        };
        for (std::size_t i = 0; i < 4; ++i) {
            PendingConstraint orientation;
            orientation.kind =
                (i % 2 == 0) ? SketchEditKind::AddHorizontal : SketchEditKind::AddVertical;
            orientation.a = whole(i, SketchSubElement::Whole);
            edit.autoConstraints.push_back(orientation);
        }
        for (std::size_t i = 0; i < 4; ++i) {
            PendingConstraint corner4;
            corner4.kind = SketchEditKind::AddCoincident;
            corner4.a = whole(i, SketchSubElement::EndPoint);
            corner4.b = whole((i + 1) % 4, SketchSubElement::StartPoint);
            edit.autoConstraints.push_back(corner4);
        }
        // NO snap inference on either click: neither is a corner of the result.
        // The first is the centre, which no entity occupies, and the second is
        // opposite a derived corner -- inferring a coincidence for a point the
        // user did not put a cursor on is the guess section 26 forbids.
        finish();
        return edit;
    }
    case SketchTool::ThreePointCircle: {
        // Three points on the rim. Degenerate when they are collinear, which
        // is what a user gets by clicking along an edge -- refused by dropping
        // the last click so the next one can finish the circle, rather than
        // making a circle of absurd radius.
        const Vec2 a = pending_[0];
        const Vec2 b = pending_[1];
        const Vec2 c = pending_[2];
        const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) <= kSketchToleranceMm) {
            pending_.pop_back();
            pendingSnaps_.pop_back();
            return edit;
        }
        edit.kind = SketchEditKind::AddThreePointCircle;
        edit.points = pending_;
        edit.label = "Add circle through 3 points";
        // Each clicked point that landed on existing geometry earns a
        // PointOnObject, which is what makes this a circle THROUGH those
        // points rather than one that merely passes near them today (4.2).
        for (std::size_t i = 0; i < 3; ++i) {
            if (pendingSnaps_[i].ref.entityId == kInvalidSketchEntityId) continue;
            PendingConstraint through;
            through.kind = SketchEditKind::AddPointOnObject;
            through.a = PendingRef{false, 0, pendingSnaps_[i].ref, pendingSnaps_[i].ref.subElement};
            through.b = PendingRef{true, 0, SketchElementRef{}, SketchSubElement::Whole};
            edit.autoConstraints.push_back(through);
        }
        finish();
        return edit;
    }
    case SketchTool::ThreePointArc: {
        // Both tips first, then a point the arc must pass through -- the order
        // Onshape uses, and the one that lets the first two clicks snap to the
        // geometry the arc is joining.
        const Vec2 start = pending_[0];
        const Vec2 end = pending_[1];
        const Vec2 through = pending_[2];
        const double area =
            (end.x - start.x) * (through.y - start.y) - (end.y - start.y) * (through.x - start.x);
        if (Distance(start, end) <= kSketchToleranceMm ||
            std::abs(area) <= kSketchToleranceMm) {
            pending_.pop_back();
            pendingSnaps_.pop_back();
            return edit;
        }
        edit.kind = SketchEditKind::AddThreePointArc;
        edit.points = pending_;
        edit.label = "Add arc through 3 points";
        // The TIPS earn coincidences, unlike the centre-first arc where the
        // tips are wherever the radius put them. Here the user placed them.
        inferCoincidence(0, 0, SketchSubElement::StartPoint);
        inferCoincidence(1, 0, SketchSubElement::EndPoint);
        inferOriginFix(0, 0, SketchSubElement::StartPoint);
        inferOriginFix(1, 0, SketchSubElement::EndPoint);
        finish();
        return edit;
    }
    case SketchTool::TangentArc: {
        // TWO CLICKS, and the shape is then forced: an arc that starts at the
        // given end, leaves it in the direction the existing curve was going,
        // and passes through the second click. There is exactly one such arc,
        // so there is no third click to make and nothing for the tool to guess.
        //
        // The GEOMETRY is worked out in ApplySketchEdit: it needs the direction
        // the existing curve leaves that point in, and this function has the
        // clicks but not the sketch.
        if (Distance(pending_[0], pending_[1]) <= kSketchToleranceMm) {
            pending_.pop_back();
            pendingSnaps_.pop_back();
            return edit;
        }
        edit.kind = SketchEditKind::AddTangentArc;
        edit.points = pending_;
        edit.label = "Add tangent arc";

        // ONLY AN ENDPOINT WILL DO. A click in open space, or on the middle of
        // a curve, leaves this invalid -- and ApplySketchEdit says so rather
        // than drawing a plain arc the user did not ask for.
        const SketchElementRef& from = pendingSnaps_[0].ref;
        if (from.subElement == SketchSubElement::StartPoint ||
            from.subElement == SketchSubElement::EndPoint)
            edit.tangentFrom = from;

        // The joint is NOT inferred here: ApplySketchEdit builds it, next to
        // the tangency, because the two are one idea and splitting them across
        // two mechanisms is how one of them gets forgotten.
        //
        // The far end IS an ordinary click on an ordinary tip, so it earns what
        // any other click on a tip earns.
        inferCoincidence(1, 0, SketchSubElement::EndPoint);
        inferOriginFix(1, 0, SketchSubElement::EndPoint);

        // Chaining, like the line tool: tangent arcs are drawn in runs.
        const Vec2 last = pending_[1];
        finish();
        pending_.push_back(last);
        pendingSnaps_.push_back(PendingSnap{});
        chainFromCreatedEntity_ = true;
        return edit;
    }
    case SketchTool::Ellipse:
    case SketchTool::EllipticalArc: {
        // CENTRE, THE LONG AXIS, THEN THE WIDTH -- and for the open one, where
        // to stop.
        //
        // The second click gives BOTH the major radius and the rotation at
        // once, which is why it is the long axis rather than the short one: an
        // ellipse's rotation is measured to its major axis, so pointing at the
        // major end says the two things that have to agree in a single gesture.
        //
        // The third click's PERPENDICULAR distance to that axis is the minor
        // radius -- the same rule the slot tool uses, and for the same reason:
        // sliding along the axis must not change the width.
        const Vec2 centre = pending_[0];
        const double du = pending_[1].x - centre.x;
        const double dv = pending_[1].y - centre.y;
        const double major = std::sqrt(du * du + dv * dv);
        if (major <= kSketchToleranceMm) {
            pending_.pop_back();
            pendingSnaps_.pop_back();
            return edit;
        }
        const double across =
            std::abs((pending_[2].x - centre.x) * dv - (pending_[2].y - centre.y) * du) / major;
        // MAJOR MUST STAY THE LONGER ONE (see SketchEllipse). A width click
        // past the major end is refused by dropping it, so the next click can
        // finish the ellipse -- the same way a degenerate circle click is
        // handled.
        if (across <= kSketchToleranceMm || across > major) {
            pending_.pop_back();
            pendingSnaps_.pop_back();
            return edit;
        }
        edit.kind = tool_ == SketchTool::Ellipse ? SketchEditKind::AddEllipse
                                                 : SketchEditKind::AddEllipticalArc;
        edit.points = pending_;
        edit.label = tool_ == SketchTool::Ellipse ? "Add ellipse" : "Add elliptical arc";
        // The CENTRE is the one click that lands exactly where the user put it;
        // the other two are a radius and a perpendicular distance, and a
        // coincidence on either would pin a point the cursor was never on.
        inferCoincidence(0, 0, SketchSubElement::CenterPoint);
        inferOriginFix(0, 0, SketchSubElement::CenterPoint);
        finish();
        return edit;
    }
    case SketchTool::Polygon: {
        const double radius = Distance(pending_[0], pending_[1]);
        if (radius <= kSketchToleranceMm) {
            pending_.pop_back();
            pendingSnaps_.pop_back();
            return edit;
        }
        edit.kind = SketchEditKind::AddPolygon;
        edit.points = pending_;
        edit.polygonSides = polygonSides_;
        edit.label = "Add polygon";
        // The construction circle is entity 0 and the sides follow it, so the
        // indexes below match what ApplySketchEdit creates.
        //
        // WHAT MAKES IT REGULAR, and it is worth counting: n sides carry 4n
        // degrees of freedom and the circle 3, so 4n + 3 in all. The corner
        // coincidences take 2n, one vertex per corner on the circle takes n,
        // and n-1 equal sides take n-1 -- 4n-1 in total, leaving exactly 4:
        // the centre, the radius and the rotation. That is a regular polygon
        // and nothing more, which is what the DOF readout will say.
        const auto side = [](std::size_t i, SketchSubElement part) {
            return PendingRef{true, i + 1, SketchElementRef{}, part};
        };
        const auto circle = []() {
            return PendingRef{true, 0, SketchElementRef{}, SketchSubElement::Whole};
        };
        const std::size_t sides = static_cast<std::size_t>(polygonSides_);
        for (std::size_t i = 0; i < sides; ++i) {
            PendingConstraint corner;
            corner.kind = SketchEditKind::AddCoincident;
            corner.a = side(i, SketchSubElement::EndPoint);
            corner.b = side((i + 1) % sides, SketchSubElement::StartPoint);
            edit.autoConstraints.push_back(corner);

            PendingConstraint onCircle;
            onCircle.kind = SketchEditKind::AddPointOnObject;
            onCircle.a = side(i, SketchSubElement::StartPoint);
            onCircle.b = circle();
            edit.autoConstraints.push_back(onCircle);

            if (i + 1 < sides) {
                PendingConstraint equal;
                equal.kind = SketchEditKind::AddEqual;
                equal.a = side(i, SketchSubElement::Whole);
                equal.b = side(i + 1, SketchSubElement::Whole);
                edit.autoConstraints.push_back(equal);
            }
        }
        inferCoincidence(0, 0, SketchSubElement::CenterPoint);
        inferOriginFix(0, 0, SketchSubElement::CenterPoint);
        finish();
        return edit;
    }
    case SketchTool::Slot: {
        // Two centres, then the width. The third click sets the radius from
        // its PERPENDICULAR distance to the centre line, so sliding along the
        // slot does not change the width -- which is what a user dragging to
        // set a width expects, and what using the plain distance would get
        // wrong the moment they drifted sideways.
        const Vec2 a = pending_[0];
        const Vec2 b = pending_[1];
        const Vec2 across = pending_[2];
        const double du = b.x - a.x;
        const double dv = b.y - a.y;
        const double span = std::sqrt(du * du + dv * dv);
        if (span <= kSketchToleranceMm) {
            pending_.pop_back();
            pendingSnaps_.pop_back();
            return edit;
        }
        const double radius =
            std::abs((across.x - a.x) * dv - (across.y - a.y) * du) / span;
        if (radius <= kSketchToleranceMm) {
            pending_.pop_back();
            pendingSnaps_.pop_back();
            return edit;
        }

        edit.kind = SketchEditKind::AddSlot;
        edit.points = pending_;
        edit.label = "Add slot";

        // FOUR ENTITIES in this order: line, end arc, line, end arc -- walking
        // once round the outline. ApplySketchEdit lays them out the same way,
        // and the indexes below are that order.
        const auto part = [](std::size_t index, SketchSubElement which) {
            return PendingRef{true, index, SketchElementRef{}, which};
        };
        const auto whole = [](std::size_t index) {
            return PendingRef{true, index, SketchElementRef{}, SketchSubElement::Whole};
        };

        // WHAT MAKES IT A SLOT, and it is worth counting: two lines carry 4
        // degrees of freedom each and two arcs 5 each, so 18 in all. A slot
        // has 5 -- both centres and the radius -- so the constraints have to
        // take 13. Four corner coincidences take 8, one equal radius takes 1,
        // and four tangencies take 4. Thirteen exactly, which is what the DOF
        // readout will say if this is right and will not if it is not.
        //
        // The first version of this tool got 9, not 5, because the tangencies
        // took NOTHING: asked as "the centre is r from the line" at a point a
        // coincidence had already pinned, the residual sits at a maximum and
        // its gradient vanishes. The arithmetic above was right and the
        // sketch still kinked. It is the equation that changed, not the
        // count -- see SolveResidual::Kind::TangentAtPoint.
        struct Corner {
            std::size_t from;
            SketchSubElement fromPart;
            std::size_t to;
            SketchSubElement toPart;
        };
        static const Corner kCorners[4] = {
            {0, SketchSubElement::EndPoint, 1, SketchSubElement::EndPoint},
            {1, SketchSubElement::StartPoint, 2, SketchSubElement::StartPoint},
            {2, SketchSubElement::EndPoint, 3, SketchSubElement::EndPoint},
            {3, SketchSubElement::StartPoint, 0, SketchSubElement::StartPoint},
        };
        for (const Corner& corner : kCorners) {
            PendingConstraint join;
            join.kind = SketchEditKind::AddCoincident;
            join.a = part(corner.from, corner.fromPart);
            join.b = part(corner.to, corner.toPart);
            edit.autoConstraints.push_back(join);
        }

        PendingConstraint sameRadius;
        sameRadius.kind = SketchEditKind::AddEqual;
        sameRadius.a = whole(1);
        sameRadius.b = whole(3);
        edit.autoConstraints.push_back(sameRadius);

        // DERIVED FROM THE CORNERS, not listed a second time. A tangency has
        // to be AT the corner its coincidence made -- a tangency somewhere
        // else on the line is the equation that holds nothing (see
        // TangentConstraint::at) -- so the two lists would have to agree about
        // all four corners forever. Deriving one from the other is the only
        // way they cannot drift.
        //
        // Even indexes are the sides, odd ones the ends, which is the order
        // ApplySketchEdit builds them in.
        for (const Corner& corner : kCorners) {
            const bool fromIsLine = corner.from % 2 == 0;
            PendingConstraint smooth;
            smooth.kind = SketchEditKind::AddTangent;
            // THE TOUCHING END FIRST: the constraint records which end of the
            // LINE the arc meets, and that is what makes it hold.
            smooth.a = part(fromIsLine ? corner.from : corner.to,
                            fromIsLine ? corner.fromPart : corner.toPart);
            smooth.b = whole(fromIsLine ? corner.to : corner.from);
            edit.autoConstraints.push_back(smooth);
        }

        // The two CENTRES are what the user pointed at, so they are what earns
        // a snap -- but neither is an entity of the slot, so there is nothing
        // for a coincidence to attach to. Only the origin fix applies, and only
        // through the arcs that sit on those centres.
        inferOriginFix(0, 1, SketchSubElement::CenterPoint);
        inferOriginFix(1, 3, SketchSubElement::CenterPoint);
        finish();
        return edit;
    }
    case SketchTool::Select:
        break;
    }
    return edit;
}

void SketchCanvasModel::afterApply(const std::vector<SketchEntityId>& createdEntities) {
    if (!chainFromCreatedEntity_) return;
    chainFromCreatedEntity_ = false;
    if (createdEntities.empty() || pendingSnaps_.empty()) return;
    // The chain's next start point is the previous line's END, by identity and
    // not by coordinate: the next segment gets a real Coincident constraint, so
    // dragging the corner later moves both segments. A chain that only agreed
    // numerically would come apart on the first solve.
    pendingSnaps_[0] =
        PendingSnap{SketchElementRef{createdEntities.front(), SketchSubElement::EndPoint}, false};
}

SketchEdit SketchCanvasModel::requestConstraint(const Sketch& sketch, SketchEditKind kind,
                                                std::string* whyNot) const {
    SketchEdit edit;
    const auto refuse = [&](std::string reason) {
        if (whyNot != nullptr) *whyNot = std::move(reason);
        return SketchEdit{};
    };
    if (selection_.empty()) return refuse("Select something first.");

    switch (kind) {
    case SketchEditKind::AddCoincident: {
        if (selection_.size() != 2)
            return refuse("Coincident needs exactly 2 points; " +
                          std::to_string(selection_.size()) + " selected.");
        for (const SketchElementRef& ref : selection_)
            if (!IsPointRef(sketch, ref))
                return refuse(DescribeElementRef(sketch, ref) +
                              " is not a point. Coincident joins two points.");
        if (SameRef(selection_[0], selection_[1]))
            return refuse("Both selections are the same point.");
        edit.kind = kind;
        edit.refs = selection_;
        edit.label = "Add coincident";
        return edit;
    }
    case SketchEditKind::AddHorizontal:
    case SketchEditKind::AddVertical: {
        for (const SketchElementRef& ref : selection_)
            if (!IsLineRef(sketch, ref))
                return refuse(DescribeElementRef(sketch, ref) + " is not a line. " +
                              SketchEditKindName(kind) + " applies to lines.");
        edit.kind = kind;
        // Normalised to Whole: the constraint takes an ENTITY, so selecting a
        // line by one of its endpoints must not produce two different
        // constraints for what the user sees as one line.
        for (const SketchElementRef& ref : selection_)
            edit.refs.push_back(SketchElementRef{ref.entityId, SketchSubElement::Whole});
        edit.label = std::string("Add ") + SketchEditKindName(kind);
        return edit;
    }
    case SketchEditKind::AddFix: {
        for (const SketchElementRef& ref : selection_)
            if (!IsPointRef(sketch, ref))
                return refuse(DescribeElementRef(sketch, ref) +
                              " is not a point. Fix pins a point where it is.");
        edit.kind = kind;
        edit.refs = selection_;
        edit.label = "Add fix";
        return edit;
    }
    case SketchEditKind::AddSymmetric: {
        // THREE: the two points and the mirror. Order-independent, because
        // roadmap 13.1 makes selection order the user's business -- the line is
        // picked out by being the line.
        if (selection_.size() != 3)
            return refuse("Symmetric needs 2 points and the line to mirror them across; " +
                          std::to_string(selection_.size()) + " selected.");
        std::vector<SketchElementRef> points;
        SketchElementRef mirror{};
        int mirrors = 0;
        for (const SketchElementRef& ref : selection_) {
            if (IsPointRef(sketch, ref)) {
                points.push_back(ref);
            } else if (IsLineRef(sketch, ref)) {
                mirror = ref;
                ++mirrors;
            } else {
                return refuse(DescribeElementRef(sketch, ref) +
                              " is neither a point nor the line to mirror across.");
            }
        }
        if (points.size() != 2 || mirrors != 1)
            return refuse("Symmetric needs exactly 2 points and 1 line.");
        if (SameRef(points[0], points[1]))
            return refuse("Both points are the same point.");
        if (points[0].entityId == mirror.entityId || points[1].entityId == mirror.entityId)
            return refuse("A point cannot be mirrored across the line it belongs to.");
        edit.kind = kind;
        // POINTS FIRST, then the mirror -- so ApplySketchEdit reads one order
        // whatever order the clicks came in.
        edit.refs = {points[0], points[1],
                     SketchElementRef{mirror.entityId, SketchSubElement::Whole}};
        edit.label = "Add symmetric";
        return edit;
    }

    // --- M13 ---------------------------------------------------------------

    case SketchEditKind::AddParallel:
    case SketchEditKind::AddPerpendicular: {
        if (selection_.size() != 2)
            return refuse(std::string(SketchEditKindName(kind)) + " needs exactly 2 lines; " +
                          std::to_string(selection_.size()) + " selected.");
        for (const SketchElementRef& ref : selection_)
            if (!IsLineRef(sketch, ref))
                return refuse(DescribeElementRef(sketch, ref) + " is not a line. " +
                              SketchEditKindName(kind) + " relates two lines.");
        if (selection_[0].entityId == selection_[1].entityId)
            return refuse("Both selections are the same line.");
        edit.kind = kind;
        for (const SketchElementRef& ref : selection_)
            edit.refs.push_back(SketchElementRef{ref.entityId, SketchSubElement::Whole});
        edit.label = std::string("Add ") + SketchEditKindName(kind);
        return edit;
    }

    case SketchEditKind::AddEqual: {
        if (selection_.size() != 2)
            return refuse("Equal needs exactly 2 entities; " +
                          std::to_string(selection_.size()) + " selected.");
        if (selection_[0].entityId == selection_[1].entityId)
            return refuse("Both selections are the same entity.");
        const bool bothLines =
            IsLineRef(sketch, selection_[0]) && IsLineRef(sketch, selection_[1]);
        const bool bothCurves =
            IsCurveRef(sketch, selection_[0]) && IsCurveRef(sketch, selection_[1]);
        if (!bothLines && !bothCurves)
            return refuse("Equal needs two lines (equal length) or two circles/arcs "
                          "(equal radius), not one of each.");
        edit.kind = kind;
        for (const SketchElementRef& ref : selection_)
            edit.refs.push_back(SketchElementRef{ref.entityId, SketchSubElement::Whole});
        edit.label = "Add equal";
        return edit;
    }

    case SketchEditKind::AddConcentric: {
        if (selection_.size() != 2)
            return refuse("Concentric needs exactly 2 curves; " +
                          std::to_string(selection_.size()) + " selected.");
        // NAME THE COMMAND THEY WANTED. "Not a circle or an arc" is true and
        // useless; two selected points is overwhelmingly someone trying to join
        // them, and Coincident is the command that does it.
        std::size_t selectedPoints = 0;
        for (const SketchElementRef& ref : selection_)
            if (IsPointRef(sketch, ref)) ++selectedPoints;
        if (selectedPoints == 2)
            return refuse("Concentric joins two curves by their centres. To join two POINTS, "
                          "use Coincident.");
        for (const SketchElementRef& ref : selection_)
            if (!IsCentredRef(sketch, ref))
                return refuse(DescribeElementRef(sketch, ref) +
                              " has no centre. To put a POINT at a curve's centre, select the "
                              "point and the centre and use Coincident.");
        if (selection_[0].entityId == selection_[1].entityId)
            return refuse("Both selections are the same curve.");
        edit.kind = kind;
        for (const SketchElementRef& ref : selection_)
            edit.refs.push_back(SketchElementRef{ref.entityId, SketchSubElement::Whole});
        edit.label = "Add concentric";
        return edit;
    }

    case SketchEditKind::AddMidpoint:
    case SketchEditKind::AddPointOnObject: {
        const bool midpoint = kind == SketchEditKind::AddMidpoint;
        if (selection_.size() != 2)
            return refuse(std::string(SketchEditKindName(kind)) +
                          " needs a point and something to put it on; " +
                          std::to_string(selection_.size()) + " selected.");

        // Order-independent: the user should not have to remember which to
        // click first. Exactly one of the two must read as a point and the
        // other as the thing it goes on -- if both could be either, the
        // selection is ambiguous and is refused rather than guessed.
        const auto isHost = [&](const SketchElementRef& ref) {
            if (ref.subElement != SketchSubElement::Whole) return false;
            return midpoint ? IsLineRef(sketch, ref)
                            : (IsLineRef(sketch, ref) || IsCurveRef(sketch, ref));
        };
        int pointIndex = -1;
        int hostIndex = -1;
        for (int i = 0; i < 2; ++i) {
            const int other = 1 - i;
            if (IsPointRef(sketch, selection_[static_cast<std::size_t>(i)]) &&
                isHost(selection_[static_cast<std::size_t>(other)])) {
                pointIndex = i;
                hostIndex = other;
                break;
            }
        }
        if (pointIndex < 0) {
            return refuse(midpoint
                              ? "Midpoint needs one point and one line."
                              : "Point-on-object needs one point and one line, circle or arc.");
        }
        const SketchElementRef& point = selection_[static_cast<std::size_t>(pointIndex)];
        const SketchElementRef& host = selection_[static_cast<std::size_t>(hostIndex)];
        if (point.entityId == host.entityId)
            return refuse(midpoint ? "That point belongs to the line itself."
                                   : "That point is already part of that entity.");
        edit.kind = kind;
        // Order is FIXED here -- point first, host second -- so ApplySketchEdit
        // never has to re-derive which is which.
        edit.refs.push_back(point);
        edit.refs.push_back(SketchElementRef{host.entityId, SketchSubElement::Whole});
        edit.label = midpoint ? "Add midpoint" : "Add point-on-object";
        return edit;
    }

    case SketchEditKind::AddTangent: {
        if (selection_.size() != 2)
            return refuse("Tangent needs exactly 2 entities; " +
                          std::to_string(selection_.size()) + " selected.");
        if (selection_[0].entityId == selection_[1].entityId)
            return refuse("Both selections are the same entity.");
        const bool curve0 = IsCurveRef(sketch, selection_[0]);
        const bool curve1 = IsCurveRef(sketch, selection_[1]);
        const bool line0 = IsLineRef(sketch, selection_[0]);
        const bool line1 = IsLineRef(sketch, selection_[1]);
        const bool spline0 = IsSplineRef(sketch, selection_[0]);
        const bool spline1 = IsSplineRef(sketch, selection_[1]);
        const bool oval0 = IsEllipseRef(sketch, selection_[0]);
        const bool oval1 = IsEllipseRef(sketch, selection_[1]);
        // AN ELLIPSE, but only against a line -- the closed form has no contact
        // point in it, so that is the one pair this can hold. Refused here
        // rather than in the solve session so the reason arrives naming what
        // was clicked.
        if ((oval0 && !line1) || (oval1 && !line0)) {
            if (oval0 || oval1)
                return refuse("An ellipse can be tangent to a line so far. Against a circle, "
                              "an arc or another ellipse the touch point has to be solved for.");
        }
        if (!((curve0 && curve1) || (line0 && curve1) || (curve0 && line1) || spline0 || spline1 ||
              (oval0 && line1) || (oval1 && line0)))
            return refuse("Tangent needs a line and a curve, two curves, a spline's end, or a "
                          "line and an ellipse.");
        // A SPLINE HAS A DIFFERENT TANGENT AT EVERY POINT, so the only tangency
        // it can be part of is one at a named end -- and an end only counts as
        // named when a coincidence has pinned it (below). A closed spline has no
        // ends at all, so it is refused here rather than at solve time, where
        // the reason would arrive as a red sketch instead of a sentence.
        const auto openSpline = [&](const SketchElementRef& ref) {
            const SketchEntity* entity = sketch.findEntity(ref.entityId);
            const auto* spline =
                entity == nullptr ? nullptr : std::get_if<SketchSpline>(&entity->geometry);
            return spline != nullptr && !spline->closed && spline->points.size() >= 2;
        };
        if ((spline0 && !openSpline(selection_[0])) || (spline1 && !openSpline(selection_[1])))
            return refuse("A closed spline has no ends, so it has no one tangent direction.");

        edit.kind = kind;
        // THE SPLINE FIRST when there is one, otherwise THE LINE FIRST, so
        // nothing downstream has to re-derive which of the two owns the touch
        // point. The spline outranks the line because a line is tangent
        // anywhere along itself while a spline is only tangent at an end --
        // the more constrained of the two has to be the one that names it.
        // A SPLINE OUTRANKS EVERYTHING, then a line. Written as one condition
        // per rank rather than a chain of ternaries: the chain read
        // `spline1 ? !spline0 : (!line0 && line1)`, which put the LINE first
        // whenever the spline was selected first -- and the solve session then
        // refused the constraint the user had just made, naming a rule they
        // had followed.
        const bool eitherIsSpline = spline0 || spline1;
        const bool leadIsSecond = eitherIsSpline ? (spline1 && !spline0) : (!line0 && line1);
        for (const std::size_t index : {leadIsSecond ? std::size_t{1} : std::size_t{0},
                                        leadIsSecond ? std::size_t{0} : std::size_t{1}})
            edit.refs.push_back(
                SketchElementRef{selection_[index].entityId, SketchSubElement::Whole});
        edit.label = "Add tangent";

        // DO THEY ALREADY MEET AT A KNOWN POINT? If a coincidence has pinned
        // one end of the leading entity onto the other, then that is where they
        // touch, and the distance form of tangency would hold nothing there
        // (see TangentConstraint::at). So the corner is looked up ONCE, now, and
        // stored -- read from the CONSTRAINTS, not from which end happens to
        // sit nearest the other today, which a drag could change.
        if (!(curve0 && curve1)) {
            const SketchEntityId leadId = edit.refs[0].entityId;
            const SketchEntityId otherId = edit.refs[1].entityId;
            for (const SketchConstraint& constraint : sketch.constraints()) {
                const auto* joint = std::get_if<CoincidentConstraint>(&constraint.data);
                if (joint == nullptr) continue;
                const auto touchEnd = [&](const SketchElementRef& onLead,
                                          const SketchElementRef& onOther) {
                    if (onLead.entityId != leadId || onOther.entityId != otherId) return false;
                    return onLead.subElement == SketchSubElement::StartPoint ||
                           onLead.subElement == SketchSubElement::EndPoint;
                };
                if (touchEnd(joint->a, joint->b)) {
                    edit.refs[0].subElement = joint->a.subElement;
                    break;
                }
                if (touchEnd(joint->b, joint->a)) {
                    edit.refs[0].subElement = joint->b.subElement;
                    break;
                }
            }
            // A SPLINE THAT STILL DOES NOT KNOW WHICH END is refused here,
            // with the thing to do about it. Left to the solver it would come
            // back as "say which end", which is true but does not say that the
            // way to say it is to join the two first.
            if (edit.refs[0].subElement == SketchSubElement::Whole &&
                (spline0 || spline1) && IsSplineRef(sketch, edit.refs[0]))
                return refuse("Join the spline's end to the other entity first -- tangency "
                              "needs to know which end touches.");
        }

        if (curve0 && curve1) {
            // WHICH tangency the user meant, decided ONCE from where the curves
            // are now: two circles that currently overlap or nest were asked to
            // touch from the inside; two that are apart, from the outside.
            // Storing the answer is what stops a later drag from redefining it.
            Vec2 centreA{};
            Vec2 centreB{};
            double radiusA = 0.0;
            double radiusB = 0.0;
            const bool okA = CurveCentreAndRadius(sketch, selection_[0].entityId, &centreA,
                                                  &radiusA);
            const bool okB = CurveCentreAndRadius(sketch, selection_[1].entityId, &centreB,
                                                  &radiusB);
            if (!okA || !okB) return refuse("Could not read those curves.");
            const double centres = Distance(centreA, centreB);
            const double outer = std::abs(centres - (radiusA + radiusB));
            const double inner = std::abs(centres - std::abs(radiusA - radiusB));
            edit.tangentInternal = inner < outer;
        }
        return edit;
    }

    default:
        break;
    }
    return refuse(std::string(SketchEditKindName(kind)) + " is not a constraint command.");
}

bool SketchCanvasModel::IsConstraintCommand(SketchEditKind kind) noexcept {
    switch (kind) {
    case SketchEditKind::AddCoincident:
    case SketchEditKind::AddHorizontal:
    case SketchEditKind::AddVertical:
    case SketchEditKind::AddFix:
    case SketchEditKind::AddParallel:
    case SketchEditKind::AddPerpendicular:
    case SketchEditKind::AddEqual:
    case SketchEditKind::AddConcentric:
    case SketchEditKind::AddMidpoint:
    case SketchEditKind::AddPointOnObject:
    case SketchEditKind::AddTangent:
    case SketchEditKind::AddSymmetric:
        return true;
    default:
        return false;
    }
}

SketchEdit SketchCanvasModel::requestDimension(const Sketch& sketch, SketchEditKind explicitKind,
                                               std::string* whyNot) const {
    SketchEdit edit;
    const auto refuse = [&](std::string reason) {
        if (whyNot != nullptr) *whyNot = std::move(reason);
        return SketchEdit{};
    };
    if (selection_.empty()) return refuse("Select geometry to dimension first.");

    // What the selection looks like, once.
    std::size_t lines = 0, curves = 0, points = 0, ellipses = 0;
    for (const SketchElementRef& ref : selection_) {
        if (IsPointRef(sketch, ref)) ++points;
        if (IsLineRef(sketch, ref) && ref.subElement == SketchSubElement::Whole) ++lines;
        if (IsCurveRef(sketch, ref)) ++curves;
        if (IsEllipseRef(sketch, ref)) ++ellipses;
    }

    SketchEditKind kind = explicitKind;
    if (kind == SketchEditKind::None) {
        // Roadmap 7.1's selection -> dimension table, restricted to the five
        // dimensional constraints Core has. A selection outside the table is
        // REFUSED WITH THE TABLE, not silently ignored.
        if (selection_.size() == 1 && lines == 1) kind = SketchEditKind::AddLength;
        else if (selection_.size() == 1 && curves == 1) {
            const SketchEntity* entity = EntityOf(sketch, selection_.front());
            kind = (entity != nullptr && std::holds_alternative<SketchCircle>(entity->geometry))
                       ? SketchEditKind::AddDiameter
                       : SketchEditKind::AddRadius;
        } else if (selection_.size() == 1 && ellipses == 1) {
            // AN ELLIPSE HAS TWO, so the bare Dimension command picks the
            // MAJOR one and says so. It cannot guess -- but refusing outright
            // would make the general command useless on an ellipse, and the
            // minor axis is one explicit command away.
            kind = SketchEditKind::AddMajorAxis;
        } else if (selection_.size() == 2 && points == 2) kind = SketchEditKind::AddDistance;
        else if (selection_.size() == 2 && lines == 2) kind = SketchEditKind::AddAngle;
        else
            return refuse("Cannot tell which dimension this selection means. "
                          "One line = length, one circle = diameter, one arc = radius, "
                          "one ellipse = major axis, two points = distance, "
                          "two lines = angle.");
    }

    switch (kind) {
    case SketchEditKind::AddLength:
        if (!(selection_.size() == 1 && IsLineRef(sketch, selection_.front())))
            return refuse("Length needs exactly one line.");
        break;
    case SketchEditKind::AddRadius:
    case SketchEditKind::AddDiameter:
        if (!(selection_.size() == 1 && IsCurveRef(sketch, selection_.front())))
            return refuse(std::string(SketchEditKindName(kind)) +
                          " needs exactly one circle or arc.");
        break;
    case SketchEditKind::AddDistance:
    case SketchEditKind::AddHorizontalDistance:
    case SketchEditKind::AddVerticalDistance:
        if (!(selection_.size() == 2 && points == 2))
            return refuse(std::string(SketchEditKindName(kind)) + " needs exactly 2 points.");
        break;
    case SketchEditKind::AddMajorAxis:
    case SketchEditKind::AddMinorAxis:
    case SketchEditKind::AddEllipseRotation:
        if (!(selection_.size() == 1 && ellipses == 1))
            return refuse(std::string(SketchEditKindName(kind)) + " needs exactly one ellipse.");
        break;
    case SketchEditKind::AddAngle:
        if (!(selection_.size() == 2 && lines == 2))
            return refuse("Angle needs exactly 2 lines.");
        break;
    case SketchEditKind::AddPointLineDistance:
        if (!(selection_.size() == 2 && points == 1 && lines == 1))
            return refuse("Distance to a line needs one point and one line.");
        break;
    default:
        return refuse(std::string(SketchEditKindName(kind)) + " is not a dimension.");
    }

    edit.kind = kind;
    if (kind == SketchEditKind::AddDistance) {
        edit.refs = selection_;
    } else if (kind == SketchEditKind::AddPointLineDistance) {
        // POINT FIRST, whichever order the user clicked in. Roadmap 13.1 makes
        // selection order the user's business, not the command's, so a command
        // that only worked one way round would be a rule nobody was told.
        const bool pointFirst = IsPointRef(sketch, selection_[0]);
        edit.refs = {selection_[pointFirst ? 0 : 1],
                     SketchElementRef{selection_[pointFirst ? 1 : 0].entityId,
                                      SketchSubElement::Whole}};
    } else if (kind == SketchEditKind::AddHorizontalDistance ||
               kind == SketchEditKind::AddVerticalDistance) {
        // ORDERED so the value comes out positive.
        //
        // These two are signed (see HorizontalDistanceConstraint), so which
        // point is `a` decides the sign. Selection order is whatever order the
        // user happened to click in, and a dimension that reads "-40" because
        // of that is a dimension nobody trusts. Ordering here -- once, at
        // creation -- leaves the constraint itself honestly signed.
        edit.refs = selection_;
        bool okA = false;
        bool okB = false;
        const Vec2 a = ResolveElementPoint(sketch, edit.refs[0], &okA);
        const Vec2 b = ResolveElementPoint(sketch, edit.refs[1], &okB);
        if (okA && okB) {
            const double delta = kind == SketchEditKind::AddHorizontalDistance ? b.x - a.x
                                                                              : b.y - a.y;
            if (delta < 0.0) std::swap(edit.refs[0], edit.refs[1]);
        }
    } else {
        for (const SketchElementRef& ref : selection_)
            edit.refs.push_back(SketchElementRef{ref.entityId, SketchSubElement::Whole});
    }
    edit.label = std::string("Add ") + SketchEditKindName(kind) + " dimension";
    return edit;
}

SketchEdit SketchCanvasModel::requestDelete(const Sketch& sketch, std::string* whyNot) const {
    SketchEdit edit;
    if (selection_.empty()) {
        if (whyNot != nullptr) *whyNot = "Select geometry to delete first.";
        return edit;
    }
    // Whole entities: deleting "the start point of a line" is not a thing, and
    // the cascade (ADR-M5-009) is defined on entities.
    for (const SketchElementRef& ref : selection_) {
        if (EntityOf(sketch, ref) == nullptr) continue;
        const SketchElementRef whole{ref.entityId, SketchSubElement::Whole};
        const bool already =
            std::any_of(edit.refs.begin(), edit.refs.end(),
                        [&](const SketchElementRef& held) { return SameRef(held, whole); });
        if (!already) edit.refs.push_back(whole);
    }
    if (edit.refs.empty()) {
        if (whyNot != nullptr) *whyNot = "Nothing in the selection still exists.";
        return edit;
    }
    edit.kind = SketchEditKind::DeleteEntities;
    edit.label = "Delete sketch geometry";
    return edit;
}

} // namespace paramcad
