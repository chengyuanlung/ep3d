#pragma once

#include "Core/Geometry/MathTypes.h"

#include <Geom_BSplineCurve.hxx>
#include <gp_Pnt.hxx>

#include <vector>

namespace paramcad {

// THE ONE interpolation of a sketch spline (M18).
//
// It was written twice: once to build a profile edge for a solid, and once to
// build the wireframe the 3D view draws. Two copies of "how does this program
// turn seven points into a curve" is two chances to answer differently -- and
// the moment one of them learned about end tangents (below) and the other did
// not, the preview and the solid would have shown different curves through the
// same points, which is the shape of nearly every defect this project has had.
//
// GeomAPI_Interpolate, not GeomAPI_PointsToBSpline: the first passes through
// every point exactly and the second fits NEAR them. A user who clicked where
// the curve should go means the first, and a profile whose ends are only
// approximately at the neighbouring edges' ends does not close.
//
// THE END TANGENTS are supplied rather than left to OCCT. Core evaluates a
// spline as a uniform Catmull-Rom with REFLECTED ends, which makes its
// direction at the first point exactly p1 - p0 and at the last exactly
// p[n-1] - p[n-2]. Sketch tangency is written against that rule, because those
// are the two points a constraint can hold. Left to itself OCCT picks its own
// end conditions, and they are not the chord -- so a user who constrained a
// spline's end tangent to a neighbouring line would get a sketch that said
// "smooth" and a solid with a visible kink.
//
// THIS HEADER NAMES OCCT TYPES, so only translation units under src/Kernel/Occt
// may include it. The question a test needs to ask -- which way does the curve
// leave its ends -- is in OcctSplineCurve.h, which names none.
//
// Returns a null handle when the points cannot be interpolated; every caller
// has to decide what to do about that, and none of them can do it here.
Handle(Geom_BSplineCurve) InterpolateSplineThrough(const std::vector<gp_Pnt>& points, bool closed);

} // namespace paramcad
