#pragma once

#include "Core/Geometry/MathTypes.h"

namespace paramcad {

// THE single site where a rotation is applied and where two transforms are
// composed (ADR-M4-002, whose site MOVED here in M10).
//
// M4 put `RotateByQuaternion` file-local in `SketchFrame.cpp` and said why: a
// second conversion path is how frames silently disagree, the same reasoning
// ADR-M3-002 applied to unit conversion. M10 needs the same arithmetic for the
// frame hierarchy, so the choice was to duplicate it or to move it. It moved:
// `SketchFrame.cpp` now calls these, and the count of places that know how to
// turn a quaternion into geometry stayed at ONE.
//
// Anything that needs a rotation or a composition uses these. Nothing writes
// its own.

// Rotate `v` by the unit quaternion `q`, via
//   v' = v + 2w(qv x v) + 2(qv x (qv x v))
// which avoids building a rotation matrix.
Vec3 RotateByQuaternion(const Quaternion& q, Vec3 v) noexcept;

// Hamilton product. `a * b` means "apply b, then a", which is the order
// Compose relies on.
Quaternion MultiplyQuaternions(const Quaternion& a, const Quaternion& b) noexcept;

// `child` expressed in `parent`'s space: the transform that takes a point in
// the child's local coordinates all the way out to the parent's.
//
//   world = parent o child
//   rotation    = parent.rotation * child.rotation
//   translation = parent.translation + rotate(parent.rotation, child.translation)
//
// The rotation of the translation is the part that is easy to leave out and
// impossible to notice until a parent is rotated AND a child is offset -- which
// is exactly why M10's gate F composes two levels by hand.
Transform3D Compose(const Transform3D& parent, const Transform3D& child) noexcept;

// A point in `transform`'s local space, expressed in its parent's space.
Vec3 ApplyTransform(const Transform3D& transform, Vec3 local) noexcept;

} // namespace paramcad
