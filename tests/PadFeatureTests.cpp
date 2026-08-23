// M4-G/H: PadFeature integration with the M2 recompute engine (spec 12
// required graph and behaviour, spec 18 "Recompute" matrix).
//
// Core-only: the fake kernel models the axis-aligned rectangle and full circle
// cross-sections these tests use, so incremental recompute behaviour is
// verified without linking OCCT. Real geometry lives in tests/Kernel/.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Fakes/FakeGeometryKernel.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;
constexpr double kLengthAbsTol = 1e-6;
constexpr double kVolumeMassRelTol = 1e-9;

void ExpectRel(double actual, double expected, double relTol) {
    EXPECT_NEAR(actual, expected, relTol * std::max(1.0, std::fabs(expected)));
}

// 100 x 50 rectangle sketch, padded by a Length parameter, with a material --
// the configuration mandatory release gate A is built on.
struct PadFixture {
    PartDocument document{"PadDoc"};
    FakeGeometryKernel kernel;
    Material* material = nullptr;
    Sketch* sketch = nullptr;
    Parameter* length = nullptr;
    Parameter* unrelated = nullptr;
    PadFeature* pad = nullptr;
    SketchEntityId bottom{kInvalidSketchEntityId};
    SketchEntityId right{kInvalidSketchEntityId};
    SketchEntityId top{kInvalidSketchEntityId};
    SketchEntityId left{kInvalidSketchEntityId};

    explicit PadFixture(double w = 100.0, double h = 50.0, double padLength = 20.0,
                        double density = 2700.0) {
        document.setGeometryKernel(&kernel);
        material = &document.addMaterial("Mat", density);
        length = &document.addParameter("PadLength", padLength, UnitType::Millimeter);
        unrelated = &document.addParameter("Unrelated", 42.0, UnitType::Millimeter);
        sketch = &document.addSketch("Sketch001");
        bottom = sketch->addLine(Vec2{0, 0}, Vec2{w, 0});
        right = sketch->addLine(Vec2{w, 0}, Vec2{w, h});
        top = sketch->addLine(Vec2{w, h}, Vec2{0, h});
        left = sketch->addLine(Vec2{0, h}, Vec2{0, 0});
        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", sketch->id(), length->id());
    }

    // Replaces the bottom and top edges so the rectangle becomes newWidth wide,
    // going through the document facade so the graph is told.
    void setWidth(double newWidth) {
        sketch->removeEntity(bottom);
        sketch->removeEntity(right);
        sketch->removeEntity(top);
        bottom = sketch->addLine(Vec2{0, 0}, Vec2{newWidth, 0});
        right = sketch->addLine(Vec2{newWidth, 0}, Vec2{newWidth, 50});
        top = sketch->addLine(Vec2{newWidth, 50}, Vec2{0, 50});
        document.markSketchDirty(sketch->id());
    }
};

// --- Required graph and first recompute (spec 12) --------------------------

TEST(PadFeatureTest, M4_PAD_001_InitialRecomputeProducesAnalyticalValues) {
    PadFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.pad->state(), ComputeState::Valid);

    const MassProperties& mp = fx.document.massProperties();
    EXPECT_TRUE(mp.valid);
    ExpectRel(mp.volumeMm3, 100.0 * 50.0 * 20.0, kVolumeMassRelTol);
    // Expected mass computed independently from the dimensions, not from the
    // reported volume (spec 15).
    ExpectRel(mp.massKg, 2700.0 * (100.0 * 50.0 * 20.0) * 1e-9, kVolumeMassRelTol);
    EXPECT_NEAR(mp.centerOfMassMm.x, 50.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.y, 25.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.z, 10.0, kLengthAbsTol);
}

TEST(PadFeatureTest, M4_PAD_002_PadReferencesAreObjectIdsOnly) {
    PadFixture fx;
    EXPECT_EQ(fx.pad->sketchId(), fx.sketch->id());
    EXPECT_EQ(fx.pad->lengthParameterId(), fx.length->id());
    EXPECT_EQ(fx.pad->materialId(), fx.material->id());
    EXPECT_EQ(fx.pad->typeName(), "Pad");
}

// --- Incremental behaviour (spec 12, the release-gate requirement) ---------

TEST(PadFeatureTest, M4_PAD_010_PadLengthEditRebuildsPadAndMassOnly) {
    PadFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int extrudesBefore = fx.kernel.extrudeProfileCallCount;
    const int massBefore = fx.kernel.calculateMassPropertiesCallCount;

    ASSERT_TRUE(fx.document.setParameterValue(fx.length->id(), 30.0));
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.kernel.extrudeProfileCallCount, extrudesBefore + 1);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massBefore + 1);
    ExpectRel(fx.document.massProperties().volumeMm3, 100.0 * 50.0 * 30.0, kVolumeMassRelTol);
    EXPECT_NEAR(fx.document.massProperties().centerOfMassMm.z, 15.0, kLengthAbsTol);
}

