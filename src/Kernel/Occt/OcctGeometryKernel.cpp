#include "Kernel/Occt/OcctGeometryKernel.h"
#include <set>
#include <gp_Lin.hxx>
#include <GeomAbs_CurveType.hxx>
#include <BRepAdaptor_Curve.hxx>
#include "Kernel/Occt/OcctFaceQuery.h"
#include "Kernel/Occt/OcctProvenance.h"
#include "Core/Kernel/EdgeQuery.h"
#include "Kernel/Occt/OcctShape.h"
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepGProp.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Trsf.hxx>
#include <BRepLib.hxx>
#include <TopoDS_Solid.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Elips.hxx>
#include "Kernel/Occt/OcctSplineInterpolation.h"

#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Precision.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <gp_Mat.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <cmath>
#include <variant>
#include <memory>
#include <string>
#include <typeinfo>
#include <utility>

namespace paramcad {

namespace {

// Standard_Failure::GetMessageString() is frequently empty -- several OCCT
// primitives raise a typed exception carrying no text at all, which would
// leave a diagnostic that is nothing but its own prefix. Falling back to the
// exception's dynamic type name keeps every failure identifiable.
//
// typeid rather than OCCT's own DynamicType(): as of OCCT 8.0.1
// Standard_Failure derives from std::exception, not Standard_Transient, so it
// carries no DynamicType() at all (Standard_Failure.hxx:28).
std::string describe(const Standard_Failure& failure) {
    const char* message = failure.GetMessageString();
    if (message != nullptr && *message != '\0') return message;
    return std::string(typeid(failure).name()) + " (no message text)";
}

} // namespace

ShapeResult OcctGeometryKernel::createBox(const BoxDefinition& definition) {
    // The ONE place dimension validation lives (ADR-M3-001): every kernel,
    // real or fake, calls this first, so zero/negative/NaN/infinite
    // dimensions never reach BRepPrimAPI_MakeBox.
    if (!IsValidBoxDefinition(definition)) {
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "invalid box definition: every dimension must be finite and at least " +
                               std::to_string(kMinBoxDimensionMm) + " mm"};
    }

    // Corner-origin convention (spec 8): origin (0,0,0), Width +X, Height +Y,
    // Depth +Z. BRepPrimAPI_MakeBox(dx, dy, dz) builds exactly this box.
    //
    // Build() is MANDATORY before IsDone()/Shape(): BRepPrimAPI_MakeBox's
    // constructor only stores the parameters, and IsDone() reports the
    // BRepBuilderAPI_Command "done" flag that only Build() sets ("Stores the
    // solid in myShape" -- BRepPrimAPI_MakeBox.hxx). Constructing and querying
    // IsDone() without Build() therefore always reports failure, and Shape()
    // would raise StdFail_NotDone.
    //
    // Standard_Failure is caught to honour this interface's documented "never
    // throws, never UB" contract (ADR-M3-001, spec 7): validation above already
    // rejects every expected-invalid input, so reaching the handler means an
    // unforeseen OCCT-internal failure, which must still surface as a
    // structured result rather than unwinding into Core.
    try {
        BRepPrimAPI_MakeBox maker(definition.widthMm, definition.heightMm, definition.depthMm);
        maker.Build();
        if (!maker.IsDone()) {
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "OCCT failed to construct the box primitive"};
        }

        auto handle = std::make_shared<OcctShape>(maker.Shape());
        return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
    } catch (const Standard_Failure& failure) {
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           std::string("OCCT raised while constructing the box primitive: ") +
                               describe(failure)};
    }
}

