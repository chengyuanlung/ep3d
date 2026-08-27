#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/IParameterisedFeature.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Feature/SheetContour.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Recompute/IRecomputable.h"

#include <string>
#include <string_view>

namespace paramcad {

// M52 -- THE CONTOUR FLANGE: a sheet metal part built from its section.
//
// The chain of flanges and bends, given a width, becomes a solid. And the same
// chain, given to M51, becomes the flat blank -- ONE description, two answers,
// which is the whole reason it is shaped this way.
//
// THE THICKNESS IS NOT A PARAMETER OF THIS FEATURE.
//
// It is read from the PART, every rebuild, because a sheet metal part has one
// thickness and that is what makes it sheet metal. A feature carrying its own
// would be the second place the thickness lives: set the part to 3 mm, and
// this goes on building 2 mm walls that fold to a blank the part says is a
// different length. Both self-consistent, neither right.
//
// The same reason M42's balloon does not carry its item number and M38's
// section does not carry its letter -- and the same failure if it did.
class SheetContourFeature final : public Feature,
                                  public IRecomputable,
                                  public ISolidFeature,
                                  public IMaterialReferencing,
                                  public IParameterisedFeature {
public:
    std::vector<FeatureParameter> featureParameters() const override {
        return {FeatureParameter{"Width", widthParameterId_, true}};
    }

    SheetContourFeature(std::string name, SheetContour contour, ObjectId widthParameterId,
                        ObjectId materialId);
    SheetContourFeature(ObjectId id, std::string name, ComputeState state,
                        SheetContour contour, ObjectId widthParameterId, ObjectId materialId);

    ObjectId id() const noexcept override { return Feature::id(); }
    std::string_view typeName() const noexcept override { return "SheetContour"; }

    const SheetContour& contour() const noexcept { return contour_; }
    void setContour(SheetContour contour) { contour_ = std::move(contour); }
    ObjectId widthParameterId() const noexcept { return widthParameterId_; }

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
    SheetContour contour_;
    ObjectId widthParameterId_ = kInvalidObjectId;
    ObjectId materialId_ = kInvalidObjectId;
    KernelShape currentShape_;
};

} // namespace paramcad
