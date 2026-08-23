#pragma once

#include "Core/Geometry/MathTypes.h"
#include "Core/Kernel/FaceGeometry.h"
#include "Core/Sketch/SketchFrame.h"
#include "Core/Sketch/SketchTypes.h"

#include <string>
#include <vector>

namespace paramcad {

// Sketching on a picked face (M17.5, ADR-M17-027).
//
// WHAT THIS DOES NOT DO, stated first because it is the part that matters:
// the sketch is placed on the PLANE the face occupied at the moment it was
// picked. It does not hold a reference to the face. Change the feature
// underneath and the sketch stays where it is; it does not follow the face,
// and it does not break either.
//
// That is a deliberate limit, not an oversight. A sketch that tracks a face
// needs a name for that face which survives a rebuild -- the topological
// naming problem (roadmap 20) -- and this project has no such naming yet.
// The two honest options were "place on the plane and say so" and "wait for
// naming". Silently pretending a face reference exists, and quietly moving a
// user's geometry when it turned out not to, was never one of them.
//
// Qt-free and OCCT-free on purpose: everything decided here -- whether a pick
// can carry a sketch, where its origin sits, which way u and v run -- is
// testable without a display and without a kernel (UI spec 20).

// What the viewer managed to pick, which is exactly what the kernel read.
//
// An ALIAS, not a second struct. There WAS a second struct here, filled by
// copying FacePlane field by field -- and the copy dropped `boundary`, so the
// kernel read every edge of the picked face, this layer threw them away, and
// the projection was handed nothing to project. The feature did not work while
// every layer of it was correct and every test passed, because the two structs
// disagreed and nothing compared them.
//
// One definition cannot disagree with itself, and it cannot forget a field
// that gets added to it later.
using PickedFace = FacePlane;

// The picked face's boundary, brought into sketch (u,v) (M17.6, ADR-M17-029).
struct ProjectedBoundary {
    // Curves first, then the vertices and centres they contribute. One vector,
    // because everything in it is stored, drawn and converted identically --
    // splitting them would only create two loops that must agree.
    std::vector<SketchGeometry> geometry;

    // How many boundary curves could NOT be brought across, and why, in words.
    //
    // COUNTED AND REPORTED, never dropped quietly. An underlay missing three of
    // its edges looks exactly like a face with three fewer edges, and a user
    // tracing it would draw the wrong outline and never know. This is the same
    // rule the profile validator follows: a boundary with a silent gap in it is
    // worse than a refusal.
    int skipped{0};
    std::string skippedReason;
};

struct FaceSketchPlan {
    bool ok{false};
    // ALWAYS set, on success and on refusal alike. A refusal a user cannot
    // read is indistinguishable from a command that did nothing.
    std::string message;
    SketchFrame frame{};
    ProjectedBoundary reference{};
};

// The plane a picked face offers, as a sketch frame.
//
// Origin: the part origin projected onto the plane. Not the face's centre --
// a centre moves when the face is resized, so the same click on the same face
// would give a different (0,0) after an edit, and every dimension a user
// measured from the origin would mean something else. The projected part
// origin is stable under exactly the edits a parametric model is for.
//
// v runs as close to world +Z as the plane allows, so a sketch on a vertical
// face has "up" where the user is looking at up. On a horizontal face, where
// that rule has no answer, u is world +X.
FaceSketchPlan PlanSketchOnFace(const PickedFace& face);

// The boundary curves of a face, projected onto `frame` and expressed in the
// same geometry variant a sketch entity uses (M17.6).
//
// Projection is ORTHOGONAL, along the sketch normal -- which for the face the
// sketch was made on is the identity, and for any other plane is what
// "project onto this sketch" means everywhere else in CAD.
//
// What survives, and what does not:
//   * a LINE always projects to a line, unless it is perpendicular to the
//     plane, where it collapses to a point and is skipped;
//   * a CIRCLE or ARC projects to a circle or arc ONLY when its own plane is
//     parallel to the sketch's. Seen at any other angle it is an ELLIPSE, and
//     EP3D has no ellipse entity -- so it is skipped and counted, not
//     approximated. An arc quietly replaced by a circular one of the wrong
//     radius is a wrong drawing that looks right;
//   * every endpoint and every centre becomes a POINT, deduplicated, because
//     the vertices are what a user actually snaps to.
ProjectedBoundary ProjectBoundaryOntoSketch(const FaceBoundary& boundary,
                                            const SketchFrame& frame);

} // namespace paramcad