namespace {

// The wire-and-face half of profile construction, shared by extrudeProfile and
// revolveProfile (M8.2) -- extracted for the same reason BuildKernelProfile was
// promoted in Core during M8.1: two copies of edge construction, above all the
// arc-direction handling, would be two places to disagree about geometry.
//
// On failure fills `error` and returns a null face; the caller owns the
// try/catch, exactly as before. May throw OCCT exceptions like the code it was
// extracted from -- callers already catch Standard_Failure.
// ONE wire builder, for every chain this kernel ever builds: a profile's outer
// boundary, each of its holes, and -- since M19 -- a sweep's spine.
//
// `requireClosed` is the only thing that differs between them, and it is a
// parameter rather than a second copy of the function. Two copies of the
// edge-building switch would be two places to disagree about how an arc is
// trimmed or which way a reversed spline's handles point, and that is a
// geometry bug that reads as a solver bug.
//
// Reads `plane` rather than a whole profile, because a path has no holes and
// no outer boundary -- it has segments and a plane, which is all this needs.
bool BuildWireOnPlane(const ProfilePlane& profilePlane,
                      const std::vector<ProfileSegment>& segments, bool requireClosed,
                      TopoDS_Wire& out, std::string& error) {
    // The sketch plane, expressed to OCCT exactly as Core computed it --
    // Kernel/Occt never re-derives the frame, so SketchFrame stays the
    // single conversion site (ADR-M4-002).
    const gp_Pnt origin(profilePlane.origin.x, profilePlane.origin.y,
                        profilePlane.origin.z);
    const gp_Dir normal(profilePlane.normal.x, profilePlane.normal.y,
                        profilePlane.normal.z);
    const gp_Dir uDir(profilePlane.uAxis.x, profilePlane.uAxis.y, profilePlane.uAxis.z);
    const gp_Dir vDir(profilePlane.vAxis.x, profilePlane.vAxis.y, profilePlane.vAxis.z);
    const gp_Ax2 axes(origin, normal, uDir);
    const gp_Pln plane(axes);

    // (u,v) -> part-local XYZ, using the same basis Core handed over.
    const auto toWorld = [&](Vec2 uv) {
        return gp_Pnt(profilePlane.origin.x + uv.x * profilePlane.uAxis.x +
                          uv.y * profilePlane.vAxis.x,
                      profilePlane.origin.y + uv.x * profilePlane.uAxis.y +
                          uv.y * profilePlane.vAxis.y,
                      profilePlane.origin.z + uv.x * profilePlane.uAxis.z +
                          uv.y * profilePlane.vAxis.z);
    };

    // An ellipse's supporting curve, in the sketch plane.
    //
    // gp_Ax2's X direction is where the ellipse's MAJOR axis points, so the
    // rotation is applied by turning that direction rather than by rotating
    // anything afterwards -- one place, and the same convention the sketch
    // stores.
    const auto ellipseOf = [&](Vec2 centre, double major, double minor, double rotation) {
        const gp_Dir major2d(uDir.XYZ() * std::cos(rotation) + vDir.XYZ() * std::sin(rotation));
        return gp_Elips(gp_Ax2(toWorld(centre), normal, major2d), major, minor);
    };

    // The B-spline THROUGH a list of points. The interpolation itself lives in
    // InterpolateSplineThrough -- ONE copy, shared with the wireframe the 3D
    // view draws, so a preview and the solid it previews cannot show different
    // curves through the same points.
    const auto splineThrough = [&](const std::vector<Vec2>& points, bool closed,
                                   const std::map<int, Vec2>& handles)
        -> Handle(Geom_BSplineCurve) {
        std::vector<gp_Pnt> world;
        world.reserve(points.size());
        for (const Vec2& point : points) world.push_back(toWorld(point));
        // A TANGENT IS A DIRECTION, so it is rotated onto the plane but NOT
        // translated onto it: the difference of two placed points, which drops
        // the origin the way subtracting always does.
        const gp_Pnt origin = toWorld(Vec2{0.0, 0.0});
        std::map<int, gp_Vec> placed;
        for (const auto& [index, tangent] : handles)
            placed.emplace(index, gp_Vec(origin, toWorld(tangent)));
        return InterpolateSplineThrough(world, closed, placed);
    };

    // ONE wire builder, used for the outer boundary and for every hole. The
    // loops differ in what they MEAN, not in how they are built, and two copies
    // of the edge-building switch would be two places to disagree about how an
    // arc is trimmed.
    BRepBuilderAPI_MakeWire wireMaker;
    for (const ProfileSegment& segment : segments) {
        if (const auto* line = std::get_if<ProfileLineSegment>(&segment)) {
            BRepBuilderAPI_MakeEdge edge(toWorld(line->start), toWorld(line->end));
            edge.Build();
            if (!edge.IsDone()) {
                error = "OCCT could not build a line edge for the profile";
                return false;
            }
            wireMaker.Add(edge.Edge());
        } else if (const auto* arc = std::get_if<ProfileArcSegment>(&segment)) {
            // Build the arc's supporting circle in the sketch plane, then
            // trim it by parameter. Angles are measured from +u, which is
            // exactly the gp_Ax2 reference direction set above, so no angle
            // conversion is needed.
            const gp_Circ circle(gp_Ax2(toWorld(arc->center), normal, uDir), arc->radiusMm);
            const double first = arc->counterClockwise ? arc->startAngleRad : arc->endAngleRad;
            const double last = arc->counterClockwise ? arc->endAngleRad : arc->startAngleRad;
            BRepBuilderAPI_MakeEdge edge(circle, first, last);
            edge.Build();
            if (!edge.IsDone()) {
                error = "OCCT could not build an arc edge for the profile";
                return false;
            }
            wireMaker.Add(edge.Edge());
        } else if (const auto* spline = std::get_if<ProfileSplineSegment>(&segment)) {
            const Handle(Geom_BSplineCurve) curve =
                splineThrough(spline->points, spline->closed, spline->handles);
            if (curve.IsNull()) {
                error = "OCCT could not interpolate a spline through those points";
                return false;
            }
            BRepBuilderAPI_MakeEdge edge(curve);
            edge.Build();
            if (!edge.IsDone()) {
                error = "OCCT could not build a spline edge for the profile";
                return false;
            }
            wireMaker.Add(edge.Edge());
        } else if (const auto* ellipse = std::get_if<ProfileEllipseSegment>(&segment)) {
            BRepBuilderAPI_MakeEdge edge(ellipseOf(ellipse->center, ellipse->majorRadiusMm,
                                                   ellipse->minorRadiusMm,
                                                   ellipse->rotationRad));
            edge.Build();
            if (!edge.IsDone()) {
                error = "OCCT could not build an elliptical edge for the profile";
                return false;
            }
            wireMaker.Add(edge.Edge());
        } else if (const auto* piece = std::get_if<ProfileEllipticalArcSegment>(&segment)) {
            // Trimmed by PARAMETER, which is the same number this project
            // stores: OCCT's gp_Elips is parametrised exactly as
            // centre + R(rot)*(a cos t, b sin t), so there is nothing to
            // convert and nothing to get wrong. Feeding it a geometric angle
            // would land the ends in the right places and cover the wrong
            // piece of curve between them.
            const gp_Elips support = ellipseOf(piece->center, piece->majorRadiusMm,
                                               piece->minorRadiusMm, piece->rotationRad);
            const double first = piece->counterClockwise ? piece->startParamRad
                                                         : piece->endParamRad;
            const double last = piece->counterClockwise ? piece->endParamRad
                                                        : piece->startParamRad;
            BRepBuilderAPI_MakeEdge edge(support, first, last);
            edge.Build();
            if (!edge.IsDone()) {
                error = "OCCT could not build an elliptical arc edge for the profile";
                return false;
            }
            wireMaker.Add(edge.Edge());
        } else {
            const auto& full = std::get<ProfileCircleSegment>(segment);
            const gp_Circ circle(gp_Ax2(toWorld(full.center), normal, uDir), full.radiusMm);
            BRepBuilderAPI_MakeEdge edge(circle);
            edge.Build();
            if (!edge.IsDone()) {
                error = "OCCT could not build a circular edge for the profile";
                return false;
            }
            wireMaker.Add(edge.Edge());
        }
    }

    wireMaker.Build();
    if (!wireMaker.IsDone()) {
        error = "OCCT could not assemble the profile edges into a wire";
        return false;
    }
    out = wireMaker.Wire();
    // A PATH IS ALLOWED TO HAVE ENDS. A profile's boundary is not, and that
    // check is what stops a face being built from a chain with a gap in it --
    // OCCT would accept the gap and hand back a solid nobody asked for.
    if (requireClosed && !out.Closed()) {
        error = "profile wire is not closed";
        return false;
    }
    return true;
}

// ONE closed wire, swept along a spine, as a SOLID (M19).
//
// BRepOffsetAPI_MakePipeShell rather than MakePipe, and that is the whole
// lesson of this function. MakePipe given a face sweeps its BOUNDARY: the side
// walls come out and the two caps do not, so the result is an open shell whose
// volume integrates to about zero. Downstream nothing notices -- mass
// properties return a number, the viewer draws faces -- and the only symptom is
// a bent pipe that weighs nothing.
//
// MakePipeShell::MakeSolid() closes it properly, capping both ends. Its default
// law is corrected Frenet, which keeps the section upright round a bend instead
// of letting it spin with the curve's torsion.
TopoDS_Shape SweepWireToSolid(const TopoDS_Wire& spine, const TopoDS_Wire& section,
                              std::string& error) {
    BRepOffsetAPI_MakePipeShell pipe(spine);
    pipe.Add(section, Standard_False, Standard_False);
    pipe.Build();
    if (!pipe.IsDone()) {
        error = "OCCT could not sweep that section along that path";
        return TopoDS_Shape();
    }
    if (!pipe.MakeSolid()) {
        error = "the sweep produced a surface that could not be closed into a solid";
        return TopoDS_Shape();
    }
    return pipe.Shape();
}

TopoDS_Face BuildFaceForProfile(const PlanarProfileDefinition& profile, std::string& error) {
    const gp_Pnt origin(profile.plane.origin.x, profile.plane.origin.y, profile.plane.origin.z);
    const gp_Dir normal(profile.plane.normal.x, profile.plane.normal.y, profile.plane.normal.z);
    const gp_Dir uDir(profile.plane.uAxis.x, profile.plane.uAxis.y, profile.plane.uAxis.z);
    const gp_Pln plane(gp_Ax3(origin, normal, uDir));

    TopoDS_Wire outer;
    if (!BuildWireOnPlane(profile.plane, profile.segments, true, outer, error))
        return TopoDS_Face();

    BRepBuilderAPI_MakeFace faceMaker(plane, outer);
    faceMaker.Build();
    if (!faceMaker.IsDone()) {
        error = "OCCT could not build a planar face from the profile";
        return TopoDS_Face();
    }

    // HOLES. Each inner wire is added REVERSED: OCCT reads a face's boundary
    // orientation to tell material from void, so an inner wire wound the same
    // way as the outer one describes a second outer boundary rather than a
    // hole -- and the result is a face OCCT accepts and a solid nobody wanted.
    for (const std::vector<ProfileSegment>& inner : profile.innerLoops) {
        TopoDS_Wire hole;
        if (!BuildWireOnPlane(profile.plane, inner, true, hole, error)) return TopoDS_Face();
        faceMaker.Add(TopoDS::Wire(hole.Reversed()));
        faceMaker.Build();
        if (!faceMaker.IsDone()) {
            error = "OCCT could not cut a hole into the profile face";
            return TopoDS_Face();
        }
    }
    return faceMaker.Face();
}

} // namespace

