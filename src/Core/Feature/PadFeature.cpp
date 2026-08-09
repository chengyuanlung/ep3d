#include "Core/Feature/PadFeature.h"
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

// Translates a validated semantic loop into the kernel-neutral definition
// (ADR-M4-003). Every segment is looked up by SketchEntityId, never by
// position, and orientation from the validator is applied here so the kernel
// receives a loop that already reads start-to-end.
bool BuildKernelProfile(const Sketch& sketch, const ValidatedProfile& validated,
                        PlanarProfileDefinition& out) {
    const SketchFrame& frame = sketch.frame();
    out.plane.origin = frame.toWorld(Vec2{0.0, 0.0});
    out.plane.uAxis = frame.uAxis();
    out.plane.vAxis = frame.vAxis();
    out.plane.normal = frame.normal();
    out.segments.clear();
    out.segments.reserve(validated.outer.entities.size());

    for (const OrientedSketchEntityRef& ref : validated.outer.entities) {
        const SketchEntity* entity = sketch.findEntity(ref.entityId);
        if (entity == nullptr) return false;

        if (const auto* line = std::get_if<SketchLine>(&entity->geometry)) {
            out.segments.push_back(ref.reversed
                                       ? ProfileLineSegment{line->end, line->start}
                                       : ProfileLineSegment{line->start, line->end});
        } else if (const auto* arc = std::get_if<SketchArc>(&entity->geometry)) {
            // Reversing an arc swaps its endpoints AND its direction, so the
            // traversal still runs start-to-end along the same curve.
            out.segments.push_back(
                ref.reversed ? ProfileArcSegment{arc->center, arc->radiusMm, arc->endAngleRad,
                                                 arc->startAngleRad, !arc->counterClockwise}
                             : ProfileArcSegment{arc->center, arc->radiusMm, arc->startAngleRad,
                                                 arc->endAngleRad, arc->counterClockwise});
        } else if (const auto* circle = std::get_if<SketchCircle>(&entity->geometry)) {
            out.segments.push_back(ProfileCircleSegment{circle->center, circle->radiusMm});
        } else {
            return false; // a Point can never be a loop member
        }
    }
    return true;
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
    if (!BuildKernelProfile(*sketch, profile.profile, definition))
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

    currentShape_ = std::move(result.shape);
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
