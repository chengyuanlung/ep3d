#include "Core/Kernel/ProfileDefinition.h"
#include <cmath>
#include <cstddef>

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
            } else if constexpr (std::is_same_v<T, ProfileSplineSegment>) {
                if (value.points.size() < 2) return false;
                for (const Vec2& point : value.points)
                    if (!IsFinite(point)) return false;
                // NO TWO NEIGHBOURS ON TOP OF EACH OTHER: OCCT's interpolator
                // refuses the whole curve for one repeated point, with a
                // message that names neither the point nor the caller.
                for (std::size_t i = 1; i < value.points.size(); ++i) {
                    const double du = value.points[i].x - value.points[i - 1].x;
                    const double dv = value.points[i].y - value.points[i - 1].y;
                    if (std::sqrt(du * du + dv * dv) < kMinExtrusionDistanceMm) return false;
                }
                return true;
            } else if constexpr (std::is_same_v<T, ProfileEllipseSegment> ||
                                 std::is_same_v<T, ProfileEllipticalArcSegment>) {
                if (!IsFinite(value.center)) return false;
                if (!std::isfinite(value.majorRadiusMm) || !std::isfinite(value.minorRadiusMm))
                    return false;
                if (!std::isfinite(value.rotationRad)) return false;
                if (value.minorRadiusMm < kMinExtrusionDistanceMm) return false;
                // MAJOR >= MINOR, because the rotation is measured to the major
                // axis. OCCT refuses a gp_Elips with them the wrong way round,
                // and it is better to say so here -- where the caller can be
                // named -- than to surface an unstructured kernel failure.
                if (value.majorRadiusMm < value.minorRadiusMm) return false;
                if constexpr (std::is_same_v<T, ProfileEllipticalArcSegment>)
                    return std::isfinite(value.startParamRad) &&
                           std::isfinite(value.endParamRad);
                return true;
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

bool IsValidSignedExtrusionDistance(double distanceMm) noexcept {
    return std::isfinite(distanceMm) && std::fabs(distanceMm) >= kMinExtrusionDistanceMm;
}

bool IsValidRevolveAngle(double angleRad) noexcept {
    return std::isfinite(angleRad) && angleRad >= kMinRevolveAngleRad &&
           angleRad <= kMaxRevolveAngleRad;
}

bool IsValidPathDefinition(const PlanarPathDefinition& path) noexcept {
    // THE SAME SEGMENT RULES as a profile's boundary, through the same
    // predicate -- a path is made of the same curves and the arithmetic that
    // makes one degenerate makes the other degenerate too. A second copy of
    // "is this arc's radius sane" is a second chance to answer differently.
    PlanarProfileDefinition asProfile;
    asProfile.plane = path.plane;
    asProfile.segments = path.segments;
    return IsValidProfileDefinition(asProfile);
}

bool IsValidProfileDefinition(const PlanarProfileDefinition& profile) noexcept {
    if (profile.segments.empty()) return false;

    const ProfilePlane& plane = profile.plane;
    if (!IsFinite(plane.origin)) return false;
    if (!IsUnitish(plane.uAxis) || !IsUnitish(plane.vAxis) || !IsUnitish(plane.normal))
        return false;

    for (const ProfileSegment& segment : profile.segments)
        if (!IsValidSegment(segment)) return false;

    // Inner loops (M17). Held to the same rules, and an EMPTY one is refused:
    // a hole with no boundary is a caller mistake that would otherwise reach
    // OCCT as an empty wire and surface as an unstructured failure.
    for (const std::vector<ProfileSegment>& inner : profile.innerLoops) {
        if (inner.empty()) return false;
        for (const ProfileSegment& segment : inner)
            if (!IsValidSegment(segment)) return false;
    }
    return true;
}

} // namespace paramcad
