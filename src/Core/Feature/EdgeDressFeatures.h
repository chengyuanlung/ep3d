#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Recompute/IRecomputable.h"
#include <string>
#include <string_view>

namespace paramcad {

// Fillet and Chamfer (M8.3, ADR-M8-006): the edge-dressing consumers.
//
// ONE FILE for both, unlike every other feature pair, and deliberately: the
// two are the same feature with a different kernel verb -- consume a base,
// read one length Parameter, dress every edge. Splitting them into four files
// would manufacture the divergence this project keeps finding between
// parallel kinds; here the parallelism is enforced by a shared base class,
// so a guard added to one cannot be forgotten on the other.
//
// ALL edges, deliberately (ADR-M8-006). Selective edges need an edge the
// document can NAME, and the only name available today is an OCCT explorer
// position -- transient topology, which ADR-M4-004 forbids as identity.
// Selection waits for the roadmap's selection architecture; nothing here
// pre-decides its shape.
//
// Chain semantics are Pocket's exactly: base by ObjectId through
// ISolidFeature, no cached upstream geometry, consumedSolidId() declares the
// chain edge for display and physics (ADR-M8-001/003).
class EdgeDressFeature : public Feature,
                         public IRecomputable,
                         public ISolidFeature,
                         public IMaterialReferencing {
public:
    ObjectId id() const noexcept override { return Feature::id(); }

    ObjectId baseFeatureId() const noexcept { return baseFeatureId_; }
    ObjectId sizeParameterId() const noexcept { return sizeParameterId_; }
    ObjectId materialId() const noexcept override { return materialId_; }
    void clearMaterialReference() noexcept override { materialId_ = kInvalidObjectId; }
    void setMaterialReference(ObjectId materialId) noexcept override {
        materialId_ = materialId;
    }

    const KernelShape& currentShape() const noexcept override { return currentShape_; }
    ComputeState currentState() const noexcept override { return Feature::state(); }
    ObjectId consumedSolidId() const noexcept override { return baseFeatureId_; }

    // Vestigial M1 contract, never called by the document engine (ADR-M3-004).
    bool recompute() override;
    RecomputeResult recompute(const RecomputeContext& context) override;

protected:
    EdgeDressFeature(std::string name, ObjectId baseFeatureId, ObjectId sizeParameterId,
                     ObjectId materialId);
    EdgeDressFeature(ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
                     ObjectId sizeParameterId, ObjectId materialId);

    // The one point of divergence: which kernel verb, and what to call the
    // size in a diagnostic.
    virtual ShapeResult applyDress(IGeometryKernel& kernel, const KernelShape& base,
                                   double sizeMm) const = 0;
    virtual const char* dressNoun() const noexcept = 0;

private:
    ObjectId baseFeatureId_;
    ObjectId sizeParameterId_;
    ObjectId materialId_;
    KernelShape currentShape_;
};

class FilletFeature final : public EdgeDressFeature {
public:
    FilletFeature(std::string name, ObjectId baseFeatureId, ObjectId radiusParameterId,
                  ObjectId materialId)
        : EdgeDressFeature(std::move(name), baseFeatureId, radiusParameterId, materialId) {}
    FilletFeature(ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
                  ObjectId radiusParameterId, ObjectId materialId)
        : EdgeDressFeature(id, std::move(name), state, baseFeatureId, radiusParameterId,
                           materialId) {}
    std::string_view typeName() const noexcept override { return "Fillet"; }

protected:
    ShapeResult applyDress(IGeometryKernel& kernel, const KernelShape& base,
                           double sizeMm) const override;
    const char* dressNoun() const noexcept override { return "fillet"; }
};

class ChamferFeature final : public EdgeDressFeature {
public:
    ChamferFeature(std::string name, ObjectId baseFeatureId, ObjectId distanceParameterId,
                   ObjectId materialId)
        : EdgeDressFeature(std::move(name), baseFeatureId, distanceParameterId, materialId) {}
    ChamferFeature(ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
                   ObjectId distanceParameterId, ObjectId materialId)
        : EdgeDressFeature(id, std::move(name), state, baseFeatureId, distanceParameterId,
                           materialId) {}
    std::string_view typeName() const noexcept override { return "Chamfer"; }

protected:
    ShapeResult applyDress(IGeometryKernel& kernel, const KernelShape& base,
                           double sizeMm) const override;
    const char* dressNoun() const noexcept override { return "chamfer"; }
};

} // namespace paramcad
