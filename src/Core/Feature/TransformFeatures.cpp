#include "Core/Document/ResolveObject.h"
#include "Core/Feature/TransformFeatures.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Document/PartDocument.h"
#include "Core/Geometry/Transform.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/RecomputeContext.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <string>
#include <optional>
#include <utility>
#include <variant>

namespace paramcad {

namespace {

} // namespace

TransformFeature::TransformFeature(std::string name, ObjectId baseFeatureId, ObjectId frameId,
                                   ObjectId materialId)
    : Feature(std::move(name)), baseFeatureId_(baseFeatureId), frameId_(frameId),
      materialId_(materialId) {}

TransformFeature::TransformFeature(ObjectId id, std::string name, ComputeState state,
                                   ObjectId baseFeatureId, ObjectId frameId, ObjectId materialId)
    : Feature(id, std::move(name), state), baseFeatureId_(baseFeatureId), frameId_(frameId),
      materialId_(materialId) {}

bool TransformFeature::frameWorldOrFail(const RecomputeContext& context, Transform3D& out,
                                       std::string& why) const {
    if (context.part().findFrame(frameId_) == nullptr) {
        why = std::string(typeName()) + " frame is missing";
        return false;
    }
    out = context.part().worldTransform(frameId_);
    return true;
}

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
        ResolveSolidFeature(context.registry, context.part().activeChainBase(baseFeatureId_));
    if (base == nullptr)
        return fail(noun + " base feature not found or does not produce a solid");
    if (base->currentState() != ComputeState::Valid)
        return fail(noun + " base feature is not in a valid state");
    if (!base->currentShape().isValid()) return fail(noun + " base feature has no valid shape");

    ShapeResult copies = buildCopies(context, base->currentShape());
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

ShapeResult MirrorFeature::buildCopies(const RecomputeContext& context,
                                       const KernelShape& base) {
    Transform3D frameWorld;
    std::string why;
    if (!frameWorldOrFail(context, frameWorld, why))
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed, why};
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

ShapeResult PatternFeature::buildCopies(const RecomputeContext& context,
                                        const KernelShape& base) {
    Transform3D frameWorld;
    std::string why;
    if (!frameWorldOrFail(context, frameWorld, why))
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed, why};
    const Parameter* count = ResolveParameter(context.registry, countParameterId_);
    if (count == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "pattern count parameter not found"};
    const Parameter* spacing = ResolveParameter(context.registry, spacingParameterId_);
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

CircularPatternFeature::CircularPatternFeature(std::string name, ObjectId baseFeatureId,
                                               ObjectId frameId, ObjectId countParameterId,
                                               ObjectId stepParameterId, ObjectId materialId)
    : TransformFeature(std::move(name), baseFeatureId, frameId, materialId),
      countParameterId_(countParameterId), stepParameterId_(stepParameterId) {}

CircularPatternFeature::CircularPatternFeature(ObjectId id, std::string name, ComputeState state,
                                               ObjectId baseFeatureId, ObjectId frameId,
                                               ObjectId countParameterId,
                                               ObjectId stepParameterId, ObjectId materialId)
    : TransformFeature(id, std::move(name), state, baseFeatureId, frameId, materialId),
      countParameterId_(countParameterId), stepParameterId_(stepParameterId) {}

ShapeResult CircularPatternFeature::buildCopies(const RecomputeContext& context,
                                                const KernelShape& base) {
    Transform3D frameWorld;
    std::string why;
    if (!frameWorldOrFail(context, frameWorld, why))
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed, why};

