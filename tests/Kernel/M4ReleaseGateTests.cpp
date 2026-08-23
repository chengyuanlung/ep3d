// M4 mandatory release gates A-E (spec 19-23), executed against REAL OCCT
// geometry rather than the fake kernel. M4 cannot be declared complete if any
// of these fails.
//
// Every expected value is an independent analytical result, never derived from
// what the kernel just reported.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;
constexpr double kLengthAbsTol = 1e-6;       // mm
constexpr double kVolumeMassRelTol = 1e-9;   // planar/prismatic: exact
constexpr double kCurvedRelTol = 1e-6;       // circular cross-sections

void ExpectRel(double actual, double expected, double relTol) {
    EXPECT_NEAR(actual, expected, relTol * std::max(1.0, std::fabs(expected)));
}

// Counting wrapper so the gates can assert WHICH nodes recomputed, not merely
// that the numbers came out right.
class CountingKernel final : public IGeometryKernel {
public:
    ShapeResult createBox(const BoxDefinition& definition) override {
        ++createBoxCallCount;
        return inner_.createBox(definition);
    }
    ShapeResult extrudeProfile(const PlanarProfileDefinition& profile,
                               double distanceMm) override {
        ++extrudeProfileCallCount;
        return inner_.extrudeProfile(profile, distanceMm);
    }
    KernelMassPropertiesResult calculateMassProperties(const KernelShape& shape) override {
        ++calculateMassPropertiesCallCount;
        return inner_.calculateMassProperties(shape);
    }
    ShapeResult subtractShape(const KernelShape& base, const KernelShape& tool) override {
        ++subtractCallCount;
        return inner_.subtractShape(base, tool);
    }
    ShapeResult shellSolid(const KernelShape& base, const FaceSelection& openFaces,
                           double thicknessMm) override {
        return inner_.shellSolid(base, openFaces, thicknessMm);
    }
    ShapeResult draftFaces(const KernelShape& base, const FaceSelection& faces,
                           const FaceQuery& neutral, double angleRad) override {
        return inner_.draftFaces(base, faces, neutral, angleRad);
    }
    KernelBoundsResult boundsOfShape(const KernelShape& shape) override {
        return inner_.boundsOfShape(shape);
    }
    ShapeResult sweepProfile(const PlanarProfileDefinition& profile,
                             const PlanarPathDefinition& path) override {
        return inner_.sweepProfile(profile, path);
    }
    ShapeResult loftProfiles(const std::vector<PlanarProfileDefinition>& profiles)
        override {
        return inner_.loftProfiles(profiles);
    }
    ShapeResult revolveProfile(const PlanarProfileDefinition& profile, const Vec3& axisOriginMm,
                               const Vec3& axisDirection, double angleRad) override {
        ++revolveCallCount;
        return inner_.revolveProfile(profile, axisOriginMm, axisDirection, angleRad);
    }
    ShapeResult filletEdges(const KernelShape& shape, const EdgeSelection& selection,
                            double radiusMm) override {
        ++filletCallCount;
        return inner_.filletEdges(shape, selection, radiusMm);
    }
    ShapeResult chamferEdges(const KernelShape& shape, const EdgeSelection& selection,
                             double distanceMm) override {
        ++chamferCallCount;
        return inner_.chamferEdges(shape, selection, distanceMm);
    }
    // M10.6 verbs. Forwarded, uncounted: these suites predate them and none
    // of their gates is about mirroring, so counting would add a member every
    // fixture has to ignore.
    ShapeResult mirrorShape(const KernelShape& shape, const Vec3& planeOriginMm,
                            const Vec3& planeNormal) override {
        return inner_.mirrorShape(shape, planeOriginMm, planeNormal);
    }
    ShapeResult translateShape(const KernelShape& shape, const Vec3& offsetMm) override {
        return inner_.translateShape(shape, offsetMm);
    }
    ShapeResult fuseShapes(const KernelShape& a, const KernelShape& b) override {
        return inner_.fuseShapes(a, b);
    }


