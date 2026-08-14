#pragma once

#include "Core/Kernel/IGeometryKernel.h"

namespace paramcad {

// The only Core-facing IGeometryKernel implementation backed by real OCCT
// geometry (ADR-M3-001/003). Lives entirely in the OCCT-linked
// ParametricCADKernelOcct target; Core and BoxFeature never name this type
// directly -- only src/App/main.cpp (and OCCT-linked test files) construct
// one and inject it via PartDocument::setGeometryKernel.
class OcctGeometryKernel final : public IGeometryKernel {
public:
    ShapeResult createBox(const BoxDefinition& definition) override;
    ShapeResult extrudeProfile(const PlanarProfileDefinition& profile,
                               double distanceMm) override;
    KernelMassPropertiesResult calculateMassProperties(const KernelShape& shape) override;
    ShapeResult subtractShape(const KernelShape& base, const KernelShape& tool) override;
};

} // namespace paramcad