    const Parameter* count = ResolveParameter(context.registry, countParameterId_);
    if (count == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "circular pattern count parameter not found"};
    const Parameter* step = ResolveParameter(context.registry, stepParameterId_);
    if (step == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "circular pattern step parameter not found"};
    // THE UNIT, checked the way a revolve's is. A step of 60 stored as
    // millimetres reads as 60 RADIANS -- nine and a half turns -- and lands
    // nowhere near where the drawing said, while looking like a number.
    if (step->unit() != UnitType::Radian)
        return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                           "circular pattern step parameter must carry UnitType::Radian"};

    // The same whole-number rule the linear pattern states, for the same
    // reason: a value that cannot mean what it says is an error, not something
    // to round (ADR-M3-009).
    const double rawCount = count->value();
    if (!std::isfinite(rawCount) || rawCount < 1.0 ||
        rawCount != static_cast<double>(static_cast<long long>(rawCount)))
        return ShapeResult{KernelShape{}, KernelError::NonFinite,
                           "circular pattern count must be a whole number of at least 1"};
    if (!std::isfinite(step->value()) || std::fabs(step->value()) < kMinRevolveAngleRad)
        return ShapeResult{KernelShape{}, KernelError::NonFinite,
                           "circular pattern step must be a finite, non-zero angle"};

    const long long instances = static_cast<long long>(rawCount);
    if (instances == 1) return context.kernel->translateShape(base, Vec3{0.0, 0.0, 0.0});

    // The frame's local +Z is the axis, its world orientation applied -- the
    // same convention the mirror's plane normal and the linear pattern's
    // direction follow.
    const Vec3 axis = RotateByQuaternion(frameWorld.rotation, Vec3{0.0, 0.0, 1.0});
    const Vec3 origin = frameWorld.translation;
    KernelShape accumulated;
    for (long long i = 1; i < instances; ++i) {
        // EACH COPY FROM THE BASE, by i steps -- not from the previous copy by
        // one. Chaining would accumulate the rounding of every step before it,
        // so the last instance of a long ring would land visibly off.
        ShapeResult copy = context.kernel->rotateShape(base, origin, axis,
                                                       step->value() * static_cast<double>(i));
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

CurvePatternFeature::CurvePatternFeature(std::string name, ObjectId baseFeatureId,
                                         ObjectId pathSketchId, ObjectId countParameterId,
                                         ObjectId materialId)
    // NO FRAME. A curve pattern is defined against a path, so it carries
    // kInvalidObjectId here and never asks frameWorldOrFail for one.
    : TransformFeature(std::move(name), baseFeatureId, kInvalidObjectId, materialId),
      pathSketchId_(pathSketchId), countParameterId_(countParameterId) {}

CurvePatternFeature::CurvePatternFeature(ObjectId id, std::string name, ComputeState state,
                                         ObjectId baseFeatureId, ObjectId pathSketchId,
                                         ObjectId countParameterId, ObjectId materialId)
    : TransformFeature(id, std::move(name), state, baseFeatureId, kInvalidObjectId, materialId),
      pathSketchId_(pathSketchId), countParameterId_(countParameterId) {}

ShapeResult CurvePatternFeature::buildCopies(const RecomputeContext& context,
                                             const KernelShape& base) {
    const Parameter* count = ResolveParameter(context.registry, countParameterId_);
    if (count == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "curve pattern count parameter not found"};
    const double rawCount = count->value();
    if (!std::isfinite(rawCount) || rawCount < 1.0 ||
        rawCount != static_cast<double>(static_cast<long long>(rawCount)))
        return ShapeResult{KernelShape{}, KernelError::NonFinite,
                           "curve pattern count must be a whole number of at least 1"};

    const Sketch* path = ResolveSketch(context.registry, pathSketchId_);
    if (path == nullptr)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "curve pattern path sketch not found"};
    if (context.part().sketchSupportFrameIsMissing(path->id()))
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "curve pattern path sketch's support frame is missing"};

    // THE SAME CHAIN A SWEEP FOLLOWS (M19). One walker, so "along that curve"
    // means the same thing to both -- including which end it starts from,
    // which BuildPath decides from the drawing rather than from entity ids.
    const PathResult walked = BuildPath(*path);
    if (!walked)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "invalid curve pattern path: " + walked.message};

    const long long instances = static_cast<long long>(rawCount);
    if (instances == 1) return context.kernel->translateShape(base, Vec3{0.0, 0.0, 0.0});

    // WHERE ALONG IT. The stations are sampled from the path's own geometry in
    // the sketch's plane, then placed in the world through that sketch's
    // effective frame -- the one conversion site (ADR-M4-002).
    const SketchFrame frame = context.part().effectiveSketchFrame(path->id());
    const std::vector<Vec2> walkPoints = PathPolyline(*path, walked.path);
    if (walkPoints.size() < 2)
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "the curve pattern's path is too short to space anything along"};

    // Arc length at every sample, so the stations can be EVENLY SPACED by
    // length rather than by parameter. Parameter spacing bunches the copies up
    // wherever the curve is slow, which on a spline is most of it.
    std::vector<double> along;
    along.reserve(walkPoints.size());
    along.push_back(0.0);
    for (std::size_t i = 1; i < walkPoints.size(); ++i)
        along.push_back(along.back() + std::hypot(walkPoints[i].x - walkPoints[i - 1].x,
                                                  walkPoints[i].y - walkPoints[i - 1].y));
    const double total = along.back();
    if (!(total > kMinExtrusionDistanceMm))
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "the curve pattern's path has no length"};

    const auto pointAt = [&](double distance) {
        for (std::size_t i = 1; i < along.size(); ++i) {
            if (along[i] < distance) continue;
            const double span = along[i] - along[i - 1];
            const double t = span > 0.0 ? (distance - along[i - 1]) / span : 0.0;
            return Vec2{walkPoints[i - 1].x + (walkPoints[i].x - walkPoints[i - 1].x) * t,
                        walkPoints[i - 1].y + (walkPoints[i].y - walkPoints[i - 1].y) * t};
        }
        return walkPoints.back();
    };

    // Instance 0 IS the base, sitting where it already is, and the offsets are
    // measured from the path's start. So a base drawn at the path's start walks
    // the whole path, and one drawn elsewhere keeps its offset from it -- which
    // is what a user who put the two side by side expects.
    const Vec3 start = frame.toWorld(walkPoints.front());
    KernelShape accumulated;
    for (long long i = 1; i < instances; ++i) {
        const double distance = total * static_cast<double>(i) / static_cast<double>(instances - 1);
        const Vec3 station = frame.toWorld(pointAt(distance));
        ShapeResult copy = context.kernel->translateShape(
            base, Vec3{station.x - start.x, station.y - start.y, station.z - start.z});
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
