#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISketchConsuming.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Recompute/IRecomputable.h"
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

// Runs a solid through two or more sketches, in the order given (M19).
//
// THE ORDER IS THE USER'S, and it is stored rather than derived. Lofting
// A-B-C and A-C-B are different solids, and sorting the sections by anything
// the feature could see -- plane height, distance from the origin, sketch id --
// would be the program deciding what the drawing meant. A list, in order, is
// the only representation that cannot get that wrong.
//
// Two is the minimum and it is a refusal, not a fallback: a loft through one
// section has no second section to run to, and answering with an extrusion of
// some invented depth would be inventing the user's intent.
//
// Like a Pad, a loft builds from nothing: consumedSolidId() stays invalid and
// it is a legal chain BASE.
class LoftFeature final : public Feature,
                          public IRecomputable,
                          public ISolidFeature,
                          public ISketchConsuming,
                          public IMaterialReferencing {
public:
    LoftFeature(std::string name, std::vector<ObjectId> sectionSketchIds, ObjectId materialId);
    // Restore constructor (deserialization): keeps the persisted id/state.
    LoftFeature(ObjectId id, std::string name, ComputeState state,
                std::vector<ObjectId> sectionSketchIds, ObjectId materialId);

    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Loft"; }

    const std::vector<ObjectId>& sectionSketchIds() const noexcept { return sectionSketchIds_; }

    // ISketchConsuming answers with the FIRST section.
    //
    // The interface asks for one and a loft consumes several, so this reports
    // the one the solid starts at. Every section is a real dependency and the
    // graph carries an edge for each; those edges are wired explicitly, not
    // derived from this method, so nothing is lost by it answering narrowly.
    // EVERY SECTION, not just the first. Naming one left the others
    // deletable out from under the loft that runs through them.
    std::vector<ConsumedSketch> consumedSketches() const override {
        std::vector<ConsumedSketch> all;
        all.reserve(sectionSketchIds_.size());
        for (const ObjectId section : sectionSketchIds_)
            all.push_back(ConsumedSketch{section, true});
        return all;
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

    // Real execution: validate every section's loop in Core, convert each
    // through its OWN sketch's effective frame, and loft through the injected
    // kernel. Commits only on success.
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    std::vector<ObjectId> sectionSketchIds_;
    ObjectId materialId_;
    KernelShape currentShape_;
};

} // namespace paramcad
