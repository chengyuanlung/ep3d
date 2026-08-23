#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/FaceQuery.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Recompute/IRecomputable.h"
#include <string>
#include <vector>
#include <string_view>

namespace paramcad {

// Tapers the faces it names, so the part can come out of a mould (M20).
//
// A CHAIN feature like Shell: it consumes a base and produces a new solid.
//
// THE FACES ARE QUERIES, and so is the NEUTRAL face -- re-answered against
// whatever the base currently is, for the reason FaceQuery exists at all
// (ADR-M17-036).
//
// The neutral face is what the taper pivots on: it keeps its size, and the
// pull direction is ITS OWN NORMAL. That is not a convenience -- a draft is
// what lets a part leave a mould, and the direction it leaves in is away from
// the surface it was sitting on.
//
// The angle is a Parameter carrying UnitType::Radian, checked like a revolve's:
// 10 stored in a Millimeter parameter reads as 10 RADIANS, which is not a
// draft, it is a shape turned inside out.
class DraftFeature final : public Feature,
                           public IRecomputable,
                           public ISolidFeature,
                           public IMaterialReferencing {
public:
    DraftFeature(std::string name, ObjectId baseFeatureId, FaceSelection faces,
                 FaceQuery neutral, ObjectId angleParameterId, ObjectId materialId);
    // Restore constructor (deserialization): keeps the persisted id/state.
    DraftFeature(ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
                 FaceSelection faces, FaceQuery neutral, ObjectId angleParameterId,
                 ObjectId materialId);

    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Draft"; }

    ObjectId baseFeatureId() const noexcept { return baseFeatureId_; }
    std::vector<ObjectId> consumedSolidIds() const override { return {baseFeatureId_}; }
    const FaceSelection& faces() const noexcept { return faces_; }
    void setFaces(FaceSelection faces) { faces_ = std::move(faces); }
    const FaceQuery& neutralFace() const noexcept { return neutral_; }
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

    // Real execution: resolve the base solid and the angle Parameter, check
    // its unit, then taper through the injected kernel. Commits only on
    // success.
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    ObjectId baseFeatureId_;
    FaceSelection faces_;
    FaceQuery neutral_;
    ObjectId angleParameterId_;
    ObjectId materialId_;
    KernelShape currentShape_;
};

} // namespace paramcad
