#pragma once

#include "Core/Geometry/MathTypes.h"

#include <optional>

namespace paramcad {

// The support plane a Sketch's (u,v) coordinates live on (ADR-M4-002).
//
// A value embedded in the Sketch, not a reference to a ReferenceFrame object:
// ReferenceFrame exists since M0 but is not registered, not persisted, and the
// Origin frame is re-created fresh on load (ADR-009 D6). Promoting frames to
// registered, persisted, graph-participating document objects is a real change
// to the M0-M2 contract that a sketch milestone should not smuggle in. The
// stored type is Transform3D -- the same type ReferenceFrame uses -- so that
// when frames do become first-class (M5+), a Sketch can gain an optional
// supportFrameId with this transform as the fallback, and neither entity
// storage nor the (u,v) convention changes.
//
// Convention: +u maps to the frame's local +X, +v to local +Y, and the sketch
// normal (the Pad extrusion direction, spec 12) to local +Z. The default is the
// world XY plane, so world-XY behaviour is a CASE of the general path rather
// than a shortcut around it -- a world-XY hardcode stays invisible until
// something is not at the origin, which is why Gate D exists.
class SketchFrame {
public:
    SketchFrame() = default;
    explicit SketchFrame(Transform3D transform) noexcept : transform_(transform) {}

    static SketchFrame WorldXY() noexcept { return SketchFrame{}; }

    // Frame translated so its origin sits at `origin` in part-local XYZ,
    // with axes unrotated.
    static SketchFrame Translated(Vec3 origin) noexcept;

    // Frame rotated by `angleRad` about the given unit axis, then translated.
    // The axis is normalized internally; a degenerate axis yields no rotation.
    static SketchFrame Rotated(Vec3 axis, double angleRad, Vec3 origin = Vec3{}) noexcept;

    // Frame lying on an arbitrary plane: +u along `uAxis`, +z along `normal`,
    // origin at `origin` (M17.5, sketch-on-face).
    //
    // `uAxis` is ORTHOGONALISED against the normal rather than trusted, because
    // the two come from different sources -- a kernel face supplies the normal,
    // and the u direction is a convention chosen alongside it. Feeding a basis
    // that is a degree off square into the quaternion would produce a frame
    // that is not a rotation, and every (u,v) on the sketch would land slightly
    // off the plane it claims to be on.
    //
    // REFUSES rather than guesses when the basis is degenerate -- zero-length,
    // parallel, or non-finite. There is no defensible plane to fall back to,
    // and world XY is the one answer guaranteed to put the geometry somewhere
    // the user did not pick.
    static std::optional<SketchFrame> FromBasis(Vec3 origin, Vec3 uAxis, Vec3 normal) noexcept;

    // The frame a sketch gets when it is made ON A FACE (M17.14).
    //
    // THE conversion site for that convention, shared by the command that
    // creates such a sketch and by the recompute that re-resolves it when the
    // face moves. Two copies would be two answers to "which way is up on this
    // face", and the sketch would jump the first time it was rebuilt.
    //
    //   origin: the part origin projected onto the plane -- stable under the
    //           edits a parametric model is for, unlike the face's centre.
    //   v:      as close to world +Z as the plane allows, so "up" on a
    //           vertical face is up in the world. On a horizontal face there
    //           is no such direction, and u is world +X -- which makes the top
    //           of a box read exactly like the world XY plane.
    static std::optional<SketchFrame> OnFace(Vec3 pointOnPlane, Vec3 normal) noexcept;

    const Transform3D& transform() const noexcept { return transform_; }
    void setTransform(const Transform3D& transform) noexcept { transform_ = transform; }

    // THE single conversion site (ADR-M4-002): no other code composes this
    // rotation. A second conversion path is how frames silently disagree,
    // which is the same reasoning ADR-M3-002 applied to unit conversion.
    Vec3 toWorld(Vec2 uv) const noexcept;

    // Basis vectors in part-local XYZ.
    Vec3 uAxis() const noexcept;
    Vec3 vAxis() const noexcept;
    Vec3 normal() const noexcept;

private:
    Transform3D transform_{};
};

} // namespace paramcad
