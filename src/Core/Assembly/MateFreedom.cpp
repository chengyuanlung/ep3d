#include "Core/Assembly/MateFreedom.h"

#include "Core/Geometry/Transform.h"

#include <cmath>

namespace paramcad {

std::string_view toString(MateComponent component) noexcept {
    switch (component) {
        case MateComponent::TX: return "x";
        case MateComponent::TY: return "y";
        case MateComponent::TZ: return "z";
        case MateComponent::RX: return "about x";
        case MateComponent::RY: return "about y";
        case MateComponent::RZ: return "about z";
    }
    return "?";
}

std::array<double, kMateComponentCount> ComponentsOf(const Transform3D& transform) noexcept {
    std::array<double, kMateComponentCount> out{};
    out[0] = transform.translation.x;
    out[1] = transform.translation.y;
    out[2] = transform.translation.z;

    // The rotation as an axis-angle vector. The quaternion's vector part is
    // sin(angle/2) along the axis, so the axis comes from normalising it and
    // the angle from atan2 of the two halves -- atan2 rather than acos(w)
    // because acos loses all its precision exactly where this matters most,
    // near zero rotation, which is where a converged solve lives.
    const double w = transform.rotation.w;
    const double vx = transform.rotation.x;
    const double vy = transform.rotation.y;
    const double vz = transform.rotation.z;
    const double sine = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (sine < 1e-15) {
        // Identity, to the precision a double can carry. The limit of
        // (angle/sine) is 2, so the small-angle answer is 2*v -- which is
        // smooth through zero, unlike dividing by a vanishing sine.
        out[3] = 2.0 * vx;
        out[4] = 2.0 * vy;
        out[5] = 2.0 * vz;
        return out;
    }
    const double angle = 2.0 * std::atan2(sine, w);
    const double scale = angle / sine;
    out[3] = vx * scale;
    out[4] = vy * scale;
    out[5] = vz * scale;
    return out;
}

Transform3D TransformOfComponents(
    const std::array<double, kMateComponentCount>& values) noexcept {
    Transform3D out;
    const double rx = values[3];
    const double ry = values[4];
    const double rz = values[5];
    const double angle = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (angle < 1e-15) {
        out.rotation = Quaternion{1.0, 0.0, 0.0, 0.0};
    } else {
        const double half = angle / 2.0;
        const double s = std::sin(half) / angle;
        out.rotation = Quaternion{std::cos(half), rx * s, ry * s, rz * s};
    }
    // ROTATE FIRST, THEN TRANSLATE. The translation is in the parent frame, so
    // it is not turned by the rotation -- which is what "the connector moved 5
    // mm along its own Z and also turned" has to mean for a slider that is
    // also a hinge (a cylindrical mate).
    out.translation = Vec3{values[0], values[1], values[2]};
    return out;
}

} // namespace paramcad
