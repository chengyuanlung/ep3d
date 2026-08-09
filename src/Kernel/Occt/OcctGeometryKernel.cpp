#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Kernel/Occt/OcctShape.h"
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Mat.hxx>
#include <gp_Pnt.hxx>
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
