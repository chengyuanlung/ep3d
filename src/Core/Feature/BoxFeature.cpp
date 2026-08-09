#include "Core/Feature/BoxFeature.h"
#include "Core/Document/ObjectRegistry.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/RecomputeContext.h"
#include <utility>
#include <variant>

namespace paramcad {

namespace {

const Parameter* resolveParameter(const ObjectRegistry& registry, ObjectId id) {
    const ObjectRegistry::ObjectRef* ref = registry.find(id);
    if (ref == nullptr) return nullptr;
    auto* const* parameter = std::get_if<Parameter*>(ref);
    return parameter != nullptr ? *parameter : nullptr;
}

} // namespace

BoxFeature::BoxFeature(std::string name, ObjectId widthParameterId, ObjectId heightParameterId,
                       ObjectId depthParameterId, ObjectId materialId)
    : Feature(std::move(name)), widthParameterId_(widthParameterId),
      heightParameterId_(heightParameterId), depthParameterId_(depthParameterId),
      materialId_(materialId) {}

BoxFeature::BoxFeature(ObjectId id, std::string name, ComputeState state,
                       ObjectId widthParameterId, ObjectId heightParameterId,
                       ObjectId depthParameterId, ObjectId materialId)
    : Feature(id, std::move(name), state), widthParameterId_(widthParameterId),
      heightParameterId_(heightParameterId), depthParameterId_(depthParameterId),
      materialId_(materialId) {}

bool BoxFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult BoxFeature::recompute(const RecomputeContext& context) {
    if (context.kernel == nullptr) {
        setState(ComputeState::Failed);
        return {RecomputeStatus::Failed, "no geometry kernel configured"};
    }

    const Parameter* width = resolveParameter(context.registry, widthParameterId_);
    const Parameter* height = resolveParameter(context.registry, heightParameterId_);
    const Parameter* depth = resolveParameter(context.registry, depthParameterId_);
    if (width == nullptr || height == nullptr || depth == nullptr) {
        setState(ComputeState::Failed);
        return {RecomputeStatus::Failed, "width/height/depth parameter not found"};
    }

    const BoxDefinition definition{width->value(), height->value(), depth->value()};

    // Transactional (ADR-M3-001/spec 12): build into a LOCAL result first;
    // currentShape_ is reassigned ONLY on success, so a failed build leaves
    // it byte-for-byte unchanged. Staleness is carried entirely by
    // ComputeState, never by mutating or clearing the shape.
    ShapeResult result = context.kernel->createBox(definition);
    if (!result) {
        setState(ComputeState::Failed);
        return {RecomputeStatus::Failed, result.message};
    }
    currentShape_ = result.shape;
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