ShapeResult OcctGeometryKernel::extrudeProfile(const PlanarProfileDefinition& profile,
                                               double distanceMm) {
    // Single validation site (ADR-M3-001 extended to profiles): every kernel,
    // real or fake, calls these first, so degenerate input never reaches OCCT
    // and always surfaces as a structured InvalidDimension.
    if (!IsValidProfileDefinition(profile)) {
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "invalid profile definition: empty, non-finite, or degenerate "
                           "plane/segment"};
    }
    // SIGNED (M17.8, ADR-M17-031): a negative distance extrudes to the other
    // side of the plane. Only the magnitude has to clear the floor -- the sign
    // is a direction, not a size.
    if (!IsValidSignedExtrusionDistance(distanceMm)) {
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "invalid extrusion distance: must be finite and at least " +
                               std::to_string(kMinExtrusionDistanceMm) +
                               " mm away from zero (negative extrudes the other way)"};
    }

    try {
        std::string error;
        const TopoDS_Face face = BuildFaceForProfile(profile, error);
        if (face.IsNull())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed, error};

        // Extrude along the plane normal (spec 12: +sketch normal in M4).
        const gp_Vec direction(profile.plane.normal.x * distanceMm,
                               profile.plane.normal.y * distanceMm,
                               profile.plane.normal.z * distanceMm);
        BRepPrimAPI_MakePrism prism(face, direction);
        prism.Build();
        if (!prism.IsDone())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "OCCT could not extrude the profile face"};

        auto handle = std::make_shared<OcctShape>(prism.Shape());
        return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
    } catch (const Standard_Failure& failure) {
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           std::string("OCCT raised while extruding the profile: ") +
                               describe(failure)};
    }
}

