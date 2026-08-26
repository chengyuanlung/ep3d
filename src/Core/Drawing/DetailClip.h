#pragma once

#include "Core/Drawing/ProjectedGeometry.h"
#include "Core/Geometry/MathTypes.h"

#include <vector>

namespace paramcad {

// M49 -- WHAT A DETAIL VIEW SHOWS: the parent's projection, inside a circle.
//
// A detail view is not a new projection. It is the SAME curves the parent
// already has, cropped to a circle and drawn bigger -- which is why this is
// arithmetic on ProjectedCurve rather than another trip through the kernel.
// Hidden-line removal is the expensive operation in this block and doing it
// twice for the same camera would be paying for an answer already on hand.
//
// THE RULE THAT MATTERS, AND IT IS NOT ABOUT PIXELS:
//
// A curve wholly inside the circle comes back UNCHANGED AND STILL ITSELF. A
// hole is a ProjectedArc, and it has to still be a ProjectedArc afterwards --
// because a diameter dimension attaches to a circle, and DXF writes a CIRCLE
// entity. Clip everything to line segments and the detail LOOKS perfect: same
// shape on the paper, same picture on the plot. What is gone is the only thing
// that made it a hole rather than a thirty-sided polygon, and the drafter who
// tries to dimension it either cannot, or gets 39.97 across the flats of a
// polygon that used to be 40.
//
// That failure survives every visual check, which is precisely why it is
// stated here and pinned by a test rather than left to the implementation to
// happen to get right.
//
// A curve that CROSSES the boundary is trimmed, and stays what it was: the
// inside part of an arc is a shorter arc, not a polyline.

// Everything inside the circle, in MODEL millimetres -- the same units the
// parent's curves are in (see ProjectedGeometry.h). The scale a detail is
// drawn at is applied once, at draw time, exactly as it is for every other
// view; a crop that baked the enlargement in would put the scale into the
// measurement.
std::vector<ProjectedCurve> ClipToCircle(const std::vector<ProjectedCurve>& curves,
                                         Vec2 centreMm, double radiusMm);

// Is this point inside? Exposed because the refusal below and the clip have to
// agree about what "inside" means, and two answers to that is the shape of
// defect this project keeps closing.
bool InsideCircle(Vec2 point, Vec2 centreMm, double radiusMm) noexcept;

} // namespace paramcad
