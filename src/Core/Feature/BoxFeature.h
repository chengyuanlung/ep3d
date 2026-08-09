#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Recompute/IRecomputable.h"
#include <string>
#include <string_view>

namespace paramcad {

// Parametric rectangular solid feature -- M3's first real geometry feature.
// Inherits BOTH Feature (persistent identity/state/typeName, the M1/M2
// document-object contract) and IRecomputable (the real M2/M3 execution
// contract) -- see ADR-M3-004 for how the two ComputeState copies this
// creates are kept from becoming competing sources of truth (the graph's
// ComputeState is authoritative; Feature::state() is a manually-synchronized
// cache, updated only from inside recompute(context) below).
class BoxFeature final : public Feature,
                        public IRecomputable,
                        public ISolidFeature,
                        public IMaterialReferencing {
public:
    BoxFeature(std::string name, ObjectId widthParameterId, ObjectId heightParameterId,
               ObjectId depthParameterId, ObjectId materialId);
    // Restore constructor (deserialization): keeps the persisted id/state.
    BoxFeature(ObjectId id, std::string name, ComputeState state, ObjectId widthParameterId,
               ObjectId heightParameterId, ObjectId depthParameterId, ObjectId materialId);

    // Declaring id() directly in BoxFeature hides Feature::id() (non-virtual)
    // and overrides IRecomputable::id() (pure virtual) -- both refer to the
    // exact same single stored Feature::id_, so there is only ever one
    // answer, never two competing ids.
    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Box"; }

    ObjectId widthParameterId() const noexcept { return widthParameterId_; }
    ObjectId heightParameterId() const noexcept { return heightParameterId_; }
    ObjectId depthParameterId() const noexcept { return depthParameterId_; }
    ObjectId materialId() const noexcept override { return materialId_; }
    void clearMaterialReference() noexcept override { materialId_ = kInvalidObjectId; }
    void setMaterialReference(ObjectId materialId) noexcept override {
        materialId_ = materialId;
    }

    // Last successfully built shape. Untouched by a failed recompute --
    // transactional retention (ADR-M3-001/004): a failed build leaves this
    // byte-for-byte unchanged; staleness is communicated exclusively through
    // state()/ComputeState, never by mutating or clearing this.
    const KernelShape& currentShape() const noexcept override { return currentShape_; }
    ComputeState currentState() const noexcept override { return Feature::state(); }

    // Vestigial M1 recompute contract (ADR-M3-004): NOT called by the
    // document recompute engine. Kept only so this type satisfies Feature's
    // existing pure virtual without touching Feature.h's M1/M2 contract.
    bool recompute() override;

    // Real M2/M3 execution path: resolves Width/Height/Depth through the
    // registry (never scans document containers), builds a box through the
    // injected kernel (ADR-M3-003) transactionally, and synchronizes
    // Feature::state() at the same point (ADR-M3-004).
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    ObjectId widthParameterId_;
    ObjectId heightParameterId_;
    ObjectId depthParameterId_;
    ObjectId materialId_;
    KernelShape currentShape_;
};

} // namespace paramcad
