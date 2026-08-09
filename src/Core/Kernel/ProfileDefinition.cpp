#include "Core/Kernel/ProfileDefinition.h"
#include <cmath>

namespace paramcad {

namespace {

bool IsFinite(Vec2 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y); }

bool IsFinite(Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

double Length(Vec3 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

bool IsUnitish(Vec3 v) noexcept {
    if (!IsFinite(v)) return false;
    const double length = Length(v);
    // Generous band: the point is to catch a zero or wildly wrong basis vector,
    // not to police floating-point normalization error.
    return length > 0.5 && length < 2.0;
}

bool IsValidSegment(const ProfileSegment& segment) noexcept {
    return std::visit(
        [](const auto& value) -> bool {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ProfileLineSegment>) {
                return IsFinite(value.start) && IsFinite(value.end);
            } else if constexpr (std::is_same_v<T, ProfileArcSegment>) {
                return IsFinite(value.center) && std::isfinite(value.radiusMm) &&
                       value.radiusMm >= kMinExtrusionDistanceMm &&
                       std::isfinite(value.startAngleRad) && std::isfinite(value.endAngleRad);
            } else {
                static_assert(std::is_same_v<T, ProfileCircleSegment>);
                return IsFinite(value.center) && std::isfinite(value.radiusMm) &&
                       value.radiusMm >= kMinExtrusionDistanceMm;
            }
        },
        segment);
}

} // namespace

bool IsValidExtrusionDistance(double distanceMm) noexcept {
    return std::isfinite(distanceMm) && distanceMm >= kMinExtrusionDistanceMm;
}

bool IsValidProfileDefinition(const PlanarProfileDefinition& profile) noexcept {
    if (profile.segments.empty()) return false;

    const ProfilePlane& plane = profile.plane;
    if (!IsFinite(plane.origin)) return false;
    if (!IsUnitish(plane.uAxis) || !IsUnitish(plane.vAxis) || !IsUnitish(plane.normal))
        return false;

    for (const ProfileSegment& segment : profile.segments)
        if (!IsValidSegment(segment)) return false;
    return true;
}

} // namespace paramcad
