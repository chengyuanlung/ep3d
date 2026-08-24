#pragma once

#include "Core/Feature/Feature.h"
#include "Core/Feature/IMaterialReferencing.h"
#include "Core/Feature/IParameterisedFeature.h"
#include "Core/Feature/ISketchConsuming.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Geometry/MathTypes.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Recompute/IRecomputable.h"

#include <string>
#include <vector>

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
    std::vector<ObjectId> consumedSolidIds() const override { return {baseFeatureId_}; }

    ObjectId materialId() const noexcept override { return materialId_; }

protected:
    TransformFeature(std::string name, ObjectId baseFeatureId, ObjectId frameId,
                     ObjectId materialId);
    TransformFeature(ObjectId id, std::string name, ComputeState state, ObjectId baseFeatureId,
                     ObjectId frameId, ObjectId materialId);

    // What the concrete feature does to the base's shape before the fuse.
    // Returns an empty result to mean "nothing to add".
    //
    // NO FRAME ARGUMENT since M21. It used to take one, and `recompute` looked
    // it up and refused when it was gone -- which was right for a mirror and a
    // linear pattern, and wrong the moment a CURVE pattern arrived: its copies
    // are defined against a sketch's path, and it has no frame to lose.
    //
    // Requiring one anyway would have meant either a frame nobody used or a
    // flag saying whether this feature has one, and a flag is how "two things
    // must agree" starts. Each feature now asks for what it actually needs,
    // through frameWorldOrFail below when that is a frame.
    virtual ShapeResult buildCopies(const RecomputeContext& context,
                                    const KernelShape& base) = 0;

    // The frame's world transform, or false with the reason.
    //
    // ONE implementation for the three features that are defined against a
    // frame. A transform whose frame is gone has no plane and no axis, and
    // defaulting to the world origin would move the geometry silently -- the
    // same refusal a missing sketch support frame gets (M10 gate I).
    bool frameWorldOrFail(const RecomputeContext& context, Transform3D& out,
                          std::string& why) const;

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
    ShapeResult buildCopies(const RecomputeContext& context,
                            const KernelShape& base) override;
};

// Copies the base `count - 1` times along its frame's local +X, each `spacing`
// further than the last, and fuses them all.
//
// `count` and `spacing` are PARAMETERS, not literals, because a pattern whose
// count cannot be driven is not parametric -- and because M9's undo, M8's
// selective recompute and M5's expressions all work on Parameters and on
// nothing else.
class PatternFeature final : public TransformFeature, public IParameterisedFeature {
public:
    std::vector<FeatureParameter> featureParameters() const override {
        return {FeatureParameter{"Count", countParameterId_, false},
                FeatureParameter{"Spacing", spacingParameterId_, true}};
    }
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
    ShapeResult buildCopies(const RecomputeContext& context,
                            const KernelShape& base) override;

private:
    ObjectId countParameterId_;
    ObjectId spacingParameterId_;
};

// Turns the base `count - 1` times about its frame's local +Z, each `step`
// further round than the last, and fuses them all (M21).
//
// +Z BY THE SAME CONVENTION the other two follow: the mirror plane is the
// frame's XY, the linear direction is its +X, and the rotation axis is its +Z.
// Three features, one convention -- a fourth axis chosen per feature would be
// three things to remember instead of one.
//
// THE STEP IS PER INSTANCE, not a total sweep. Six instances at 60 degrees is
// a full ring; six at 360 would be six copies on top of each other. The
// alternative -- "spread `count` evenly over this total" -- cannot express a
// partial ring without dividing, and it makes the number in the parameter
// table mean something different depending on the count beside it.
//
// The step's unit is checked: it must carry UnitType::Radian, for the reason a
// revolve's angle is checked. A step of 60 stored as millimetres reads as 60
// radians, which is nine and a half turns and lands nowhere near where the
// drawing said.
class CircularPatternFeature final : public TransformFeature, public IParameterisedFeature {
public:
    std::vector<FeatureParameter> featureParameters() const override {
        return {FeatureParameter{"Count", countParameterId_, false},
                FeatureParameter{"Step", stepParameterId_, true}};
    }
    CircularPatternFeature(std::string name, ObjectId baseFeatureId, ObjectId frameId,
                           ObjectId countParameterId, ObjectId stepParameterId,
                           ObjectId materialId = kInvalidObjectId);
    CircularPatternFeature(ObjectId id, std::string name, ComputeState state,
                           ObjectId baseFeatureId, ObjectId frameId, ObjectId countParameterId,
                           ObjectId stepParameterId, ObjectId materialId);

    ObjectId countParameterId() const noexcept { return countParameterId_; }
    ObjectId stepParameterId() const noexcept { return stepParameterId_; }

    std::string_view typeName() const noexcept override { return "CircularPattern"; }

protected:
    ShapeResult buildCopies(const RecomputeContext& context,
                            const KernelShape& base) override;

private:
    ObjectId countParameterId_;
    ObjectId stepParameterId_;
};

// Places `count` copies of the base ALONG A SKETCH'S PATH, evenly spaced by arc
// length, and fuses them all (M21).
//
// The path is the same chain a sweep follows (M19's BuildPath), which is why
// this arrives now rather than earlier: "along that curve" needs a curve the
// program can walk, and a spine is exactly that.
//
// The copies are TRANSLATED, not swept: each one is the base moved from the
// path's start to the i-th station along it. They are not turned to follow the
// curve's tangent, and that is a limit rather than a decision -- see the ADR.
class CurvePatternFeature final : public TransformFeature, public ISketchConsuming, public IParameterisedFeature {
public:
    std::vector<FeatureParameter> featureParameters() const override {
        return {FeatureParameter{"Count", countParameterId_, false}};
    }
    CurvePatternFeature(std::string name, ObjectId baseFeatureId, ObjectId pathSketchId,
                        ObjectId countParameterId, ObjectId materialId = kInvalidObjectId);
    CurvePatternFeature(ObjectId id, std::string name, ComputeState state,
                        ObjectId baseFeatureId, ObjectId pathSketchId,
                        ObjectId countParameterId, ObjectId materialId);

    ObjectId pathSketchId() const noexcept { return pathSketchId_; }
    ObjectId countParameterId() const noexcept { return countParameterId_; }

    // IT READS A SKETCH, and until M26.8 it did not say so. That left its
    // path DELETABLE while the pattern still walked it, and had the tree
    // mark the path "Failed" for not closing into a loop a path is never
    // meant to close into.
    //
    // A PATH IS A CURVE, so it does not need a closed profile.
    std::vector<ConsumedSketch> consumedSketches() const override {
        return {ConsumedSketch{pathSketchId_, false}};
    }

    std::string_view typeName() const noexcept override { return "CurvePattern"; }

protected:
    ShapeResult buildCopies(const RecomputeContext& context,
                            const KernelShape& base) override;

private:
    ObjectId pathSketchId_;
    ObjectId countParameterId_;
};

} // namespace paramcad