TEST(PadFeatureTest, M4_PAD_011_DensityEditRecomputesMassOnlyNotGeometry) {
    PadFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int extrudesBefore = fx.kernel.extrudeProfileCallCount;
    const int massBefore = fx.kernel.calculateMassPropertiesCallCount;
    const double volumeBefore = fx.document.massProperties().volumeMm3;

    ASSERT_TRUE(fx.document.setMaterialDensity(7850.0));
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.kernel.extrudeProfileCallCount, extrudesBefore)
        << "a density-only change rebuilt geometry";
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massBefore + 1);
    ExpectRel(fx.document.massProperties().volumeMm3, volumeBefore, kVolumeMassRelTol);
    ExpectRel(fx.document.massProperties().massKg, 7850.0 * 100000.0 * 1e-9,
              kVolumeMassRelTol);
}

TEST(PadFeatureTest, M4_PAD_012_SketchEditRebuildsPadAndMass) {
    PadFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int extrudesBefore = fx.kernel.extrudeProfileCallCount;
    const int massBefore = fx.kernel.calculateMassPropertiesCallCount;

    fx.setWidth(120.0);
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.kernel.extrudeProfileCallCount, extrudesBefore + 1);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massBefore + 1);
    ExpectRel(fx.document.massProperties().volumeMm3, 120.0 * 50.0 * 20.0, kVolumeMassRelTol);
    EXPECT_NEAR(fx.document.massProperties().centerOfMassMm.x, 60.0, kLengthAbsTol);
}

TEST(PadFeatureTest, M4_PAD_013_UnrelatedParameterRecomputesNeither) {
    PadFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int extrudesBefore = fx.kernel.extrudeProfileCallCount;
    const int massBefore = fx.kernel.calculateMassPropertiesCallCount;

    ASSERT_TRUE(fx.document.setParameterValue(fx.unrelated->id(), 99.0));
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.kernel.extrudeProfileCallCount, extrudesBefore);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massBefore);
}

// --- Transactional failure and recovery (spec 13) -------------------------

TEST(PadFeatureTest, M4_PAD_020_BrokenProfileFailsWithoutCommittingAShape) {
    PadFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_EQ(fx.pad->state(), ComputeState::Valid);

    // Break the loop: remove one side. The retained shape must be untouched.
    ASSERT_TRUE(fx.sketch->removeEntity(fx.top));
    ASSERT_TRUE(fx.document.markSketchDirty(fx.sketch->id()));

    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(fx.pad->state(), ComputeState::Failed);
    EXPECT_TRUE(fx.pad->currentShape().isValid())
        << "a failed rebuild discarded the last valid shape";

    bool sawDiagnostic = false;
    for (const auto& item : report.items)
        if (!item.message.empty()) sawDiagnostic = true;
    EXPECT_TRUE(sawDiagnostic);

    // Downstream must not report the retained numbers as current (ADR-M3-006).
    EXPECT_FALSE(fx.document.massProperties().valid);
}

TEST(PadFeatureTest, M4_PAD_021_RepairRecoversDeterministically) {
    PadFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.sketch->removeEntity(fx.top));
    ASSERT_TRUE(fx.document.markSketchDirty(fx.sketch->id()));
    ASSERT_FALSE(fx.document.recompute().success);

    // Put the side back.
    fx.top = fx.sketch->addLine(Vec2{100, 50}, Vec2{0, 50});
    ASSERT_TRUE(fx.document.markSketchDirty(fx.sketch->id()));

    EXPECT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.pad->state(), ComputeState::Valid);
    EXPECT_TRUE(fx.document.massProperties().valid);
    ExpectRel(fx.document.massProperties().volumeMm3, 100000.0, kVolumeMassRelTol);
}

TEST(PadFeatureTest, M4_PAD_022_InvalidPadLengthsFailCleanly) {
    // -5.0 LEFT THIS LIST at M17.8 (ADR-M17-031). A negative length is no
    // longer invalid: it is a direction, and it pads to the other side of the
    // sketch plane. What is still invalid is a length with no MAGNITUDE --
    // zero, or anything that is not a finite number. See M17_PAD_030 below for
    // what a negative one now does.
    for (double bad : {0.0, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()}) {
        PadFixture fx;
        ASSERT_TRUE(fx.document.recompute().success);
        ASSERT_TRUE(fx.document.setParameterValue(fx.length->id(), bad));

        const DocumentRecomputeReport report = fx.document.recompute();
        EXPECT_FALSE(report.success) << "pad length " << bad << " was accepted";
        EXPECT_EQ(fx.pad->state(), ComputeState::Failed);
        EXPECT_FALSE(fx.document.massProperties().valid);
    }
}

