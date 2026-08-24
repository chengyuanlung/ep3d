#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IParameterisedFeature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISketchConsuming.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Recompute/IRecomputable.h"
#include <string>
#include <string_view>

namespace paramcad {

// The first general profile-based solid feature (spec 12): extrudes a Sketch's
// validated outer profile along the sketch normal by a Length Parameter.
//
// Follows BoxFeature's structure exactly -- inherits Feature (persistent
// identity/state/typeName) and IRecomputable (the real execution contract),
// with the graph's ComputeState authoritative and Feature::state() a
// synchronized cache (ADR-M3-004/007).
//
// References are ObjectIds only: the Sketch and the Length Parameter. No OCCT
// topology, no index, no pointer is ever stored as identity (ADR-M4-004).
class PadFeature final : public Feature,
                        public IRecomputable,
                        public ISolidFeature,
                        public ISketchConsuming,
                        public IMaterialReferencing, public IParameterisedFeature {
public:
    std::vector<FeatureParameter> featureParameters() const override {
        return {FeatureParameter{"Length", lengthParameterId_, true}};
    }
    PadFeature(std::string name, ObjectId sketchId, ObjectId lengthParameterId,
               ObjectId materialId);
    // Restore constructor (deserialization): keeps the persisted id/state.
    PadFeature(ObjectId id, std::string name, ComputeState state, ObjectId sketchId,
               ObjectId lengthParameterId, ObjectId materialId);

    // Resolves to the single stored Feature::id_ -- see BoxFeature for why this
    // declaration is needed and why it cannot yield two competing ids.
    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Pad"; }

    ObjectId sketchId() const noexcept { return sketchId_; }
    // ISketchConsuming: the same answer, asked the way code that does not know
    // this type has to ask it.
    // A pad sweeps an AREA, so an unclosed sketch really is a failure here.
    std::vector<ConsumedSketch> consumedSketches() const override {
        return {ConsumedSketch{sketchId_, true}};
    }
    ObjectId lengthParameterId() const noexcept { return lengthParameterId_; }
    ObjectId materialId() const noexcept override { return materialId_; }
    void clearMaterialReference() noexcept override { materialId_ = kInvalidObjectId; }
    void setMaterialReference(ObjectId materialId) noexcept override {
        materialId_ = materialId;
    }

    // Last successfully built shape, retained byte-for-byte across a failed
    // rebuild (ADR-M3-001/004); staleness travels through ComputeState only.
    const KernelShape& currentShape() const noexcept override { return currentShape_; }
    ComputeState currentState() const noexcept override { return Feature::state(); }

    // Vestigial M1 contract, never called by the document engine (ADR-M3-004).
    bool recompute() override;

    // Real execution path: resolve Sketch + Length through the registry,
    // validate the profile in Core (ADR-M4-005), convert to a kernel-neutral
    // definition, extrude through the injected kernel, and commit only on
    // success.
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    ObjectId sketchId_;
    ObjectId lengthParameterId_;
    ObjectId materialId_;
    KernelShape currentShape_;
};

} // namespace paramcad
