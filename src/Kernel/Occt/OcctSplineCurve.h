#pragma once

#include "Core/Geometry/MathTypes.h"

#include <vector>

namespace paramcad {

// Which way the interpolated curve LEAVES each end, as a unit vector in the
// sketch plane's own (u, v).
//
// A kernel-neutral door, for the same reason BoundsOf is one (ADR-M4-004): the
// claim that "the chord is the tangent" spans Core's Catmull-Rom evaluator and
// OCCT's interpolator, and without a way to ask the second one, nothing in the
// program can check that the two still agree. Sketch tangency is written
// against that claim, so it is worth a door of its own.
//
// Names no OCCT type, so a test may include it. The interpolation itself is in
// OcctSplineInterpolation.h, which does.
//
// `ok` is false when there is no curve -- fewer than two points, or points OCCT
// refuses to interpolate.
struct SplineEndDirections {
    bool ok{false};
    Vec2 atStart{};
    Vec2 atEnd{};
};

SplineEndDirections SplineEndDirectionsOf(const std::vector<Vec2>& points, bool closed);

} // namespace paramcad
