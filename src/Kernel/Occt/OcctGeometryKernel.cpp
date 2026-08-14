#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Kernel/Occt/OcctShape.h"
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepGProp.hxx>
#include <BRepAlgoAPI_Cut.hxx>
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
TopoDS_Face BuildFaceForProfile(const PlanarProfileDefinition& profile, std::string& error) {
    // The sketch plane, expressed to OCCT exactly as Core computed it --
    // Kernel/Occt never re-derives the frame, so SketchFrame stays the
    // single conversion site (ADR-M4-002).
    const gp_Pnt origin(profile.plane.origin.x, profile.plane.origin.y,
                        profile.plane.origin.z);
    const gp_Dir normal(profile.plane.normal.x, profile.plane.normal.y,
                        profile.plane.normal.z);
    const gp_Dir uDir(profile.plane.uAxis.x, profile.plane.uAxis.y, profile.plane.uAxis.z);
    const gp_Ax2 axes(origin, normal, uDir);
    const gp_Pln plane(axes);

    // (u,v) -> part-local XYZ, using the same basis Core handed over.
    const auto toWorld = [&](Vec2 uv) {
        return gp_Pnt(profile.plane.origin.x + uv.x * profile.plane.uAxis.x +
                          uv.y * profile.plane.vAxis.x,
                      profile.plane.origin.y + uv.x * profile.plane.uAxis.y +
                          uv.y * profile.plane.vAxis.y,
                      profile.plane.origin.z + uv.x * profile.plane.uAxis.z +
                          uv.y * profile.plane.vAxis.z);
    };

    BRepBuilderAPI_MakeWire wireMaker;
    for (const ProfileSegment& segment : profile.segments) {
        if (const auto* line = std::get_if<ProfileLineSegment>(&segment)) {
            BRepBuilderAPI_MakeEdge edge(toWorld(line->start), toWorld(line->end));
            edge.Build();
            if (!edge.IsDone()) {
                error = "OCCT could not build a line edge for the profile";
                return TopoDS_Face();
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
                return TopoDS_Face();
            }
            wireMaker.Add(edge.Edge());
        } else {
            const auto& full = std::get<ProfileCircleSegment>(segment);
            const gp_Circ circle(gp_Ax2(toWorld(full.center), normal, uDir), full.radiusMm);
            BRepBuilderAPI_MakeEdge edge(circle);
            edge.Build();
            if (!edge.IsDone()) {
                error = "OCCT could not build a circular edge for the profile";
                return TopoDS_Face();
            }
            wireMaker.Add(edge.Edge());
        }
    }

    wireMaker.Build();
    if (!wireMaker.IsDone()) {
        error = "OCCT could not assemble the profile edges into a wire";
        return TopoDS_Face();
    }
    const TopoDS_Wire wire = wireMaker.Wire();
    if (!wire.Closed()) {
        error = "profile wire is not closed";
        return TopoDS_Face();
    }

    BRepBuilderAPI_MakeFace faceMaker(plane, wire);
    faceMaker.Build();
    if (!faceMaker.IsDone()) {
        error = "OCCT could not build a planar face from the profile";
        return TopoDS_Face();
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
    if (!IsValidExtrusionDistance(distanceMm)) {
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "invalid extrusion distance: must be finite and at least " +
                               std::to_string(kMinExtrusionDistanceMm) + " mm"};
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


ShapeResult OcctGeometryKernel::filletAllEdges(const KernelShape& shape, double radiusMm) {
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
        for (int i = 1; i <= edges.Extent(); ++i)
            fillet.Add(radiusMm, TopoDS::Edge(edges(i)));
        fillet.Build();
        if (!fillet.IsDone())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "OCCT could not fillet every edge at this radius; the radius "
                               "may exceed what the geometry accommodates"};
        // IsDone() is NOT sufficient, and a test had to fail to prove it: a
        // radius wider than half the part's thickness (15 on a 20mm slab)
        // reported done while producing self-intersecting geometry. The
        // analyzer is the check ChFi3d itself does not make.
        if (!BRepCheck_Analyzer(fillet.Shape()).IsValid())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "the filleted result is not a valid solid; the radius exceeds "
                               "what the geometry accommodates"};
        auto handle = std::make_shared<OcctShape>(fillet.Shape());
        return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
    } catch (const Standard_Failure& failure) {
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           std::string("OCCT raised while filleting: ") + describe(failure)};
    }
}

ShapeResult OcctGeometryKernel::chamferAllEdges(const KernelShape& shape, double distanceMm) {
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
        // One distance = the symmetric 45-degree bevel, which is what a bare
        // "chamfer 2mm" means on a drawing.
        for (int i = 1; i <= edges.Extent(); ++i)
            chamfer.Add(distanceMm, TopoDS::Edge(edges(i)));
        chamfer.Build();
        if (!chamfer.IsDone())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "OCCT could not chamfer every edge at this distance; the "
                               "distance may exceed what the geometry accommodates"};
        // Same analyzer as the fillet, same reason (see above).
        if (!BRepCheck_Analyzer(chamfer.Shape()).IsValid())
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "the chamfered result is not a valid solid; the distance "
                               "exceeds what the geometry accommodates"};
        auto handle = std::make_shared<OcctShape>(chamfer.Shape());
        return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
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
        BRepGProp::VolumeProperties(occtShape->shape(), props);
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
