#pragma once

#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Geometry/MathTypes.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Recompute/IRecomputable.h"

#include <string>

namespace paramcad {

// M10.6 -- Mirror and Pattern, the two features ADR-M9-006 deferred to M10.
//
// It deferred them because both are a TRANSFORM plus a BOOLEAN, and a transform
// needs something stable to be defined against. That something is now a
// ReferenceFrame, so the deferral is closed rather than renewed:
//
//   Mirror  = reflect the base across a frame's XY plane, then fuse
//   Pattern = translate copies along a frame's X axis, then fuse
//
// Both CONSUME a base solid, exactly as Pocket and the dress features do, so
// they inherit the whole chain: ADR-M8-001's base-by-ObjectId rule,
// ADR-M8-008's consumed-once rule, ADR-M9-002's suppression semantics, and the
// tail display of ADR-M8-003. Neither needed a new chain concept.
//
// THE PLANE AND THE AXIS COME FROM THE FRAME, by the same convention
// SketchFrame uses and for the same reason (one convention, not two):
//   * the mirror plane is the frame's XY plane -- origin at the frame's origin,
//     normal along its local +Z;
//   * the pattern direction is the frame's local +X.
// A frame's WORLD transform is what is used, so a mirror under a moved parent
// moves with it.
class TransformFeature : public Feature, public IRecomputable, public ISolidFeature,
                         public IMaterialReferencing {
public:
    ObjectId baseFeatureId() const noexcept { return baseFeatureId_; }
    ObjectId frameId() const noexcept { return frameId_; }

    ObjectId id() const noexcept override { return Feature::id(); }
    bool recompute() override { return false; } // vestigial M1 contract
    RecomputeResult recompute(const RecomputeContext& context) override;

    const KernelShape& currentShape() const noexcept override { return shape_; }
    ComputeState currentState() const noexcept override { return state(); }
    // The chain declaration: a transform feature consumes its base, so the
    // viewer and the mass node follow IT and not the thing it was built from.
    ObjectId consumedSolidId() const noexcept override { return baseFeatureId_; }

    ObjectId materialId() const noexcept override { return materialId_; }

protected:
    TransformFeature(std::string name, ObjectId baseFeatureId, ObjectId frameId,
                     ObjectId materialId);
    TransformFeature(ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
                     ObjectId frameId, ObjectId materialId);

    // What the concrete feature does to the base's shape before the fuse.
    // Returns an empty result to mean "nothing to add".
    virtual ShapeResult buildCopies(const RecomputeContext& context, const KernelShape& base,
                                    const Transform3D& frameWorld) = 0;

private:
    friend class PartDocument;
    void clearMaterialReference() noexcept override { materialId_ = kInvalidObjectId; }
    void setMaterialReference(ObjectId materialId) noexcept override { materialId_ = materialId; }

    ObjectId baseFeatureId_;
    ObjectId frameId_;
    ObjectId materialId_;
    KernelShape shape_;
};

// Reflects the base across its frame's XY plane and fuses the two.
class MirrorFeature final : public TransformFeature {
public:
    MirrorFeature(std::string name, ObjectId baseFeatureId, ObjectId frameId,
                  ObjectId materialId = kInvalidObjectId);
    MirrorFeature(ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
                  ObjectId frameId, ObjectId materialId);

    std::string_view typeName() const noexcept override { return "Mirror"; }

protected:
    ShapeResult buildCopies(const RecomputeContext& context, const KernelShape& base,
                            const Transform3D& frameWorld) override;
};

// Copies the base `count - 1` times along its frame's local +X, each `spacing`
// further than the last, and fuses them all.
//
// `count` and `spacing` are PARAMETERS, not literals, because a pattern whose
// count cannot be driven is not parametric -- and because M9's undo, M8's
// selective recompute and M5's expressions all work on Parameters and on
// nothing else.
class PatternFeature final : public TransformFeature {
public:
    PatternFeature(std::string name, ObjectId baseFeatureId, ObjectId frameId,
                   ObjectId countParameterId, ObjectId spacingParameterId,
                   ObjectId materialId = kInvalidObjectId);
    PatternFeature(ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
                   ObjectId frameId, ObjectId countParameterId, ObjectId spacingParameterId,
                   ObjectId materialId);

    ObjectId countParameterId() const noexcept { return countParameterId_; }
    ObjectId spacingParameterId() const noexcept { return spacingParameterId_; }

    std::string_view typeName() const noexcept override { return "Pattern"; }

protected:
    ShapeResult buildCopies(const RecomputeContext& context, const KernelShape& base,
                            const Transform3D& frameWorld) override;

private:
    ObjectId countParameterId_;
    ObjectId spacingParameterId_;
};

} // namespace paramcad
