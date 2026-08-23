#include "Kernel/Occt/OcctSketchWireframe.h"

#include "Kernel/Occt/OcctShape.h"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include "Kernel/Occt/OcctSplineInterpolation.h"

#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Precision.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <memory>
#include <variant>

namespace paramcad {

namespace {

gp_Pnt ToWorld(const ProfilePlane& plane, Vec2 uv) {
    // The same mapping SketchFrame::toWorld performs, expressed in the
    // kernel-neutral plane the caller handed over. It is not a second
    // convention: ProfilePlane's axes come from PlaneOfSketchFrame, which is
    // the one conversion site.
    return gp_Pnt(plane.origin.x + plane.uAxis.x * uv.x + plane.vAxis.x * uv.y,
                  plane.origin.y + plane.uAxis.y * uv.x + plane.vAxis.y * uv.y,
                  plane.origin.z + plane.uAxis.z * uv.x + plane.vAxis.z * uv.y);
}

// The circle a sketch circle or arc lies on, oriented so that OCCT's parameter
// zero is the sketch's +u axis and its parameters increase counter-clockwise
// about the sketch normal. That is what lets the stored angles be passed
// straight through: any other axis placement would need every angle converted,
// and a conversion applied in one branch and forgotten in another is how an
// arc ends up drawn on the wrong side of its chord.
gp_Circ CircleOn(const ProfilePlane& plane, Vec2 centre, double radiusMm) {
    const gp_Ax2 axes(ToWorld(plane, centre),
                      gp_Dir(plane.normal.x, plane.normal.y, plane.normal.z),
                      gp_Dir(plane.uAxis.x, plane.uAxis.y, plane.uAxis.z));
    return gp_Circ(axes, radiusMm);
}

// The same idea for an ellipse: gp_Ax2's X direction is where the MAJOR axis
// points, so the sketch's rotation is applied by turning that direction rather
// than by rotating anything afterwards -- and OCCT's parameter is then exactly
// the parameter the sketch stores.
gp_Elips EllipseOn(const ProfilePlane& plane, Vec2 centre, double major, double minor,
                   double rotation) {
    const gp_Dir normal(plane.normal.x, plane.normal.y, plane.normal.z);
    const gp_Dir uDir(plane.uAxis.x, plane.uAxis.y, plane.uAxis.z);
    const gp_Dir vDir(plane.vAxis.x, plane.vAxis.y, plane.vAxis.z);
    const gp_Dir majorDir(uDir.XYZ() * std::cos(rotation) + vDir.XYZ() * std::sin(rotation));
    return gp_Elips(gp_Ax2(ToWorld(plane, centre), normal, majorDir), major, minor);
}

} // namespace

SketchWireframe BuildSketchWireframe(const std::vector<SketchGeometry>& geometry,
                                     const ProfilePlane& plane) {
    SketchWireframe result;

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);

