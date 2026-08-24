#include "Core/Feature/PadFeature.h"
#include "Core/Document/ObjectRegistry.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Document/PartDocument.h"
#include "Core/Recompute/RecomputeContext.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"
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

const Sketch* resolveSketch(const ObjectRegistry& registry, ObjectId id) {
    // The const overload yields const pointees (R2R4-M1); these resolvers
    // already returned const pointers, so the projection matches their intent.
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* sketch = std::get_if<const Sketch*>(&*ref);
    return sketch != nullptr ? *sketch : nullptr;
}


} // namespace

PadFeature::PadFeature(std::string name, ObjectId sketchId, ObjectId lengthParameterId,
                       ObjectId materialId)
    : Feature(std::move(name)), sketchId_(sketchId), lengthParameterId_(lengthParameterId),
      materialId_(materialId) {}

PadFeature::PadFeature(ObjectId id, std::string name, ComputeState state, ObjectId sketchId,
                       ObjectId lengthParameterId, ObjectId materialId)
    : Feature(id, std::move(name), state), sketchId_(sketchId),
      lengthParameterId_(lengthParameterId), materialId_(materialId) {}

bool PadFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult PadFeature::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        setState(ComputeState::Failed);
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    const Sketch* sketch = resolveSketch(context.registry, sketchId_);
    if (sketch == nullptr) return fail("pad sketch not found");

    const Parameter* length = resolveParameter(context.registry, lengthParameterId_);
    if (length == nullptr) return fail("pad length parameter not found");

    // Profile validation is a pure function run here, never a cached node
    // (ADR-M4-005): a second cached copy of derived state would need its own
    // currency flag kept coherent with the graph, which is exactly the failure
    // mode ADR-M3-006/007 record.
    const ProfileResult profile = BuildProfile(*sketch);
    if (!profile) return fail("invalid profile: " + profile.message);

    PlanarProfileDefinition definition;
    // A support frame that is GONE fails loudly (M10 gate I). Falling back to
    // the embedded plane would move the geometry back to world XY on its own,
    // silently, which is exactly what a deleted reference must never do.
    if (context.part().sketchSupportFrameIsMissing(sketch->id()))
        return fail("pad sketch's support frame is missing");
    // The sketch's EFFECTIVE plane, which is its support frame's world
    // transform when it has one (M10.2, ADR-M10-003). Reading `sketch->frame()`
    // here instead would leave the geometry at the origin after the frame moved.
    if (!BuildKernelProfile(*sketch, profile.profile,
                            context.part().effectiveSketchFrame(sketch->id()), definition))
        return fail("profile references an entity that is no longer in the sketch");

    // Transactional (spec 13, mirroring BoxFeature): build into a LOCAL result
    // first. currentShape_ is reassigned ONLY on success, so a failed build
    // leaves the last valid shape byte-for-byte unchanged and no partial solid
    // is ever committed.
    ShapeResult result = context.kernel->extrudeProfile(definition, length->value());
    if (!result)
        return fail(result.message.empty() ? "kernel failed to extrude the profile"
                                           : result.message);
    if (!result.shape.isValid()) return fail("kernel returned an invalid shape");

    // WHO MADE THIS (M17.13, ADR-M17-035). A pad starts from nothing, so
    // every face of the prism is its own -- which is what lets a later feature
    // say "the edges of everything Pad001 created".
    currentShape_ = context.kernel->tagCreatedFaces(result.shape, KernelShape{},
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
