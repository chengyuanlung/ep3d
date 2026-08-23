#include "Kernel/Occt/OcctSplineCurve.h"

#include "Kernel/Occt/OcctSplineInterpolation.h"

#include <GeomAPI_Interpolate.hxx>
#include <Precision.hxx>
#include <TColStd_HArray1OfBoolean.hxx>
#include <TColgp_Array1OfVec.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>

namespace paramcad {

Handle(Geom_BSplineCurve) InterpolateSplineThrough(const std::vector<gp_Pnt>& points,
                                                   bool closed,
                                                   const std::map<int, gp_Vec>& handles) {
    if (points.size() < 2) return {};
    const Standard_Integer count = static_cast<Standard_Integer>(points.size());

    Handle(TColgp_HArray1OfPnt) through = new TColgp_HArray1OfPnt(1, count);
    for (std::size_t i = 0; i < points.size(); ++i)
        through->SetValue(static_cast<Standard_Integer>(i + 1), points[i]);

    GeomAPI_Interpolate interpolate(through, closed ? Standard_True : Standard_False,
                                    Precision::Confusion());

    // WHICH TANGENTS THIS CURVE IS TOLD, and which are left to OCCT.
    //
    // A point with a HANDLE is told its handle -- that is what a handle is. An
    // END without one is told the chord, because Core's Hermite evaluator uses
    // the chord there (ADR-M18-001). Everything else is left unset, which is
    // what lets OCCT run a smooth curve through the middle.
    //
    // A CLOSED spline has no ends to condition; it is periodic, and OCCT closes
    // it smoothly on its own. Its handled points still get theirs.
    TColgp_Array1OfVec tangents(1, count);
    Handle(TColStd_HArray1OfBoolean) flags = new TColStd_HArray1OfBoolean(1, count);
    for (Standard_Integer i = 1; i <= count; ++i) flags->SetValue(i, Standard_False);

    const auto tell = [&](int index, const gp_Vec& direction) {
        if (direction.Magnitude() <= Precision::Confusion()) return;
        tangents.SetValue(index + 1, direction);
        flags->SetValue(index + 1, Standard_True);
    };
    if (!closed) {
        if (handles.find(0) == handles.end()) tell(0, gp_Vec(points[0], points[1]));
        const int last = static_cast<int>(points.size()) - 1;
        if (handles.find(last) == handles.end())
            tell(last, gp_Vec(points[static_cast<std::size_t>(last - 1)],
                              points[static_cast<std::size_t>(last)]));
    }
    for (const auto& [index, direction] : handles) {
        if (index < 0 || index >= static_cast<int>(points.size())) continue;
        tell(index, direction);
    }

    bool anyTold = false;
    for (Standard_Integer i = 1; i <= count; ++i)
        if (flags->Value(i)) anyTold = true;
    // Scale=FALSE: the MAGNITUDE is honoured, not just the direction.
    //
    // Core's Hermite uses the tangent's length -- it is how hard the curve is
    // pulled that way before it turns, and for a handle it is half of what the
    // user set. Letting OCCT rescale would give the solid a different shape
    // from the sketch through the same points, which is the disagreement this
    // whole file exists to close.
    if (anyTold) interpolate.Load(tangents, flags, Standard_False);

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
