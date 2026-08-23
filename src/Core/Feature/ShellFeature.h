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

// Hollows the solid a previous feature made, opening the faces it names (M20).
//
// A CHAIN feature, like Pocket, Fillet and Chamfer: it consumes a base and
// produces a new solid, so `consumedSolidId()` names the base and it is never
// a chain root. There is nothing to hollow before something has been built.
//
// THE FACES ARE QUERIES, re-answered against whatever the base currently is.
// A shell that remembered "face 3" would open whatever happens to be third
// after the part changed -- which is the whole reason FaceQuery exists
// (ADR-M17-036), and the reason a fillet's edges are queries too.
//
// The thickness is a Parameter, so a wall can be driven by an expression and
// changed from the parameter table like every other dimension in the document.
class ShellFeature final : public Feature,
                           public IRecomputable,
                           public ISolidFeature,
                           public IMaterialReferencing {
public:
    ShellFeature(std::string name, ObjectId baseFeatureId, FaceSelection openFaces,
                 ObjectId thicknessParameterId, ObjectId materialId);
    // Restore constructor (deserialization): keeps the persisted id/state.
    ShellFeature(ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
                 FaceSelection openFaces, ObjectId thicknessParameterId, ObjectId materialId);

    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Shell"; }

    ObjectId baseFeatureId() const noexcept { return baseFeatureId_; }
    std::vector<ObjectId> consumedSolidIds() const override { return {baseFeatureId_}; }
    const FaceSelection& openFaces() const noexcept { return openFaces_; }
    void setOpenFaces(FaceSelection faces) { openFaces_ = std::move(faces); }
    ObjectId thicknessParameterId() const noexcept { return thicknessParameterId_; }

    ObjectId materialId() const noexcept override { return materialId_; }
    void clearMaterialReference() noexcept override { materialId_ = kInvalidObjectId; }
    void setMaterialReference(ObjectId materialId) noexcept override {
        materialId_ = materialId;
    }

    const KernelShape& currentShape() const noexcept override { return currentShape_; }
    ComputeState currentState() const noexcept override { return Feature::state(); }

    // Vestigial M1 contract, never called by the document engine (ADR-M3-004).
    bool recompute() override;

    // Real execution: resolve the base solid and the thickness Parameter, then
    // hollow through the injected kernel. Commits only on success.
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    ObjectId baseFeatureId_;
    FaceSelection openFaces_;
    ObjectId thicknessParameterId_;
    ObjectId materialId_;
    KernelShape currentShape_;
};

} // namespace paramcad
