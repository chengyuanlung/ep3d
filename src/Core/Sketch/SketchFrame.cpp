#include "Core/Sketch/SketchFrame.h"
#include <cmath>

namespace paramcad {

namespace {

// Rotate v by the unit quaternion q, via the standard
//   v' = v + 2*w*(qv x v) + 2*(qv x (qv x v))
// formulation, which avoids building a rotation matrix.
//
// File-local on purpose (ADR-M4-002): keeping the only quaternion-application
// in this translation unit is what makes "conversion lives in exactly one
// place" enforceable rather than merely stated.
Vec3 RotateByQuaternion(const Quaternion& q, Vec3 v) noexcept {
    const Vec3 qv{q.x, q.y, q.z};
    const Vec3 t{2.0 * (qv.y * v.z - qv.z * v.y),
                 2.0 * (qv.z * v.x - qv.x * v.z),
                 2.0 * (qv.x * v.y - qv.y * v.x)};
    return Vec3{v.x + q.w * t.x + (qv.y * t.z - qv.z * t.y),
                v.y + q.w * t.y + (qv.z * t.x - qv.x * t.z),
                v.z + q.w * t.z + (qv.x * t.y - qv.y * t.x)};
}

} // namespace

SketchFrame SketchFrame::Translated(Vec3 origin) noexcept {
    Transform3D transform;
    transform.translation = origin;
    return SketchFrame{transform};
}

SketchFrame SketchFrame::Rotated(Vec3 axis, double angleRad, Vec3 origin) noexcept {
    const double length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    Transform3D transform;
    transform.translation = origin;
    // A degenerate axis has no rotation to express; returning identity is the
    // deterministic answer rather than dividing by zero.
    if (!std::isfinite(length) || length < 1e-12 || !std::isfinite(angleRad))
        return SketchFrame{transform};

    const double half = angleRad * 0.5;
    const double s = std::sin(half) / length;
    transform.rotation = Quaternion{std::cos(half), axis.x * s, axis.y * s, axis.z * s};
    return SketchFrame{transform};
}

Vec3 SketchFrame::toWorld(Vec2 uv) const noexcept {
    const Vec3 local{uv.x, uv.y, 0.0};
    const Vec3 rotated = RotateByQuaternion(transform_.rotation, local);
    return Vec3{rotated.x + transform_.translation.x,
                rotated.y + transform_.translation.y,
                rotated.z + transform_.translation.z};
}

Vec3 SketchFrame::uAxis() const noexcept {
    return RotateByQuaternion(transform_.rotation, Vec3{1.0, 0.0, 0.0});
}

Vec3 SketchFrame::vAxis() const noexcept {
    return RotateByQuaternion(transform_.rotation, Vec3{0.0, 1.0, 0.0});
}

Vec3 SketchFrame::normal() const noexcept {
    return RotateByQuaternion(transform_.rotation, Vec3{0.0, 0.0, 1.0});
}

} // namespace paramcad
