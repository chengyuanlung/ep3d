#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISketchConsuming.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Recompute/IRecomputable.h"
#include "Core/Sketch/SketchTypes.h"
#include <string>
#include <string_view>

namespace paramcad {

// The second base-capable solid feature (M8.2, ADR-M8-005): revolves a
// Sketch's validated outer profile about one of the sketch's OWN LINES by an
// Angle Parameter.
//
// The axis is a SketchEntityId, and that is the decision worth defending. The
// alternatives were a world axis (X/Y/Z -- not what a drawing means; a revolve
// axis is a fact about the sketch, not about the document frame) or a free
// origin+direction pair (numbers with no identity, un-referencable by any
// later edit). A line the user drew, referenced the same way dimensions and
// constraints reference lines (ADR-M4-001), keeps the axis semantic, stable
// across save/load, and editable by editing the line.
//
// The axis line MAY be a member of the profile loop -- revolving a rectangle
// about its own left edge is the canonical first cylinder -- or any other line
// in the same sketch. What it must be is a LINE in THIS sketch; anything else
// fails with a diagnostic, never a guess. This sentence was FALSE as first
// shipped (round 1's R1-M1: the unconditional exclusion broke the loop and the
// canonical cylinder was refused); the member case now works via the fallback
// in recompute() and is pinned by GATE_RH -- the claim exists because the gate
// does, not the other way around.
//
// Like Pad and unlike Pocket, Revolve builds from nothing: consumedSolidId()
// stays invalid, and it is a legal chain BASE -- a Pocket can cut a revolved
// solid with no change to either feature, which is what resolving bases by
// capability (ISolidFeature) bought in M8.1, and Gate R-D proves it.
class RevolveFeature final : public Feature,
                             public IRecomputable,
                             public ISolidFeature,
                             public ISketchConsuming,
                             public IMaterialReferencing {
public:
    RevolveFeature(std::string name, ObjectId sketchId, SketchEntityId axisEntityId,
                   ObjectId angleParameterId, ObjectId materialId);
    // Restore constructor (deserialization): keeps the persisted id/state.
    RevolveFeature(ObjectId id, std::string name, ComputeState state, ObjectId sketchId,
                   SketchEntityId axisEntityId, ObjectId angleParameterId, ObjectId materialId);

    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Revolve"; }

    ObjectId sketchId() const noexcept { return sketchId_; }
    // ISketchConsuming: the same answer, asked the way code that does not know
    // this type has to ask it.
    ObjectId consumedSketchId() const noexcept override { return sketchId_; }
    SketchEntityId axisEntityId() const noexcept { return axisEntityId_; }
    ObjectId angleParameterId() const noexcept { return angleParameterId_; }
    ObjectId materialId() const noexcept override { return materialId_; }
    void clearMaterialReference() noexcept override { materialId_ = kInvalidObjectId; }
    void setMaterialReference(ObjectId materialId) noexcept override {
        materialId_ = materialId;
    }

    const KernelShape& currentShape() const noexcept override { return currentShape_; }
    ComputeState currentState() const noexcept override { return Feature::state(); }

    // Vestigial M1 contract, never called by the document engine (ADR-M3-004).
    bool recompute() override;

    // Real execution: validate the profile in Core, resolve the axis line and
    // convert its endpoints to world through the sketch's own frame (the ONE
    // conversion site, ADR-M4-002), read the Angle Parameter -- which must
    // carry UnitType::Radian -- and revolve through the injected kernel.
    // Commits only on success.
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    ObjectId sketchId_;
    SketchEntityId axisEntityId_;
    ObjectId angleParameterId_;
    ObjectId materialId_;
    KernelShape currentShape_;
};

} // namespace paramcad
