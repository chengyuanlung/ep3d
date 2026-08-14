#include "Core/Feature/PocketFeature.h"
#include "Core/Document/ObjectRegistry.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/RecomputeContext.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"
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

const Sketch* resolveSketch(const ObjectRegistry& registry, ObjectId id) {
    const ObjectRegistry::ObjectRef* ref = registry.find(id);
    if (ref == nullptr) return nullptr;
    auto* const* sketch = std::get_if<Sketch*>(ref);
    return sketch != nullptr ? *sketch : nullptr;
}

// The base is resolved by CAPABILITY, not by concrete type (ADR-M3-007, the
// same resolution MassPropertiesNode uses): any feature that produces a solid
// can be pocketed, so tomorrow's Revolve is a legal base with no change here.
const ISolidFeature* resolveSolidFeature(const ObjectRegistry& registry, ObjectId id) {
    if (id == kInvalidObjectId) return nullptr;
    const ObjectRegistry::ObjectRef* ref = registry.find(id);
    if (ref == nullptr) return nullptr;
    auto* const* recomputable = std::get_if<IRecomputable*>(ref);
    if (recomputable == nullptr) return nullptr;
    return dynamic_cast<const ISolidFeature*>(*recomputable);
}

} // namespace

PocketFeature::PocketFeature(std::string name, ObjectId baseFeatureId, ObjectId sketchId,
                             ObjectId depthParameterId, ObjectId materialId)
    : Feature(std::move(name)), baseFeatureId_(baseFeatureId), sketchId_(sketchId),
      depthParameterId_(depthParameterId), materialId_(materialId) {}

PocketFeature::PocketFeature(ObjectId id, std::string name, ComputeState state,
                             ObjectId baseFeatureId, ObjectId sketchId,
                             ObjectId depthParameterId, ObjectId materialId)
    : Feature(id, std::move(name), state), baseFeatureId_(baseFeatureId), sketchId_(sketchId),
      depthParameterId_(depthParameterId), materialId_(materialId) {}

bool PocketFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult PocketFeature::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        setState(ComputeState::Failed);
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    // The base FIRST, and its state is checked, not just its existence. A
    // pocket cut into a Failed base's retained shape would be a boolean against
    // stale geometry, presented as current -- spec 34's Critical in one step
    // (M8 spec 6). The graph guarantees the base ran before this feature in the
    // same pass (the base -> pocket edge); this check is what makes the
    // guarantee inspectable rather than assumed.
    const ISolidFeature* base = resolveSolidFeature(context.registry, baseFeatureId_);
    if (base == nullptr)
        return fail("pocket base feature not found or does not produce a solid");
    if (base->currentState() != ComputeState::Valid)
        return fail("pocket base feature is not in a valid state");
    if (!base->currentShape().isValid()) return fail("pocket base feature has no valid shape");

    const Sketch* sketch = resolveSketch(context.registry, sketchId_);
    if (sketch == nullptr) return fail("pocket sketch not found");

    const Parameter* depth = resolveParameter(context.registry, depthParameterId_);
    if (depth == nullptr) return fail("pocket depth parameter not found");

    // Profile validation is the same pure function Pad runs (ADR-M4-005); the
    // tool is a real profile with all of a profile's rules.
    const ProfileResult profile = BuildProfile(*sketch);
    if (!profile) return fail("invalid pocket profile: " + profile.message);

    PlanarProfileDefinition definition;
    if (!BuildKernelProfile(*sketch, profile.profile, definition))
        return fail("pocket profile references an entity that is no longer in the sketch");

    // The tool grows along the sketch's +normal by Depth -- the SAME direction
    // Pad grows from the same plane (ADR-M8-002). A pocket sketched on the
    // pad's own plane therefore cuts INTO the material it sits on, and a depth
    // equal to the pad's length cuts through. The extrude call itself rejects
    // non-finite and non-positive depths with a diagnostic, so the dimension
    // floor is enforced in exactly one place.
    ShapeResult tool = context.kernel->extrudeProfile(definition, depth->value());
    if (!tool)
        return fail(tool.message.empty() ? "kernel failed to extrude the pocket tool"
                                         : tool.message);
    if (!tool.shape.isValid()) return fail("kernel returned an invalid pocket tool");

    // Transactional, as everywhere (spec 13): build into a LOCAL result;
    // currentShape_ is reassigned only on success, so a failed cut leaves the
    // last valid result byte-for-byte unchanged.
    ShapeResult result = context.kernel->subtractShape(base->currentShape(), tool.shape);
    if (!result)
        return fail(result.message.empty() ? "kernel failed to cut the pocket" : result.message);
    if (!result.shape.isValid()) return fail("kernel returned an invalid cut result");

    currentShape_ = std::move(result.shape);
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
