#pragma once

#include "Core/Geometry/MathTypes.h"

#include <cstdint>
#include <vector>

namespace paramcad {

// One boundary curve of a face, in part-local XYZ (M17.6, ADR-M17-029).
//
// Kernel-neutral, like everything else that crosses the Core <-> Kernel
// boundary (ADR-M4-003/004): no OCCT type, no topology handle, and no index
// used as identity. A kernel fills these in; the projection that turns them
// into sketch geometry never learns which kernel produced them.
//
// Kind::Unsupported is a FIRST-CLASS answer, not a failure. A face bounded by
// a spline or an ellipse is a perfectly good face -- EP3D simply has no sketch
// entity that could hold its projection (Ellipse and Spline are each their own
// milestone). Reporting the curve as unsupported lets the projection say "3 of
// 7 edges could not be projected" instead of quietly returning a boundary with
// holes in it, which is the same shape of lie as a profile that silently drops
// a segment.
struct FaceCurve {
    enum class Kind { Line, Circle, Arc, Unsupported };

    Kind kind{Kind::Unsupported};
    Vec3 start{};        // Line, Arc
    Vec3 end{};          // Line, Arc
    Vec3 center{};       // Circle, Arc
    double radiusMm{0.0};// Circle, Arc
    // The curve's own plane normal, for Circle and Arc. Compared against the
    // sketch normal to decide whether the projection is still a circle: a
    // circle seen edge-on projects to an ellipse, and EP3D has no ellipse.
    Vec3 axis{};
};

using FaceBoundary = std::vector<FaceCurve>;

// A planar face, as everything downstream of a pick needs it.
//
// ONE struct, in Core, deliberately. There were two: this one, filled by the
// kernel, and a `PickedFace` in the viewer, filled by copying this one field
// by field. The copy dropped `boundary` -- so the kernel read every edge of
// the face, the viewer threw them away, and the projection was handed an empty
// boundary and drew nothing. Every layer was correct and the feature did not
// work.
//
// A hand-written copy between two structs that hold the same data is a defect
// waiting for the next field to be added. Neither struct named an OCCT type,
// so there was never a reason for the second one to exist: the kernel fills
// this in, the viewer carries it, and the Qt-free planner reads it.
struct FacePlane {
    bool isFace{false};  // the pick landed on a face at all
    bool planar{false};  // and that face is a plane, not a cylinder or a spline
    Vec3 point{};        // a point on that plane; need not be inside the face
    Vec3 normal{};       // unit, pointing OUT of the material
    FaceBoundary boundary{}; // the face's edges, in part-local XYZ

    // WHICH FEATURE made this face, as the opaque tag the kernel was given
    // (M17.13, ADR-M17-035). Zero means "not known" -- which is the honest
    // answer when the face was read without its owning shape, and is why it is
    // not an ObjectId: kInvalidObjectId would be a claim about a document this
    // layer knows nothing about.
    //
    // It is what lets a click on an INNER face become a query. "The outermost
    // face towards +Z" cannot name a pocket floor; "what Pocket001 created"
    // can.
    std::uint64_t createdBy{0};
};

} // namespace paramcad