    int createBoxCallCount = 0;
    int subtractCallCount = 0;
    int revolveCallCount = 0;
    int filletCallCount = 0;
    int chamferCallCount = 0;
    int extrudeProfileCallCount = 0;
    int calculateMassPropertiesCallCount = 0;

private:
    OcctGeometryKernel inner_;
};

struct RectanglePad {
    PartDocument document{"GateDoc"};
    CountingKernel kernel;
    Sketch* sketch = nullptr;
    Parameter* padLength = nullptr;
    PadFeature* pad = nullptr;
    SketchEntityId bottom{kInvalidSketchEntityId};
    SketchEntityId right{kInvalidSketchEntityId};
    SketchEntityId top{kInvalidSketchEntityId};
    SketchEntityId left{kInvalidSketchEntityId};

    explicit RectanglePad(SketchFrame frame = SketchFrame::WorldXY(), double width = 100.0,
                          double height = 50.0, double length = 20.0, double density = 2700.0) {
        document.setGeometryKernel(&kernel);
        document.addMaterial("Aluminium", density);
        padLength = &document.addParameter("PadLength", length, UnitType::Millimeter);
        sketch = &document.addSketch("Sketch001", frame);
        bottom = sketch->addLine(Vec2{0, 0}, Vec2{width, 0});
        right = sketch->addLine(Vec2{width, 0}, Vec2{width, height});
        top = sketch->addLine(Vec2{width, height}, Vec2{0, height});
        left = sketch->addLine(Vec2{0, height}, Vec2{0, 0});
        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", sketch->id(), padLength->id());
    }

    // Rebuild the rectangle at a new width, going through the facade so the
    // graph learns the Pad is stale.
    void setWidth(double width) {
        sketch->removeEntity(bottom);
        sketch->removeEntity(right);
        sketch->removeEntity(top);
        bottom = sketch->addLine(Vec2{0, 0}, Vec2{width, 0});
        right = sketch->addLine(Vec2{width, 0}, Vec2{width, 50});
        top = sketch->addLine(Vec2{width, 50}, Vec2{0, 50});
        document.markSketchDirty(sketch->id());
    }
};

// --- Gate A: rectangle (spec 19) -------------------------------------------

TEST(M4GateTest, GATE_A_RectanglePadAndParametricEdits) {
    RectanglePad fx;
    ASSERT_TRUE(fx.document.recompute().success);

    // Initial: 100 x 50 x 20 -> 100000 mm^3, 0.27 kg, COM (50,25,10).
    {
        const MassProperties& mp = fx.document.massProperties();
        ExpectRel(mp.volumeMm3, 100000.0, kVolumeMassRelTol);
        ExpectRel(mp.massKg, 0.27, kVolumeMassRelTol);
        EXPECT_NEAR(mp.centerOfMassMm.x, 50.0, kLengthAbsTol);
        EXPECT_NEAR(mp.centerOfMassMm.y, 25.0, kLengthAbsTol);
        EXPECT_NEAR(mp.centerOfMassMm.z, 10.0, kLengthAbsTol);
    }

    // Pad 20 -> 30: Pad + Mass recompute, sketch semantics NOT rebuilt.
    const int extrudesBefore = fx.kernel.extrudeProfileCallCount;
    const int massBefore = fx.kernel.calculateMassPropertiesCallCount;
    const std::size_t entitiesBefore = fx.sketch->entities().size();
    const SketchEntityId bottomIdBefore = fx.bottom;

    ASSERT_TRUE(fx.document.setParameterValue(fx.padLength->id(), 30.0));
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.kernel.extrudeProfileCallCount, extrudesBefore + 1);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massBefore + 1);
    // "Sketch not unnecessarily rebuilt": its entities and their identities are
    // untouched by a length-only edit.
    EXPECT_EQ(fx.sketch->entities().size(), entitiesBefore);
    EXPECT_EQ(fx.bottom, bottomIdBefore);
    EXPECT_NE(fx.sketch->findEntity(bottomIdBefore), nullptr);
    {
        const MassProperties& mp = fx.document.massProperties();
        ExpectRel(mp.volumeMm3, 150000.0, kVolumeMassRelTol);
        ExpectRel(mp.massKg, 0.405, kVolumeMassRelTol);
        EXPECT_NEAR(mp.centerOfMassMm.x, 50.0, kLengthAbsTol);
        EXPECT_NEAR(mp.centerOfMassMm.y, 25.0, kLengthAbsTol);
        EXPECT_NEAR(mp.centerOfMassMm.z, 15.0, kLengthAbsTol);
    }

    // Width 100 -> 120 by editing sketch geometry: Profile/Pad/Mass affected.
    fx.setWidth(120.0);
    ASSERT_TRUE(fx.document.recompute().success);
    {
        const MassProperties& mp = fx.document.massProperties();
        ExpectRel(mp.volumeMm3, 180000.0, kVolumeMassRelTol);
        ExpectRel(mp.massKg, 0.486, kVolumeMassRelTol);
        EXPECT_NEAR(mp.centerOfMassMm.x, 60.0, kLengthAbsTol);
        EXPECT_NEAR(mp.centerOfMassMm.y, 25.0, kLengthAbsTol);
        EXPECT_NEAR(mp.centerOfMassMm.z, 15.0, kLengthAbsTol);
    }
}

