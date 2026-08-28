#include "Core/Feature/HelixFeature.h"

#include "Core/Document/ResolveObject.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/RecomputeContext.h"

#include <utility>

namespace paramcad {

HelixFeature::HelixFeature(std::string name, ObjectId wireDiameterParameterId,
                           ObjectId meanDiameterParameterId, ObjectId pitchParameterId,
                           ObjectId turnsParameterId, ObjectId materialId)
    : Feature(std::move(name)), wireDiameterParameterId_(wireDiameterParameterId),
      meanDiameterParameterId_(meanDiameterParameterId), pitchParameterId_(pitchParameterId),
      turnsParameterId_(turnsParameterId), materialId_(materialId) {}

HelixFeature::HelixFeature(ObjectId id, std::string name, ComputeState state,
                           ObjectId wireDiameterParameterId, ObjectId meanDiameterParameterId,
                           ObjectId pitchParameterId, ObjectId turnsParameterId,
                           ObjectId materialId)
    : Feature(id, std::move(name), state),
      wireDiameterParameterId_(wireDiameterParameterId),
      meanDiameterParameterId_(meanDiameterParameterId), pitchParameterId_(pitchParameterId),
      turnsParameterId_(turnsParameterId), materialId_(materialId) {}

bool HelixFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult HelixFeature::recompute(const RecomputeContext& context) {
    if (context.kernel == nullptr) {
        setState(ComputeState::Failed);
        return {RecomputeStatus::Failed, "no geometry kernel configured"};
    }

    const Parameter* wire = ResolveParameter(context.registry, wireDiameterParameterId_);
    const Parameter* mean = ResolveParameter(context.registry, meanDiameterParameterId_);
    const Parameter* pitch = ResolveParameter(context.registry, pitchParameterId_);
    const Parameter* turns = ResolveParameter(context.registry, turnsParameterId_);
    if (wire == nullptr || mean == nullptr || pitch == nullptr || turns == nullptr) {
        setState(ComputeState::Failed);
        return {RecomputeStatus::Failed,
                "a helix needs a wire diameter, a mean diameter, a pitch and a turn count, "
                "and one of them is gone"};
    }

    // DIAMETERS IN, RADII OUT, and the halving happens here rather than at the
    // call site. A user types the diameter because that is what a caliper
    // reads and what a catalogue prints; the kernel wants radii; and doing the
    // conversion in the feature keeps it in one place for the reason
    // ADR-M3-002 gives about units.
    HelixDefinition definition;
    definition.wireRadiusMm = wire->value() / 2.0;
    definition.helixRadiusMm = mean->value() / 2.0;
    definition.pitchMm = pitch->value();
    definition.turns = turns->value();

    // Transactional (ADR-M3-001): built into a local result, committed only on
    // success, so a failed build leaves the last good coil untouched.
    ShapeResult result = context.kernel->createHelicalWire(definition);
    if (!result) {
        setState(ComputeState::Failed);
        return {RecomputeStatus::Failed, result.message};
    }
    currentShape_ = context.kernel->tagCreatedFaces(result.shape, KernelShape{},
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
