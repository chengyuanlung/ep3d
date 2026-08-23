#include "Core/Feature/SweepFeature.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Document/PartDocument.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Recompute/RecomputeContext.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace paramcad {

namespace {

const Sketch* resolveSketch(const ObjectRegistry& registry, ObjectId id) {
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* sketch = std::get_if<const Sketch*>(&*ref);
    return sketch != nullptr ? *sketch : nullptr;
}

} // namespace

SweepFeature::SweepFeature(std::string name, ObjectId profileSketchId, ObjectId pathSketchId,
                           ObjectId materialId)
    : Feature(std::move(name)), profileSketchId_(profileSketchId), pathSketchId_(pathSketchId),
      materialId_(materialId) {}

SweepFeature::SweepFeature(ObjectId id, std::string name, ComputeState state,
                           ObjectId profileSketchId, ObjectId pathSketchId, ObjectId materialId)
    : Feature(id, std::move(name), state), profileSketchId_(profileSketchId),
      pathSketchId_(pathSketchId), materialId_(materialId) {}

bool SweepFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult SweepFeature::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        setState(ComputeState::Failed);
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    // THE TWO SKETCHES MUST BE DIFFERENT, and this is checked before anything
    // else because the failure it prevents is silent. A section swept along a
    // spine drawn on its OWN plane is swept along a direction inside itself:
    // OCCT returns a surface, its volume integrates to zero, and every reading
    // downstream -- mass, bounds, the drawn faces -- looks like a result.
    if (profileSketchId_ == pathSketchId_)
        return fail("a sweep needs two different sketches: a section swept along a path on its "
                    "own plane has no volume");

    const Sketch* profileSketch = resolveSketch(context.registry, profileSketchId_);
    if (profileSketch == nullptr) return fail("sweep profile sketch not found");
    const Sketch* pathSketch = resolveSketch(context.registry, pathSketchId_);
    if (pathSketch == nullptr) return fail("sweep path sketch not found");

    const ProfileResult profile = BuildProfile(*profileSketch);
    if (!profile) return fail("invalid sweep profile: " + profile.message);

    const PathResult path = BuildPath(*pathSketch);
    if (!path) return fail("invalid sweep path: " + path.message);

    // A support frame that is GONE fails loudly (M10 gate I). Falling back to
    // the embedded plane would move the geometry back to world XY on its own,
    // silently, which is exactly what a deleted reference must never do.
    if (context.document.sketchSupportFrameIsMissing(profileSketch->id()))
        return fail("sweep profile sketch's support frame is missing");
    if (context.document.sketchSupportFrameIsMissing(pathSketch->id()))
        return fail("sweep path sketch's support frame is missing");

    // EACH THROUGH ITS OWN sketch's effective frame. The whole point of a sweep
    // is that the two lie on different planes, so sharing one frame here would
    // flatten the path onto the profile and produce the degenerate case the
    // check above exists to refuse.
    PlanarProfileDefinition profileDefinition;
    if (!BuildKernelProfile(*profileSketch, profile.profile,
                            context.document.effectiveSketchFrame(profileSketch->id()),
                            profileDefinition))
        return fail("sweep profile references an entity that is no longer in the sketch");

    PlanarPathDefinition pathDefinition;
    if (!BuildKernelPath(*pathSketch, path.path,
                         context.document.effectiveSketchFrame(pathSketch->id()),
                         pathDefinition))
        return fail("sweep path references an entity that is no longer in the sketch");

    // Transactional as everywhere: local result, committed only on success.
    ShapeResult result = context.kernel->sweepProfile(profileDefinition, pathDefinition);
    if (!result)
        return fail(result.message.empty() ? "kernel failed to sweep the profile"
                                           : result.message);
    if (!result.shape.isValid()) return fail("kernel returned an invalid swept shape");

    // Like a Pad, a sweep starts from nothing: every face is its own.
    currentShape_ = context.kernel->tagCreatedFaces(result.shape, KernelShape{},
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
