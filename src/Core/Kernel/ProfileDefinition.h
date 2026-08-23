#pragma once

#include "Core/Geometry/MathTypes.h"
#include <variant>
#include <map>
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

// An ellipse, and a piece of one (M17.25).
//
// `rotationRad` turns the MAJOR axis away from +u. The two params are the
// ellipse's own PARAMETER, not an angle from the centre:
//
//     point(t) = centre + R(rotation) * (major cos t, minor sin t)
//
// which is also exactly how OCCT parametrises a gp_Elips -- so the kernel
// trims by these numbers directly, with no conversion to get wrong. Handing it
// geometric angles instead would build an edge that starts and ends in the
// right places and covers the wrong piece of the curve in between.
struct ProfileEllipseSegment {
    Vec2 center{};
    double majorRadiusMm{0.0};
    double minorRadiusMm{0.0};
    double rotationRad{0.0};
};

struct ProfileEllipticalArcSegment {
    Vec2 center{};
    double majorRadiusMm{0.0};
    double minorRadiusMm{0.0};
    double rotationRad{0.0};
    double startParamRad{0.0};
    double endParamRad{0.0};
    bool counterClockwise{true};
};

// A smooth curve THROUGH `points`, in order (M17.26).
//
// The points, not a knot vector and not control points: the kernel interpolates
// them itself, so Core never has to know how OCCT parametrises a B-spline and
// the file never stores a basis that a later OCCT could compute differently.
// What crosses this boundary is what the user drew.
//
// Core samples the same points with its own cubic for drawing and for deciding
// which loop contains which; the two agree exactly AT the points and to within
// a fraction of a chord between them (see SampleSpline).
struct ProfileSplineSegment {
    std::vector<Vec2> points;
    bool closed{false};
    // The tangent at the points that carry a handle, by index (M18). Crossing
    // this boundary because it is part of WHAT THE USER DREW: a spline with a
    // handle and one without are different curves through the same points, and
    // a profile that dropped them would build a solid that does not match the
    // sketch it came from.
    std::map<int, Vec2> handles;
};

using ProfileSegment =
    std::variant<ProfileLineSegment, ProfileArcSegment, ProfileCircleSegment,
                 ProfileEllipseSegment, ProfileEllipticalArcSegment, ProfileSplineSegment>;

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
    // The OUTER boundary, ordered and oriented.
    std::vector<ProfileSegment> segments;
    // HOLES. Each is a closed loop strictly inside the outer boundary, ordered
    // and oriented like it (M17).
    //
    // A separate vector rather than a flag on the segments, because "which loop
    // is this segment in" is the question a flat list cannot answer -- and the
    // kernel must not be the one deciding where one loop ends and the next
    // begins. Core has already worked that out, along with which loop contains
    // which; the kernel builds what it is handed (ADR-M4-003).
    std::vector<std::vector<ProfileSegment>> innerLoops;
};

// A PATH to sweep along (M19).
//
// The same segment vocabulary as a profile, and deliberately its OWN TYPE
// rather than a PlanarProfileDefinition with the holes left empty. A profile
// promises a CLOSED, oriented boundary -- every reader of one is entitled to
// assume it comes back to where it started -- and a sweep path usually does
// not. Handing an open chain over in a type that promises closure is the shape
// of defect this project keeps paying for: both sides correct, the contract
// between them a matter of convention.
//
// `closed` is stated rather than inferred by comparing the first and last
// points. A ring path and an open path that happens to end near its start are
// different intentions, and a tolerance is not the place to guess which was
// meant.
struct PlanarPathDefinition {
    ProfilePlane plane{};
    // In order, head to tail. Each segment's start meets the previous one's
    // end; Core has already walked and oriented them, exactly as it does for a
    // profile's boundary (ADR-M4-003).
    std::vector<ProfileSegment> segments;
    bool closed{false};
};

bool IsValidPathDefinition(const PlanarPathDefinition& path) noexcept;

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
// values. Inner loops are held to the SAME segment rules as the outer one, and
// an EMPTY inner loop is refused -- a hole with no boundary is not a hole, it
// is a caller mistake that would otherwise reach OCCT as an empty wire.
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

// A SIGNED extrusion distance (M17.8, ADR-M17-031).
//
// The sign chooses which side of the sketch plane the material goes on; only
// the MAGNITUDE has to clear the floor. This exists as its own predicate
// because IsValidExtrusionDistance is also what guards a fillet radius and a
// chamfer distance, and neither of those has a meaningful negative -- relaxing
// the shared one would have quietly accepted a fillet of -2 mm.
//
// Why signed at all: a sketch made on a FACE has its normal pointing OUT of
// the solid, so that a pad grows away from the part (ADR-M17-028). A pocket
// from the same plane then builds its tool outside the material and cuts
// nothing. A negative depth is the direct way to say "the other way", and it
// is the same word a user would use.
bool IsValidSignedExtrusionDistance(double distanceMm) noexcept;
bool IsValidRevolveAngle(double angleRad) noexcept;

} // namespace paramcad
