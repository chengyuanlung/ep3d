#include "Core/Physics/MassPropertiesNode.h"
#include "Core/Document/ObjectRegistry.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Material/Material.h"
#include "Core/Physics/MassProperties.h"
#include "Core/Recompute/RecomputeContext.h"
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <optional>
#include <variant>

namespace paramcad {

namespace {

// Resolves the source feature by CAPABILITY, not by concrete type: any
// feature that produces a solid qualifies (ISolidFeature). Naming BoxFeature
// here would have needed a second branch for PadFeature and another for every
// future solid feature -- the heterogeneity trap ADR-M3-007 records.
//
// dynamic_cast, never UB on mismatch: a registered IRecomputable that produces
// no solid (e.g. a test stub reusing this id space) yields a controlled
// nullptr.
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

const Material* resolveMaterial(const ObjectRegistry& registry, ObjectId id) {
    if (id == kInvalidObjectId) return nullptr;
    // The const overload yields const pointees (R2R4-M1); these resolvers
    // already returned const pointers, so the projection matches their intent.
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* material = std::get_if<const Material*>(&*ref);
    return material != nullptr ? *material : nullptr;
}

} // namespace

MassPropertiesNode::MassPropertiesNode() : id_(ObjectIdGenerator::Next()) {}

void MassPropertiesNode::setSource(ObjectId boxFeatureId, ObjectId materialId) noexcept {
    boxFeatureId_ = boxFeatureId;
    materialId_ = materialId;
}

// Retain the last valid numbers (ADR-M3-004) but strip their currency, so a
// direct reader of PartDocument::massProperties() can never mistake stale
// values for a current successful result (spec 2 DoD, spec 13).
//
// PartDocument::refreshMassPropertiesCurrency() is the primary guarantee, since
// it also covers the case where the graph blocks this node and never calls the
// code below. This is the defense-in-depth half, covering a direct/out-of-band
// recompute(context) call outside a graph pass -- the same reasoning as the
// upstream-state check in recompute().
RecomputeResult MassPropertiesNode::failAndMarkStale(const RecomputeContext& context,
                                                     std::string message) {
    context.document.massProperties().valid = false;
    return {RecomputeStatus::Failed, std::move(message)};
}

RecomputeResult MassPropertiesNode::recompute(const RecomputeContext& context) {
    const ISolidFeature* box = resolveSolidFeature(context.registry, boxFeatureId_);
    if (box == nullptr) return failAndMarkStale(context, "no solid feature configured");

    // Defense in depth (ADR-M3-004): the graph normally blocks this node with
    // BlockedByDependency before it is even invoked whenever the box failed
    // this pass or is still Failed from an earlier pass; this guards a
    // direct/out-of-band call too (e.g. a unit test calling recompute()
    // outside a graph pass), satisfying "failed build cannot expose
    // dangling/partial shape" beyond what the graph alone guarantees.
    if (box->currentState() != ComputeState::Valid)
        return failAndMarkStale(context, "upstream solid feature is not valid");

    // Density policy (ADR-M3-005, spec 13): zero density is valid (a
    // real, useful "no mass assigned" CAD state); negative/non-finite must
    // fail. No material assigned behaves like density == 0.
    double densityKgPerM3 = 0.0;
    if (materialId_ != kInvalidObjectId) {
        const Material* material = resolveMaterial(context.registry, materialId_);
        if (material == nullptr) return failAndMarkStale(context, "material not found");
        densityKgPerM3 = material->density();
    }
    if (!std::isfinite(densityKgPerM3) || densityKgPerM3 < 0.0)
        return failAndMarkStale(context, "invalid density: must be finite and non-negative");

    if (context.kernel == nullptr)
        return failAndMarkStale(context, "no geometry kernel configured");
    const KernelMassPropertiesResult kmpResult =
        context.kernel->calculateMassProperties(box->currentShape());
    if (!kmpResult) return failAndMarkStale(context, kmpResult.message);

    // Single traceable unit-conversion site (ADR-M3-002): production code
    // must never multiply densityKgPerM3 by a raw mm^3/mm^5 value without
    // going through these two named constants.
    constexpr double kMm3ToM3 = 1e-9;
    constexpr double kMm5ToM5 = 1e-15;
    const KernelMassProperties& kmp = kmpResult.properties;
    const double volumeM3 = kmp.volumeMm3 * kMm3ToM3;

    MassProperties result;
    result.volumeMm3 = kmp.volumeMm3;
    result.massKg = densityKgPerM3 * volumeM3;
    result.centerOfMassMm = kmp.centerOfMassMm; // already the CAD-facing unit, no conversion
    for (std::size_t i = 0; i < kmp.secondMomentMm5.m.size(); ++i)
        result.inertiaTensorKgM2.m[i] = densityKgPerM3 * kmp.secondMomentMm5.m[i] * kMm5ToM5;
    result.valid = true;

    // Commit only on full success (transactional, spec 12): no failure path
    // above overwrites the document's last valid NUMBERS, exactly mirroring
    // BoxFeature::currentShape_'s retention policy -- they only strip the
    // `valid` flag that marks those numbers as current (ADR-M3-004).
    context.document.massProperties() = result;
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
