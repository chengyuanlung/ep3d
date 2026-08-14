#pragma once

#include "Core/Geometry/MathTypes.h"
#include <variant>
#include <vector>

namespace paramcad {

// Kernel-neutral description of a planar profile to extrude (ADR-M4-003).
// Pure data, zero OCCT: this is what crosses the Core -> Kernel boundary, so
// nothing here may name an OCCT type, a topology handle, or an index used as
// identity (ADR-M4-004).
//
// Segments are in sketch-local (u,v) millimetres and are already ORDERED and
// ORIENTED: the kernel receives a loop that reads start-to-end and is known to
// close. Deciding what a valid loop is remains Core's job (ADR-M4-005) -- the
// kernel only builds what it is handed, and reports a structured failure if
// OCCT still refuses.

struct ProfileLineSegment {
    Vec2 start{};
    Vec2 end{};
};

// Angles in radians from the sketch +u axis. Already oriented: the arc runs
// from startAngleRad to endAngleRad in the direction counterClockwise selects.
struct ProfileArcSegment {
    Vec2 center{};
    double radiusMm{0.0};
    double startAngleRad{0.0};
    double endAngleRad{0.0};
    bool counterClockwise{true};
};

// A closed curve that forms an entire loop on its own.
struct ProfileCircleSegment {
    Vec2 center{};
    double radiusMm{0.0};
};

using ProfileSegment =
    std::variant<ProfileLineSegment, ProfileArcSegment, ProfileCircleSegment>;

// The plane the profile lies on, in part-local XYZ. Given as explicit vectors
// rather than a Transform3D so the kernel never has to know how this project
// composes rotations -- SketchFrame remains the single conversion site
// (ADR-M4-002), and it is what fills these in.
//
// origin is the image of (0,0); uAxis and vAxis are unit vectors; normal is
// the extrusion direction (spec 12: +sketch normal).
struct ProfilePlane {
    Vec3 origin{0.0, 0.0, 0.0};
    Vec3 uAxis{1.0, 0.0, 0.0};
    Vec3 vAxis{0.0, 1.0, 0.0};
    Vec3 normal{0.0, 0.0, 1.0};
};

struct PlanarProfileDefinition {
    ProfilePlane plane{};
    std::vector<ProfileSegment> segments;
};

// The ONE place extrusion input validation lives, mirroring
// IsValidBoxDefinition's role for boxes (ADR-M3-001): every IGeometryKernel
// implementation, real or fake, calls this first, so a degenerate profile or
// distance can never reach a kernel-internal call and surface as an
// unstructured error.
//
// Rejects: an empty segment list, a non-finite or non-positive extrusion
// distance (or one below kMinExtrusionDistanceMm, the same 1e-6 mm floor
// ADR-M3-009 established for box dimensions), a non-finite plane, a degenerate
// plane basis, and any segment carrying non-finite or non-positive-radius
// values.
inline constexpr double kMinExtrusionDistanceMm = 1e-6;

// Smallest revolve sweep that is still a solid, and the largest one that is
// still a single revolution (M8.2, ADR-M8-005). The floor matches the arc
// model's own kMinSweepRad reasoning: below it the swept wedge is thinner than
// anything the length tolerances can measure. Above 2*pi is REFUSED rather
// than wrapped -- a wrapped angle silently produces the same solid as
// angle mod 2*pi, which is a value error disguised as success.
inline constexpr double kMinRevolveAngleRad = 1e-6;
inline constexpr double kMaxRevolveAngleRad = 6.283185307179586476925286766559; // 2*pi

bool IsValidProfileDefinition(const PlanarProfileDefinition& profile) noexcept;
bool IsValidExtrusionDistance(double distanceMm) noexcept;
bool IsValidRevolveAngle(double angleRad) noexcept;

} // namespace paramcad
