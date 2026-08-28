#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Recompute/IRecomputable.h"

#include <string>
#include <string_view>

namespace paramcad {

// A ROUND WIRE WOUND INTO A HELIX (M60).
//
// A chain BASE, like a Box and a Pad: it builds from nothing.
//
// WHY IT EXISTS AS A FEATURE at all, when M39 refused to model a thread as a
// helix and was right to: a thread has a blank to draw instead, and the
// drawing says M8x1.25 either way. A SPRING HAS NOTHING ELSE IT IS. There is
// no blank, no simplified stand-in, and no way to check a clearance or a mass
// without the coil.
//
// PLAIN HELIX VOCABULARY, not spring vocabulary. It takes a wire, a mean
// diameter, a pitch and a number of turns, because that is what a helix is --
// and an auger, a worm and a coil are the same four numbers. Naming the
// parameters after the one caller that has them today would make the second
// caller either rename them or pretend.
class HelixFeature final : public Feature,
                           public IRecomputable,
                           public ISolidFeature,
                           public IMaterialReferencing {
public:
    HelixFeature(std::string name, ObjectId wireDiameterParameterId,
                 ObjectId meanDiameterParameterId, ObjectId pitchParameterId,
                 ObjectId turnsParameterId, ObjectId materialId);
    HelixFeature(ObjectId id, std::string name, ComputeState state,
                 ObjectId wireDiameterParameterId, ObjectId meanDiameterParameterId,
                 ObjectId pitchParameterId, ObjectId turnsParameterId, ObjectId materialId);

    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "Helix"; }

    ObjectId wireDiameterParameterId() const noexcept { return wireDiameterParameterId_; }
    ObjectId meanDiameterParameterId() const noexcept { return meanDiameterParameterId_; }
    ObjectId pitchParameterId() const noexcept { return pitchParameterId_; }
    // A COUNT, so the parameter behind it is Unitless rather than a length:
    // turns are not millimetres, and M18 paid once for a number carrying the
    // wrong unit.
    ObjectId turnsParameterId() const noexcept { return turnsParameterId_; }

    ObjectId materialId() const noexcept override { return materialId_; }
    void clearMaterialReference() noexcept override { materialId_ = kInvalidObjectId; }
    void setMaterialReference(ObjectId materialId) noexcept override {
        materialId_ = materialId;
    }

    const KernelShape& currentShape() const noexcept override { return currentShape_; }
    ComputeState currentState() const noexcept override { return Feature::state(); }

    bool recompute() override;
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    ObjectId wireDiameterParameterId_;
    ObjectId meanDiameterParameterId_;
    ObjectId pitchParameterId_;
    ObjectId turnsParameterId_;
    ObjectId materialId_;
    KernelShape currentShape_;
};

} // namespace paramcad