TEST(PadFeatureTest, M17_PAD_030_ANegativePadLengthBuildsOnTheOtherSideOfThePlane) {
    // The direction a pad grows is the sketch's +normal. A sketch made ON A
    // FACE has that normal pointing out of the solid (ADR-M17-028), which is
    // what makes a pad on a face grow away from the part -- and it is also why
    // there has to be a way to say "the other way" without moving the sketch.
    PadFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const double volume = fx.document.massProperties().volumeMm3;
    const Vec3 centre = fx.document.massProperties().centerOfMassMm;
    ASSERT_GT(volume, 0.0);

    ASSERT_TRUE(fx.document.setParameterValue(fx.length->id(), -20.0));
    ASSERT_TRUE(fx.document.recompute().success) << "a negative length was refused";
    EXPECT_EQ(fx.pad->state(), ComputeState::Valid);

    // SAME SIZE. A solid built the other way is on the other side of the
    // plane, not made of negative material -- a signed volume would travel
    // straight out to the mass readout and the status bar.
    EXPECT_NEAR(fx.document.massProperties().volumeMm3, volume, volume * 1e-9);
    // ...and on the OTHER SIDE. The sketch is on world XY with normal +Z, so
    // the centre of mass mirrors through z = 0.
    EXPECT_NEAR(fx.document.massProperties().centerOfMassMm.z, -centre.z, 1e-9);
    EXPECT_NEAR(fx.document.massProperties().centerOfMassMm.x, centre.x, 1e-9);
}

TEST(PadFeatureTest, M4_PAD_023_DeletingTheSketchFailsPadWithoutCrashing) {
    // Spec 25 adversarial: delete a Sketch a Pad still references.
    PadFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.removeObject(fx.sketch->id()));

    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(fx.pad->state(), ComputeState::Failed);
    EXPECT_FALSE(fx.document.massProperties().valid);
}

TEST(PadFeatureTest, M4_PAD_024_SelfIntersectingSketchIsRejectedNotBuilt) {
    PadFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);

    // Turn the rectangle into a bowtie by swapping one side's endpoints.
    ASSERT_TRUE(fx.sketch->removeEntity(fx.bottom));
    ASSERT_TRUE(fx.sketch->removeEntity(fx.top));
    fx.sketch->addLine(Vec2{0, 0}, Vec2{100, 50});
    fx.sketch->addLine(Vec2{100, 0}, Vec2{0, 50});
    ASSERT_TRUE(fx.document.markSketchDirty(fx.sketch->id()));

    EXPECT_FALSE(fx.document.recompute().success);
    EXPECT_EQ(fx.pad->state(), ComputeState::Failed);
}

// --- Circle pad (gate B shape) ---------------------------------------------

TEST(PadFeatureTest, M4_PAD_030_CircleProfilePad) {
    PartDocument document{"CircleDoc"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    document.addMaterial("Mat", 2700.0);
    Parameter& length = document.addParameter("PadLength", 30.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addCircle(Vec2{0, 0}, 10.0);
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());

    ASSERT_TRUE(document.recompute().success);
    EXPECT_EQ(pad.state(), ComputeState::Valid);
    // Independent oracle: pi * r^2 * h.
    ExpectRel(document.massProperties().volumeMm3, kPi * 10.0 * 10.0 * 30.0,
              kVolumeMassRelTol);
    EXPECT_NEAR(document.massProperties().centerOfMassMm.z, 15.0, kLengthAbsTol);
}

// --- Frames (gate D shape) --------------------------------------------------

TEST(PadFeatureTest, M4_PAD_040_TranslatedSketchFrameMovesTheSolidNotItsSize) {
    PartDocument document{"FrameDoc"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    document.addMaterial("Mat", 2700.0);
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001", SketchFrame::Translated(Vec3{10, 20, 30}));
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    document.addPadFeature(body, "Pad001", sketch.id(), length.id());

    ASSERT_TRUE(document.recompute().success);
    const MassProperties& mp = document.massProperties();
    ExpectRel(mp.volumeMm3, 100000.0, kVolumeMassRelTol); // unchanged by the move
    EXPECT_NEAR(mp.centerOfMassMm.x, 60.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.y, 45.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.z, 40.0, kLengthAbsTol);
}

TEST(PadFeatureTest, M4_PAD_041_RotatedSketchFrameKeepsVolume) {
    PartDocument document{"FrameDoc"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    document.addMaterial("Mat", 2700.0);
    Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
    Sketch& sketch =
        document.addSketch("Sketch001", SketchFrame::Rotated(Vec3{1, 0, 0}, kPi / 2));
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
    sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
    sketch.addLine(Vec2{0, 50}, Vec2{0, 0});
    Body& body = document.addBody("Body001");
    document.addPadFeature(body, "Pad001", sketch.id(), length.id());

    ASSERT_TRUE(document.recompute().success);
    const MassProperties& mp = document.massProperties();
    ExpectRel(mp.volumeMm3, 100000.0, kVolumeMassRelTol);
    // Sketch (50,25) under +90 about X maps to world (50,0,25); the normal
    // points along -y so the solid extends 10 mm that way.
    EXPECT_NEAR(mp.centerOfMassMm.x, 50.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.y, -10.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.z, 25.0, kLengthAbsTol);
}

} // namespace
