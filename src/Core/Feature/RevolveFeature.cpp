#include "Core/Feature/RevolveFeature.h"
#include "Core/Document/ObjectRegistry.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Document/PartDocument.h"
#include "Core/Recompute/RecomputeContext.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"
#include <cmath>
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

RevolveFeature::RevolveFeature(std::string name, ObjectId sketchId, SketchEntityId axisEntityId,
                               ObjectId angleParameterId, ObjectId materialId)
    : Feature(std::move(name)), sketchId_(sketchId), axisEntityId_(axisEntityId),
      angleParameterId_(angleParameterId), materialId_(materialId) {}

RevolveFeature::RevolveFeature(ObjectId id, std::string name, ComputeState state,
                               ObjectId sketchId, SketchEntityId axisEntityId,
                               ObjectId angleParameterId, ObjectId materialId)
    : Feature(id, std::move(name), state), sketchId_(sketchId), axisEntityId_(axisEntityId),
      angleParameterId_(angleParameterId), materialId_(materialId) {}

bool RevolveFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult RevolveFeature::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        setState(ComputeState::Failed);
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    const Sketch* sketch = resolveSketch(context.registry, sketchId_);
    if (sketch == nullptr) return fail("revolve sketch not found");

    const Parameter* angle = resolveParameter(context.registry, angleParameterId_);
    if (angle == nullptr) return fail("revolve angle parameter not found");
    // The one unit check a feature performs, and it is here because the failure
    // mode is silent otherwise: 180 stored in a Millimeter parameter reads as
    // 180 RADIANS (28+ turns, wrapped by nothing -- the kernel refuses > 2*pi,
    // but 3.1 "millimetres" would sail through as a plausible sweep). Length
    // features never had this trap: a length parameter misfiled as Radian
    // fails the solve long before any feature reads it.
    if (angle->unit() != UnitType::Radian)
        return fail("revolve angle parameter must carry UnitType::Radian");

    // The AXIS: a line, in THIS sketch, resolved by SketchEntityId exactly as
    // constraints resolve their targets (ADR-M4-001). Never a guess: a missing
    // entity, or one that is not a line, is a diagnostic.
    const SketchEntity* axisEntity = sketch->findEntity(axisEntityId_);
    if (axisEntity == nullptr) return fail("revolve axis entity is not in the sketch");
    const auto* axisLine = std::get_if<SketchLine>(&axisEntity->geometry);
    if (axisLine == nullptr) return fail("revolve axis entity is not a line");

    // The axis is CONSTRUCTION geometry for profile purposes: it lives in the
    // sketch but contributes no edge, so validation runs with it excluded.
    // Without this, every revolve-with-axis-line sketch was rejected as
    // unchainable -- which is how the first run of every M8.2 gate failed.
    ProfileResult profile = BuildProfile(*sketch, axisEntityId_);
    if (!profile) {
        // The axis MAY instead be a MEMBER of the profile loop (ADR-M8-005):
        // revolving a rectangle about its own edge is the canonical first
        // cylinder. Excluding a member axis breaks the loop -- which is what
        // just failed -- so the profile is rebuilt WITH the axis; if that
        // closes, the axis is a member edge, participating as boundary and
        // axis at once. Round 1 (R1-M1) found the shipped code refusing this
        // case while the ADR claimed it; the fallback is what makes the claim
        // true, and GATE_RH pins it.
        ProfileResult withAxis = BuildProfile(*sketch);
        if (!withAxis) return fail("invalid revolve profile: " + profile.message);
        profile = std::move(withAxis);
    }

    PlanarProfileDefinition definition;
    // A support frame that is GONE fails loudly (M10 gate I). Falling back to
    // the embedded plane would move the geometry back to world XY on its own,
    // silently, which is exactly what a deleted reference must never do.
    if (context.part().sketchSupportFrameIsMissing(sketch->id()))
        return fail("revolve sketch's support frame is missing");
    // The sketch's EFFECTIVE plane, which is its support frame's world
    // transform when it has one (M10.2, ADR-M10-003). Reading `sketch->frame()`
    // here instead would leave the geometry at the origin after the frame moved.
    if (!BuildKernelProfile(*sketch, profile.profile,
                            context.part().effectiveSketchFrame(sketch->id()), definition))
        return fail("revolve profile references an entity that is no longer in the sketch");

    // Axis endpoints to world through the sketch's own frame -- the ONE
    // conversion site (ADR-M4-002). The kernel receives world origin+direction
    // and never re-derives a frame.
    const SketchFrame& frame = sketch->frame();
    const Vec3 start = frame.toWorld(axisLine->start);
    const Vec3 end = frame.toWorld(axisLine->end);
    const Vec3 direction{end.x - start.x, end.y - start.y, end.z - start.z};

    // Transactional as everywhere: local result, committed only on success.
    ShapeResult result =
        context.kernel->revolveProfile(definition, start, direction, angle->value());
    if (!result)
        return fail(result.message.empty() ? "kernel failed to revolve the profile"
                                           : result.message);
    if (!result.shape.isValid()) return fail("kernel returned an invalid revolved shape");

    // Like a Pad, a revolve starts from nothing: every face is its own.
    currentShape_ = context.kernel->tagCreatedFaces(result.shape, KernelShape{},
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
