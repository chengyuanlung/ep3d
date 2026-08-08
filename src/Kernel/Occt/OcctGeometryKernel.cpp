#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Kernel/Occt/OcctShape.h"
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Mat.hxx>
#include <gp_Pnt.hxx>
#include <memory>
#include <utility>

namespace paramcad {

ShapeResult OcctGeometryKernel::createBox(const BoxDefinition& definition) {
    // The ONE place dimension validation lives (ADR-M3-001): every kernel,
    // real or fake, calls this first, so zero/negative/NaN/infinite
    // dimensions never reach BRepPrimAPI_MakeBox.
    if (!IsValidBoxDefinition(definition)) {
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "invalid box definition: dimensions must be finite and positive"};
    }

    // Corner-origin convention (spec 8): origin (0,0,0), Width +X, Height +Y,
    // Depth +Z. BRepPrimAPI_MakeBox(dx, dy, dz) builds exactly this box.
    BRepPrimAPI_MakeBox maker(definition.widthMm, definition.heightMm, definition.depthMm);
    if (!maker.IsDone()) {
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "OCCT failed to construct the box primitive"};
    }

    auto handle = std::make_shared<OcctShape>(maker.Shape());
    return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
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
    BRepGProp::VolumeProperties(occtShape->shape(), props);

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
