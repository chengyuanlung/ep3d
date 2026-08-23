#include "Core/Feature/DraftFeature.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Document/PartDocument.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/RecomputeContext.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace paramcad {

namespace {

const Parameter* resolveParameter(const ObjectRegistry& registry, ObjectId id) {
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* parameter = std::get_if<const Parameter*>(&*ref);
    return parameter != nullptr ? *parameter : nullptr;
}

const ISolidFeature* resolveSolidFeature(const ObjectRegistry& registry, ObjectId id) {
    if (id == kInvalidObjectId) return nullptr;
    // The const overload yields const pointees (R2R4-M1).
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* recomputable = std::get_if<const IRecomputable*>(&*ref);
    if (recomputable == nullptr) return nullptr;
    return dynamic_cast<const ISolidFeature*>(*recomputable);
}

} // namespace

DraftFeature::DraftFeature(std::string name, ObjectId baseFeatureId, FaceSelection faces,
                           FaceQuery neutral, ObjectId angleParameterId, ObjectId materialId)
    : Feature(std::move(name)), baseFeatureId_(baseFeatureId), faces_(std::move(faces)),
      neutral_(std::move(neutral)), angleParameterId_(angleParameterId),
      materialId_(materialId) {}

DraftFeature::DraftFeature(ObjectId id, std::string name, ComputeState state,
                           ObjectId baseFeatureId, FaceSelection faces, FaceQuery neutral,
                           ObjectId angleParameterId, ObjectId materialId)
    : Feature(id, std::move(name), state), baseFeatureId_(baseFeatureId),
      faces_(std::move(faces)), neutral_(std::move(neutral)),
      angleParameterId_(angleParameterId), materialId_(materialId) {}

bool DraftFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult DraftFeature::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        setState(ComputeState::Failed);
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    const ISolidFeature* base = resolveSolidFeature(
        context.registry, context.document.activeChainBase(baseFeatureId_));
    if (base == nullptr) return fail("draft base feature not found or does not produce a solid");
    if (base->currentState() != ComputeState::Valid)
        return fail("draft base feature is not in a valid state");
    if (!base->currentShape().isValid()) return fail("draft base feature has no valid shape");

    const Parameter* angle = resolveParameter(context.registry, angleParameterId_);
    if (angle == nullptr) return fail("draft angle parameter not found");
    // THE UNIT CHECK a revolve makes, for the same reason: 10 stored in a
    // Millimeter parameter reads as 10 RADIANS. A revolve at least gets caught
    // by the kernel's 2*pi ceiling; a draft of 10 radians wraps to about 213
    // degrees, which is a plausible-looking taper of a shape turned inside out.
    if (angle->unit() != UnitType::Radian)
        return fail("draft angle parameter must carry UnitType::Radian");

    if (faces_.empty())
        return fail("this draft has no face to taper");
    if (neutral_.empty())
        return fail("this draft has no neutral face: the taper needs something to pivot on, "
                    "and that face also decides which way the part is pulled");

    // Transactional as everywhere: local result, committed only on success.
    ShapeResult result = context.kernel->draftFaces(base->currentShape(), faces_, neutral_,
                                                    angle->value());
    if (!result)
        return fail(result.message.empty() ? "kernel failed to taper those faces"
                                           : result.message);
    if (!result.shape.isValid()) return fail("kernel returned an invalid draft result");

    currentShape_ = context.kernel->tagCreatedFaces(result.shape, base->currentShape(),
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
