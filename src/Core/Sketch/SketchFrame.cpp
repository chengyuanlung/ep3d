#include "Core/Sketch/SketchFrame.h"
#include "Core/Geometry/Transform.h"
#include <cmath>

namespace paramcad {

// The quaternion arithmetic MOVED to Core/Geometry/Transform.h in M10 and is
// used from there. ADR-M4-002 asked for exactly one site, not for that site to
// be this file: M10's frame hierarchy needs the same composition, and the
// choice was to duplicate it or to move it.

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

namespace {

double Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

bool Finite(Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Unit vector, or nullopt when there is no direction to speak of.
std::optional<Vec3> Normalized(Vec3 v) noexcept {
    if (!Finite(v)) return std::nullopt;
    const double length = std::sqrt(Dot(v, v));
    if (!std::isfinite(length) || length < 1e-12) return std::nullopt;
    return Vec3{v.x / length, v.y / length, v.z / length};
}

} // namespace

std::optional<SketchFrame> SketchFrame::FromBasis(Vec3 origin, Vec3 uAxis,
                                                  Vec3 normal) noexcept {
    if (!Finite(origin)) return std::nullopt;
    const std::optional<Vec3> n = Normalized(normal);
    if (!n) return std::nullopt;

    // u, squared up against the normal. What survives is the component of the
    // requested direction that actually lies IN the plane.
    const double along = Dot(uAxis, *n);
    const std::optional<Vec3> u = Normalized(Vec3{uAxis.x - n->x * along, uAxis.y - n->y * along,
                                                  uAxis.z - n->z * along});
    if (!u) return std::nullopt; // u was parallel to the normal: no plane direction left
    const Vec3 v = Cross(*n, *u); // unit by construction, both operands unit and square

    // Rotation matrix with columns (u, v, n) -> quaternion, by Shepperd's
    // method: the branch with the largest divisor is chosen so the division is
    // never near zero. A single-formula conversion loses precision, and can
    // divide by zero outright, for rotations near 180 degrees -- which is
    // exactly the case of sketching on the BOTTOM face of a solid.
    const double m00 = u->x, m01 = v.x, m02 = n->x;
    const double m10 = u->y, m11 = v.y, m12 = n->y;
    const double m20 = u->z, m21 = v.z, m22 = n->z;
    const double trace = m00 + m11 + m22;

    Quaternion q;
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        q = Quaternion{0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s};
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        q = Quaternion{(m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s};
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        q = Quaternion{(m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s};
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        q = Quaternion{(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s};
    }

    const double norm = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (!std::isfinite(norm) || norm < 1e-12) return std::nullopt;
    Transform3D transform;
    transform.translation = origin;
    transform.rotation = Quaternion{q.w / norm, q.x / norm, q.y / norm, q.z / norm};
    return SketchFrame{transform};
}

std::optional<SketchFrame> SketchFrame::OnFace(Vec3 pointOnPlane, Vec3 normal) noexcept {
    if (!Finite(pointOnPlane) || !Finite(normal)) return std::nullopt;
    const std::optional<Vec3> n = Normalized(normal);
    if (!n) return std::nullopt;

    // The part origin dropped onto the plane. Derived from the plane's own
    // point, so it does not matter WHERE on the plane that point is -- every
    // point on the plane gives the same answer.
    const double offset = Dot(pointOnPlane, *n);
    const Vec3 origin{n->x * offset, n->y * offset, n->z * offset};

    // u horizontal, so v = n x u points upward. On a horizontal face there is
    // no horizontal direction the plane distinguishes, so world +X is the
    // convention.
    constexpr double kHorizontalCosine = 0.999;
    const Vec3 worldUp{0.0, 0.0, 1.0};
    const Vec3 u = std::fabs(Dot(*n, worldUp)) > kHorizontalCosine
                       ? Vec3{1.0, 0.0, 0.0}
                       : Cross(worldUp, *n);
    return FromBasis(origin, u, *n);
}

Vec3 SketchFrame::toWorld(Vec2 uv) const noexcept {
    return ApplyTransform(transform_, Vec3{uv.x, uv.y, 0.0});
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
