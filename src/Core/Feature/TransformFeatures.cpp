#include "Core/Feature/TransformFeatures.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Document/PartDocument.h"
#include "Core/Geometry/Transform.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/RecomputeContext.h"

#include <cmath>
#include <optional>
#include <utility>
#include <variant>

namespace paramcad {

namespace {

const Parameter* resolveParameter(const ObjectRegistry& registry, ObjectId id) {
    if (id == kInvalidObjectId) return nullptr;
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* parameter = std::get_if<const Parameter*>(&*ref);
    return parameter != nullptr ? *parameter : nullptr;
}

const ISolidFeature* resolveSolidFeature(const ObjectRegistry& registry, ObjectId id) {
    if (id == kInvalidObjectId) return nullptr;
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* recomputable = std::get_if<const IRecomputable*>(&*ref);
    if (recomputable == nullptr) return nullptr;
    return dynamic_cast<const ISolidFeature*>(*recomputable);
}

} // namespace

TransformFeature::TransformFeature(std::string name, ObjectId baseFeatureId, ObjectId frameId,
                                   ObjectId materialId)
    : Feature(std::move(name)), baseFeatureId_(baseFeatureId), frameId_(frameId),
      materialId_(materialId) {}

TransformFeature::TransformFeature(ObjectId id, std::string name, ComputeState state,
                                   ObjectId baseFeatureId, ObjectId frameId, ObjectId materialId)
    : Feature(id, std::move(name), state), baseFeatureId_(baseFeatureId), frameId_(frameId),
      materialId_(materialId) {}

RecomputeResult TransformFeature::recompute(const RecomputeContext& context) {
    const std::string noun(typeName());
    const auto fail = [&](std::string message) {
        setState(ComputeState::Failed);
        // The last good shape is RETAINED (ADR-M3-001/004): staleness travels
        // through ComputeState, never by throwing the geometry away.
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    // The base, resolved through ACTIVITY exactly as Pocket and the dress
    // features resolve theirs (ADR-M9-002): suppressed and rolled-back links
    // are walked past, and a chain that runs out fails loudly rather than
    // building on geometry the user switched off.
    const ISolidFeature* base =
        resolveSolidFeature(context.registry, context.document.activeChainBase(baseFeatureId_));
    if (base == nullptr)
        return fail(noun + " base feature not found or does not produce a solid");
    if (base->currentState() != ComputeState::Valid)
        return fail(noun + " base feature is not in a valid state");
    if (!base->currentShape().isValid()) return fail(noun + " base feature has no valid shape");

    // The frame must exist. A transform defined against a frame that is gone
    // has no plane and no axis, and defaulting to the world origin would move
    // the geometry silently -- the same refusal a missing sketch support frame
    // gets (M10 gate I).
    if (context.document.findFrame(frameId_) == nullptr)
        return fail(noun + " frame is missing");
    const Transform3D frameWorld = context.document.worldTransform(frameId_);

    ShapeResult copies = buildCopies(context, base->currentShape(), frameWorld);
    if (!copies) return fail(copies.message.empty() ? "kernel failed to build the " + noun
                                                    : copies.message);
    if (!copies.shape.isValid()) return fail("kernel returned an invalid " + noun + " result");

    // Transactional: the result is local until every step has succeeded.
    ShapeResult fused = context.kernel->fuseShapes(base->currentShape(), copies.shape);
    if (!fused)
        return fail(fused.message.empty() ? "kernel failed to fuse the " + noun : fused.message);
    if (!fused.shape.isValid()) return fail("kernel returned an invalid fuse result");

    shape_ = std::move(fused.shape);
    setState(ComputeState::Valid);
    return RecomputeResult{RecomputeStatus::Success, {}};
}

// --- Mirror -----------------------------------------------------------------

MirrorFeature::MirrorFeature(std::string name, ObjectId baseFeatureId, ObjectId frameId,
                             ObjectId materialId)
    : TransformFeature(std::move(name), baseFeatureId, frameId, materialId) {}

MirrorFeature::MirrorFeature(ObjectId id, std::string name, ComputeState state,
                             ObjectId baseFeatureId, ObjectId frameId, ObjectId materialId)
    : TransformFeature(id, std::move(name), state, baseFeatureId, frameId, materialId) {}

ShapeResult MirrorFeature::buildCopies(const RecomputeContext& context, const KernelShape& base,
                                       const Transform3D& frameWorld) {
    // The frame's XY plane: its origin, and its local +Z as the normal. Same
    // convention SketchFrame uses for a sketch plane, because a frame should
    // mean one thing (roadmap §5 -- sketch-on-frame and mirror-about-frame are
    // the same coordinate system read two ways).
    const Vec3 normal = RotateByQuaternion(frameWorld.rotation, Vec3{0.0, 0.0, 1.0});
    return context.kernel->mirrorShape(base, frameWorld.translation, normal);
}

// --- Pattern ----------------------------------------------------------------

PatternFeature::PatternFeature(std::string name, ObjectId baseFeatureId, ObjectId frameId,
                               ObjectId countParameterId, ObjectId spacingParameterId,
                               ObjectId materialId)
    : TransformFeature(std::move(name), baseFeatureId, frameId, materialId),
      countParameterId_(countParameterId), spacingParameterId_(spacingParameterId) {}

PatternFeature::PatternFeature(ObjectId id, std::string name, ComputeState state,
                               ObjectId baseFeatureId, ObjectId frameId,
                               ObjectId countParameterId, ObjectId spacingParameterId,
                               ObjectId materialId)
    : TransformFeature(id, std::move(name), state, baseFeatureId, frameId, materialId),
      countParameterId_(countParameterId), spacingParameterId_(spacingParameterId) {}

ShapeResult PatternFeature::buildCopies(const RecomputeContext& context, const KernelShape& base,
                                        const Transform3D& frameWorld) {
    const Parameter* count = resolveParameter(context.registry, countParameterId_);
    if (count == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "pattern count parameter not found"};
    const Parameter* spacing = resolveParameter(context.registry, spacingParameterId_);
    if (spacing == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "pattern spacing parameter not found"};

    // A count is a WHOLE number of instances, and the base is the first of
    // them. Refusing a fractional or non-finite count here rather than
    // truncating it is the same choice ADR-M3-009 made for dimensions: a value
    // that cannot mean what it says is an error, not something to round.
    const double rawCount = count->value();
    if (!std::isfinite(rawCount) || rawCount < 1.0 ||
        rawCount != static_cast<double>(static_cast<long long>(rawCount)))
        return ShapeResult{KernelShape{}, KernelError::NonFinite,
                           "pattern count must be a whole number of at least 1"};
    if (!std::isfinite(spacing->value()) || spacing->value() <= 0.0)
        return ShapeResult{KernelShape{}, KernelError::NonFinite,
                           "pattern spacing must be finite and positive"};

    const long long instances = static_cast<long long>(rawCount);
    // A count of 1 is the base alone: legal, and it must produce the base
    // rather than an error or an empty shape -- "the pattern is switched down
    // to one" is an ordinary state a user passes through while typing.
    if (instances == 1)
        return context.kernel->translateShape(base, Vec3{0.0, 0.0, 0.0});

    // The frame's local +X is the direction, its world orientation applied.
    const Vec3 axis = RotateByQuaternion(frameWorld.rotation, Vec3{1.0, 0.0, 0.0});
    KernelShape accumulated;
    for (long long i = 1; i < instances; ++i) {
        const double distance = spacing->value() * static_cast<double>(i);
        ShapeResult copy = context.kernel->translateShape(
            base, Vec3{axis.x * distance, axis.y * distance, axis.z * distance});
        if (!copy) return copy;
        if (!accumulated.isValid()) {
            accumulated = std::move(copy.shape);
            continue;
        }
        ShapeResult merged = context.kernel->fuseShapes(accumulated, copy.shape);
        if (!merged) return merged;
        accumulated = std::move(merged.shape);
    }
    return ShapeResult{std::move(accumulated), KernelError::None, {}};
}

} // namespace paramcad
