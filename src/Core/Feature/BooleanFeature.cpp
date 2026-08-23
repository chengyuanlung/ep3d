#include "Core/Feature/BooleanFeature.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Document/PartDocument.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Recompute/RecomputeContext.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace paramcad {

namespace {

const ISolidFeature* resolveSolidFeature(const ObjectRegistry& registry, ObjectId id) {
    if (id == kInvalidObjectId) return nullptr;
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* recomputable = std::get_if<const IRecomputable*>(&*ref);
    if (recomputable == nullptr) return nullptr;
    return dynamic_cast<const ISolidFeature*>(*recomputable);
}

} // namespace

const char* BooleanOperationName(BooleanOperation operation) noexcept {
    switch (operation) {
    case BooleanOperation::Union: return "Union";
    case BooleanOperation::Subtract: return "Subtract";
    case BooleanOperation::Intersect: return "Intersect";
    }
    return "Union";
}

BooleanFeature::BooleanFeature(std::string name, BooleanOperation operation,
                               ObjectId targetFeatureId, ObjectId toolFeatureId,
                               ObjectId materialId)
    : Feature(std::move(name)), operation_(operation), targetFeatureId_(targetFeatureId),
      toolFeatureId_(toolFeatureId), materialId_(materialId) {}

BooleanFeature::BooleanFeature(ObjectId id, std::string name, ComputeState state,
                               BooleanOperation operation, ObjectId targetFeatureId,
                               ObjectId toolFeatureId, ObjectId materialId)
    : Feature(id, std::move(name), state), operation_(operation),
      targetFeatureId_(targetFeatureId), toolFeatureId_(toolFeatureId),
      materialId_(materialId) {}

bool BooleanFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult BooleanFeature::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        setState(ComputeState::Failed);
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    // A SOLID CANNOT BE COMBINED WITH ITSELF. The result would be the same
    // solid for a union, the same solid for an intersect, and nothing at all
    // for a subtract -- three answers, none of them what anybody meant, and
    // two of which look like success.
    if (targetFeatureId_ == toolFeatureId_)
        return fail("a boolean needs two different solids");

    // Both through ACTIVITY, exactly as every other chain feature resolves its
    // base (ADR-M9-002): suppressing a middle feature closes the chain over it.
    const ISolidFeature* target = resolveSolidFeature(
        context.registry, context.document.activeChainBase(targetFeatureId_));
    const ISolidFeature* tool = resolveSolidFeature(
        context.registry, context.document.activeChainBase(toolFeatureId_));
    if (target == nullptr) return fail("boolean target feature not found or is not a solid");
    if (tool == nullptr) return fail("boolean tool feature not found or is not a solid");
    if (target->currentState() != ComputeState::Valid)
        return fail("boolean target feature is not in a valid state");
    if (tool->currentState() != ComputeState::Valid)
        return fail("boolean tool feature is not in a valid state");
    if (!target->currentShape().isValid()) return fail("boolean target has no valid shape");
    if (!tool->currentShape().isValid()) return fail("boolean tool has no valid shape");

    // Transactional as everywhere: local result, committed only on success.
    ShapeResult result;
    switch (operation_) {
    case BooleanOperation::Union:
        result = context.kernel->fuseShapes(target->currentShape(), tool->currentShape());
        break;
    case BooleanOperation::Subtract:
        // TARGET MINUS TOOL, in that order, which is why the order is stored.
        result = context.kernel->subtractShape(target->currentShape(), tool->currentShape());
        break;
    case BooleanOperation::Intersect:
        result = context.kernel->intersectShapes(target->currentShape(), tool->currentShape());
        break;
    }
    if (!result)
        return fail(result.message.empty()
                        ? std::string("kernel failed to ") + BooleanOperationName(operation_)
                        : result.message);
    if (!result.shape.isValid()) return fail("kernel returned an invalid boolean result");

    // Against the TARGET, so the history the target carried comes forward and
    // the faces this boolean made are recorded as its own. The tool's history
    // is deliberately not merged: after the operation its faces either survive
    // as part of the result -- in which case they are new faces of a new solid
    // -- or they are gone, and claiming the tool still made them would let a
    // later query resolve to a face of something that no longer exists.
    currentShape_ = context.kernel->tagCreatedFaces(result.shape, target->currentShape(),
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
