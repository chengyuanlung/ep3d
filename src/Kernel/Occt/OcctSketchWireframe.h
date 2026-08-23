#pragma once

#include "Core/Kernel/KernelShape.h"
#include "Core/Kernel/ProfileDefinition.h"
#include "Core/Sketch/SketchTypes.h"

#include <vector>

namespace paramcad {

// A sketch drawn in the 3D view (M17.7, ADR-M17-030).
//
// A sketch is only half-visible while it lives exclusively on the 2D canvas: a
// user who finishes one and looks at the part cannot see where it sits in
// relation to everything else, which is the whole reason a sketch has a plane.
// So the part view draws it as a WIREFRAME on that plane.
//
// This lives in the kernel layer for the same reason PlaneOfFace does: a test
// can build the wireframe from real geometry and ask a real kernel where it
// ended up. Left in the widget, the one thing that could go wrong -- a sketch
// on a tilted face drawn flat at the world origin -- would be invisible to
// every test and obvious to every user.
struct SketchWireframe {
    KernelShape shape;
    // What went in, counted by what came out. A wireframe that quietly drops
    // an entity is a picture of a sketch the user does not have.
    int edges{0};
    int vertices{0};
    int skipped{0};

    bool empty() const noexcept { return edges == 0 && vertices == 0; }
};

// Every entity as an edge or a vertex, placed on `plane` in part-local XYZ.
//
// Takes plain geometry rather than a Sketch so the caller decides what belongs
// in the picture -- the viewer passes the sketch's entities and deliberately
// NOT its projected reference geometry, which is a copy of edges the solid
// already draws and would only fight with them for the same pixels.
SketchWireframe BuildSketchWireframe(const std::vector<SketchGeometry>& geometry,
                                     const ProfilePlane& plane);

// The axis-aligned bounds of a shape, in part-local XYZ.
//
// The kernel-neutral door that lets a test ask WHERE geometry ended up without
// naming an OCCT type (ADR-M4-004). `ok` is false for an empty or foreign
// handle -- an empty shape has no bounds, and returning a box at the origin
// would be indistinguishable from geometry that really is there.
struct KernelBounds {
    bool ok{false};
    Vec3 min{};
    Vec3 max{};
};

KernelBounds BoundsOf(const KernelShape& shape);

} // namespace paramcad