ShapeResult OcctGeometryKernel::sweepProfile(const PlanarProfileDefinition& profile,
                                             const PlanarPathDefinition& path) {
    // Single validation site, as everywhere: degenerate input never reaches
    // OCCT and always surfaces as a structured InvalidDimension.
    if (!IsValidProfileDefinition(profile))
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "sweep profile is empty or degenerate"};
    if (!IsValidPathDefinition(path))
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "sweep path is empty or degenerate"};

    const auto fail = [](KernelError code, std::string message) {
        return ShapeResult{KernelShape{}, code, std::move(message)};
    };

    try {
        std::string error;
        TopoDS_Wire spine;
        if (!BuildWireOnPlane(path.plane, path.segments, path.closed, spine, error))
            return fail(KernelError::GeometryConstructionFailed,
                        error.empty() ? "could not build the sweep path" : error);

        TopoDS_Wire outer;
        if (!BuildWireOnPlane(profile.plane, profile.segments, true, outer, error))
            return fail(KernelError::GeometryConstructionFailed,
                        error.empty() ? "could not build the sweep profile" : error);

        TopoDS_Shape swept = SweepWireToSolid(spine, outer, error);
        if (swept.IsNull())
            return fail(KernelError::GeometryConstructionFailed,
                        error.empty() ? "the sweep produced no shape" : error);

        // HOLES ARE SWEPT AND SUBTRACTED, not carried along inside a face.
        //
        // A swept FACE loses its inner loops the moment the spine bends, so the
        // bore has to be made the same way the outside was -- its own pipe --
        // and then cut. That is also exactly what the hole means: the volume
        // the section's inner loop carves out along the same path.
        for (const std::vector<ProfileSegment>& inner : profile.innerLoops) {
            TopoDS_Wire hole;
            if (!BuildWireOnPlane(profile.plane, inner, true, hole, error))
                return fail(KernelError::GeometryConstructionFailed,
                            error.empty() ? "could not build a hole in the sweep profile"
                                          : error);
            const TopoDS_Shape bore = SweepWireToSolid(spine, hole, error);
            if (bore.IsNull())
                return fail(KernelError::GeometryConstructionFailed,
                            error.empty() ? "could not sweep a hole along the path" : error);
            BRepAlgoAPI_Cut cut(swept, bore);
            cut.Build();
            if (!cut.IsDone())
                return fail(KernelError::GeometryConstructionFailed,
                            "OCCT could not cut the swept hole out of the sweep");
            swept = cut.Shape();
        }

        // A SWEEP THAT WEIGHS NOTHING is a shell that was never closed, and it
        // is indistinguishable from success everywhere downstream. Refused here
        // instead of handed on.
        GProp_GProps volumeProps;
        BRepGProp::VolumeProperties(swept, volumeProps);
        if (!(std::fabs(volumeProps.Mass()) > kMinExtrusionDistanceMm))
            return fail(KernelError::GeometryConstructionFailed,
                        "the sweep produced a surface rather than a solid");

        auto handle = std::make_shared<OcctShape>(swept);
        return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
    } catch (const Standard_Failure& failure) {
        return fail(KernelError::GeometryConstructionFailed,
                    std::string("OCCT refused the sweep: ") +
                        (failure.GetMessageString() != nullptr ? failure.GetMessageString()
                                                               : "no message"));
    }
}

ShapeResult OcctGeometryKernel::loftProfiles(
    const std::vector<PlanarProfileDefinition>& profiles) {
    // TWO OR MORE. A loft through one profile is not an extrusion by another
    // name -- it has no second section to run to -- and answering with
    // something is worse than saying so.
    if (profiles.size() < 2)
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "a loft needs at least two profiles"};
    for (const PlanarProfileDefinition& one : profiles) {
        if (!IsValidProfileDefinition(one))
            return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                               "a loft profile is empty or degenerate"};
        // HOLES ARE REFUSED, and said rather than dropped. ThruSections runs
        // through WIRES, so there is nowhere for an inner loop to go: it would
        // be silently ignored and the loft would come back solid where the user
        // drew a hole. Lofting holes needs each section's inner loops paired up
        // with the next section's, which is a decision this does not make yet.
        if (!one.innerLoops.empty())
            return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                               "a loft profile cannot have holes yet: each section's holes "
                               "would have to be paired with the next section's"};
    }

    try {
        // isSolid, and NOT ruled: ruled joins the sections with straight
        // segments, which is a different shape and a worse default -- a loft
        // through three sections is normally wanted smooth.
        BRepOffsetAPI_ThruSections generator(Standard_True, Standard_False,
                                             Precision::Confusion());
        for (const PlanarProfileDefinition& one : profiles) {
            std::string error;
            TopoDS_Wire wire;
            if (!BuildWireOnPlane(one.plane, one.segments, true, wire, error))
                return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                                   error.empty() ? "could not build a loft section" : error};
            generator.AddWire(wire);
        }
        generator.Build();
        if (!generator.IsDone())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "OCCT could not loft through those sections"};

        const TopoDS_Shape lofted = generator.Shape();
        if (lofted.IsNull())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "the loft produced no shape"};
        auto handle = std::make_shared<OcctShape>(lofted);
        return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
    } catch (const Standard_Failure& failure) {
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           std::string("OCCT refused the loft: ") +
                               (failure.GetMessageString() != nullptr
                                    ? failure.GetMessageString()
                                    : "no message")};
    }
}

