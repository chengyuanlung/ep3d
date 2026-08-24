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

    ShapeResult placeShape(const KernelShape& shape, const Transform3D& placement) override;
    ShapeResult rotateShape(const KernelShape& shape, const Vec3& axisOriginMm,
                            const Vec3& axisDirection, double angleRad) override;
    ShapeResult intersectShapes(const KernelShape& a, const KernelShape& b) override;
    ShapeResult shellSolid(const KernelShape& base, const FaceSelection& openFaces,
                           double thicknessMm) override;
    ShapeResult draftFaces(const KernelShape& base, const FaceSelection& faces,
                           const FaceQuery& neutral, double angleRad) override;
    IoResult exportStep(const KernelShape& shape, const std::string& path) override;
    ShapeResult importStep(const std::string& path) override;
    IoResult exportStl(const KernelShape& shape, const std::string& path,
                       double deflectionMm) override;
    KernelInterferenceResult measureInterference(const KernelShape& a,
                                                 const KernelShape& b) override;
    KernelBoundsResult boundsOfShape(const KernelShape& shape) override;
    ShapeResult filletEdges(const KernelShape& shape, const EdgeSelection& selection,
                            double radiusMm) override;
    ShapeResult chamferEdges(const KernelShape& shape, const EdgeSelection& selection,
                             double distanceMm) override;
    ShapeResult mirrorShape(const KernelShape& shape, const Vec3& planeOriginMm,
                            const Vec3& planeNormal) override;
    ShapeResult translateShape(const KernelShape& shape, const Vec3& offsetMm) override;
    ShapeResult fuseShapes(const KernelShape& a, const KernelShape& b) override;
    ShapeResult compoundOf(const std::vector<KernelShape>& shapes) override;
    int countSolids(const KernelShape& shape) override;
};

} // namespace paramcad