TEST(M4GateTest, GATE_A_DensityOnlyEditDoesNotRebuildGeometry) {
    RectanglePad fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int extrudesBefore = fx.kernel.extrudeProfileCallCount;
    const double volumeBefore = fx.document.massProperties().volumeMm3;

    ASSERT_TRUE(fx.document.setMaterialDensity(7850.0));
    ASSERT_TRUE(fx.document.recompute().success);

    EXPECT_EQ(fx.kernel.extrudeProfileCallCount, extrudesBefore);
    ExpectRel(fx.document.massProperties().volumeMm3, volumeBefore, kVolumeMassRelTol);
    ExpectRel(fx.document.massProperties().massKg, 7850.0 * 100000.0 * 1e-9,
              kVolumeMassRelTol);
}

// --- Gate B: circle (spec 20) ----------------------------------------------

TEST(M4GateTest, GATE_B_CirclePad) {
    PartDocument document{"CircleGate"};
    CountingKernel kernel;
    document.setGeometryKernel(&kernel);
    document.addMaterial("Aluminium", 2700.0);
    Parameter& length = document.addParameter("PadLength", 30.0, UnitType::Millimeter);
    Sketch& sketch = document.addSketch("Sketch001");
    sketch.addCircle(Vec2{0, 0}, 10.0);
    Body& body = document.addBody("Body001");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());

    ASSERT_TRUE(document.recompute().success);
    EXPECT_EQ(pad.state(), ComputeState::Valid);

    const MassProperties& mp = document.massProperties();
    ExpectRel(mp.volumeMm3, kPi * 10.0 * 10.0 * 30.0, kCurvedRelTol);
    EXPECT_NEAR(mp.centerOfMassMm.x, 0.0, 1e-6);
    EXPECT_NEAR(mp.centerOfMassMm.y, 0.0, 1e-6);
    EXPECT_NEAR(mp.centerOfMassMm.z, 15.0, 1e-6);
}

// --- Gate C: failure and recovery (spec 21) --------------------------------

