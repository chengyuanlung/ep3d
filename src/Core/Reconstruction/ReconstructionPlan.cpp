#include "Core/Reconstruction/ReconstructionPlan.h"

#include <cmath>
#include <set>

namespace paramcad {
namespace {

std::size_t ToIndex(PlanParameterSlot slot) noexcept {
    return static_cast<std::size_t>(slot);
}

} // namespace

const char* ReconstructionOriginName(ReconstructionOrigin origin) noexcept {
    switch (origin) {
        case ReconstructionOrigin::ExplicitSource: return "explicit in source";
        case ReconstructionOrigin::InferredGeometric: return "inferred from geometry";
        case ReconstructionOrigin::InferredPlacement: return "inferred placement";
    }
    return "unknown";
}

const char* ReconstructionSkipReasonName(ReconstructionSkipReason reason) noexcept {
    switch (reason) {
        case ReconstructionSkipReason::NoTargetGeometry: return "no matching geometry";
        case ReconstructionSkipReason::AmbiguousTarget: return "ambiguous target";
        case ReconstructionSkipReason::ValueDisagreesWithGeometry:
            return "stated value disagrees with geometry";
        case ReconstructionSkipReason::TextOverride: return "text override";
        case ReconstructionSkipReason::UnsupportedKind: return "unsupported dimension kind";
        case ReconstructionSkipReason::InvalidValue: return "invalid value";
        case ReconstructionSkipReason::NameCollision: return "name collision";
    }
    return "unknown";
}

const PlannedParameter* ReconstructionPlan::find(PlanParameterSlot slot) const noexcept {
    if (slot == kInvalidPlanParameterSlot) return nullptr;
    const std::size_t index = ToIndex(slot);
    if (index >= parameters.size()) return nullptr;
    return &parameters[index];
}

PlanValidation ValidatePlan(const ReconstructionPlan& plan) {
    if (plan.sketchId == kInvalidObjectId)
        return {false, "the plan names no sketch"};

    // Names must be unique within the plan. Two Parameters called Width is not
    // a cosmetic problem: the second would be a separate Parameter with the
    // same label, and every UI that lists them by name would show a user two
    // identical rows controlling different geometry (spec 9).
    std::set<std::string> names;
    for (const PlannedParameter& parameter : plan.parameters) {
        if (parameter.name.empty())
            return {false, "a planned Parameter has no name"};
        if (!names.insert(parameter.name).second)
            return {false, "two planned Parameters are both called '" + parameter.name + "'"};

        if (!std::isfinite(parameter.value))
            return {false, "Parameter '" + parameter.name + "' has a non-finite value"};

        // Length-like Parameters carry length-like values. The floor is the
        // model's own (kMinSketchDimensionMm), not a second opinion, so a
        // dimension M7 accepts cannot be one the Sketch would refuse.
        if (parameter.unit == UnitType::Millimeter &&
            parameter.value < kMinSketchDimensionMm)
            return {false, "Parameter '" + parameter.name +
                               "' is below the smallest dimension the sketch model accepts"};
    }

    for (const PlannedConstraint& constraint : plan.constraints) {
        const bool dimensional = IsDimensional(constraint.data);

        // A dimensional constraint with no Parameter is the exact shape of
        // "the edit reports success and controls nothing" (spec 34, Critical).
        if (dimensional && constraint.parameter == kInvalidPlanParameterSlot)
            return {false, std::string("a ") + ConstraintKindName(constraint.data) +
                               " constraint binds no Parameter"};

        // And the reverse, which is how a mis-wired plan looks: a Horizontal
        // that thinks it drives a value.
        if (!dimensional && constraint.parameter != kInvalidPlanParameterSlot)
            return {false, std::string("a ") + ConstraintKindName(constraint.data) +
                               " constraint carries a Parameter it cannot use"};

        if (!dimensional) continue;

        const PlannedParameter* bound = plan.find(constraint.parameter);
        if (bound == nullptr)
            return {false, std::string("a ") + ConstraintKindName(constraint.data) +
                               " constraint binds a Parameter that is not in this plan"};

        // Unit compatibility, checked here rather than left to the solver.
        // Spec 10 forbids Unitless standing in for a length, and a Length
        // constraint reading an angle is not a subtle bug -- it is a document
        // that rebuilds to the wrong size and says nothing.
        const bool lengthLike = std::holds_alternative<LengthConstraint>(constraint.data) ||
                                std::holds_alternative<DistanceConstraint>(constraint.data) ||
                                std::holds_alternative<RadiusConstraint>(constraint.data) ||
                                std::holds_alternative<DiameterConstraint>(constraint.data);
        if (lengthLike && bound->unit != UnitType::Millimeter)
            return {false, std::string("a ") + ConstraintKindName(constraint.data) +
                               " constraint binds Parameter '" + bound->name +
                               "', which is not a length"};

        if (std::holds_alternative<AngleConstraint>(constraint.data) &&
            bound->unit != UnitType::Radian)
            return {false, "an Angle constraint binds Parameter '" + bound->name +
                               "', which is not an angle"};
    }

    return {true, {}};
}

} // namespace paramcad
