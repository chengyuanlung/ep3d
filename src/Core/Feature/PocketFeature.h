#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISketchConsuming.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Recompute/IRecomputable.h"
#include <string>
#include <vector>
#include <string_view>

namespace paramcad {

// The first CONSUMING solid feature (M8 spec 5, ADR-M8-001): removes material
// from an upstream feature's result by extruding a Sketch profile into a tool
// prism and subtracting it through the kernel.
//
// This is the feature that founds the chain. Pad and Box build from nothing;
// Pocket is defined by what it consumes, and every structural decision here is
// about keeping that reference semantic:
//
//   - the base is referenced by the base FEATURE's ObjectId -- never by
//     position in Body::features(), never by pointer, never by kernel handle
//     (spec A03, ADR-M4-004);
//   - the base's geometry is read through ISolidFeature at recompute time and
//     is NEVER cached here -- a copy would be a second version of upstream
//     state whose staleness nothing tracks, the exact failure family
//     ADR-M3-006/007 record;
//   - the dependency edge base -> pocket is wired by the PartDocument facade,
//     so editing anything upstream dirties this feature through the ordinary
//     M2 machinery. Nothing about the chain schedules itself.
//
// Follows PadFeature's structure otherwise: Feature carries identity and the
// synchronized ComputeState cache, IRecomputable is the execution contract.
class PocketFeature final : public Feature,
                            public IRecomputable,
                            public ISolidFeature,
                            public ISketchConsuming,
                            public IMaterialReferencing {
public:
    PocketFeature(std::string name, ObjectId baseFeatureId, ObjectId sketchId,
                  ObjectId depthParameterId, ObjectId materialId);
    // Restore constructor (deserialization): keeps the persisted id/state.
    PocketFeature(ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
                  ObjectId sketchId, ObjectId depthParameterId, ObjectId materialId);

    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Pocket"; }

    ObjectId baseFeatureId() const noexcept { return baseFeatureId_; }
    ObjectId sketchId() const noexcept { return sketchId_; }
    // ISketchConsuming: the same answer, asked the way code that does not know
    // this type has to ask it.
    ObjectId consumedSketchId() const noexcept override { return sketchId_; }
    ObjectId depthParameterId() const noexcept { return depthParameterId_; }
    ObjectId materialId() const noexcept override { return materialId_; }
    void clearMaterialReference() noexcept override { materialId_ = kInvalidObjectId; }
    void setMaterialReference(ObjectId materialId) noexcept override {
        materialId_ = materialId;
    }

    // Last successfully built RESULT (base minus tool), retained byte-for-byte
    // across a failed rebuild (ADR-M3-001/004).
    const KernelShape& currentShape() const noexcept override { return currentShape_; }
    ComputeState currentState() const noexcept override { return Feature::state(); }
    // The chain declaration (ADR-M8-003): the base this feature consumes, so
    // the viewer and any other tail-follower can tell an intermediate result
    // from a final one without naming concrete feature types.
    std::vector<ObjectId> consumedSolidIds() const override { return {baseFeatureId_}; }

    // Vestigial M1 contract, never called by the document engine (ADR-M3-004).
    bool recompute() override;

    // Real execution: resolve the base through ISolidFeature, require it
    // Valid, build and validate the tool profile in Core, extrude the tool
    // along the sketch normal by the Depth Parameter (the same direction Pad
    // grows, ADR-M8-002), subtract through the kernel, commit only on success.
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    ObjectId baseFeatureId_;
    ObjectId sketchId_;
    ObjectId depthParameterId_;
    ObjectId materialId_;
    KernelShape currentShape_;
};

} // namespace paramcad
