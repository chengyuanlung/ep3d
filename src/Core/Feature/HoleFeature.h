#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IParameterisedFeature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISketchConsuming.h"
#include "Core/Feature/HoleStandards.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Recompute/IRecomputable.h"
#include <string>
#include <utility>
#include <vector>
#include <string_view>

namespace paramcad {

// Drills holes at a sketch's POINTS (M20).
//
// The locations are the sketch's SketchPoint entities, which is what makes a
// hole parametric rather than a coordinate: a point can be dimensioned,
// constrained to an edge, made symmetric with another. Onshape's hole tool
// works the same way, and for the same reason -- "20 mm from that edge" has to
// survive the edge moving.
//
// A sketch with no points drills nothing, and that is REFUSED rather than
// quietly succeeding: a hole feature that removes no material is one the user
// meant to do something, and a chain that carries it forward unchanged looks
// exactly like a chain that worked.
//
// WHY NOT A POCKET OF CIRCLES. It would build the same solid today, and it
// would say nothing. A hole knows it is round: the diameter is one number
// rather than a radius constraint on each circle, "through all" is a question
// it can ask, and counterbores and threads are things it can grow later. A
// pocket that happens to be circular is a coincidence the model cannot use.
//
// THE DEPTH is signed, exactly as a Pocket's is (ADR-M17-031), and for the
// same reason: a sketch made on a FACE has its normal pointing out of the
// solid, so the default direction drills away from the material. A depth of
// zero means THROUGH ALL -- the one value that cannot mean a real depth, so it
// is free to mean the thing users ask for most.
class HoleFeature final : public Feature,
                          public IRecomputable,
                          public ISolidFeature,
                          public ISketchConsuming,
                          public IMaterialReferencing, public IParameterisedFeature {
public:
    // BOTH numbers. A hole is a diameter AND a depth, and a depth of
    // nought means THROUGH ALL -- which a user needs to be able to read
    // as well as type.
    std::vector<FeatureParameter> featureParameters() const override {
        return {FeatureParameter{"Diameter", diameterParameterId_, false},
                FeatureParameter{"Depth", depthParameterId_, false}};
    }
    HoleFeature(std::string name, ObjectId baseFeatureId, ObjectId sketchId,
                ObjectId diameterParameterId, ObjectId depthParameterId, ObjectId materialId);
    // Restore constructor (deserialization): keeps the persisted id/state.
    HoleFeature(ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
                ObjectId sketchId, ObjectId diameterParameterId, ObjectId depthParameterId,
                ObjectId materialId);

    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Hole"; }

    ObjectId baseFeatureId() const noexcept { return baseFeatureId_; }
    std::vector<ObjectId> consumedSolidIds() const override { return {baseFeatureId_}; }
    ObjectId sketchId() const noexcept { return sketchId_; }
    // A hole reads POINTS. Four dots that never join into a loop is the
    // drawing it wants, not a broken one.
    std::vector<ConsumedSketch> consumedSketches() const override {
        return {ConsumedSketch{sketchId_, false}};
    }
    ObjectId diameterParameterId() const noexcept { return diameterParameterId_; }
    ObjectId depthParameterId() const noexcept { return depthParameterId_; }

    // WHAT SCREW THIS HOLE IS FOR, AND WHAT SHAPE ITS MOUTH IS (M39).
    //
    // The screw is a SENTENCE -- "M8x1.25, tapped" -- and every number that
    // follows from it is looked up at recompute rather than stored. A hole
    // that carried both a designation and a drilled diameter would be two
    // answers to one question, and the way anybody would find out they
    // disagreed is a part that came back untappable (see HoleStandards.h).
    //
    // WHAT IS NOT MODELLED IS THE THREAD ITSELF. A tapped hole is a plain
    // cylinder at the tap drill size with a thread written on it, which is
    // what every CAD system worth using does: modelled helices make files
    // enormous, sections unreadable and booleans fragile, and the drawing --
    // which is what the shop works from -- says M8x1.25 either way.
    HoleKind kind() const noexcept { return kind_; }
    void setKind(HoleKind kind) noexcept { kind_ = kind; }
    const HoleScrew& screw() const noexcept { return screw_; }
    void setScrew(HoleScrew screw) { screw_ = std::move(screw); }

    // Every number this hole is cut from and the sentence a drawing writes
    // over it, from ONE call -- so the cut and the callout cannot disagree.
    // `typedDiameterMm` and `depthMm` are the parameters' current values.
    HoleSizes sizes(double typedDiameterMm, double depthMm) const {
        return SizeAHole(screw_, kind_, typedDiameterMm, depthMm);
    }

    ObjectId materialId() const noexcept override { return materialId_; }
    void clearMaterialReference() noexcept override { materialId_ = kInvalidObjectId; }
    void setMaterialReference(ObjectId materialId) noexcept override {
        materialId_ = materialId;
    }

    const KernelShape& currentShape() const noexcept override { return currentShape_; }
    ComputeState currentState() const noexcept override { return Feature::state(); }

    // Vestigial M1 contract, never called by the document engine (ADR-M3-004).
    bool recompute() override;

    // Real execution: one circular tool per sketch point, extruded along the
    // sketch's normal and cut from the base. Commits only on success.
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    ObjectId baseFeatureId_;
    ObjectId sketchId_;
    ObjectId diameterParameterId_;
    ObjectId depthParameterId_;
    ObjectId materialId_;
    HoleKind kind_ = HoleKind::Simple;
    HoleScrew screw_;
    KernelShape currentShape_;
};

} // namespace paramcad
