#include "Core/Feature/HoleFeature.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Document/PartDocument.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/RecomputeContext.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace paramcad {

namespace {

const Parameter* resolveParameter(const ObjectRegistry& registry, ObjectId id) {
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* parameter = std::get_if<const Parameter*>(&*ref);
    return parameter != nullptr ? *parameter : nullptr;
}

const Sketch* resolveSketch(const ObjectRegistry& registry, ObjectId id) {
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* sketch = std::get_if<const Sketch*>(&*ref);
    return sketch != nullptr ? *sketch : nullptr;
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

HoleFeature::HoleFeature(std::string name, ObjectId baseFeatureId, ObjectId sketchId,
                         ObjectId diameterParameterId, ObjectId depthParameterId,
                         ObjectId materialId)
    : Feature(std::move(name)), baseFeatureId_(baseFeatureId), sketchId_(sketchId),
      diameterParameterId_(diameterParameterId), depthParameterId_(depthParameterId),
      materialId_(materialId) {}

HoleFeature::HoleFeature(ObjectId id, std::string name, ComputeState state,
                         ObjectId baseFeatureId, ObjectId sketchId, ObjectId diameterParameterId,
                         ObjectId depthParameterId, ObjectId materialId)
    : Feature(id, std::move(name), state), baseFeatureId_(baseFeatureId), sketchId_(sketchId),
      diameterParameterId_(diameterParameterId), depthParameterId_(depthParameterId),
      materialId_(materialId) {}

bool HoleFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult HoleFeature::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        setState(ComputeState::Failed);
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    const ISolidFeature* base = resolveSolidFeature(
        context.registry, context.document.activeChainBase(baseFeatureId_));
    if (base == nullptr) return fail("hole base feature not found or does not produce a solid");
    if (base->currentState() != ComputeState::Valid)
        return fail("hole base feature is not in a valid state");
    if (!base->currentShape().isValid()) return fail("hole base feature has no valid shape");

    const Sketch* sketch = resolveSketch(context.registry, sketchId_);
    if (sketch == nullptr) return fail("hole sketch not found");
    if (context.document.sketchSupportFrameIsMissing(sketch->id()))
        return fail("hole sketch's support frame is missing");

    const Parameter* diameter = resolveParameter(context.registry, diameterParameterId_);
    if (diameter == nullptr) return fail("hole diameter parameter not found");
    const Parameter* depth = resolveParameter(context.registry, depthParameterId_);
    if (depth == nullptr) return fail("hole depth parameter not found");
    if (!(diameter->value() > 0.0)) return fail("a hole's diameter has to be positive");

    // WHERE, from the sketch's POINTS. Construction points count: a point is
    // reference geometry by nature, and a user who marks hole centres as
    // construction has said something about the drawing, not about the holes.
    std::vector<Vec2> centres;
    for (const SketchEntity& entity : sketch->entities())
        if (const auto* point = std::get_if<SketchPoint>(&entity.geometry))
            centres.push_back(point->position);
    if (centres.empty())
        return fail("this hole's sketch has no points, so it would drill nothing -- put a point "
                    "where each hole goes");

    // HOW DEEP. Zero means THROUGH ALL, and the depth is then taken from how
    // far the solid actually reaches rather than from a generous guess: a guess
    // works until somebody builds a part deeper than it, and then the hole
    // stops part-way with nothing to say.
    //
    // The extra millimetre is so the tool emerges cleanly on both faces. A cut
    // that ends exactly ON a face leaves OCCT deciding whether the two are
    // coincident, and a coplanar-face boolean is the one case it is worst at.
    double reach = depth->value();
    if (std::fabs(reach) < kMinExtrusionDistanceMm) {
        const KernelBoundsResult bounds = context.kernel->boundsOfShape(base->currentShape());
        if (!bounds.ok)
            return fail("a through hole needs to know how far the part reaches, and this kernel "
                        "cannot say: " +
                        bounds.message);
        const double span = std::hypot(std::hypot(bounds.max.x - bounds.min.x,
                                                  bounds.max.y - bounds.min.y),
                                       bounds.max.z - bounds.min.z);
        reach = span + 1.0;
    }

    // ONE TOOL PER POINT, cut in turn.
    //
    // Not one profile with several loops: a profile's inner loops are HOLES IN
    // IT, so two circles in one profile describe a ring, not two discs, and the
    // solid would come out with the material between them removed as well.
    ShapeResult carried{base->currentShape(), KernelError::None, {}};
    for (const Vec2& centre : centres) {
        PlanarProfileDefinition tool;
        tool.plane = PlaneOfSketchFrame(context.document.effectiveSketchFrame(sketch->id()));
        tool.segments = {ProfileCircleSegment{centre, diameter->value() / 2.0}};

        // A THROUGH hole runs A FULL SPAN EACH WAY from the sketch plane.
        //
        // Not half a span each way, which is what this did first and what a
        // two-metre part caught: the sketch is usually on the TOP face, so
        // half a span downwards stops in the middle -- and the result is a
        // blind hole that looks exactly like a through one from above.
        //
        // A full span each way covers the part wherever the plane sits on it,
        // because no two points of the part are further apart than that. It
        // also puts both ends clear of the material, which matters: a cut that
        // ends exactly ON a face leaves OCCT deciding whether the two are
        // coincident, and a coplanar-face boolean is the one case it is worst
        // at.
        if (std::fabs(depth->value()) < kMinExtrusionDistanceMm) {
            ShapeResult behind = context.kernel->extrudeProfile(tool, -reach);
            if (!behind)
                return fail(behind.message.empty() ? "kernel failed to build a hole tool"
                                                   : behind.message);
            ShapeResult ahead = context.kernel->extrudeProfile(tool, reach);
            if (!ahead)
                return fail(ahead.message.empty() ? "kernel failed to build a hole tool"
                                                  : ahead.message);
            ShapeResult joined = context.kernel->fuseShapes(behind.shape, ahead.shape);
            if (!joined)
                return fail(joined.message.empty() ? "kernel failed to join a hole tool"
                                                   : joined.message);
            ShapeResult cut = context.kernel->subtractShape(carried.shape, joined.shape);
            if (!cut) return fail(cut.message.empty() ? "kernel failed to drill a hole"
                                                      : cut.message);
            carried = std::move(cut);
            continue;
        }

        ShapeResult bit = context.kernel->extrudeProfile(tool, reach);
        if (!bit)
            return fail(bit.message.empty() ? "kernel failed to build a hole tool" : bit.message);
        ShapeResult cut = context.kernel->subtractShape(carried.shape, bit.shape);
        if (!cut)
            return fail(cut.message.empty() ? "kernel failed to drill a hole" : cut.message);
        carried = std::move(cut);
    }

    if (!carried.shape.isValid()) return fail("kernel returned an invalid drilled shape");

    // The bores are this feature's; everything the base knew is carried forward.
    currentShape_ = context.kernel->tagCreatedFaces(carried.shape, base->currentShape(),
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