TEST(M4GateTest, GATE_C_BrokenEndpointFailsThenRepairRecovers) {
    RectanglePad fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const double volumeBefore = fx.document.massProperties().volumeMm3;

    // Break one corner well beyond the connectivity tolerance.
    ASSERT_TRUE(fx.sketch->removeEntity(fx.left));
    fx.left = fx.sketch->addLine(Vec2{0, 50}, Vec2{5.0, 0.0}); // 5 mm gap
    ASSERT_TRUE(fx.document.markSketchDirty(fx.sketch->id()));

    const DocumentRecomputeReport failed = fx.document.recompute();
    EXPECT_FALSE(failed.success);
    EXPECT_EQ(fx.pad->state(), ComputeState::Failed);

    bool sawDiagnostic = false;
    for (const auto& item : failed.items)
        if (!item.message.empty()) sawDiagnostic = true;
    EXPECT_TRUE(sawDiagnostic) << "no diagnostic identified the cause";

    // Downstream is not current, but the last valid shape is retained.
    EXPECT_FALSE(fx.document.massProperties().valid);
    EXPECT_TRUE(fx.pad->currentShape().isValid());

    // Repair.
    ASSERT_TRUE(fx.sketch->removeEntity(fx.left));
    fx.left = fx.sketch->addLine(Vec2{0, 50}, Vec2{0, 0});
    ASSERT_TRUE(fx.document.markSketchDirty(fx.sketch->id()));

    EXPECT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.pad->state(), ComputeState::Valid);
    EXPECT_TRUE(fx.document.massProperties().valid);
    ExpectRel(fx.document.massProperties().volumeMm3, volumeBefore, kVolumeMassRelTol);
}

// --- Gate D: transformed frames (spec 22) ----------------------------------

TEST(M4GateTest, GATE_D_TranslatedFrameKeepsLocalDimensions) {
    RectanglePad fx{SketchFrame::Translated(Vec3{10, 20, 30})};
    ASSERT_TRUE(fx.document.recompute().success);

    const MassProperties& mp = fx.document.massProperties();
    ExpectRel(mp.volumeMm3, 100000.0, kVolumeMassRelTol); // unchanged by the move
    EXPECT_NEAR(mp.centerOfMassMm.x, 60.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.y, 45.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.z, 40.0, kLengthAbsTol);
}

TEST(M4GateTest, GATE_D_RotatedFrameKeepsVolumeAndOrientsCorrectly) {
    // The world-XY hardcode detector (spec 22): identical local geometry on a
    // tilted plane must give identical volume and a correctly transformed COM.
    RectanglePad fx{SketchFrame::Rotated(Vec3{1, 0, 0}, kPi / 2)};
    ASSERT_TRUE(fx.document.recompute().success);

    const MassProperties& mp = fx.document.massProperties();
    ExpectRel(mp.volumeMm3, 100000.0, kVolumeMassRelTol);
    EXPECT_NEAR(mp.centerOfMassMm.x, 50.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.y, -10.0, kLengthAbsTol);
    EXPECT_NEAR(mp.centerOfMassMm.z, 25.0, kLengthAbsTol);
}

TEST(M4GateTest, GATE_D_RotatedFrameMatchesWorldXyVolumeExactly) {
    RectanglePad flat;
    RectanglePad tilted{SketchFrame::Rotated(Vec3{0, 1, 0}, kPi / 3, Vec3{7, -3, 2})};
    ASSERT_TRUE(flat.document.recompute().success);
    ASSERT_TRUE(tilted.document.recompute().success);

    ExpectRel(tilted.document.massProperties().volumeMm3,
              flat.document.massProperties().volumeMm3, kVolumeMassRelTol);
    ExpectRel(tilted.document.massProperties().massKg,
              flat.document.massProperties().massKg, kVolumeMassRelTol);
}

// --- Gate E: save / load / recompute (spec 23) -----------------------------

