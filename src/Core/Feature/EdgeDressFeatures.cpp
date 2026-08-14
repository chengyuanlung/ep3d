#include "Core/Feature/EdgeDressFeatures.h"
#include "Core/Document/ObjectRegistry.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/RecomputeContext.h"
#include <string>
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

const ISolidFeature* resolveSolidFeature(const ObjectRegistry& registry, ObjectId id) {
    if (id == kInvalidObjectId) return nullptr;
    const ObjectRegistry::ObjectRef* ref = registry.find(id);
    if (ref == nullptr) return nullptr;
    auto* const* recomputable = std::get_if<IRecomputable*>(ref);
    if (recomputable == nullptr) return nullptr;
    return dynamic_cast<const ISolidFeature*>(*recomputable);
}

} // namespace

EdgeDressFeature::EdgeDressFeature(std::string name, ObjectId baseFeatureId,
                                   ObjectId sizeParameterId, ObjectId materialId)
    : Feature(std::move(name)), baseFeatureId_(baseFeatureId),
      sizeParameterId_(sizeParameterId), materialId_(materialId) {}

EdgeDressFeature::EdgeDressFeature(ObjectId id, std::string name, ComputeState state,
                                   ObjectId baseFeatureId, ObjectId sizeParameterId,
                                   ObjectId materialId)
    : Feature(id, std::move(name), state), baseFeatureId_(baseFeatureId),
      sizeParameterId_(sizeParameterId), materialId_(materialId) {}

bool EdgeDressFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult EdgeDressFeature::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        setState(ComputeState::Failed);
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };
    const std::string noun = dressNoun();

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    // The base, exactly as Pocket resolves and checks it -- including the
    // state check, which is the same documented defense-in-depth behind the
    // engine's blocking of failed prerequisites (see PocketFeature::recompute;
    // GATE_E2's counter guards the engine-level contract for the whole chain).
    const ISolidFeature* base = resolveSolidFeature(context.registry, baseFeatureId_);
    if (base == nullptr)
        return fail(noun + " base feature not found or does not produce a solid");
    if (base->currentState() != ComputeState::Valid)
        return fail(noun + " base feature is not in a valid state");
    if (!base->currentShape().isValid()) return fail(noun + " base feature has no valid shape");

    const Parameter* size = resolveParameter(context.registry, sizeParameterId_);
    if (size == nullptr) return fail(noun + " size parameter not found");

    // Transactional as everywhere: local result, committed only on success --
    // and the kernel's own validation is the single site for the size floor.
    ShapeResult result = applyDress(*context.kernel, base->currentShape(), size->value());
    if (!result)
        return fail(result.message.empty() ? "kernel failed to apply the " + noun
                                           : result.message);
    if (!result.shape.isValid()) return fail("kernel returned an invalid " + noun + " result");

    currentShape_ = std::move(result.shape);
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

ShapeResult FilletFeature::applyDress(IGeometryKernel& kernel, const KernelShape& base,
                                      double sizeMm) const {
    return kernel.filletAllEdges(base, sizeMm);
}

ShapeResult ChamferFeature::applyDress(IGeometryKernel& kernel, const KernelShape& base,
                                       double sizeMm) const {
    return kernel.chamferAllEdges(base, sizeMm);
}

} // namespace paramcad
