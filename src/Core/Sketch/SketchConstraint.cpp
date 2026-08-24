#include "Core/Sketch/SketchConstraint.h"
#include <optional>
#include <cmath>
#include "Core/Sketch/Sketch.h"

namespace paramcad {

const char* ConstraintKindName(const SketchConstraintData& data) noexcept {
    return std::visit(
        [](const auto& value) -> const char* {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, CoincidentConstraint>) return "Coincident";
            else if constexpr (std::is_same_v<T, HorizontalConstraint>) return "Horizontal";
            else if constexpr (std::is_same_v<T, VerticalConstraint>) return "Vertical";
            else if constexpr (std::is_same_v<T, PointsHorizontalConstraint>)
                return "PointsHorizontal";
            else if constexpr (std::is_same_v<T, PointsVerticalConstraint>)
                return "PointsVertical";
            else if constexpr (std::is_same_v<T, FixConstraint>) return "Fix";
            else if constexpr (std::is_same_v<T, DistanceConstraint>) return "Distance";
            else if constexpr (std::is_same_v<T, HorizontalDistanceConstraint>)
                return "HorizontalDistance";
            else if constexpr (std::is_same_v<T, VerticalDistanceConstraint>)
                return "VerticalDistance";
            else if constexpr (std::is_same_v<T, PointLineDistanceConstraint>)
                return "PointLineDistance";
            else if constexpr (std::is_same_v<T, SymmetricConstraint>) return "Symmetric";
            else if constexpr (std::is_same_v<T, LengthConstraint>) return "Length";
            else if constexpr (std::is_same_v<T, RadiusConstraint>) return "Radius";
            else if constexpr (std::is_same_v<T, DiameterConstraint>) return "Diameter";
            else if constexpr (std::is_same_v<T, AngleConstraint>) return "Angle";
            else if constexpr (std::is_same_v<T, ParallelConstraint>) return "Parallel";
            else if constexpr (std::is_same_v<T, PerpendicularConstraint>) return "Perpendicular";
            else if constexpr (std::is_same_v<T, EqualConstraint>) return "Equal";
            else if constexpr (std::is_same_v<T, ConcentricConstraint>) return "Concentric";
            else if constexpr (std::is_same_v<T, MidpointConstraint>) return "Midpoint";
            else if constexpr (std::is_same_v<T, PointOnObjectConstraint>)
                return "PointOnObject";
            else if constexpr (std::is_same_v<T, EllipseAxisConstraint>)
                return value.minor ? "MinorAxis" : "MajorAxis";
            else if constexpr (std::is_same_v<T, EllipseRotationConstraint>)
                return "EllipseRotation";
            else {
                static_assert(std::is_same_v<T, TangentConstraint>);
                return "Tangent";
            }
        },
        data);
}

std::vector<SketchElementRef> ReferencedElements(const SketchConstraintData& data) {
    std::vector<SketchElementRef> refs;
    VisitConstraintElements(data, [&refs](const SketchEntityId& id, SketchSubElement part) {
        refs.push_back(SketchElementRef{id, part});
    });
    return refs;
}

std::vector<SketchEntityId> ReferencedEntities(const SketchConstraintData& data) {
    std::vector<SketchEntityId> ids;
    for (const SketchElementRef& ref : ReferencedElements(data)) ids.push_back(ref.entityId);
    return ids;
}

bool IsDimensional(const SketchConstraintData& data) noexcept {
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            return std::is_same_v<T, DistanceConstraint> || std::is_same_v<T, LengthConstraint> ||
                   std::is_same_v<T, RadiusConstraint> || std::is_same_v<T, DiameterConstraint> ||
                   std::is_same_v<T, EllipseAxisConstraint> ||
                   std::is_same_v<T, EllipseRotationConstraint> ||
                   std::is_same_v<T, AngleConstraint> ||
                   std::is_same_v<T, HorizontalDistanceConstraint> ||
                   std::is_same_v<T, VerticalDistanceConstraint> ||
                   std::is_same_v<T, PointLineDistanceConstraint>;
        },
        data);
}

ObjectId BoundParameterId(const SketchConstraintData& data) noexcept {
    ObjectId id = kInvalidObjectId;
    VisitBoundParameter(data, [&id](const ObjectId& bound) { id = bound; });
    return id;
}