ShapeResult OcctGeometryKernel::revolveProfile(const PlanarProfileDefinition& profile,
                                               const Vec3& axisOriginMm,
                                               const Vec3& axisDirection, double angleRad) {
    if (!IsValidProfileDefinition(profile)) {
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "invalid profile definition: empty, non-finite, or degenerate "
                           "plane/segment"};
    }
    if (!IsValidRevolveAngle(angleRad)) {
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "invalid revolve angle: must be finite and in (0, 2*pi] radians"};
    }
    const double axisLength =
        std::sqrt(axisDirection.x * axisDirection.x + axisDirection.y * axisDirection.y +
                  axisDirection.z * axisDirection.z);
    if (!std::isfinite(axisOriginMm.x) || !std::isfinite(axisOriginMm.y) ||
        !std::isfinite(axisOriginMm.z) || !std::isfinite(axisLength) ||
        axisLength < kMinExtrusionDistanceMm) {
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "invalid revolve axis: origin and direction must be finite and the "
                           "direction non-degenerate"};
    }

    try {
        std::string error;
        const TopoDS_Face face = BuildFaceForProfile(profile, error);
        if (face.IsNull())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed, error};

        const gp_Ax1 axis(gp_Pnt(axisOriginMm.x, axisOriginMm.y, axisOriginMm.z),
                          gp_Dir(axisDirection.x, axisDirection.y, axisDirection.z));
        BRepPrimAPI_MakeRevol revol(face, axis, angleRad);
        revol.Build();
        if (!revol.IsDone())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "OCCT could not revolve the profile face"};

        auto handle = std::make_shared<OcctShape>(revol.Shape());
        return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
    } catch (const Standard_Failure& failure) {
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           std::string("OCCT raised while revolving the profile: ") +
                               describe(failure)};
    }
}

namespace {

// One place to turn a KernelShape into an OCCT shape, or say why it cannot be
// done -- the same dynamic_cast discipline every verb here uses (ADR-M3-001).
const OcctShape* AsOcct(const KernelShape& shape) {
    return dynamic_cast<const OcctShape*>(shape.handle());
}

} // namespace

ShapeResult OcctGeometryKernel::mirrorShape(const KernelShape& shape, const Vec3& planeOriginMm,
                                            const Vec3& planeNormal) {
    const OcctShape* occt = AsOcct(shape);
    if (occt == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "mirror input is not an OcctShape (null or foreign kernel)"};
    const double length = std::sqrt(planeNormal.x * planeNormal.x +
                                    planeNormal.y * planeNormal.y +
                                    planeNormal.z * planeNormal.z);
    if (!std::isfinite(length) || length < 1e-12 || !std::isfinite(planeOriginMm.x) ||
        !std::isfinite(planeOriginMm.y) || !std::isfinite(planeOriginMm.z))
        return ShapeResult{KernelShape{}, KernelError::NonFinite,
                           "mirror plane is degenerate or non-finite"};
    try {
        const gp_Ax2 plane(gp_Pnt(planeOriginMm.x, planeOriginMm.y, planeOriginMm.z),
                           gp_Dir(planeNormal.x, planeNormal.y, planeNormal.z));
        gp_Trsf mirror;
        mirror.SetMirror(plane); // reflection across the plane, not about its axis
        BRepBuilderAPI_Transform transform(occt->shape(), mirror, /*copy=*/Standard_True);
        if (!transform.IsDone())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "OCCT mirror transform did not complete"};

        // RE-ORIENT. A mirror reverses handedness, so the transformed solid's
        // shell normals point INWARD. Measured alone it still reports the right
        // volume and centre of mass -- which is exactly what makes this
        // dangerous -- but fusing it with another solid produces a shape whose
        // centre of mass is wrong while its VOLUME stays exactly right.
        //
        // M10's GATE_P caught it: two disjoint prisms, one mirrored, fused ->
        // volume 200000 exactly and the centroid off by 2%. Every volume oracle
        // in this project would have kept passing. A box happened not to show
        // it, which is why the gate that found this uses the extruded pad.
        TopoDS_Shape result = transform.Shape();
        if (result.ShapeType() == TopAbs_SOLID) {
            TopoDS_Solid solid = TopoDS::Solid(result);
            BRepLib::OrientClosedSolid(solid); // no-op when already outward
            result = solid;
        }
        auto handle = std::make_shared<OcctShape>(result);
        return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
    } catch (const Standard_Failure& error) {
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           std::string("OCCT mirror failed: ") + error.GetMessageString()};
    }
}

ShapeResult OcctGeometryKernel::translateShape(const KernelShape& shape, const Vec3& offsetMm) {
    const OcctShape* occt = AsOcct(shape);
    if (occt == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "translate input is not an OcctShape (null or foreign kernel)"};
    if (!std::isfinite(offsetMm.x) || !std::isfinite(offsetMm.y) || !std::isfinite(offsetMm.z))
        return ShapeResult{KernelShape{}, KernelError::NonFinite,
                           "translation offset is not finite"};
    try {
        gp_Trsf move;
        move.SetTranslation(gp_Vec(offsetMm.x, offsetMm.y, offsetMm.z));
        BRepBuilderAPI_Transform transform(occt->shape(), move, /*copy=*/Standard_True);
        if (!transform.IsDone())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "OCCT translate transform did not complete"};
        auto handle = std::make_shared<OcctShape>(transform.Shape());
        return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
    } catch (const Standard_Failure& error) {
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           std::string("OCCT translate failed: ") + error.GetMessageString()};
    }
}

ShapeResult OcctGeometryKernel::fuseShapes(const KernelShape& a, const KernelShape& b) {
    const OcctShape* occtA = AsOcct(a);
    const OcctShape* occtB = AsOcct(b);
    if (occtA == nullptr || occtB == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "fuse input is not an OcctShape (null or foreign kernel)"};
    try {
        BRepAlgoAPI_Fuse fuse(occtA->shape(), occtB->shape());
        if (!fuse.IsDone())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "OCCT boolean fuse did not complete"};
        // DISJOINT inputs give a compound, and its volume is the sum. That is a
        // legal result and is returned as an ordinary shape -- the same rule
        // subtractShape applies to a disjoint tool (M8 spec 6).
        auto handle = std::make_shared<OcctShape>(fuse.Shape());
        return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
    } catch (const Standard_Failure& error) {
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           std::string("OCCT fuse failed: ") + error.GetMessageString()};
    }
}

