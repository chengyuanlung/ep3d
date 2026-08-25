#include "Core/Drawing/ObjectSnap.h"

#include <algorithm>
#include <cmath>

namespace paramcad {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

Vec2 PointOnCircle(Vec2 centre, double radius, double angle) noexcept {
    return Vec2{centre.x + radius * std::cos(angle), centre.y + radius * std::sin(angle)};
}

double Distance(Vec2 a, Vec2 b) noexcept { return std::hypot(a.x - b.x, a.y - b.y); }

// The straight segments an entity presents to the modes that need pairs of
// points -- INTERSECTION, PERPENDICULAR on a line. Curves are handled
// analytically where it matters and by their flattening where it does not.
std::vector<std::pair<Vec2, Vec2>> SegmentsOf(const DrawShape& shape) {
    std::vector<std::pair<Vec2, Vec2>> segments;
    if (const auto* line = std::get_if<DrawLine>(&shape)) {
        segments.emplace_back(line->a, line->b);
        return segments;
    }
    if (const auto* polyline = std::get_if<DrawPolyline>(&shape)) {
        const std::vector<Vec2> points = FlattenShape(shape, 0.05);
        for (std::size_t i = 0; i + 1 < points.size(); ++i)
            segments.emplace_back(points[i], points[i + 1]);
        (void)polyline;
        return segments;
    }
    return segments;
}

} // namespace

std::string_view toString(SnapMode mode) noexcept {
    switch (mode) {
        case SnapMode::Endpoint: return "Endpoint";
        case SnapMode::Midpoint: return "Midpoint";
        case SnapMode::Centre: return "Centre";
        case SnapMode::Node: return "Node";
        case SnapMode::Quadrant: return "Quadrant";
        case SnapMode::Intersection: return "Intersection";
        case SnapMode::Perpendicular: return "Perpendicular";
        case SnapMode::Tangent: return "Tangent";
        case SnapMode::Nearest: return "Nearest";
    }
    return "Nearest";
}

const std::vector<SnapMode>& SnapModesByPriority() {
    // DERIVED FROM THE ENUM'S OWN ORDER, which IS the priority order -- see
    // the header. A second list written out here is the one that drifts, and
    // this project has already paid for that once with AllSketchIcons.
    static const std::vector<SnapMode> order = {
        SnapMode::Endpoint,      SnapMode::Midpoint, SnapMode::Centre,
        SnapMode::Node,          SnapMode::Quadrant, SnapMode::Intersection,
        SnapMode::Perpendicular, SnapMode::Tangent,  SnapMode::Nearest};
    return order;
}

bool SnapSettings::isOn(SnapMode mode) const noexcept {
    switch (mode) {
        case SnapMode::Endpoint: return endpoint;
        case SnapMode::Midpoint: return midpoint;
        case SnapMode::Centre: return centre;
        case SnapMode::Node: return node;
        case SnapMode::Quadrant: return quadrant;
        case SnapMode::Intersection: return intersection;
        case SnapMode::Perpendicular: return perpendicular;
        case SnapMode::Tangent: return tangent;
        case SnapMode::Nearest: return nearest;
    }
    return false;
}

SnapSettings SnapSettings::all() noexcept {
    SnapSettings settings;
    settings.endpoint = true;
    settings.midpoint = true;
    settings.centre = true;
    settings.node = true;
    settings.quadrant = true;
    settings.intersection = true;
    settings.perpendicular = true;
    settings.tangent = true;
    settings.nearest = true;
    return settings;
}

SnapSettings SnapSettings::none() noexcept {
    SnapSettings settings;
    settings.endpoint = false;
    settings.midpoint = false;
    settings.centre = false;
    settings.node = false;
    settings.quadrant = false;
    settings.intersection = false;
    settings.perpendicular = false;
    settings.tangent = false;
    settings.nearest = false;
    return settings;
}