std::optional<double> MeasureConstraint(const Sketch& sketch, const SketchConstraintData& data) {
    const auto pointOf = [&sketch](const SketchElementRef& ref) -> std::optional<Vec2> {
        const SketchEntity* entity = sketch.findEntity(ref.entityId);
        if (entity == nullptr) return std::nullopt;
        if (const auto* point = std::get_if<SketchPoint>(&entity->geometry)) {
            if (ref.subElement != SketchSubElement::Whole) return std::nullopt;
            return point->position;
        }
        if (const auto* line = std::get_if<SketchLine>(&entity->geometry)) {
            if (ref.subElement == SketchSubElement::StartPoint) return line->start;
            if (ref.subElement == SketchSubElement::EndPoint) return line->end;
            if (ref.subElement == SketchSubElement::Whole)
                return Vec2{(line->start.x + line->end.x) * 0.5,
                            (line->start.y + line->end.y) * 0.5};
            return std::nullopt;
        }
        if (const auto* circle = std::get_if<SketchCircle>(&entity->geometry)) {
            if (ref.subElement == SketchSubElement::CenterPoint ||
                ref.subElement == SketchSubElement::Whole)
                return circle->center;
            return std::nullopt;
        }
        if (const auto* arc = std::get_if<SketchArc>(&entity->geometry)) {
            if (ref.subElement == SketchSubElement::CenterPoint ||
                ref.subElement == SketchSubElement::Whole)
                return arc->center;
            if (ref.subElement == SketchSubElement::StartPoint)
                return Vec2{arc->center.x + arc->radiusMm * std::cos(arc->startAngleRad),
                            arc->center.y + arc->radiusMm * std::sin(arc->startAngleRad)};
            if (ref.subElement == SketchSubElement::EndPoint)
                return Vec2{arc->center.x + arc->radiusMm * std::cos(arc->endAngleRad),
                            arc->center.y + arc->radiusMm * std::sin(arc->endAngleRad)};
        }
        return std::nullopt;
    };

    const auto lineOf = [&sketch](SketchEntityId id) -> const SketchLine* {
        const SketchEntity* entity = sketch.findEntity(id);
        return entity == nullptr ? nullptr : std::get_if<SketchLine>(&entity->geometry);
    };

    const auto radiusOf = [&sketch](SketchEntityId id) -> std::optional<double> {
        const SketchEntity* entity = sketch.findEntity(id);
        if (entity == nullptr) return std::nullopt;
        if (const auto* circle = std::get_if<SketchCircle>(&entity->geometry))
            return circle->radiusMm;
        if (const auto* arc = std::get_if<SketchArc>(&entity->geometry)) return arc->radiusMm;
        return std::nullopt;
    };

    const auto separation = [](Vec2 a, Vec2 b) {
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        return std::sqrt(dx * dx + dy * dy);
    };

    if (const auto* length = std::get_if<LengthConstraint>(&data)) {
        const SketchLine* line = lineOf(length->line);
        if (line == nullptr) return std::nullopt;
        return separation(line->start, line->end);
    }
    if (const auto* distance = std::get_if<DistanceConstraint>(&data)) {
        const std::optional<Vec2> a = pointOf(distance->a);
        const std::optional<Vec2> b = pointOf(distance->b);
        if (!a || !b) return std::nullopt;
        return separation(*a, *b);
    }
    if (const auto* horizontal = std::get_if<HorizontalDistanceConstraint>(&data)) {
        const std::optional<Vec2> a = pointOf(horizontal->a);
        const std::optional<Vec2> b = pointOf(horizontal->b);
        if (!a || !b) return std::nullopt;
        return b->x - a->x; // SIGNED, exactly as the residual is
    }
    if (const auto* vertical = std::get_if<VerticalDistanceConstraint>(&data)) {
        const std::optional<Vec2> a = pointOf(vertical->a);
        const std::optional<Vec2> b = pointOf(vertical->b);
        if (!a || !b) return std::nullopt;
        return b->y - a->y;
    }
    if (const auto* toLine = std::get_if<PointLineDistanceConstraint>(&data)) {
        const std::optional<Vec2> point = pointOf(toLine->point);
        const SketchLine* line = lineOf(toLine->line);
        if (!point || line == nullptr) return std::nullopt;
        const double du = line->end.x - line->start.x;
        const double dv = line->end.y - line->start.y;
        const double length = std::sqrt(du * du + dv * dv);
        if (length <= kSketchToleranceMm) return std::nullopt;
        const double pu = point->x - line->start.x;
        const double pv = point->y - line->start.y;
        return (pu * dv - pv * du) / length; // signed, as the residual is
    }
    if (const auto* turn = std::get_if<EllipseRotationConstraint>(&data)) {
        const SketchEntity* entity = sketch.findEntity(turn->curve);
        if (entity == nullptr) return std::nullopt;
        if (const auto* full = std::get_if<SketchEllipse>(&entity->geometry))
            return full->rotationRad;
        if (const auto* piece = std::get_if<SketchEllipticalArc>(&entity->geometry))
            return piece->rotationRad;
        return std::nullopt;
    }
    if (const auto* axis = std::get_if<EllipseAxisConstraint>(&data)) {
        const SketchEntity* entity = sketch.findEntity(axis->curve);
        if (entity == nullptr) return std::nullopt;
        if (const auto* full = std::get_if<SketchEllipse>(&entity->geometry))
            return axis->minor ? full->minorRadiusMm : full->majorRadiusMm;
        if (const auto* piece = std::get_if<SketchEllipticalArc>(&entity->geometry))
            return axis->minor ? piece->minorRadiusMm : piece->majorRadiusMm;
        return std::nullopt;
    }
    if (const auto* radius = std::get_if<RadiusConstraint>(&data)) return radiusOf(radius->curve);
    if (const auto* diameter = std::get_if<DiameterConstraint>(&data)) {
        const std::optional<double> r = radiusOf(diameter->curve);
        return r ? std::optional<double>(*r * 2.0) : std::nullopt;
    }
    if (const auto* angle = std::get_if<AngleConstraint>(&data)) {
        const SketchLine* first = lineOf(angle->lineA);
        const SketchLine* second = lineOf(angle->lineB);
        if (first == nullptr || second == nullptr) return std::nullopt;
        const double au = first->end.x - first->start.x;
        const double av = first->end.y - first->start.y;
        const double bu = second->end.x - second->start.x;
        const double bv = second->end.y - second->start.y;
        if (std::sqrt(au * au + av * av) <= kSketchToleranceMm ||
            std::sqrt(bu * bu + bv * bv) <= kSketchToleranceMm)
            return std::nullopt;
        // atan2 of the cross and dot products, which is the signed angle
        // between them and the same quantity the residual drives -- an acos of
        // the normalised dot would lose the sign and fold 300 degrees onto 60.
        return std::atan2(au * bv - av * bu, au * bu + av * bv);
    }
    return std::nullopt; // not a dimensional constraint: nothing to measure
}

} // namespace paramcad