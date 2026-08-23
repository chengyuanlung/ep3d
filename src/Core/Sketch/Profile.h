#pragma once

#include "Core/Sketch/Sketch.h"
#include "Core/Kernel/ProfileDefinition.h"
#include "Core/Sketch/SketchTypes.h"
#include <string>
#include <vector>

namespace paramcad {

// A Profile is a SEMANTIC interpretation of Sketch entities (spec 8), never
// OCCT topology (ADR-M4-003/004). Every reference below is a SketchEntityId --
// no index, no pointer, no edge handle.

// One entity as it appears in a traversal order. `reversed` says the loop walks
// the entity from its end point to its start point, so a loop reads
// start-to-end regardless of the direction each curve was drawn in.
struct OrientedSketchEntityRef {
    SketchEntityId entityId{kInvalidSketchEntityId};
    bool reversed{false};
};

struct ProfileLoop {
    std::vector<OrientedSketchEntityRef> entities;
};

// An outer boundary and its HOLES (M17).
//
// M4 validated one loop and deferred holes; M17 pays that off. The rule that
// makes the pair unambiguous is containment: exactly ONE loop must contain all
// the others, and that one is the outer boundary. Anything else -- two loops
// side by side, a hole containing another loop -- is refused rather than
// guessed at, because each of those is a different modelling intent and none of
// them is what "one solid with holes in it" means.
struct ValidatedProfile {
    ProfileLoop outer;
    std::vector<ProfileLoop> inners;
};

enum class ProfileError {
    None,
    NoEntities,        // nothing that could form a loop
    InvalidGeometry,   // an entity fails IsValidSketchGeometry
    NotChainable,      // a closed curve that cannot be combined as drawn
    NotNested,         // several loops, but not one containing all the others
    OpenLoop,          // an endpoint has only one incident curve
    Branch,            // an endpoint has three or more incident curves
    DuplicateEntity,   // two entities span the same two endpoints
    Disconnected,      // a loop closed but entities were left over
    SelfIntersecting
};

struct ProfileResult {
    ValidatedProfile profile;
    ProfileError error = ProfileError::None;
    std::string message;
    explicit operator bool() const noexcept { return error == ProfileError::None; }
};

// Builds and validates the closed outer loop of `sketch` (ADR-M4-005).
//
// Deterministic and independent of storage order: traversal starts from the
// lowest SketchEntityId, so the same sketch always yields the same loop no
// matter what order entities were added, removed or restored in.
//
// Points are ignored rather than rejected -- they are legitimate reference
// geometry and cannot contribute an edge. Every other entity must take part in
// exactly one closed loop.
//
// Gaps are NEVER healed: two endpoints further apart than
// kProfileConnectivityToleranceMm are different points, and the resulting
// failure names both entities and the measured distance. Silently closing a
// gap is how a real modelling error becomes a wrong solid.
ProfileResult BuildProfile(const Sketch& sketch);

// The same validation with ONE entity treated as construction geometry --
// ignored exactly as Points are, contributing no edge (M8.2, ADR-M8-005).
//
// This exists for the revolve axis: a drawing that revolves a profile about a
// line carries that line IN the sketch, and a validator that counted it as
// profile geometry would reject every such sketch as unchainable. EP3D has no
// general construction-geometry concept yet (roadmap defers it); this is the
// one semantic case M8 needs, named rather than generalized.
ProfileResult BuildProfile(const Sketch& sketch, SketchEntityId excludedEntityId);

// Tolerance used to decide whether two endpoints are the same point, in mm
// (ADR-M4-005). Shares kSketchToleranceMm's value so the project keeps one
// length-scale story across M3 and M4.
inline constexpr double kProfileConnectivityToleranceMm = kSketchToleranceMm;

// Translates a validated semantic loop into the kernel-neutral definition
// (ADR-M4-003). Every segment is looked up by SketchEntityId, never by
// position, and the validator's orientation is applied here so the kernel
// receives a loop that already reads start-to-end.
//
// Was file-local in PadFeature.cpp until M8's PocketFeature needed the same
// translation for its tool prism. Promoted rather than duplicated: two copies
// of segment orientation logic would be two places to disagree about arc
// reversal, and that is a geometry bug that looks like a solver bug.
// False if a loop member no longer exists in the sketch or is a Point.
// `frame` is the sketch's EFFECTIVE plane: its support frame's world transform
// when it has one, otherwise its own embedded SketchFrame. Passed in rather
// than read off the sketch (M10, ADR-M10-003) because only the document can
// compose a frame chain, and this function must stay free of document
// knowledge. `BuildKernelProfile(sketch, validated, out)` keeps the old
// meaning -- the sketch's embedded frame -- so every pre-M10 caller and every
// test that has no frames is unchanged.
// The kernel-neutral plane a sketch frame describes.
//
// ONE conversion, shared. BuildKernelProfile filled these four fields inline;
// the 3D view needs the same plane to draw a sketch's wireframe where the
// sketch actually is (M17.7), and a second copy of the conversion would be a
// second opinion about which way v points.
ProfilePlane PlaneOfSketchFrame(const SketchFrame& frame) noexcept;

bool BuildKernelProfile(const Sketch& sketch, const ValidatedProfile& validated,
                        const SketchFrame& frame, PlanarProfileDefinition& out);

bool BuildKernelProfile(const Sketch& sketch, const ValidatedProfile& validated,
                        PlanarProfileDefinition& out);

} // namespace paramcad