    for (const SketchGeometry& item : geometry) {
        // Standard_Failure is caught per ENTITY rather than around the whole
        // loop: one entity OCCT refuses must not cost the user the other
        // nineteen. It is counted as skipped, which is what `skipped` is for.
        try {
            if (const auto* point = std::get_if<SketchPoint>(&item)) {
                BRepBuilderAPI_MakeVertex maker(ToWorld(plane, point->position));
                if (!maker.IsDone()) {
                    ++result.skipped;
                    continue;
                }
                builder.Add(compound, maker.Vertex());
                ++result.vertices;
            } else if (const auto* line = std::get_if<SketchLine>(&item)) {
                BRepBuilderAPI_MakeEdge maker(ToWorld(plane, line->start),
                                              ToWorld(plane, line->end));
                if (!maker.IsDone()) {
                    ++result.skipped; // zero length, which OCCT refuses outright
                    continue;
                }
                builder.Add(compound, maker.Edge());
                ++result.edges;
            } else if (const auto* circle = std::get_if<SketchCircle>(&item)) {
                if (circle->radiusMm <= kSketchToleranceMm) {
                    ++result.skipped;
                    continue;
                }
                BRepBuilderAPI_MakeEdge maker(CircleOn(plane, circle->center, circle->radiusMm));
                if (!maker.IsDone()) {
                    ++result.skipped;
                    continue;
                }
                builder.Add(compound, maker.Edge());
                ++result.edges;
            } else if (const auto* arc = std::get_if<SketchArc>(&item)) {
                if (arc->radiusMm <= kSketchToleranceMm) {
                    ++result.skipped;
                    continue;
                }
                // OCCT sweeps from the first parameter to the second going
                // counter-clockwise about the axis. A clockwise sketch arc is
                // the same curve traversed the other way, so its tips are
                // swapped rather than its axis flipped -- flipping the axis
                // would also flip which side of the plane the arc's normal
                // faces, and this shape is drawn, not solved.
                const double first = arc->counterClockwise ? arc->startAngleRad
                                                           : arc->endAngleRad;
                const double second = arc->counterClockwise ? arc->endAngleRad
                                                            : arc->startAngleRad;
                BRepBuilderAPI_MakeEdge maker(CircleOn(plane, arc->center, arc->radiusMm), first,
                                              second);
                if (!maker.IsDone()) {
                    ++result.skipped;
                    continue;
                }
                builder.Add(compound, maker.Edge());
                ++result.edges;
            } else if (const auto* spline = std::get_if<SketchSpline>(&item)) {
                if (spline->points.size() < kMinSplinePoints) {
                    ++result.skipped;
                    continue;
                }
                // THROUGH THE SHARED INTERPOLATION, not a second copy of it.
                // This drew the preview and OcctGeometryKernel drew the solid,
                // from two separately written calls -- so the moment one of
                // them learned about end tangents, the picture and the part
                // stopped being the same curve.
                std::vector<gp_Pnt> world;
                world.reserve(spline->points.size());
                for (const Vec2& point : spline->points) world.push_back(ToWorld(plane, point));
                const Handle(Geom_BSplineCurve) curve =
                    InterpolateSplineThrough(world, spline->closed);
                if (curve.IsNull()) {
                    ++result.skipped;
                    continue;
                }
                BRepBuilderAPI_MakeEdge maker(curve);
                if (!maker.IsDone()) {
                    ++result.skipped;
                    continue;
                }
                builder.Add(compound, maker.Edge());
                ++result.edges;
            } else if (const auto* full = std::get_if<SketchEllipse>(&item)) {
                if (full->minorRadiusMm <= kSketchToleranceMm ||
                    full->majorRadiusMm < full->minorRadiusMm) {
                    ++result.skipped;
                    continue;
                }
                BRepBuilderAPI_MakeEdge maker(EllipseOn(plane, full->center,
                                                        full->majorRadiusMm,
                                                        full->minorRadiusMm,
                                                        full->rotationRad));
                if (!maker.IsDone()) {
                    ++result.skipped;
                    continue;
                }
                builder.Add(compound, maker.Edge());
                ++result.edges;
            } else if (const auto* piece = std::get_if<SketchEllipticalArc>(&item)) {
                if (piece->minorRadiusMm <= kSketchToleranceMm ||
                    piece->majorRadiusMm < piece->minorRadiusMm) {
                    ++result.skipped;
                    continue;
                }
                // Tips swapped for a clockwise sweep, exactly as the circular
                // arc above does and for the same reason: flipping the axis
                // instead would also flip which side of the plane the curve's
                // normal faces.
                const double first = piece->counterClockwise ? piece->startParamRad
                                                             : piece->endParamRad;
                const double second = piece->counterClockwise ? piece->endParamRad
                                                              : piece->startParamRad;
                BRepBuilderAPI_MakeEdge maker(
                    EllipseOn(plane, piece->center, piece->majorRadiusMm, piece->minorRadiusMm,
                              piece->rotationRad),
                    first, second);
                if (!maker.IsDone()) {
                    ++result.skipped;
                    continue;
                }
                builder.Add(compound, maker.Edge());
                ++result.edges;
            } else {
                ++result.skipped;
            }
        } catch (const Standard_Failure&) {
            ++result.skipped;
        }
    }

    // An EMPTY compound is not a shape worth handing back: the viewer would
    // display a presentation with nothing in it, which is a scene object a user
    // can select and cannot see.
    if (result.empty()) return result;
    result.shape = KernelShape{std::make_shared<OcctShape>(compound)};
    return result;
}

KernelBounds BoundsOf(const KernelShape& shape) {
    KernelBounds bounds;
    const auto* occt = dynamic_cast<const OcctShape*>(shape.handle());
    if (occt == nullptr || occt->shape().IsNull()) return bounds;

    Bnd_Box box;
    try {
        BRepBndLib::Add(occt->shape(), box);
    } catch (const Standard_Failure&) {
        return bounds;
    }
    if (box.IsVoid()) return bounds;

    Standard_Real xMin = 0.0, yMin = 0.0, zMin = 0.0, xMax = 0.0, yMax = 0.0, zMax = 0.0;
    box.Get(xMin, yMin, zMin, xMax, yMax, zMax);
    bounds.ok = true;
    bounds.min = Vec3{xMin, yMin, zMin};
    bounds.max = Vec3{xMax, yMax, zMax};
    return bounds;
}

} // namespace paramcad