std::vector<SnapCandidate> StaticSnapPointsOf(const DrawShape& shape) {
    std::vector<SnapCandidate> candidates;
    if (const auto* point = std::get_if<DrawPoint>(&shape)) {
        candidates.push_back(SnapCandidate{point->at, SnapMode::Node});
        return candidates;
    }
    if (const auto* line = std::get_if<DrawLine>(&shape)) {
        candidates.push_back(SnapCandidate{line->a, SnapMode::Endpoint});
        candidates.push_back(SnapCandidate{line->b, SnapMode::Endpoint});
        candidates.push_back(
            SnapCandidate{Vec2{(line->a.x + line->b.x) / 2.0, (line->a.y + line->b.y) / 2.0},
                          SnapMode::Midpoint});
        return candidates;
    }
    if (const auto* circle = std::get_if<DrawCircle>(&shape)) {
        candidates.push_back(SnapCandidate{circle->centre, SnapMode::Centre});
        // THE FOUR CARDINAL POINTS. Not "the point nearest the cursor on the
        // rim" -- that is NEAREST's job, and a quadrant that moved with the
        // cursor would be useless for the thing quadrants are for, which is
        // measuring a diameter.
        for (int quarter = 0; quarter < 4; ++quarter)
            candidates.push_back(
                SnapCandidate{PointOnCircle(circle->centre, circle->radius,
                                            quarter * (kTwoPi / 4.0)),
                              SnapMode::Quadrant});
        return candidates;
    }
    if (const auto* arc = std::get_if<DrawArc>(&shape)) {
        candidates.push_back(SnapCandidate{arc->centre, SnapMode::Centre});
        candidates.push_back(SnapCandidate{
            PointOnCircle(arc->centre, arc->radius, arc->startAngle), SnapMode::Endpoint});
        candidates.push_back(SnapCandidate{
            PointOnCircle(arc->centre, arc->radius, arc->endAngle), SnapMode::Endpoint});
        // MID-SWEEP, not the midpoint of the chord. The middle of an arc is a
        // point ON it, and the chord's midpoint is not on the arc at all.
        const double sweep = NormaliseAngle(arc->endAngle - arc->startAngle);
        candidates.push_back(SnapCandidate{
            PointOnCircle(arc->centre, arc->radius, arc->startAngle + sweep / 2.0),
            SnapMode::Midpoint});
        // ...and only the quadrants the sweep actually reaches.
        for (int quarter = 0; quarter < 4; ++quarter) {
            const double cardinal = quarter * (kTwoPi / 4.0);
            if (AngleWithinArc(cardinal, arc->startAngle, arc->endAngle))
                candidates.push_back(SnapCandidate{
                    PointOnCircle(arc->centre, arc->radius, cardinal), SnapMode::Quadrant});
        }
        return candidates;
    }
    if (const auto* ellipse = std::get_if<DrawEllipse>(&shape)) {
        candidates.push_back(SnapCandidate{ellipse->centre, SnapMode::Centre});
        const double c = std::cos(ellipse->rotation);
        const double s = std::sin(ellipse->rotation);
        for (const auto& axis : {std::pair{ellipse->majorRadius, 0.0},
                                 std::pair{-ellipse->majorRadius, 0.0},
                                 std::pair{0.0, ellipse->minorRadius},
                                 std::pair{0.0, -ellipse->minorRadius}})
            candidates.push_back(SnapCandidate{
                Vec2{ellipse->centre.x + axis.first * c - axis.second * s,
                     ellipse->centre.y + axis.first * s + axis.second * c},
                SnapMode::Quadrant});
        return candidates;
    }
    if (const auto* polyline = std::get_if<DrawPolyline>(&shape)) {
        for (const DrawVertex& vertex : polyline->vertices)
            candidates.push_back(SnapCandidate{vertex.at, SnapMode::Endpoint});
        // A MIDPOINT PER SEGMENT, straight ones only. An arc segment's middle
        // is its mid-sweep, and it is reported by the arc case above when the
        // polyline is exploded -- inside a polyline it would need the bulge
        // resolved, which SegmentsOf already flattens away.
        const std::size_t count = polyline->vertices.size();
        const std::size_t last = polyline->closed ? count : (count == 0 ? 0 : count - 1);
        for (std::size_t i = 0; i < last; ++i) {
            const DrawVertex& from = polyline->vertices[i];
            const DrawVertex& to = polyline->vertices[(i + 1) % count];
            if (std::fabs(from.bulge) > 1.0e-10) continue;
            candidates.push_back(
                SnapCandidate{Vec2{(from.at.x + to.at.x) / 2.0, (from.at.y + to.at.y) / 2.0},
                              SnapMode::Midpoint});
        }
        return candidates;
    }
    if (const auto* text = std::get_if<DrawText>(&shape)) {
        candidates.push_back(SnapCandidate{text->at, SnapMode::Node});
        return candidates;
    }
    return candidates;
}

