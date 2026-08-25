#pragma once

#include "Core/Geometry/MathTypes.h"

#include <string>
#include <variant>
#include <vector>

namespace paramcad {

// WHAT A VIEW PROJECTS OUT OF A MODEL (M32.2, roadmap §24).
//
// IN MODEL MILLIMETRES, NOT PAPER MILLIMETRES, and this is the decision the
// rest of the block leans on.
//
// A dimension on a drawing reads the TRUE SIZE of the thing: a 40 mm hole
// drawn at 1:2 is still dimensioned "40". If the projection were baked at the
// paper scale, every dimension would have to divide by it, and the day one of
// them forgot the drawing would print a number that is not the part. Keeping
// model millimetres means the scale is applied ONCE, at draw and plot time,
// and never enters the measurement at all.
//
// It also means changing a view's scale does NOT reproject it. That is a real
// saving -- hidden-line removal is the expensive operation in this block -- and
// it is a consequence of the units, not a shortcut taken on top of them.

// WHICH KIND OF EDGE, because a drawing draws them differently.
//
//   Sharp    a real edge between two faces at an angle. Solid line.
//   Outline  the silhouette of a curved face -- the edge of a cylinder seen
//            side-on. It is not an edge of the solid at all; it exists only
//            for this direction of sight, and moves when the view turns.
//   Smooth   a tangent edge, where two faces meet without a crease. A fillet's
//            two boundaries. Conventionally NOT drawn on an engineering
//            drawing, which is why it is a kind rather than being silently
//            dropped: the choice belongs to the drawing, not to the projector.
enum class ProjectedEdgeKind { Sharp, Outline, Smooth };

// Whether the model itself hides it. Hidden edges are drawn dashed, or not at
// all -- again the drawing's choice, so both reach it.
enum class ProjectedVisibility { Visible, Hidden };

std::string_view toString(ProjectedEdgeKind kind) noexcept;
std::string_view toString(ProjectedVisibility visibility) noexcept;

// ANALYTIC WHERE THE MODEL WAS ANALYTIC.
//
// A hole projects to a circle, and a drawing needs to know that: a diameter
// dimension attaches to a circle, DXF writes a CIRCLE entity, and a reader
// expects a smooth curve rather than a thirty-segment polygon. Tessellating
// everything would be simpler here and worse everywhere downstream.
//
// A variant rather than one struct with unused fields, for the reason
// SketchConstraint gives: a shape that cannot hold the wrong members cannot be
// constructed wrong.
struct ProjectedLine {
    Vec2 a{};
    Vec2 b{};
};

struct ProjectedArc {
    Vec2 centre{};
    double radius = 0.0;
    // Radians, counter-clockwise from +X. A FULL circle is start == end, which
    // is how OCCT reports one and how DXF writes one.
    double startAngle = 0.0;
    double endAngle = 0.0;
    bool isFullCircle = false;
};

// The fallback: anything the projector could not name. A spline's silhouette,
// an intersection curve. Kept as points rather than refused, because a drawing
// that dropped every curve it could not classify would be missing geometry the
// model has.
struct ProjectedPolyline {
    std::vector<Vec2> points;
};

using ProjectedShape = std::variant<ProjectedLine, ProjectedArc, ProjectedPolyline>;

struct ProjectedCurve {
    ProjectedShape shape;
    ProjectedEdgeKind kind = ProjectedEdgeKind::Sharp;
    ProjectedVisibility visibility = ProjectedVisibility::Visible;
};

// The extent of what was projected, in model millimetres. A view needs it to
// know how much paper it takes at a given scale, and the sheet needs that to
// say whether it fits.
struct ProjectedExtent {
    Vec2 min{0.0, 0.0};
    Vec2 max{0.0, 0.0};
    bool empty = true;

    double widthMm() const noexcept { return empty ? 0.0 : max.x - min.x; }
    double heightMm() const noexcept { return empty ? 0.0 : max.y - min.y; }
};

struct ProjectedDrawing {
    // THE CUT FACES, as closed loops, for a section view (M38). Empty on every
    // ordinary view. They are an AREA to hatch and not edges to draw: the
    // outline is already among the curves, and drawing it again would double
    // its weight.
    std::vector<std::vector<Vec2>> cutLoops;

    std::vector<ProjectedCurve> curves;
    ProjectedExtent extent;
};

// Grows the extent to hold a curve. One place, so no caller can add geometry
// and forget to account for it -- which would leave a view reporting a paper
// footprint smaller than what it draws.
void GrowExtent(ProjectedExtent& extent, const ProjectedCurve& curve);

} // namespace paramcad
