#include "Core/Sketch/SketchConstraint.h"

namespace paramcad {

const char* ConstraintKindName(const SketchConstraintData& data) noexcept {
    return std::visit(
        [](const auto& value) -> const char* {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, CoincidentConstraint>) return "Coincident";
            else if constexpr (std::is_same_v<T, HorizontalConstraint>) return "Horizontal";
            else if constexpr (std::is_same_v<T, VerticalConstraint>) return "Vertical";
            else if constexpr (std::is_same_v<T, FixConstraint>) return "Fix";
            else if constexpr (std::is_same_v<T, DistanceConstraint>) return "Distance";
            else if constexpr (std::is_same_v<T, LengthConstraint>) return "Length";
            else if constexpr (std::is_same_v<T, RadiusConstraint>) return "Radius";
            else if constexpr (std::is_same_v<T, DiameterConstraint>) return "Diameter";
            else {
                static_assert(std::is_same_v<T, AngleConstraint>);
                return "Angle";
            }
        },
        data);
}

std::vector<SketchEntityId> ReferencedEntities(const SketchConstraintData& data) {
    std::vector<SketchEntityId> ids;
    std::visit(
        [&ids](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, CoincidentConstraint> ||
                          std::is_same_v<T, DistanceConstraint>) {
                ids.push_back(c.a.entityId);
                ids.push_back(c.b.entityId);
            } else if constexpr (std::is_same_v<T, FixConstraint>) {
                ids.push_back(c.target.entityId);
            } else if constexpr (std::is_same_v<T, HorizontalConstraint> ||
                                 std::is_same_v<T, VerticalConstraint> ||
                                 std::is_same_v<T, LengthConstraint>) {
                ids.push_back(c.line);
            } else if constexpr (std::is_same_v<T, RadiusConstraint> ||
                                 std::is_same_v<T, DiameterConstraint>) {
                ids.push_back(c.curve);
            } else {
                static_assert(std::is_same_v<T, AngleConstraint>);
                ids.push_back(c.lineA);
                ids.push_back(c.lineB);
            }
        },
        data);
    return ids;
}

bool IsDimensional(const SketchConstraintData& data) noexcept {
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            return std::is_same_v<T, DistanceConstraint> || std::is_same_v<T, LengthConstraint> ||
                   std::is_same_v<T, RadiusConstraint> || std::is_same_v<T, DiameterConstraint> ||
                   std::is_same_v<T, AngleConstraint>;
        },
        data);
}

ObjectId BoundParameterId(const SketchConstraintData& data) noexcept {
    return std::visit(
        [](const auto& value) -> ObjectId {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, DistanceConstraint> ||
                          std::is_same_v<T, LengthConstraint> ||
                          std::is_same_v<T, RadiusConstraint> ||
                          std::is_same_v<T, DiameterConstraint> ||
                          std::is_same_v<T, AngleConstraint>) {
                return value.parameterId;
            } else {
                return kInvalidObjectId;
            }
        },
        data);
}

} // namespace paramcad
