#pragma once

#include "Core/Geometry/MathTypes.h"
#include "Core/Kernel/FaceGeometry.h"
#include "Core/Kernel/FaceQuery.h"
#include "Core/Kernel/KernelShape.h"

#include <string>
#include <vector>

class TopoDS_Shape;

namespace paramcad {

// Reading the plane off a picked face (M17.5, ADR-M17-027).
//
// This lives HERE, in the kernel layer, rather than inside OcctViewWidget,
// for one reason: a test can build a real box with the real kernel and check
// the answer. Left in the widget it would be the one step of sketch-on-face
// that nothing could verify without a display and a mouse -- and it is also
// the step with the subtle case in it (face orientation), so it is the last
// step that should go unchecked.
//
// The struct it fills, FacePlane, lives in Core (Core/Kernel/FaceGeometry.h):
// it names no OCCT type, and the viewer needs to carry it.

// The plane of `shape`, if it is a planar face.
//
// The normal accounts for the face's ORIENTATION, so it points out of the
// solid rather than out of the underlying surface. Half the faces of every
// box are stored reversed, so ignoring this gets the plane right and the side
// wrong exactly half the time -- and the side is what decides whether a pad
// built on the resulting sketch grows away from the part or back into it.
FacePlane PlaneOfFace(const TopoDS_Shape& shape);

// Every face of `shape`, in the order the kernel enumerates them.
//
// The KERNEL-NEUTRAL entry point, and it exists so that the function above can
// be tested. OCCT headers are private to this library on purpose (ADR-M4-004),
// so a test cannot hand PlaneOfFace a TopoDS_Face -- but it can build a real
// box through the ordinary kernel API and ask for its faces here. Each entry
// is produced by PlaneOfFace itself, so what a test checks through this door
// is the same code the viewer runs on a picked face.
//
// A foreign or empty handle yields an empty vector, never a guess.
std::vector<FacePlane> FacesOf(const KernelShape& shape);

// The ONE face a query names -- see FaceQueryResult in Core/Kernel/FaceQuery.h.
FaceQueryResult ResolveFaceQuery(const KernelShape& shape, const FaceQuery& query);

// The same answer as TOPOLOGY rather than geometry (M20) -- for the operations
// that have to hand OCCT the face itself, a shell's opening or a draft's wall
// -- is in OcctFaceQueryTopology.h.
//
// It lives in its own header because it NAMES AN OCCT TYPE, and this one does
// not: the viewer includes this file, and the viewer is built without OCCT's
// include directories. Widening this header broke it immediately, which is the
// same lesson OcctSplineCurve.h learned in M18.

} // namespace paramcad