ShapeResult OcctGeometryKernel::subtractShape(const KernelShape& base, const KernelShape& tool) {
    // Same handle discipline as calculateMassProperties: dynamic_cast, never UB
    // on a null or foreign handle (ADR-M3-001).
    const auto* occtBase = dynamic_cast<const OcctShape*>(base.handle());
    if (occtBase == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "subtract base is not an OcctShape (null or foreign kernel)"};
    const auto* occtTool = dynamic_cast<const OcctShape*>(tool.handle());
    if (occtTool == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "subtract tool is not an OcctShape (null or foreign kernel)"};

    try {
        BRepAlgoAPI_Cut cut(occtBase->shape(), occtTool->shape());
        if (!cut.IsDone())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "OCCT boolean cut did not complete"};
        // A disjoint tool yields the base unchanged and a swallowing tool
        // yields an empty compound -- both LEGAL results (M8 spec 6), returned
        // as ordinary shapes rather than refused. The caller's mass properties
        // then read the true volume, including zero.
        auto handle = std::make_shared<OcctShape>(cut.Shape());
        return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
    } catch (const Standard_Failure& failure) {
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           std::string("OCCT raised while cutting: ") + describe(failure)};
    }
}


namespace {

// The ONE post-check site for both dress verbs. IsDone() is NOT sufficient,
// and a test had to fail to prove it: a fillet radius wider than half the
// part's thickness (15 on a 20mm slab) reported done while producing
// self-intersecting geometry. The analyzer is the check ChFi3d itself does
// not make. It used to be duplicated per verb -- and round 1 (R1-M4) deleted
// the chamfer's copy with every test staying green, exactly the divergence
// ADR-M8-006's shared Core base exists to prevent. One shared site means a
// guard added here cannot be forgotten on either twin.
ShapeResult AnalyzedDressResult(const TopoDS_Shape& shape, const char* noun,
                                const char* sizeNoun) {
    if (!BRepCheck_Analyzer(shape).IsValid())
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           std::string("the ") + noun + " result is not a valid solid; the " +
                               sizeNoun + " exceeds what the geometry accommodates"};
    auto handle = std::make_shared<OcctShape>(shape);
    return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
}

} // namespace

// --- Answering an edge query against real topology (M17.12, ADR-M17-034) ----
//
// The query is a sentence about which edges are wanted; this is where the
// sentence is answered, and it is answered AGAIN on every rebuild. Nothing
// here is stored: the map, the indices, the TopoDS handles all die with the
// call, which is the whole point -- transient topology is exactly what
// ADR-M4-004 forbids anyone from keeping as identity.
namespace {

double Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

// The face that lies furthest along `direction` AND faces that way.
//
// Two conditions, not one. "Furthest along +Z" alone would happily pick a
// vertical side face whose highest point is the top corner; a face has to
// FACE the direction to be the top face. The outward normal is what says so,
// and PlaneOfFace already accounts for orientation (M17_FQ_002).
TopoDS_Face ExtremeFace(const TopoDS_Shape& shape, Vec3 direction, bool& found) {
    found = false;
    TopoDS_Face best;
    double bestOffset = 0.0;
    constexpr double kFacing = 0.9; // within ~26 degrees of the asked direction

    for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
        const FacePlane plane = PlaneOfFace(it.Current());
        if (!plane.planar) continue;
        if (Dot(plane.normal, direction) < kFacing) continue;
        const double offset = Dot(plane.point, direction);
        if (found && offset <= bestOffset) continue;
        best = TopoDS::Face(it.Current());
        bestOffset = offset;
        found = true;
    }
    return best;
}

bool EdgeIsParallelTo(const TopoDS_Edge& edge, Vec3 direction) {
    BRepAdaptor_Curve curve(edge);
    if (curve.GetType() != GeomAbs_Line) return false; // only a straight edge has a direction
    const gp_Dir line = curve.Line().Direction();
    const double alignment =
        std::fabs(line.X() * direction.x + line.Y() * direction.y + line.Z() * direction.z);
    const double length = std::sqrt(Dot(direction, direction));
    if (length < 1e-12) return false;
    return alignment / length > 0.999;
}