TEST(M4GateTest, GATE_E_SaveLoadRecomputeEquivalence) {
    RectanglePad fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const MassProperties before = fx.document.massProperties();
    const ObjectId sketchId = fx.sketch->id();
    const ObjectId padId = fx.pad->id();
    const ObjectId lengthId = fx.padLength->id();
    std::vector<SketchEntityId> entityIds;
    for (const SketchEntity& entity : fx.sketch->entities()) entityIds.push_back(entity.id);

    std::ostringstream out;
    const SaveResult saved = savePartDocument(fx.document, out);
    ASSERT_TRUE(saved) << saved.message;

    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;

    // Identity survives.
    ASSERT_EQ(loaded.document->sketches().size(), 1u);
    const Sketch& restoredSketch = *loaded.document->sketches().front();
    EXPECT_EQ(restoredSketch.id(), sketchId);
    for (SketchEntityId id : entityIds)
        EXPECT_NE(restoredSketch.findEntity(id), nullptr);

    ASSERT_EQ(loaded.document->bodies().size(), 1u);
    ASSERT_EQ(loaded.document->bodies().front()->features().size(), 1u);
    const auto* restoredPad = dynamic_cast<const PadFeature*>(
        loaded.document->bodies().front()->features().front().get());
    ASSERT_NE(restoredPad, nullptr);
    EXPECT_EQ(restoredPad->id(), padId);
    EXPECT_EQ(restoredPad->sketchId(), sketchId);
    EXPECT_EQ(restoredPad->lengthParameterId(), lengthId);
    ASSERT_TRUE(loaded.document->material());
    EXPECT_DOUBLE_EQ(loaded.document->material()->density(), 2700.0);

    // Recompute produces equivalent geometry -- through real OCCT, without any
    // edge/wire/face identity having been persisted.
    OcctGeometryKernel loadedKernel;
    loaded.document->setGeometryKernel(&loadedKernel);
    ASSERT_TRUE(loaded.document->recompute().success);
    const MassProperties& after = loaded.document->massProperties();

    ExpectRel(after.volumeMm3, before.volumeMm3, kVolumeMassRelTol);
    ExpectRel(after.massKg, before.massKg, kVolumeMassRelTol);
    EXPECT_NEAR(after.centerOfMassMm.x, before.centerOfMassMm.x, kLengthAbsTol);
    EXPECT_NEAR(after.centerOfMassMm.y, before.centerOfMassMm.y, kLengthAbsTol);
    EXPECT_NEAR(after.centerOfMassMm.z, before.centerOfMassMm.z, kLengthAbsTol);
    for (std::size_t i = 0; i < 9; ++i)
        EXPECT_NEAR(after.inertiaTensorKgM2.m[i], before.inertiaTensorKgM2.m[i], 1e-9);

    // Dependency behaviour survives too: an edit still propagates.
    ASSERT_TRUE(loaded.document->setParameterValue(lengthId, 30.0));
    ASSERT_TRUE(loaded.document->recompute().success);
    ExpectRel(loaded.document->massProperties().volumeMm3, 150000.0, kVolumeMassRelTol);
}

TEST(M4GateTest, GATE_E_RotatedFrameSurvivesSaveLoadRecompute) {
    RectanglePad fx{SketchFrame::Rotated(Vec3{1, 0, 0}, kPi / 2, Vec3{5, 6, 7})};
    ASSERT_TRUE(fx.document.recompute().success);
    const MassProperties before = fx.document.massProperties();

    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(fx.document, out));
    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;

    OcctGeometryKernel loadedKernel;
    loaded.document->setGeometryKernel(&loadedKernel);
    ASSERT_TRUE(loaded.document->recompute().success);
    const MassProperties& after = loaded.document->massProperties();

    ExpectRel(after.volumeMm3, before.volumeMm3, kVolumeMassRelTol);
    EXPECT_NEAR(after.centerOfMassMm.x, before.centerOfMassMm.x, kLengthAbsTol);
    EXPECT_NEAR(after.centerOfMassMm.y, before.centerOfMassMm.y, kLengthAbsTol);
    EXPECT_NEAR(after.centerOfMassMm.z, before.centerOfMassMm.z, kLengthAbsTol);
}

} // namespace