SnapHit SnapTo(const std::vector<const DrawingEntity*>& entities, Vec2 cursor,
               const SnapSettings& settings, const std::optional<Vec2>& anchor) {
    const double aperture = settings.apertureMm;
    // BEST PER MODE, then the highest-priority mode that has one. Not "the
    // closest of everything": several modes match near a corner, and taking
    // the closest would give a different answer every time the cursor moved a
    // pixel. Predictable beats near.
    struct Best {
        bool found = false;
        Vec2 at{};
        double distance = 0.0;
        ObjectId entityId = kInvalidObjectId;
    };
    std::vector<Best> best(SnapModesByPriority().size());
    const auto indexOf = [](SnapMode mode) {
        const std::vector<SnapMode>& order = SnapModesByPriority();
        for (std::size_t i = 0; i < order.size(); ++i)
            if (order[i] == mode) return i;
        return order.size() - 1;
    };
    const auto offer = [&](SnapMode mode, Vec2 at, ObjectId entityId) {
        if (!settings.isOn(mode)) return;
        const double distance = Distance(cursor, at);
        if (distance > aperture) return;
        Best& slot = best[indexOf(mode)];
        if (!slot.found || distance < slot.distance)
            slot = Best{true, at, distance, entityId};
    };

    for (const DrawingEntity* entity : entities) {
        if (entity == nullptr) continue;
        for (const SnapCandidate& candidate : StaticSnapPointsOf(entity->shape()))
            offer(candidate.mode, candidate.at, entity->id());

        if (settings.nearest)
            offer(SnapMode::Nearest, entity->closestPointTo(cursor), entity->id());

        if (settings.perpendicular && anchor.has_value()) {
            // THE FOOT OF THE PERPENDICULAR FROM THE ANCHOR, not from the
            // cursor. "Perpendicular to that line" means the line being drawn
            // meets it at a right angle, and the line being drawn starts at
            // the anchor.
            if (const auto* line = std::get_if<DrawLine>(&entity->shape())) {
                const double dx = line->b.x - line->a.x;
                const double dy = line->b.y - line->a.y;
                const double lengthSquared = dx * dx + dy * dy;
                if (lengthSquared > 1.0e-12) {
                    const double t = ((anchor->x - line->a.x) * dx +
                                      (anchor->y - line->a.y) * dy) / lengthSquared;
                    offer(SnapMode::Perpendicular,
                          Vec2{line->a.x + t * dx, line->a.y + t * dy}, entity->id());
                }
            } else if (const auto* circle = std::get_if<DrawCircle>(&entity->shape())) {
                // On a circle the perpendicular foot is where the radius
                // through the anchor meets the rim -- both of them, and the
                // aperture picks.
                const double dx = anchor->x - circle->centre.x;
                const double dy = anchor->y - circle->centre.y;
                const double length = std::hypot(dx, dy);
                if (length > 1.0e-12) {
                    offer(SnapMode::Perpendicular,
                          Vec2{circle->centre.x + circle->radius * dx / length,
                               circle->centre.y + circle->radius * dy / length},
                          entity->id());
                    offer(SnapMode::Perpendicular,
                          Vec2{circle->centre.x - circle->radius * dx / length,
                               circle->centre.y - circle->radius * dy / length},
                          entity->id());
                }
            }
        }

        if (settings.tangent && anchor.has_value()) {
            if (const auto* circle = std::get_if<DrawCircle>(&entity->shape())) {
                // THE TWO TANGENT POINTS from a point outside the circle. From
                // inside there are none, and offering the nearest rim point
                // instead would draw a line that is not tangent to anything.
                const double dx = circle->centre.x - anchor->x;
                const double dy = circle->centre.y - anchor->y;
                const double distance = std::hypot(dx, dy);
                if (distance > circle->radius + 1.0e-9) {
                    const double alpha = std::asin(circle->radius / distance);
                    const double base = std::atan2(dy, dx);
                    const double along = std::sqrt(distance * distance -
                                                   circle->radius * circle->radius);
                    for (const double sign : {-1.0, 1.0}) {
                        const double angle = base + sign * alpha;
                        offer(SnapMode::Tangent,
                              Vec2{anchor->x + along * std::cos(angle),
                                   anchor->y + along * std::sin(angle)},
                              entity->id());
                    }
                }
            }
        }
    }

    // INTERSECTIONS, between every pair whose boxes could meet. Quadratic in
    // the entity count, which is why it is done LAST and only inside the
    // aperture -- a sheet with two thousand lines would otherwise spend its
    // time here on every mouse move.
    if (settings.intersection) {
        Box2D window;
        window.grow(Vec2{cursor.x - aperture, cursor.y - aperture});
        window.grow(Vec2{cursor.x + aperture, cursor.y + aperture});
        std::vector<const DrawingEntity*> near;
        for (const DrawingEntity* entity : entities)
            if (entity != nullptr && entity->bounds().touches(window)) near.push_back(entity);

        for (std::size_t i = 0; i < near.size(); ++i) {
            for (std::size_t j = i + 1; j < near.size(); ++j) {
                const DrawShape& first = near[i]->shape();
                const DrawShape& second = near[j]->shape();
                // Line against line, and line against circle -- the two that
                // matter and the two EasyCad had. Curve against curve is
                // honestly absent rather than approximated by flattening,
                // because a snap point that is 0.05 mm off the true crossing
                // is a snap point that puts a dimension on a lie.
                for (const auto& a : SegmentsOf(first)) {
                    for (const auto& b : SegmentsOf(second))
                        if (const std::optional<Vec2> at =
                                LineLineIntersection(a.first, a.second, b.first, b.second, true))
                            offer(SnapMode::Intersection, *at, near[i]->id());
                    if (const auto* circle = std::get_if<DrawCircle>(&second))
                        for (const Vec2 at : LineCircleIntersection(a.first, a.second,
                                                                    circle->centre,
                                                                    circle->radius, true))
                            offer(SnapMode::Intersection, at, near[i]->id());
                }
                for (const auto& b : SegmentsOf(second))
                    if (const auto* circle = std::get_if<DrawCircle>(&first))
                        for (const Vec2 at : LineCircleIntersection(b.first, b.second,
                                                                    circle->centre,
                                                                    circle->radius, true))
                            offer(SnapMode::Intersection, at, near[j]->id());
                if (const auto* circleA = std::get_if<DrawCircle>(&first))
                    if (const auto* circleB = std::get_if<DrawCircle>(&second))
                        for (const Vec2 at : CircleCircleIntersection(
                                 circleA->centre, circleA->radius, circleB->centre,
                                 circleB->radius))
                            offer(SnapMode::Intersection, at, near[i]->id());
            }
        }
    }

    for (std::size_t i = 0; i < best.size(); ++i) {
        if (!best[i].found) continue;
        SnapHit hit;
        hit.found = true;
        hit.at = best[i].at;
        hit.mode = SnapModesByPriority()[i];
        hit.entityId = best[i].entityId;
        return hit;
    }
    return SnapHit{};
}

} // namespace paramcad
