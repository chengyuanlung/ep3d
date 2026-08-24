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
#include <string_view>

namespace paramcad {

// Sweeps one sketch's profile along another sketch's path (M19).
//
// TWO SKETCHES, and that is the decision worth defending. A sweep needs a
// section and a spine, and they almost never lie on the same plane -- if they
// did, the section would be swept along a direction inside its own plane and
// the result would have no volume at all. So they are two sketches, referenced
// the same way every other feature references one.
//
// The alternatives were: a path drawn as construction geometry inside the
// profile's own sketch (same plane, same degeneracy), or a path given as
// numbers (no identity, un-referencable, and it could not be edited by editing
// the drawing). A sketch each keeps both halves semantic and editable, and
// keeps the recompute graph honest -- this feature is dirty when EITHER moves.
//
// The profile is NOT required to touch the path. What is swept is the section's
// SHAPE; where it starts is where the section is. Requiring contact would
// refuse the ordinary case of drawing the section on one datum and the spine on
// another, which is exactly how a pipe gets drawn.
//
// Like a Pad and unlike a Pocket, a sweep builds from nothing: consumedSolidId()
// stays invalid and it is a legal chain BASE.
class SweepFeature final : public Feature,
                           public IRecomputable,
                           public ISolidFeature,
                           public ISketchConsuming,
                           public IMaterialReferencing {
public:
    SweepFeature(std::string name, ObjectId profileSketchId, ObjectId pathSketchId,
                 ObjectId materialId);
    // Restore constructor (deserialization): keeps the persisted id/state.
    SweepFeature(ObjectId id, std::string name, ComputeState state, ObjectId profileSketchId,
                 ObjectId pathSketchId, ObjectId materialId);

    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Sweep"; }

    ObjectId profileSketchId() const noexcept { return profileSketchId_; }
    ObjectId pathSketchId() const noexcept { return pathSketchId_; }

    // BOTH SKETCHES (M26.8). The PROFILE is swept and must close; the PATH is
    // a curve and must never be expected to.
    //
    // The interface used to ask for ONE, so this reported the profile -- and
    // the path was left deletable out from under the sweep that follows it,
    // while the tree called it Failed for being the open curve a path is.
    // The PRIMARY one stays first: `consumedSketchId()` is the front of this
    // list, and "which sketch became this solid's outline" is the profile.
    std::vector<ConsumedSketch> consumedSketches() const override {
        return {ConsumedSketch{profileSketchId_, true},
                ConsumedSketch{pathSketchId_, false}};
    }

    ObjectId materialId() const noexcept override { return materialId_; }
    void clearMaterialReference() noexcept override { materialId_ = kInvalidObjectId; }
    void setMaterialReference(ObjectId materialId) noexcept override {
        materialId_ = materialId;
    }

    const KernelShape& currentShape() const noexcept override { return currentShape_; }
    ComputeState currentState() const noexcept override { return Feature::state(); }

    // Vestigial M1 contract, never called by the document engine (ADR-M3-004).
    bool recompute() override;

    // Real execution: validate the profile loop and the path chain in Core,
    // convert each through its OWN sketch's effective frame (the one
    // conversion site, ADR-M4-002), and sweep through the injected kernel.
    // Commits only on success.
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    ObjectId profileSketchId_;
    ObjectId pathSketchId_;
    ObjectId materialId_;
    KernelShape currentShape_;
};

} // namespace paramcad