// The edges one query names, as positions in `edges` -- the deduplicated map
// of the shape's edges, valid only for this call.
void CollectQuery(const TopoDS_Shape& shape, const KernelShape& carrier,
                  const TopTools_IndexedMapOfShape& edges, const EdgeQuery& query,
                  std::set<int>& into) {
    if (std::holds_alternative<AllEdges>(query)) {
        for (int i = 1; i <= edges.Extent(); ++i) into.insert(i);
        return;
    }
    if (const auto* face = std::get_if<EdgesOfExtremeFace>(&query)) {
        bool found = false;
        const TopoDS_Face target = ExtremeFace(shape, face->direction, found);
        if (!found) return; // no face faces that way: the query names nothing
        for (TopExp_Explorer it(target, TopAbs_EDGE); it.More(); it.Next()) {
            const int index = edges.FindIndex(it.Current());
            if (index > 0) into.insert(index);
        }
        return;
    }
    if (const auto* wanted = std::get_if<EdgesOfFace>(&query)) {
        // Resolved through the SAME function a tracked sketch uses, so "the
        // edges of the pocket floor" and "the sketch on the pocket floor"
        // cannot come to disagree about which face that is.
        const FaceQueryResult found = ResolveFaceQuery(carrier, wanted->face);
        if (!found.ok) return; // narrowed to none or to several: names nothing
        // FacePlane carries geometry, not topology, so the edges are taken
        // from the shape by matching the resolved face's plane -- the same
        // match the pick uses, and the only thing a plane and a face share.
        for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
            const FacePlane plane = PlaneOfFace(it.Current());
            if (!plane.planar) continue;
            const Vec3 d{plane.point.x - found.face.point.x, plane.point.y - found.face.point.y,
                         plane.point.z - found.face.point.z};
            const double along = d.x * found.face.normal.x + d.y * found.face.normal.y +
                                 d.z * found.face.normal.z;
            const double facing = plane.normal.x * found.face.normal.x +
                                  plane.normal.y * found.face.normal.y +
                                  plane.normal.z * found.face.normal.z;
            if (std::fabs(along) > 1e-6 || facing < 0.999) continue;
            for (TopExp_Explorer e(it.Current(), TopAbs_EDGE); e.More(); e.Next()) {
                const int index = edges.FindIndex(e.Current());
                if (index > 0) into.insert(index);
            }
        }
        return;
    }
    if (const auto* made = std::get_if<EdgesCreatedBy>(&query)) {
        // Answered from the PROVENANCE the shape carries, not from its
        // geometry: this is the one query that describes where a face came
        // from rather than where it is.
        const auto* occt = dynamic_cast<const OcctShape*>(carrier.handle());
        if (occt == nullptr) return;
        const auto entry = occt->provenance().find(static_cast<std::uint64_t>(made->featureId));
        if (entry == occt->provenance().end()) return; // that feature made nothing here
        for (int f = 1; f <= entry->second.Extent(); ++f)
            for (TopExp_Explorer it(entry->second(f), TopAbs_EDGE); it.More(); it.Next()) {
                const int index = edges.FindIndex(it.Current());
                if (index > 0) into.insert(index);
            }
        return;
    }
    const auto& parallel = std::get<EdgesParallelTo>(query);
    for (int i = 1; i <= edges.Extent(); ++i)
        if (EdgeIsParallelTo(TopoDS::Edge(edges(i)), parallel.direction)) into.insert(i);
}

} // namespace

// Every edge the selection names, deduplicated. An edge named by two queries is
// dressed once -- adding it twice to ChFi3d is undefined-behaviour territory,
// which is the same reason the map exists at all.
std::set<int> SelectedEdgeIndices(const TopoDS_Shape& shape, const KernelShape& carrier,
                                  const TopTools_IndexedMapOfShape& edges,
                                  const EdgeSelection& selection) {
    std::set<int> chosen;
    for (const EdgeQuery& query : selection) CollectQuery(shape, carrier, edges, query, chosen);
    return chosen;
}

FaceQueryResult OcctGeometryKernel::resolveFace(const KernelShape& shape,
                                               const FaceQuery& query) {
    return ResolveFaceQuery(shape, query);
}

KernelShape OcctGeometryKernel::tagCreatedFaces(const KernelShape& result,
                                               const KernelShape& base, std::uint64_t tag) {
    return WithProvenance(result, base, tag);
}

ShapeResult OcctGeometryKernel::filletEdges(const KernelShape& shape,
                                           const EdgeSelection& selection, double radiusMm) {
    const auto* occtShape = dynamic_cast<const OcctShape*>(shape.handle());
    if (occtShape == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "fillet input is not an OcctShape (null or foreign kernel)"};
    if (!IsValidExtrusionDistance(radiusMm))
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "invalid fillet radius: must be finite and at least " +
                               std::to_string(kMinExtrusionDistanceMm) + " mm"};

    try {
        BRepFilletAPI_MakeFillet fillet(occtShape->shape());
        // MapShapes, not a raw explorer: an explorer visits a shared edge once
        // per owning face, and Add-ing the same edge twice is undefined
        // behaviour territory in ChFi3d. The indexed map is the deduplicated
        // edge set.
        TopTools_IndexedMapOfShape edges;
        TopExp::MapShapes(occtShape->shape(), TopAbs_EDGE, edges);
        if (edges.IsEmpty())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "the shape has no edges to fillet"};
        // The query, answered NOW against this shape.
        const std::set<int> chosen = SelectedEdgeIndices(occtShape->shape(), shape, edges, selection);
        if (chosen.empty())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "no edge matched the fillet's selection (" +
                                   DescribeEdgeSelection(selection) + ")"};
        for (int index : chosen) fillet.Add(radiusMm, TopoDS::Edge(edges(index)));
        fillet.Build();
        if (!fillet.IsDone())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "OCCT could not fillet every edge at this radius; the radius "
                               "may exceed what the geometry accommodates"};
        return AnalyzedDressResult(fillet.Shape(), "filleted", "radius");
    } catch (const Standard_Failure& failure) {
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           std::string("OCCT raised while filleting: ") + describe(failure)};
    }
}

