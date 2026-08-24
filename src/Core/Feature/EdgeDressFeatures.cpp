#include "Core/Feature/EdgeDressFeatures.h"
#include "Core/Document/ObjectRegistry.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Document/PartDocument.h"
#include "Core/Recompute/RecomputeContext.h"
#include <cstdint>
#include <string>
#include <utility>
#include <optional>
#include <variant>

namespace paramcad {

namespace {

const Parameter* resolveParameter(const ObjectRegistry& registry, ObjectId id) {
    // The const overload yields const pointees (R2R4-M1); these resolvers
    // already returned const pointers, so the projection matches their intent.
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* parameter = std::get_if<const Parameter*>(&*ref);
    return parameter != nullptr ? *parameter : nullptr;
}

const ISolidFeature* resolveSolidFeature(const ObjectRegistry& registry, ObjectId id) {
    if (id == kInvalidObjectId) return nullptr;
    // The const overload yields const pointees (R2R4-M1); these resolvers
    // already returned const pointers, so the projection matches their intent.
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* recomputable = std::get_if<const IRecomputable*>(&*ref);
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
    // engine's blocking of failed prerequisites (see PocketFeature::recompute
    // for the two-half guard record: GATE_E2 in-pass; the persisted-Failed
    // barrier unit-level only, with GATE_E3 pinning the two-layer system).
    // THE BASE IS RESOLVED THROUGH ACTIVITY (M9.3/M9.4, ADR-M9-002).
    //
    // `activeChainBase` walks past links that are suppressed or rolled back, so
    // suppressing a middle feature closes the chain over it: this feature then
    // consumes what the suppressed one consumed. The STORED reference is never
    // rewritten -- suppression is a state, not an edit, and the model still
    // says what the user built.
    //
    // When the walk runs out (the base is inactive and consumes nothing) the
    // answer is kInvalidObjectId and the checks below fail LOUDLY. That is
    // required, not incidental: the base still holds its retained shape
    // (ADR-M3-001), so resolving to it anyway would cut against geometry the
    // user has switched off and produce a healthy-looking wrong solid -- the
    // exact failure M8 gate E exists to prevent, reached from a new direction.
    const ISolidFeature* base = resolveSolidFeature(
        context.registry, context.part().activeChainBase(baseFeatureId_));
    if (base == nullptr)
        return fail(noun + " base feature not found or does not produce a solid");
    if (base->currentState() != ComputeState::Valid)
        return fail(noun + " base feature is not in a valid state");
    if (!base->currentShape().isValid()) return fail(noun + " base feature has no valid shape");

    const Parameter* size = resolveParameter(context.registry, sizeParameterId_);
    if (size == nullptr) return fail(noun + " size parameter not found");

    // Transactional as everywhere: local result, committed only on success --
    // and the kernel's own validation is the single site for the size floor.
    // The selection travels to the kernel as a QUERY, answered against the
    // base's CURRENT shape -- not against whatever it looked like when the
    // user picked (ADR-M17-034).
    ShapeResult result =
        applyDress(*context.kernel, base->currentShape(), edgeSelection_, size->value());
    if (!result)
        return fail(result.message.empty() ? "kernel failed to apply the " + noun
                                           : result.message);
    if (!result.shape.isValid()) return fail("kernel returned an invalid " + noun + " result");

    // The rounded or bevelled strips are this feature's; everything the base
    // knew is carried forward.
    currentShape_ = context.kernel->tagCreatedFaces(result.shape, base->currentShape(),
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

ShapeResult FilletFeature::applyDress(IGeometryKernel& kernel, const KernelShape& base,
                                      const EdgeSelection& selection, double sizeMm) const {
    return kernel.filletEdges(base, selection, sizeMm);
}

ShapeResult ChamferFeature::applyDress(IGeometryKernel& kernel, const KernelShape& base,
                                       const EdgeSelection& selection, double sizeMm) const {
    return kernel.chamferEdges(base, selection, sizeMm);
}

} // namespace paramcad
