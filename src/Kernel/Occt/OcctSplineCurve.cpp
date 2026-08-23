#include "Kernel/Occt/OcctSplineCurve.h"

#include "Kernel/Occt/OcctSplineInterpolation.h"

#include <GeomAPI_Interpolate.hxx>
#include <Precision.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>

namespace paramcad {

Handle(Geom_BSplineCurve) InterpolateSplineThrough(const std::vector<gp_Pnt>& points,
                                                   bool closed) {
    if (points.size() < 2) return {};

    Handle(TColgp_HArray1OfPnt) handles =
        new TColgp_HArray1OfPnt(1, static_cast<Standard_Integer>(points.size()));
    for (std::size_t i = 0; i < points.size(); ++i)
        handles->SetValue(static_cast<Standard_Integer>(i + 1), points[i]);

    GeomAPI_Interpolate interpolate(handles, closed ? Standard_True : Standard_False,
                                    Precision::Confusion());

    // The chord at each end, which is what Core's evaluator uses -- see the
    // header. Scale=true keeps the DIRECTION and lets OCCT choose the
    // magnitude: the chord's length is an artefact of where the neighbouring
    // point happens to be, and forcing it would distort the first span.
    //
    // A CLOSED spline has no ends to condition; it is periodic, and OCCT
    // closes it smoothly on its own.
    if (!closed) {
        const gp_Vec initial(points[0], points[1]);
        const gp_Vec final(points[points.size() - 2], points[points.size() - 1]);
        if (initial.Magnitude() > Precision::Confusion() &&
            final.Magnitude() > Precision::Confusion())
            interpolate.Load(initial, final, Standard_True);
    }

    interpolate.Perform();
    if (!interpolate.IsDone()) return {};
    return interpolate.Curve();
}

SplineEndDirections SplineEndDirectionsOf(const std::vector<Vec2>& points, bool closed) {
    // Interpolated on the world XY plane, so (u, v) and (x, y) are the same
    // numbers and nothing has to be mapped back. The end direction of a curve
    // does not depend on which plane it was built on.
    std::vector<gp_Pnt> world;
    world.reserve(points.size());
    for (const Vec2& point : points) world.emplace_back(point.x, point.y, 0.0);

    const Handle(Geom_BSplineCurve) curve = InterpolateSplineThrough(world, closed);
    if (curve.IsNull()) return {};

    const auto unitAt = [&](double parameter) {
        gp_Pnt where;
        gp_Vec along;
        curve->D1(parameter, where, along);
        const double length = along.Magnitude();
        if (length < Precision::Confusion()) return Vec2{};
        return Vec2{along.X() / length, along.Y() / length};
    };

    SplineEndDirections out;
    out.atStart = unitAt(curve->FirstParameter());
    out.atEnd = unitAt(curve->LastParameter());
    out.ok = std::hypot(out.atStart.x, out.atStart.y) > 0.5 &&
             std::hypot(out.atEnd.x, out.atEnd.y) > 0.5;
    return out;
}

} // namespace paramcad