ShapeResult OcctGeometryKernel::chamferEdges(const KernelShape& shape,
                                            const EdgeSelection& selection, double distanceMm) {
    const auto* occtShape = dynamic_cast<const OcctShape*>(shape.handle());
    if (occtShape == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "chamfer input is not an OcctShape (null or foreign kernel)"};
    if (!IsValidExtrusionDistance(distanceMm))
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "invalid chamfer distance: must be finite and at least " +
                               std::to_string(kMinExtrusionDistanceMm) + " mm"};

    try {
        BRepFilletAPI_MakeChamfer chamfer(occtShape->shape());
        TopTools_IndexedMapOfShape edges;
        TopExp::MapShapes(occtShape->shape(), TopAbs_EDGE, edges);
        if (edges.IsEmpty())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "the shape has no edges to chamfer"};
        const std::set<int> chosen = SelectedEdgeIndices(occtShape->shape(), shape, edges, selection);
        if (chosen.empty())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "no edge matched the chamfer's selection (" +
                                   DescribeEdgeSelection(selection) + ")"};
        // One distance = the symmetric 45-degree bevel, which is what a bare
        // "chamfer 2mm" means on a drawing.
        for (int index : chosen) chamfer.Add(distanceMm, TopoDS::Edge(edges(index)));
        chamfer.Build();
        if (!chamfer.IsDone())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "OCCT could not chamfer every edge at this distance; the "
                               "distance may exceed what the geometry accommodates"};
        return AnalyzedDressResult(chamfer.Shape(), "chamfered", "distance");
    } catch (const Standard_Failure& failure) {
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           std::string("OCCT raised while chamfering: ") + describe(failure)};
    }
}

KernelMassPropertiesResult OcctGeometryKernel::calculateMassProperties(const KernelShape& shape) {
    // dynamic_cast, never UB on mismatch/null (ADR-M3-001): a KernelShape
    // that is default-invalid or came from a different kernel implementation
    // yields a controlled failure rather than dereferencing anything unsafely.
    const auto* occtShape = dynamic_cast<const OcctShape*>(shape.handle());
    if (occtShape == nullptr) {
        return KernelMassPropertiesResult{
            {}, KernelError::GeometryConstructionFailed,
            "shape handle is not an OcctShape (null or foreign kernel)"};
    }

    GProp_GProps props;
    try {
        // PER SOLID, then summed with OCCT's own combination API.
        //
        // `VolumeProperties` on a COMPOUND of disjoint solids returns the right
        // VOLUME and a wrong CENTRE OF MASS -- measured at 2% on two 100x50x20
        // prisms 200 mm apart. That is the worst possible shape for this
        // defect: every volume oracle in this project keeps passing, and the
        // centroid is the only thing that moves.
        //
        // M10's GATE_P found it, and only because its expected centroid does
        // NOT sit at the midpoint of the lumps. GATE_M and GATE_N both fuse
        // disjoint solids too and both passed -- their expected values are at
        // the symmetric centre, where the error cancels exactly. That is M8
        // GATE_RB2's coincidence lesson for the third time in this project.
        //
        // Summing per solid is exact because each solid on its own is exact,
        // and `GProp_GProps::Add` is the documented way to combine systems.
        bool summedAnySolid = false;
        for (TopExp_Explorer it(occtShape->shape(), TopAbs_SOLID); it.More(); it.Next()) {
            GProp_GProps one;
            BRepGProp::VolumeProperties(it.Current(), one, 1.0e-11);
            props.Add(one);
            summedAnySolid = true;
        }
        if (!summedAnySolid) {
        // ADAPTIVE, not the default overload (M10.6). The default integrates
        // with a fixed scheme, and M10's GATE_P caught it being ~2% wrong on
        // the CENTRE OF MASS of a fused compound -- two disjoint extruded
        // prisms, one of them mirrored -- while reporting the volume exactly.
        // A wrong centre of mass with a right volume is the worst shape of
        // this defect: every volume oracle in the project would keep passing.
        //
        // This overload iterates until the relative error is below Eps and
        // returns the error achieved. 1e-11 is far below every tolerance any
        // gate asserts, and the cost is paid only where the fixed scheme was
        // not already exact.
            // No solids at all: a shell, a face, or an empty result. Measured
            // as-is, which is what an empty difference must report (M8 spec 6).
            BRepGProp::VolumeProperties(occtShape->shape(), props, 1.0e-11);
        }
    } catch (const Standard_Failure& failure) {
        // Same "never throws" contract as createBox (ADR-M3-001).
        return KernelMassPropertiesResult{
            {}, KernelError::MassPropertiesFailed,
            std::string("OCCT raised while computing volume properties: ") +
                describe(failure)};
    }

    KernelMassProperties result;
    result.volumeMm3 = props.Mass(); // "Mass" of VolumeProperties is volume (density-independent)
    const gp_Pnt com = props.CentreOfMass();
    result.centerOfMassMm = Vec3{com.X(), com.Y(), com.Z()};

    // GProp_GProps::MatrixOfInertia() is documented by OCCT ("the matrix of
    // inertia of the system relative to its center of mass is returned by
    // the function MatrixOfInertia") to already be expressed in the central
    // coordinate system (G, Gx, Gy, Gz) -- i.e. already the COM-relative
    // second moment of volume this project's KernelMassProperties contract
    // requires (ADR-M3-002), with no further parallel-axis (Huyghens'
    // theorem) shift needed. Cross-checked against the independent
    // analytical box oracle in tests/Kernel/OcctGeometryKernelTests.cpp.
    const gp_Mat inertia = props.MatrixOfInertia();
    result.secondMomentMm5.m[0] = inertia.Value(1, 1);
    result.secondMomentMm5.m[1] = inertia.Value(1, 2);
    result.secondMomentMm5.m[2] = inertia.Value(1, 3);
    result.secondMomentMm5.m[3] = inertia.Value(2, 1);
    result.secondMomentMm5.m[4] = inertia.Value(2, 2);
    result.secondMomentMm5.m[5] = inertia.Value(2, 3);
    result.secondMomentMm5.m[6] = inertia.Value(3, 1);
    result.secondMomentMm5.m[7] = inertia.Value(3, 2);
    result.secondMomentMm5.m[8] = inertia.Value(3, 3);

    return KernelMassPropertiesResult{result, KernelError::None, {}};
}

} // namespace paramcad
