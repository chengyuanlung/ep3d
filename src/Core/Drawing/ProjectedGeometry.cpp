#include "Core/Drawing/ProjectedGeometry.h"

#include <algorithm>
#include <cmath>

namespace paramcad {

namespace {

void GrowTo(ProjectedExtent& extent, Vec2 point) {
    if (extent.empty) {
        extent.min = point;
        extent.max = point;
        extent.empty = false;
        return;
    }
    extent.min.x = std::min(extent.min.x, point.x);
    extent.min.y = std::min(extent.min.y, point.y);
    extent.max.x = std::max(extent.max.x, point.x);
    extent.max.y = std::max(extent.max.y, point.y);
}

} // namespace

std::string_view toString(ProjectedEdgeKind kind) noexcept {
    switch (kind) {
        case ProjectedEdgeKind::Sharp: return "Sharp";
        case ProjectedEdgeKind::Outline: return "Outline";
        case ProjectedEdgeKind::Smooth: return "Smooth";
    }
    return "Sharp";
}

std::string_view toString(ProjectedVisibility visibility) noexcept {
    return visibility == ProjectedVisibility::Visible ? "Visible" : "Hidden";
}

void GrowExtent(ProjectedExtent& extent, const ProjectedCurve& curve) {
    if (const auto* line = std::get_if<ProjectedLine>(&curve.shape)) {
        GrowTo(extent, line->a);
        GrowTo(extent, line->b);
        return;
    }
    if (const auto* arc = std::get_if<ProjectedArc>(&curve.shape)) {
        // THE ARC'S OWN EXTENT, not its bounding square. A quarter arc of a
        // 50 mm circle is 50 wide only if it happens to cross the axis, and
        // taking the whole circle's box would make every view claim more paper
        // than it uses -- which reads as "this does not fit" on a sheet where
        // it plainly does.
        //
        // Both ends always count; a cardinal point counts only when the sweep
        // actually reaches it.
        const auto at = [&](double angle) {
            return Vec2{arc->centre.x + arc->radius * std::cos(angle),
                        arc->centre.y + arc->radius * std::sin(angle)};
        };
        GrowTo(extent, at(arc->startAngle));
        GrowTo(extent, at(arc->endAngle));
        if (arc->isFullCircle) {
            GrowTo(extent, Vec2{arc->centre.x - arc->radius, arc->centre.y});
            GrowTo(extent, Vec2{arc->centre.x + arc->radius, arc->centre.y});
            GrowTo(extent, Vec2{arc->centre.x, arc->centre.y - arc->radius});
            GrowTo(extent, Vec2{arc->centre.x, arc->centre.y + arc->radius});
            return;
        }
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        // The sweep, measured counter-clockwise from the start, so "does it
        // pass this cardinal point" is one comparison rather than four cases.
        double sweep = arc->endAngle - arc->startAngle;
        while (sweep < 0.0) sweep += kTwoPi;
        for (int quarter = 0; quarter < 4; ++quarter) {
            const double cardinal = quarter * (kTwoPi / 4.0);
            double toIt = cardinal - arc->startAngle;
            while (toIt < 0.0) toIt += kTwoPi;
            while (toIt >= kTwoPi) toIt -= kTwoPi;
            if (toIt <= sweep) GrowTo(extent, at(cardinal));
        }
        return;
    }
    if (const auto* polyline = std::get_if<ProjectedPolyline>(&curve.shape))
        for (const Vec2 point : polyline->points) GrowTo(extent, point);
}

} // namespace paramcad
