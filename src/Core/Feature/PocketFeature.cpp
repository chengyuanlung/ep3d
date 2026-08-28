#include "Core/Document/ResolveObject.h"
#include "Core/Feature/PocketFeature.h"
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
    // (M8 spec 6).
    //
    // DEFENSE IN DEPTH, stated honestly: through the engine this branch is
    // UNREACHABLE, because dependents of failed nodes are blocked, never
    // invoked (DocumentRecomputeEngine), and RecomputeContext cannot be built
    // from public API to reach it directly. It is therefore NOT mutation-
    // guarded, and no test claims otherwise -- M7's review found an ADR
    // asserting a test that did not exist, and this comment exists so this
    // check can never grow that sentence. The engine contract this check
    // backs up is guarded in two halves (corrected TWICE by review: round 1
    // moved the credit off GATE_E2; round 2 caught the correction crediting
    // GATE_E3 with more than it kills): in-pass blocking by GATE_E2's
    // subtract counter; the persisted-Failed barrier (a base that failed in
    // a PREVIOUS pass, not dirty in this one) by
    // DependencyGraphTests.StaleFailureGates* and
    // EdgeRewireAcrossFailedPrerequisite -- UNIT LEVEL ONLY. GATE_E3
    // pins the TWO-LAYER SYSTEM (barrier + this very check) and goes red
    // only if both regress; deleting the barrier alone keeps every
    // integration test green because this check masks it. That is defense
    // in depth working, and it is stated here so no one reads GATE_E3 as a
    // barrier pin.
    // THE BASE IS RESOLVED THROUGH ACTIVITY (M9.3/M9.4, ADR-M9-002).
    //
    // `activeChainBase` walks past links that are suppressed or rolled back, so
    // suppressing a middle feature closes the chain over it: this feature then
    // consumes what the suppressed one consumed. The STORED reference is never
    // rewritten -- suppression is a state, not an edit, and the model still
    // says what the user built.
    //
    // When the walk runs out (the base is inactive and consumes nothing) the
    // answer is kInvalidObjectId and the checks below fail LOUDLY. That is
    // required, not incidental: the base still holds its retained shape
    // (ADR-M3-001), so resolving to it anyway would cut against geometry the
    // user has switched off and produce a healthy-looking wrong solid -- the
    // exact failure M8 gate E exists to prevent, reached from a new direction.
    const ISolidFeature* base = ResolveSolidFeature(
        context.registry, context.part().activeChainBase(baseFeatureId_));
    if (base == nullptr)
        return fail("pocket base feature not found or does not produce a solid");
    if (base->currentState() != ComputeState::Valid)
        return fail("pocket base feature is not in a valid state");
    if (!base->currentShape().isValid()) return fail("pocket base feature has no valid shape");

    const Sketch* sketch = ResolveSketch(context.registry, sketchId_);
    if (sketch == nullptr) return fail("pocket sketch not found");

    const Parameter* depth = ResolveParameter(context.registry, depthParameterId_);
    if (depth == nullptr) return fail("pocket depth parameter not found");

    // Profile validation is the same pure function Pad runs (ADR-M4-005); the
    // tool is a real profile with all of a profile's rules.
    const ProfileResult profile = BuildProfile(*sketch);
    if (!profile) return fail("invalid pocket profile: " + profile.message);

    PlanarProfileDefinition definition;
    // A support frame that is GONE fails loudly (M10 gate I). Falling back to
    // the embedded plane would move the geometry back to world XY on its own,
    // silently, which is exactly what a deleted reference must never do.
    if (context.part().sketchSupportFrameIsMissing(sketch->id()))
        return fail("pocket sketch's support frame is missing");
    // The sketch's EFFECTIVE plane, which is its support frame's world
    // transform when it has one (M10.2, ADR-M10-003). Reading `sketch->frame()`
    // here instead would leave the geometry at the origin after the frame moved.
    if (!BuildKernelProfile(*sketch, profile.profile,
                            context.part().effectiveSketchFrame(sketch->id()), definition))
        return fail("pocket profile references an entity that is no longer in the sketch");

    // The tool grows along the sketch's +normal by Depth -- the SAME direction
    // Pad grows from the same plane (ADR-M8-002). A pocket sketched on the
    // pad's own plane therefore cuts INTO the material it sits on, and a depth
    // equal to the pad's length cuts through.
    //
    // A NEGATIVE depth cuts the other way (M17.8, ADR-M17-031), and that is not
    // an exotic case: a sketch made on a FACE has its normal pointing out of
    // the solid, so that a pad grows away from the part (ADR-M17-028). From
    // such a plane the default direction builds the tool OUTSIDE the material,
    // and the cut removes nothing -- which is the failure checked for below.
    // The extrude call enforces the magnitude floor in exactly one place.
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

    // Against the BASE, so what gets recorded is the walls and floor the cut
    // made -- and the base's own history is carried forward, so a chain
    // accumulates a full account rather than only its last step.
    currentShape_ = context.kernel->tagCreatedFaces(result.shape, base->currentShape(),
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
