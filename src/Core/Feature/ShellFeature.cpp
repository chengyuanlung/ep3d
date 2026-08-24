#include "Core/Feature/ShellFeature.h"

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

ShellFeature::ShellFeature(std::string name, ObjectId baseFeatureId, FaceSelection openFaces,
                           ObjectId thicknessParameterId, ObjectId materialId)
    : Feature(std::move(name)), baseFeatureId_(baseFeatureId), openFaces_(std::move(openFaces)),
      thicknessParameterId_(thicknessParameterId), materialId_(materialId) {}

ShellFeature::ShellFeature(ObjectId id, std::string name, ComputeState state,
                           ObjectId baseFeatureId, FaceSelection openFaces,
                           ObjectId thicknessParameterId, ObjectId materialId)
    : Feature(id, std::move(name), state), baseFeatureId_(baseFeatureId),
      openFaces_(std::move(openFaces)), thicknessParameterId_(thicknessParameterId),
      materialId_(materialId) {}

bool ShellFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult ShellFeature::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        setState(ComputeState::Failed);
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    // Through ACTIVITY, exactly as a fillet resolves its base (ADR-M9-002):
    // suppressing a middle feature closes the chain over it, and the stored
    // reference is never rewritten because suppression is a state, not an edit.
    const ISolidFeature* base = resolveSolidFeature(
        context.registry, context.part().activeChainBase(baseFeatureId_));
    if (base == nullptr) return fail("shell base feature not found or does not produce a solid");
    if (base->currentState() != ComputeState::Valid)
        return fail("shell base feature is not in a valid state");
    if (!base->currentShape().isValid()) return fail("shell base feature has no valid shape");

    const Parameter* thickness = resolveParameter(context.registry, thicknessParameterId_);
    if (thickness == nullptr) return fail("shell thickness parameter not found");

    // A SHELL WITH NO OPENING is refused here as well as in the kernel, and the
    // duplication is deliberate: this message names the feature the user is
    // looking at, and it fires before anything is resolved.
    if (openFaces_.empty())
        return fail("this shell has no face to open: a hollow with no way in weighs less than "
                    "the part but looks exactly like it");

    // Transactional as everywhere: local result, committed only on success.
    // The faces travel to the kernel as QUERIES, answered against the base's
    // CURRENT shape -- not against whatever it looked like when the user picked.
    ShapeResult result =
        context.kernel->shellSolid(base->currentShape(), openFaces_, thickness->value());
    if (!result)
        return fail(result.message.empty() ? "kernel failed to hollow the solid" : result.message);
    if (!result.shape.isValid()) return fail("kernel returned an invalid shell");

    // The cavity's walls are this feature's; everything the base knew is
    // carried forward.
    currentShape_ = context.kernel->tagCreatedFaces(result.shape, base->currentShape(),
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
