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
    ShapeResult sweepProfile(const PlanarProfileDefinition& profile,
                             const PlanarPathDefinition& path) override;
    ShapeResult loftProfiles(const std::vector<PlanarProfileDefinition>& profiles) override;
    ShapeResult revolveProfile(const PlanarProfileDefinition& profile, const Vec3& axisOriginMm,
                               const Vec3& axisDirection, double angleRad) override;
    FaceQueryResult resolveFace(const KernelShape& shape, const FaceQuery& query) override;

    KernelShape tagCreatedFaces(const KernelShape& result, const KernelShape& base,
                                std::uint64_t tag) override;

    ShapeResult filletEdges(const KernelShape& shape, const EdgeSelection& selection,
                            double radiusMm) override;
    ShapeResult chamferEdges(const KernelShape& shape, const EdgeSelection& selection,
                             double distanceMm) override;
    ShapeResult mirrorShape(const KernelShape& shape, const Vec3& planeOriginMm,
                            const Vec3& planeNormal) override;
    ShapeResult translateShape(const KernelShape& shape, const Vec3& offsetMm) override;
    ShapeResult fuseShapes(const KernelShape& a, const KernelShape& b) override;
};

} // namespace paramcad
