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
#include <vector>

namespace paramcad {

// WHICH boolean. Three operations, one feature -- they differ in what they
// mean, not in what they need, and three near-identical feature classes would
// be three places to forget the same thing.
enum class BooleanOperation { Union, Subtract, Intersect };

const char* BooleanOperationName(BooleanOperation operation) noexcept;

// Combines TWO solids into one (M21).
//
// This is the multi-body feature, and it is the first thing in this program
// that consumes two solids rather than one. That is why `consumedSolidIds()`
// became plural: with a single id, a boolean could declare only one of its
// operands and the other would stay a live chain tail -- so the viewer would
// draw the leftover next to the result, and the part would appear twice.
//
// ORDER MATTERS FOR SUBTRACT and not for the other two. It is stored rather
// than normalised, because "A minus B" and "B minus A" are different parts and
// nothing here can tell which the user meant. Union and Intersect are
// commutative and the order is kept anyway: it costs nothing, and a feature
// that reordered its own operands would make the file disagree with the
// drawing for no gain.
//
// AN EMPTY INTERSECTION IS REFUSED. Two solids that do not overlap intersect
// to nothing, which is a real geometric answer and a useless feature -- and an
// empty shape carried down a chain looks exactly like a chain that worked: the
// viewer draws nothing and the mass is nought, which is also what a correct
// tiny part looks like.
class BooleanFeature final : public Feature,
                             public IRecomputable,
                             public ISolidFeature,
                             public IMaterialReferencing {
public:
    BooleanFeature(std::string name, BooleanOperation operation, ObjectId targetFeatureId,
                   ObjectId toolFeatureId, ObjectId materialId);
    // Restore constructor (deserialization): keeps the persisted id/state.
    BooleanFeature(ObjectId id, std::string name, ComputeState state, BooleanOperation operation,
                   ObjectId targetFeatureId, ObjectId toolFeatureId, ObjectId materialId);

    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Boolean"; }

    BooleanOperation operation() const noexcept { return operation_; }
    ObjectId targetFeatureId() const noexcept { return targetFeatureId_; }
    ObjectId toolFeatureId() const noexcept { return toolFeatureId_; }

    // BOTH, in the order they are used. The target first, because that is the
    // one a chain walk should step to: a subtract belongs to the thing it cuts.
    std::vector<ObjectId> consumedSolidIds() const override {
        return {targetFeatureId_, toolFeatureId_};
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

    // Real execution: resolve both solids and combine them through the
    // injected kernel. Commits only on success.
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    BooleanOperation operation_;
    ObjectId targetFeatureId_;
    ObjectId toolFeatureId_;
    ObjectId materialId_;
    KernelShape currentShape_;
};

} // namespace paramcad
