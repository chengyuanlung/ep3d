#include "Core/Feature/LoftFeature.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Document/PartDocument.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Recompute/RecomputeContext.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"

#include <algorithm>
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

LoftFeature::LoftFeature(std::string name, std::vector<ObjectId> sectionSketchIds,
                         ObjectId materialId)
    : Feature(std::move(name)), sectionSketchIds_(std::move(sectionSketchIds)),
      materialId_(materialId) {}

LoftFeature::LoftFeature(ObjectId id, std::string name, ComputeState state,
                         std::vector<ObjectId> sectionSketchIds, ObjectId materialId)
    : Feature(id, std::move(name), state), sectionSketchIds_(std::move(sectionSketchIds)),
      materialId_(materialId) {}

bool LoftFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult LoftFeature::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        setState(ComputeState::Failed);
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");
    if (sectionSketchIds_.size() < 2)
        return fail("a loft needs at least two sections");

    // THE SAME SKETCH TWICE is refused. Two sections in the same place have no
    // distance between them to run through, so the loft between that pair is
    // degenerate -- and OCCT's complaint about it names neither sketch.
    for (std::size_t i = 0; i < sectionSketchIds_.size(); ++i)
        for (std::size_t j = i + 1; j < sectionSketchIds_.size(); ++j)
            if (sectionSketchIds_[i] == sectionSketchIds_[j])
                return fail("a loft cannot use the same sketch twice");

    std::vector<PlanarProfileDefinition> definitions;
    definitions.reserve(sectionSketchIds_.size());
    for (const ObjectId sketchId : sectionSketchIds_) {
        const Sketch* sketch = resolveSketch(context.registry, sketchId);
        if (sketch == nullptr) return fail("a loft section sketch was not found");

        const ProfileResult profile = BuildProfile(*sketch);
        if (!profile)
            return fail("invalid loft section '" + sketch->name() + "': " + profile.message);

        if (context.part().sketchSupportFrameIsMissing(sketch->id()))
            return fail("loft section '" + sketch->name() + "' has a missing support frame");

        // EACH THROUGH ITS OWN frame -- the sections lie on different planes,
        // which is the entire point of a loft.
        PlanarProfileDefinition definition;
        if (!BuildKernelProfile(*sketch, profile.profile,
                                context.part().effectiveSketchFrame(sketch->id()), definition))
            return fail("loft section '" + sketch->name() +
                        "' references an entity that is no longer in the sketch");
        // IN THE ORDER THEY WERE GIVEN. Nothing here sorts them, and nothing
        // downstream may either.
        definitions.push_back(std::move(definition));
    }

    // Transactional as everywhere: local result, committed only on success.
    ShapeResult result = context.kernel->loftProfiles(definitions);
    if (!result)
        return fail(result.message.empty() ? "kernel failed to loft those sections"
                                           : result.message);
    if (!result.shape.isValid()) return fail("kernel returned an invalid lofted shape");

    // Like a Pad, a loft starts from nothing: every face is its own.
    currentShape_ = context.kernel->tagCreatedFaces(result.shape, KernelShape{},
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
