#include "Core/Geometry/Transform.h"

namespace paramcad {

Vec3 RotateByQuaternion(const Quaternion& q, Vec3 v) noexcept {
    const Vec3 qv{q.x, q.y, q.z};
    const Vec3 t{2.0 * (qv.y * v.z - qv.z * v.y),
                 2.0 * (qv.z * v.x - qv.x * v.z),
                 2.0 * (qv.x * v.y - qv.y * v.x)};
    return Vec3{v.x + q.w * t.x + (qv.y * t.z - qv.z * t.y),
                v.y + q.w * t.y + (qv.z * t.x - qv.x * t.z),
                v.z + q.w * t.z + (qv.x * t.y - qv.y * t.x)};
}

Quaternion MultiplyQuaternions(const Quaternion& a, const Quaternion& b) noexcept {
    return Quaternion{a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
                      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

Transform3D Compose(const Transform3D& parent, const Transform3D& child) noexcept {
    Transform3D result;
    result.rotation = MultiplyQuaternions(parent.rotation, child.rotation);
    const Vec3 offset = RotateByQuaternion(parent.rotation, child.translation);
    result.translation = Vec3{parent.translation.x + offset.x, parent.translation.y + offset.y,
                              parent.translation.z + offset.z};
    return result;
}

Transform3D Inverse(const Transform3D& transform) noexcept {
    Transform3D result;
    // The conjugate, which for a unit quaternion IS the inverse rotation.
    result.rotation = Quaternion{transform.rotation.w, -transform.rotation.x,
                                 -transform.rotation.y, -transform.rotation.z};
    const Vec3 back = RotateByQuaternion(result.rotation, transform.translation);
    result.translation = Vec3{-back.x, -back.y, -back.z};
    return result;
}

Vec3 ApplyTransform(const Transform3D& transform, Vec3 local) noexcept {
    const Vec3 rotated = RotateByQuaternion(transform.rotation, local);
    return Vec3{rotated.x + transform.translation.x, rotated.y + transform.translation.y,
                rotated.z + transform.translation.z};
}

} // namespace paramcad
