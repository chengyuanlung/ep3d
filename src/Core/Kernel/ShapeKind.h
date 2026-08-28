#pragma once

#include <string>
#include <string_view>

namespace paramcad {

// M59 -- WHAT KIND OF THING THIS SHAPE IS.
//
// Everything in EP3D up to here has been a solid. Every feature makes one,
// every kernel operation returns one, and every consumer -- mass properties,
// booleans, the drawing projector, M55's measure tool -- assumes one without
// asking, because until now the assumption could not be wrong.
//
// Surfaces break that, and the way they break it is the dangerous way: a shell
// is a perfectly valid KernelShape. It builds, it draws, it has a bounding box,
// and OCCT will hand back mass properties for it -- a volume of ZERO, which is
// a number, and which every caller downstream will treat as one. A part that
// weighs nothing, a centre of mass at the origin, a cut list line at 0 kg.
//
// So the kind is asked rather than assumed, and it is DERIVED FROM THE SHAPE
// and never stored. A stored kind is a second thing that has to agree with the
// geometry, and the day a boolean turns a solid into a shell it would still say
// solid (ADR-M4-004 for the same reason topology is never stored as identity).

enum class ShapeKind {
    Empty,    // nothing at all
    Vertex,   // a point
    Wire,     // edges, no area
    Face,     // one surface patch
    Shell,    // faces sewn together, open or closed
    Solid,    // a shell that encloses a volume, and the kernel knows it does
    Compound, // several of the above in one shape
};

std::string_view toString(ShapeKind kind) noexcept;
bool ParseShapeKind(std::string_view text, ShapeKind& into) noexcept;
// What a message calls it: "a shell", "a solid", "a wire".
std::string_view NameOf(ShapeKind kind) noexcept;

// WHY THIS IS NOT SOMETHING YOU CAN WEIGH, or empty when it is.
//
// One sentence per kind, in one place, so that the measure tool, a mass
// calculation and a cut list all refuse the same thing the same way -- and so
// that adding a kind means the compiler asks what it should say.
//
// A CLOSED SHELL IS STILL NOT A SOLID, and that distinction is the one worth
// spelling out: a shell can enclose a volume perfectly and OCCT will still
// decline to integrate over it, because a solid is a shell the kernel has been
// TOLD bounds material. Sewing a surface model into something watertight is
// most of the way there and is not there, and a user who has done the work
// deserves to be told which step is missing rather than handed a zero.
std::string WhyNotASolid(ShapeKind kind);

} // namespace paramcad
