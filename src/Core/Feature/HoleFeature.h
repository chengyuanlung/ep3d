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

// Drills holes at a sketch's POINTS (M20).
//
// The locations are the sketch's SketchPoint entities, which is what makes a
// hole parametric rather than a coordinate: a point can be dimensioned,
// constrained to an edge, made symmetric with another. Onshape's hole tool
// works the same way, and for the same reason -- "20 mm from that edge" has to
// survive the edge moving.
//
// A sketch with no points drills nothing, and that is REFUSED rather than
// quietly succeeding: a hole feature that removes no material is one the user
// meant to do something, and a chain that carries it forward unchanged looks
// exactly like a chain that worked.
//
// WHY NOT A POCKET OF CIRCLES. It would build the same solid today, and it
// would say nothing. A hole knows it is round: the diameter is one number
// rather than a radius constraint on each circle, "through all" is a question
// it can ask, and counterbores and threads are things it can grow later. A
// pocket that happens to be circular is a coincidence the model cannot use.
//
// THE DEPTH is signed, exactly as a Pocket's is (ADR-M17-031), and for the
// same reason: a sketch made on a FACE has its normal pointing out of the
// solid, so the default direction drills away from the material. A depth of
// zero means THROUGH ALL -- the one value that cannot mean a real depth, so it
// is free to mean the thing users ask for most.
class HoleFeature final : public Feature,
                          public IRecomputable,
                          public ISolidFeature,
                          public ISketchConsuming,
                          public IMaterialReferencing {
public:
    HoleFeature(std::string name, ObjectId baseFeatureId, ObjectId sketchId,
                ObjectId diameterParameterId, ObjectId depthParameterId, ObjectId materialId);
    // Restore constructor (deserialization): keeps the persisted id/state.
    HoleFeature(ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
                ObjectId sketchId, ObjectId diameterParameterId, ObjectId depthParameterId,
                ObjectId materialId);

    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Hole"; }

    ObjectId baseFeatureId() const noexcept { return baseFeatureId_; }
    ObjectId consumedSolidId() const noexcept override { return baseFeatureId_; }
    ObjectId sketchId() const noexcept { return sketchId_; }
    ObjectId consumedSketchId() const noexcept override { return sketchId_; }
    ObjectId diameterParameterId() const noexcept { return diameterParameterId_; }
    ObjectId depthParameterId() const noexcept { return depthParameterId_; }

    ObjectId materialId() const noexcept override { return materialId_; }
    void clearMaterialReference() noexcept override { materialId_ = kInvalidObjectId; }
    void setMaterialReference(ObjectId materialId) noexcept override {
        materialId_ = materialId;
    }

    const KernelShape& currentShape() const noexcept override { return currentShape_; }
    ComputeState currentState() const noexcept override { return Feature::state(); }

    // Vestigial M1 contract, never called by the document engine (ADR-M3-004).
    bool recompute() override;

    // Real execution: one circular tool per sketch point, extruded along the
    // sketch's normal and cut from the base. Commits only on success.
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    ObjectId baseFeatureId_;
    ObjectId sketchId_;
    ObjectId diameterParameterId_;
    ObjectId depthParameterId_;
    ObjectId materialId_;
    KernelShape currentShape_;
};

} // namespace paramcad
