#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Feature/SweepFeature.h"
#include "Core/Feature/ShellFeature.h"
#include "Core/Feature/BooleanFeature.h"
#include "Core/Feature/TransformFeatures.h"
#include "Core/Feature/DraftFeature.h"
#include "Core/Feature/ImportFeature.h"
#include <filesystem>
#include <cstdio>
#include "Core/Feature/HoleFeature.h"
#include "Core/Feature/LoftFeature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Sketch/Profile.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

// Wraps a real OcctGeometryKernel and counts calls, so these integration
// tests can assert "BoxFeature +1 / MassPropertiesNode +1" recompute deltas
// (spec 19) against REAL OCCT geometry, exactly mirroring the counting
// pattern CountingRecomputable/FakeGeometryKernel use elsewhere in the suite.
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
    ShapeResult placeShape(const KernelShape& shape, const Transform3D& placement) override {
        ++placeShapeCallCount;
        (void)shape;
        (void)placement;
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "this counting kernel does not model placements"};
    }
    int placeShapeCallCount = 0;

    ShapeResult rotateShape(const KernelShape& shape, const Vec3& axisOriginMm,
                            const Vec3& axisDirection, double angleRad) override {
        return inner_.rotateShape(shape, axisOriginMm, axisDirection, angleRad);
    }
    ShapeResult intersectShapes(const KernelShape& a, const KernelShape& b) override {
        return inner_.intersectShapes(a, b);
    }
    ShapeResult shellSolid(const KernelShape& base, const FaceSelection& openFaces,
                           double thicknessMm) override {
        return inner_.shellSolid(base, openFaces, thicknessMm);
    }
    ShapeResult draftFaces(const KernelShape& base, const FaceSelection& faces,
                           const FaceQuery& neutral, double angleRad) override {
        return inner_.draftFaces(base, faces, neutral, angleRad);
    }
    IoResult exportStep(const KernelShape& shape, const std::string& path) override {
        return inner_.exportStep(shape, path);
    }
    ShapeResult importStep(const std::string& path) override {
        return inner_.importStep(path);
    }
    IoResult exportStl(const KernelShape& shape, const std::string& path,
                       double deflectionMm) override {
        return inner_.exportStl(shape, path, deflectionMm);
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

constexpr double kLengthAbsTol = 1e-6;   // mm
constexpr double kVolumeMassRelTol = 1e-9;

void ExpectRel(double actual, double expected, double relTol) {
    const double tolerance = relTol * std::max(1.0, std::fabs(expected));
    EXPECT_NEAR(actual, expected, tolerance);
}

struct RealBoxFixture {
    PartDocument document{"OcctDoc"};
    CountingKernel kernel;
    Material* material = nullptr;
    Parameter* width = nullptr;
    Parameter* height = nullptr;
    Parameter* depth = nullptr;
    BoxFeature* box = nullptr;

    RealBoxFixture(double w = 100.0, double h = 50.0, double d = 20.0, double density = 2700.0) {
        document.setGeometryKernel(&kernel);
        material = &document.addMaterial("Mat", density);
        width = &document.addParameter("Width", w, UnitType::Millimeter);
        height = &document.addParameter("Height", h, UnitType::Millimeter);
        depth = &document.addParameter("Depth", d, UnitType::Millimeter);
        Body& body = document.addBody("Body001");
        box = &document.addBoxFeature(body, "Box001", width->id(), height->id(), depth->id());
    }
};

// --- Recompute (real geometry) ----------------------------------------------

TEST(OcctRecomputeIntegrationTest, M3_RECOMPUTE_001_WidthChangeRecomputesBoxAndMass) {
    RealBoxFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int boxCount = fx.kernel.createBoxCallCount;
    const int massCount = fx.kernel.calculateMassPropertiesCallCount;

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.kernel.createBoxCallCount, boxCount + 1);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massCount + 1);
    ExpectRel(fx.document.massProperties().volumeMm3, 120.0 * 50.0 * 20.0, kVolumeMassRelTol);
}

TEST(OcctRecomputeIntegrationTest, M3_RECOMPUTE_002_HeightChangeRecomputesBoxAndMass) {
    RealBoxFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int boxCount = fx.kernel.createBoxCallCount;
    const int massCount = fx.kernel.calculateMassPropertiesCallCount;

    ASSERT_TRUE(fx.document.setParameterValue(fx.height->id(), 70.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.kernel.createBoxCallCount, boxCount + 1);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massCount + 1);
    ExpectRel(fx.document.massProperties().volumeMm3, 100.0 * 70.0 * 20.0, kVolumeMassRelTol);
}

TEST(OcctRecomputeIntegrationTest, M3_RECOMPUTE_003_DepthChangeRecomputesBoxAndMass) {
    RealBoxFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int boxCount = fx.kernel.createBoxCallCount;
    const int massCount = fx.kernel.calculateMassPropertiesCallCount;

    ASSERT_TRUE(fx.document.setParameterValue(fx.depth->id(), 35.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.kernel.createBoxCallCount, boxCount + 1);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massCount + 1);
    ExpectRel(fx.document.massProperties().volumeMm3, 100.0 * 50.0 * 35.0, kVolumeMassRelTol);
}

TEST(OcctRecomputeIntegrationTest, M3_RECOMPUTE_004_DensityOnlyRecomputesMassNotGeometry) {
    RealBoxFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const int boxCount = fx.kernel.createBoxCallCount;
    const int massCount = fx.kernel.calculateMassPropertiesCallCount;
    const double volumeBefore = fx.document.massProperties().volumeMm3;

    ASSERT_TRUE(fx.document.setMaterialDensity(7850.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.kernel.createBoxCallCount, boxCount); // geometry NOT rebuilt
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massCount + 1);
    EXPECT_EQ(fx.document.massProperties().volumeMm3, volumeBefore);
}

TEST(OcctRecomputeIntegrationTest, M3_RECOMPUTE_005_UnrelatedParameterRecomputesNeither) {
    RealBoxFixture fx;
    Parameter& unrelated = fx.document.addParameter("Unrelated", 1.0, UnitType::Unitless);
    ASSERT_TRUE(fx.document.recompute().success);
    const int boxCount = fx.kernel.createBoxCallCount;
    const int massCount = fx.kernel.calculateMassPropertiesCallCount;

    ASSERT_TRUE(fx.document.setParameterValue(unrelated.id(), 2.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.kernel.createBoxCallCount, boxCount);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massCount);
}

// --- Negative / recovery (real geometry) ------------------------------------

TEST(OcctRecomputeIntegrationTest, M3_NEGATIVE_001_ZeroWidthFailsCleanlyNoCrash) {
    RealBoxFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 0.0));
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(fx.box->state(), ComputeState::Failed);
}

TEST(OcctRecomputeIntegrationTest, M3_RECOVERY_001_RecoverAfterInvalidDimension) {
    RealBoxFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), -1.0));
    ASSERT_FALSE(fx.document.recompute().success);
    ASSERT_EQ(fx.box->state(), ComputeState::Failed);

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 100.0));
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_TRUE(report.success);
    EXPECT_EQ(fx.box->state(), ComputeState::Valid);
    ExpectRel(fx.document.massProperties().volumeMm3, 100.0 * 50.0 * 20.0, kVolumeMassRelTol);
}

// --- Mandatory release gate (spec 19, verbatim steps A-E) -------------------

TEST(IntegrationTest, M3_GATE_ReleaseScenario) {
    // Initial: Width=100mm Height=50mm Depth=20mm Density=2700 kg/m^3.
    // Expect: Volume=100000mm^3, Mass=0.27kg, COM=(50,25,10)mm.
    RealBoxFixture fx(100.0, 50.0, 20.0, 2700.0);
    ASSERT_TRUE(fx.document.recompute().success);
    {
        const MassProperties& mp = fx.document.massProperties();
        ExpectRel(mp.volumeMm3, 100000.0, kVolumeMassRelTol);
        ExpectRel(mp.massKg, 0.27, kVolumeMassRelTol);
        EXPECT_NEAR(mp.centerOfMassMm.x, 50.0, kLengthAbsTol);
        EXPECT_NEAR(mp.centerOfMassMm.y, 25.0, kLengthAbsTol);
        EXPECT_NEAR(mp.centerOfMassMm.z, 10.0, kLengthAbsTol);
    }
    const int boxCount0 = fx.kernel.createBoxCallCount;
    const int massCount0 = fx.kernel.calculateMassPropertiesCallCount;

    // A -- Width -> 120 mm. Expect BoxFeature +1, MassProperties +1,
    // Volume=120000mm^3, Mass=0.324kg, COM=(60,25,10)mm.
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.kernel.createBoxCallCount, boxCount0 + 1);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massCount0 + 1);
    {
        const MassProperties& mp = fx.document.massProperties();
        ExpectRel(mp.volumeMm3, 120000.0, kVolumeMassRelTol);
        ExpectRel(mp.massKg, 0.324, kVolumeMassRelTol);
        EXPECT_NEAR(mp.centerOfMassMm.x, 60.0, kLengthAbsTol);
        EXPECT_NEAR(mp.centerOfMassMm.y, 25.0, kLengthAbsTol);
        EXPECT_NEAR(mp.centerOfMassMm.z, 10.0, kLengthAbsTol);
    }
    const int boxCountA = fx.kernel.createBoxCallCount;
    const int massCountA = fx.kernel.calculateMassPropertiesCallCount;
    const Matrix3 inertiaBeforeDensityChange = fx.document.massProperties().inertiaTensorKgM2;

    // B -- Density -> 7850. Expect BoxFeature count UNCHANGED,
    // MassProperties +1, Volume unchanged, Mass=0.942kg, COM unchanged,
    // physical inertia changes.
    ASSERT_TRUE(fx.document.setMaterialDensity(7850.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.kernel.createBoxCallCount, boxCountA); // unchanged: no geometry rebuild
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massCountA + 1);
    {
        const MassProperties& mp = fx.document.massProperties();
        ExpectRel(mp.volumeMm3, 120000.0, kVolumeMassRelTol); // unchanged
        ExpectRel(mp.massKg, 0.942, kVolumeMassRelTol);
        EXPECT_NEAR(mp.centerOfMassMm.x, 60.0, kLengthAbsTol); // unchanged
        EXPECT_NEAR(mp.centerOfMassMm.y, 25.0, kLengthAbsTol);
        EXPECT_NEAR(mp.centerOfMassMm.z, 10.0, kLengthAbsTol);
        EXPECT_NE(mp.inertiaTensorKgM2.m[0], inertiaBeforeDensityChange.m[0]); // inertia changes
    }

    // C -- Width -> 0. Expect feature failure, downstream blocked/not-current,
    // diagnostic, no crash.
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 0.0));
    const DocumentRecomputeReport failedReport = fx.document.recompute();
    EXPECT_FALSE(failedReport.success);
    EXPECT_EQ(fx.box->state(), ComputeState::Failed);
    bool sawDiagnostic = false;
    for (const auto& item : failedReport.items)
        if (!item.message.empty()) sawDiagnostic = true;
    EXPECT_TRUE(sawDiagnostic);
    // "Downstream blocked / not current" at the DATA level, not just in the
    // report (spec 2 DoD, ADR-M3-006): the retained numbers must stop claiming
    // to be current, or a reader of massProperties() cannot tell they are stale.
    EXPECT_FALSE(fx.document.massProperties().valid);

    // D -- Width -> 80. Expect recovery: Volume=80000mm^3,
    // Mass=0.628kg at density 7850.
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 80.0));
    const DocumentRecomputeReport recovered = fx.document.recompute();
    EXPECT_TRUE(recovered.success);
    EXPECT_EQ(fx.box->state(), ComputeState::Valid);
    {
        const MassProperties& mp = fx.document.massProperties();
        ExpectRel(mp.volumeMm3, 80000.0, kVolumeMassRelTol);
        ExpectRel(mp.massKg, 0.628, kVolumeMassRelTol);
    }

    // E -- Save / load / recompute. Expect equivalent dimensions, material,
    // Volume, Mass, COM, inertia, stable IDs and dependency behavior.
    const std::string saved = [&] {
        std::ostringstream out;
        const SaveResult result = savePartDocument(fx.document, out);
        EXPECT_TRUE(result) << result.message;
        return out.str();
    }();
    const LoadResult loaded = [&] {
        std::istringstream in(saved);
        return loadPartDocument(in);
    }();
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->id(), fx.document.id());
    ASSERT_TRUE(loaded.document->material());
    EXPECT_EQ(loaded.document->material()->density(), 7850.0);

    CountingKernel loadedKernel;
    loaded.document->setGeometryKernel(&loadedKernel);
    const DocumentRecomputeReport loadedReport = loaded.document->recompute();
    ASSERT_TRUE(loadedReport.success);
    const MassProperties& loadedProperties = loaded.document->massProperties();
    const MassProperties& originalProperties = fx.document.massProperties();
    ExpectRel(loadedProperties.volumeMm3, originalProperties.volumeMm3, kVolumeMassRelTol);
    ExpectRel(loadedProperties.massKg, originalProperties.massKg, kVolumeMassRelTol);
    EXPECT_NEAR(loadedProperties.centerOfMassMm.x, originalProperties.centerOfMassMm.x,
               kLengthAbsTol);
    EXPECT_NEAR(loadedProperties.centerOfMassMm.y, originalProperties.centerOfMassMm.y,
               kLengthAbsTol);
    EXPECT_NEAR(loadedProperties.centerOfMassMm.z, originalProperties.centerOfMassMm.z,
               kLengthAbsTol);
}

} // namespace

// --- M19: SWEEP and LOFT as FEATURES, through a real document ----------------

namespace {

// A square of `side` centred on the sketch origin, added to `sketch`.
void AddSquare(PartDocument& document, ObjectId sketchId, double side) {
    const double h = side / 2.0;
    document.addSketchEntity(sketchId, SketchLine{Vec2{-h, -h}, Vec2{h, -h}});
    document.addSketchEntity(sketchId, SketchLine{Vec2{h, -h}, Vec2{h, h}});
    document.addSketchEntity(sketchId, SketchLine{Vec2{h, h}, Vec2{-h, h}});
    document.addSketchEntity(sketchId, SketchLine{Vec2{-h, h}, Vec2{-h, -h}});
}

// The XZ plane through the origin: u = +X, v = +Z. A path drawn on it climbs
// out of the XY plane a profile sits on, which is what a sweep needs.
SketchFrame WorldXZFrame() {
    const std::optional<SketchFrame> frame =
        SketchFrame::FromBasis(Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, -1, 0});
    EXPECT_TRUE(frame.has_value());
    return frame.value_or(SketchFrame::WorldXY());
}

double VolumeOf(PartDocument& document, const ISolidFeature& feature,
                OcctGeometryKernel& kernel) {
    const KernelMassPropertiesResult properties =
        kernel.calculateMassProperties(feature.currentShape());
    EXPECT_TRUE(properties) << properties.message;
    (void)document;
    return properties.properties.volumeMm3;
}

} // namespace

TEST(OcctRecomputeIntegrationTest, M19_FEAT_001_ASweepBuildsASolidFromTwoSketches) {
    // The whole path, end to end: two sketches on two planes, one feature, one
    // recompute, a solid with the volume arithmetic says it should have.
    OcctGeometryKernel kernel;
    PartDocument document{"SweepDoc"};
    document.setGeometryKernel(&kernel);

    Sketch& section = document.addSketch("Section");
    AddSquare(document, section.id(), 20.0);

    Sketch& spine = document.addSketch("Spine", WorldXZFrame());
    document.addSketchEntity(spine.id(), SketchLine{Vec2{0, 0}, Vec2{0, 100}});

    Body& body = document.addBody("Body");
    SweepFeature& sweep = document.addSweepFeature(body, "Sweep1", section.id(), spine.id());

    ASSERT_TRUE(document.recompute().success);
    ASSERT_EQ(sweep.currentState(), ComputeState::Valid);
    EXPECT_NEAR(VolumeOf(document, sweep, kernel), 20.0 * 20.0 * 100.0, 1e-6 * 40000.0);
}

TEST(OcctRecomputeIntegrationTest, M19_FEAT_002_MovingTheSPINEChangesTheSolid) {
    // The reason a sweep depends on BOTH sketches. An edge from only the
    // section would leave this feature clean after an edit that changed its
    // shape -- and it would stay wrong until something unrelated dirtied it,
    // which is the worst kind of stale.
    OcctGeometryKernel kernel;
    PartDocument document{"SweepDoc"};
    document.setGeometryKernel(&kernel);

    Sketch& section = document.addSketch("Section");
    AddSquare(document, section.id(), 20.0);
    Sketch& spine = document.addSketch("Spine", WorldXZFrame());
    const SketchEntityId line =
        document.addSketchEntity(spine.id(), SketchLine{Vec2{0, 0}, Vec2{0, 100}});

    Body& body = document.addBody("Body");
    SweepFeature& sweep = document.addSweepFeature(body, "Sweep1", section.id(), spine.id());
    ASSERT_TRUE(document.recompute().success);
    const double before = VolumeOf(document, sweep, kernel);

    // Twice as long a spine, and nothing else touched.
    ASSERT_TRUE(document.setSketchEntityGeometry(spine.id(), line,
                                                 SketchLine{Vec2{0, 0}, Vec2{0, 200}}));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(VolumeOf(document, sweep, kernel), before * 2.0, 1e-6 * before);
}

TEST(OcctRecomputeIntegrationTest, M19_FEAT_003_ASweepAlongItsOWNSketchIsREFUSED) {
    // A section swept along a spine on its own plane is swept along a direction
    // inside itself: OCCT returns a surface, its volume integrates to zero, and
    // every reading downstream looks like a result. Refused before it starts,
    // with the reason.
    OcctGeometryKernel kernel;
    PartDocument document{"SweepDoc"};
    document.setGeometryKernel(&kernel);

    Sketch& only = document.addSketch("Only");
    AddSquare(document, only.id(), 20.0);

    Body& body = document.addBody("Body");
    SweepFeature& sweep = document.addSweepFeature(body, "Sweep1", only.id(), only.id());

    (void)document.recompute();
    EXPECT_EQ(sweep.currentState(), ComputeState::Failed);
}

TEST(OcctRecomputeIntegrationTest, M19_FEAT_004_ALoftRunsThroughItsSectionsInORDER) {
    // Two squares 40 mm apart make a prism. The value of this test is not the
    // number but that the feature reached the kernel with two definitions on
    // two different planes -- a loft that flattened them onto one frame would
    // have nothing to run between.
    OcctGeometryKernel kernel;
    PartDocument document{"LoftDoc"};
    document.setGeometryKernel(&kernel);

    Sketch& bottom = document.addSketch("Bottom");
    AddSquare(document, bottom.id(), 20.0);

    const std::optional<SketchFrame> raised =
        SketchFrame::FromBasis(Vec3{0, 0, 40}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    ASSERT_TRUE(raised.has_value());
    Sketch& top = document.addSketch("Top", *raised);
    AddSquare(document, top.id(), 20.0);

    Body& body = document.addBody("Body");
    LoftFeature& loft = document.addLoftFeature(body, "Loft1", {bottom.id(), top.id()});

    ASSERT_TRUE(document.recompute().success);
    ASSERT_EQ(loft.currentState(), ComputeState::Valid);
    EXPECT_NEAR(VolumeOf(document, loft, kernel), 20.0 * 20.0 * 40.0, 1e-6 * 16000.0);
}

TEST(OcctRecomputeIntegrationTest, M19_FEAT_005_ALoftOfONESectionIsREFUSED) {
    OcctGeometryKernel kernel;
    PartDocument document{"LoftDoc"};
    document.setGeometryKernel(&kernel);

    Sketch& only = document.addSketch("Only");
    AddSquare(document, only.id(), 20.0);

    Body& body = document.addBody("Body");
    LoftFeature& loft = document.addLoftFeature(body, "Loft1", {only.id()});

    (void)document.recompute();
    EXPECT_EQ(loft.currentState(), ComputeState::Failed);
}

TEST(OcctRecomputeIntegrationTest, M19_FEAT_006_EVERYLoftSectionIsADependency) {
    // Editing the LAST section has to rebuild the loft. A graph that only knew
    // about the first would leave the solid describing a drawing that no longer
    // exists.
    OcctGeometryKernel kernel;
    PartDocument document{"LoftDoc"};
    document.setGeometryKernel(&kernel);

    Sketch& bottom = document.addSketch("Bottom");
    AddSquare(document, bottom.id(), 20.0);
    const std::optional<SketchFrame> raised =
        SketchFrame::FromBasis(Vec3{0, 0, 40}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    ASSERT_TRUE(raised.has_value());
    Sketch& top = document.addSketch("Top", *raised);
    AddSquare(document, top.id(), 20.0);

    Body& body = document.addBody("Body");
    LoftFeature& loft = document.addLoftFeature(body, "Loft1", {bottom.id(), top.id()});
    ASSERT_TRUE(document.recompute().success);
    const double before = VolumeOf(document, loft, kernel);

    // Shrink the TOP section to a 10 mm square: the loft becomes a frustum.
    std::vector<SketchEntityId> old;
    for (const SketchEntity& entity : top.entities()) old.push_back(entity.id);
    for (const SketchEntityId id : old) ASSERT_TRUE(document.removeSketchEntity(top.id(), id));
    AddSquare(document, top.id(), 10.0);

    ASSERT_TRUE(document.recompute().success);
    const double after = VolumeOf(document, loft, kernel);
    EXPECT_LT(after, before) << "the loft did not follow its last section";

    const double expected = 40.0 / 3.0 * (400.0 + 100.0 + std::sqrt(400.0 * 100.0));
    EXPECT_NEAR(after, expected, 1e-6 * expected);
}

namespace {

// The message the engine recorded for one feature. A test that only checks
// ComputeState::Failed cannot tell a refusal WITH A REASON from a refusal that
// happened for some other reason further down -- which is exactly how a guard
// can be removed without a single test noticing.
std::string FailureMessageFor(const DocumentRecomputeReport& report, ObjectId featureId) {
    for (const RecomputeItemReport& item : report.items)
        if (item.id == featureId && item.status != RecomputeStatus::Success) return item.message;
    return {};
}

} // namespace

TEST(OcctRecomputeIntegrationTest, M19_FEAT_007_ALoftHandsTheKernelTheOrderItWasGiven) {
    // Lofting A-B-C and A-C-B are different solids, so nothing in the feature
    // may reorder the sections -- not by id, not by plane height.
    //
    // Checked by building the SAME loft twice: once as a feature, and once by
    // calling the kernel directly with the three definitions in the order they
    // were asked for. If the feature reorders on the way through, the two
    // volumes part company.
    //
    // Comparing two DIFFERENT orders against each other -- the obvious test --
    // cannot do this. Any reorder the feature applied would apply to both, and
    // the two would still differ. This compares against what was asked for.
    //
    // A pure REVERSAL is deliberately not what this catches: running through
    // the same sections backwards is the same solid, so it is a change no
    // measurement of the geometry can see. That is a fact about lofts, not a
    // gap in the test.
    OcctGeometryKernel kernel;
    PartDocument document{"LoftDoc"};
    document.setGeometryKernel(&kernel);

    // The middle section is OFFSET SIDEWAYS, so the order genuinely matters.
    const auto sectionOn = [&](const char* name, Vec3 origin, double side) -> ObjectId {
        const std::optional<SketchFrame> frame =
            SketchFrame::FromBasis(origin, Vec3{1, 0, 0}, Vec3{0, 0, 1});
        EXPECT_TRUE(frame.has_value());
        Sketch& sketch = document.addSketch(name, frame.value_or(SketchFrame::WorldXY()));
        AddSquare(document, sketch.id(), side);
        return sketch.id();
    };
    const ObjectId a = sectionOn("A", Vec3{0, 0, 0}, 20.0);
    const ObjectId b = sectionOn("B", Vec3{40, 0, 20}, 20.0);
    const ObjectId c = sectionOn("C", Vec3{0, 0, 40}, 20.0);

    Body& body = document.addBody("Body");
    LoftFeature& loft = document.addLoftFeature(body, "Loft1", {a, b, c});
    ASSERT_TRUE(document.recompute().success);
    ASSERT_EQ(loft.currentState(), ComputeState::Valid);
    const KernelMassPropertiesResult built = kernel.calculateMassProperties(loft.currentShape());
    ASSERT_TRUE(built) << built.message;

    // The same three, by hand, in the same order.
    std::vector<PlanarProfileDefinition> asAsked;
    for (const ObjectId sketchId : {a, b, c}) {
        const Sketch* sketch = document.findSketch(sketchId);
        ASSERT_NE(sketch, nullptr);
        const ProfileResult profile = BuildProfile(*sketch);
        ASSERT_TRUE(profile) << profile.message;
        PlanarProfileDefinition definition;
        ASSERT_TRUE(BuildKernelProfile(*sketch, profile.profile, sketch->frame(), definition));
        asAsked.push_back(std::move(definition));
    }
    const ShapeResult expected = kernel.loftProfiles(asAsked);
    ASSERT_TRUE(expected) << expected.message;
    const KernelMassPropertiesResult reference = kernel.calculateMassProperties(expected.shape);
    ASSERT_TRUE(reference) << reference.message;

    EXPECT_NEAR(built.properties.volumeMm3, reference.properties.volumeMm3,
                1e-6 * reference.properties.volumeMm3)
        << "the feature handed the kernel a different order than it was given";

    // ...and the order really does matter here, or the check above would hold
    // for a feature that shuffled them freely.
    const ShapeResult shuffled = kernel.loftProfiles({asAsked[1], asAsked[0], asAsked[2]});
    ASSERT_TRUE(shuffled) << shuffled.message;
    const KernelMassPropertiesResult other = kernel.calculateMassProperties(shuffled.shape);
    ASSERT_TRUE(other) << other.message;
    EXPECT_GT(std::fabs(other.properties.volumeMm3 - reference.properties.volumeMm3), 1.0);
}

TEST(OcctRecomputeIntegrationTest, M19_FEAT_008_ASingleSectionLoftSaysSoBeforeLookingAnythingUp) {
    // The feature's own guard, distinguishable from the kernel's. It fires
    // BEFORE the sections are resolved, so a one-section loft naming a sketch
    // that is not there still reports the count -- the thing the author can
    // actually fix -- rather than a missing-sketch error that is true but
    // beside the point.
    OcctGeometryKernel kernel;
    PartDocument document{"LoftDoc"};
    document.setGeometryKernel(&kernel);
    Parameter& notASketch = document.addParameter("L", 10.0, UnitType::Millimeter);

    Body& body = document.addBody("Body");
    LoftFeature& loft = document.addLoftFeature(body, "Loft1", {notASketch.id()});

    const DocumentRecomputeReport report = document.recompute();
    EXPECT_EQ(loft.currentState(), ComputeState::Failed);
    EXPECT_NE(FailureMessageFor(report, loft.id()).find("at least two sections"),
              std::string::npos)
        << FailureMessageFor(report, loft.id());
}

TEST(OcctRecomputeIntegrationTest, M19_FEAT_009_ASweepAlongItsOwnSketchSaysWHY) {
    // Removing this guard does not make the sweep succeed -- the kernel's own
    // zero-volume check catches it -- so a test that only asserted Failed would
    // pass with the guard gone. What is lost is the SENTENCE: OCCT's complaint
    // names neither sketch and offers nothing to do about it.
    OcctGeometryKernel kernel;
    PartDocument document{"SweepDoc"};
    document.setGeometryKernel(&kernel);

    Sketch& only = document.addSketch("Only");
    AddSquare(document, only.id(), 20.0);

    Body& body = document.addBody("Body");
    SweepFeature& sweep = document.addSweepFeature(body, "Sweep1", only.id(), only.id());

    const DocumentRecomputeReport report = document.recompute();
    EXPECT_EQ(sweep.currentState(), ComputeState::Failed);
    EXPECT_NE(FailureMessageFor(report, sweep.id()).find("two different sketches"),
              std::string::npos)
        << FailureMessageFor(report, sweep.id());
}

// --- M20: SHELL, DRAFT and HOLE as FEATURES ----------------------------------

namespace {

constexpr double kPi = 3.14159265358979323846;

// A pad of `side` x `side` x `height`, as the base every M20 feature dresses.
struct PaddedBox {
    PartDocument document{"M20Doc"};
    ObjectId sketchId{kInvalidObjectId};
    ObjectId padId{kInvalidObjectId};
    Body* body{nullptr};

    PaddedBox(OcctGeometryKernel& kernel, double side, double height) {
        document.setGeometryKernel(&kernel);
        Sketch& sketch = document.addSketch("Base");
        sketchId = sketch.id();
        AddSquare(document, sketchId, side);
        Parameter& tall = document.addParameter("H", height, UnitType::Millimeter);
        body = &document.addBody("Body");
        padId = document.addPadFeature(*body, "Pad1", sketchId, tall.id()).id();
    }
};

double VolumeOfFeature(OcctGeometryKernel& kernel, const ISolidFeature& feature) {
    const KernelMassPropertiesResult properties =
        kernel.calculateMassProperties(feature.currentShape());
    EXPECT_TRUE(properties) << properties.message;
    return properties.properties.volumeMm3;
}

FaceQuery FaceTowards(Vec3 direction) {
    FaceQuery query;
    query.extremeTowards = direction;
    return query;
}

} // namespace

TEST(OcctRecomputeIntegrationTest, M20_FEAT_001_AShellHollowsThePadItSitsOn) {
    // 60 x 60 x 40 hollowed to a 5 mm wall with the top open: the cavity is
    // 50 x 50 x 35, computed from the arithmetic rather than from the kernel.
    OcctGeometryKernel kernel;
    PaddedBox part{kernel, 60.0, 40.0};
    Parameter& wall = part.document.addParameter("W", 5.0, UnitType::Millimeter);
    ShellFeature& shell = part.document.addShellFeature(*part.body, "Shell1", part.padId,
                                                        {FaceTowards(Vec3{0, 0, 1})}, wall.id());

    ASSERT_TRUE(part.document.recompute().success);
    ASSERT_EQ(shell.currentState(), ComputeState::Valid);
    const double expected = 60.0 * 60.0 * 40.0 - 50.0 * 50.0 * 35.0;
    EXPECT_NEAR(VolumeOfFeature(kernel, shell), expected, 1e-6 * expected);
}

TEST(OcctRecomputeIntegrationTest, M20_FEAT_002_ChangingTheWALLRebuildsTheShell) {
    // The thickness is a Parameter, so it is a dependency. A shell that did not
    // depend on it would keep its old wall after the number changed -- and stay
    // wrong until something unrelated dirtied it.
    OcctGeometryKernel kernel;
    PaddedBox part{kernel, 60.0, 40.0};
    Parameter& wall = part.document.addParameter("W", 5.0, UnitType::Millimeter);
    ShellFeature& shell = part.document.addShellFeature(*part.body, "Shell1", part.padId,
                                                        {FaceTowards(Vec3{0, 0, 1})}, wall.id());
    ASSERT_TRUE(part.document.recompute().success);
    const double thin = VolumeOfFeature(kernel, shell);

    ASSERT_TRUE(part.document.setParameterValue(wall.id(), 10.0));
    ASSERT_TRUE(part.document.recompute().success);
    const double thick = VolumeOfFeature(kernel, shell);

    const double expected = 60.0 * 60.0 * 40.0 - 40.0 * 40.0 * 30.0;
    EXPECT_GT(thick, thin) << "a thicker wall leaves more material";
    EXPECT_NEAR(thick, expected, 1e-6 * expected);
}

TEST(OcctRecomputeIntegrationTest, M20_FEAT_003_AShellWithNOOpenFaceIsREFUSED) {
    OcctGeometryKernel kernel;
    PaddedBox part{kernel, 60.0, 40.0};
    Parameter& wall = part.document.addParameter("W", 5.0, UnitType::Millimeter);
    ShellFeature& shell =
        part.document.addShellFeature(*part.body, "Shell1", part.padId, {}, wall.id());

    const DocumentRecomputeReport report = part.document.recompute();
    EXPECT_EQ(shell.currentState(), ComputeState::Failed);
    EXPECT_NE(FailureMessageFor(report, shell.id()).find("no face to open"), std::string::npos)
        << FailureMessageFor(report, shell.id());
}

TEST(OcctRecomputeIntegrationTest, M20_FEAT_004_ADraftTapersAWallByTheAngleItIsGiven) {
    OcctGeometryKernel kernel;
    PaddedBox part{kernel, 60.0, 40.0};
    const double angleRad = 8.0 * kPi / 180.0;
    Parameter& angle = part.document.addParameter("A", angleRad, UnitType::Radian);

    FaceQuery wall;
    wall.facing = Vec3{0, 1, 0};
    DraftFeature& draft = part.document.addDraftFeature(
        *part.body, "Draft1", part.padId, {wall}, FaceTowards(Vec3{0, 0, -1}), angle.id());

    ASSERT_TRUE(part.document.recompute().success);
    ASSERT_EQ(draft.currentState(), ComputeState::Valid);

    const double before = 60.0 * 60.0 * 40.0;
    const double wedge = 60.0 * 40.0 * 40.0 * std::tan(angleRad) / 2.0;
    EXPECT_NEAR(std::fabs(VolumeOfFeature(kernel, draft) - before), wedge, 1e-6 * wedge);
}

TEST(OcctRecomputeIntegrationTest, M20_FEAT_005_ADraftAngleInMILLIMETRESIsREFUSED) {
    // The trap a revolve already guards: 8 stored in a Millimeter parameter
    // reads as 8 RADIANS. A revolve gets caught by the kernel's 2*pi ceiling;
    // a draft of 8 radians wraps to a plausible-looking taper of a shape turned
    // inside out.
    OcctGeometryKernel kernel;
    PaddedBox part{kernel, 60.0, 40.0};
    Parameter& angle = part.document.addParameter("A", 8.0, UnitType::Millimeter);

    FaceQuery wall;
    wall.facing = Vec3{0, 1, 0};
    DraftFeature& draft = part.document.addDraftFeature(
        *part.body, "Draft1", part.padId, {wall}, FaceTowards(Vec3{0, 0, -1}), angle.id());

    const DocumentRecomputeReport report = part.document.recompute();
    EXPECT_EQ(draft.currentState(), ComputeState::Failed);
    EXPECT_NE(FailureMessageFor(report, draft.id()).find("UnitType::Radian"), std::string::npos)
        << FailureMessageFor(report, draft.id());
}

TEST(OcctRecomputeIntegrationTest, M20_FEAT_006_AHoleDrillsAtEverySketchPoint) {
    // Two points, two bores. The volume says how many were drilled and how big
    // they are, both computed from the arithmetic.
    OcctGeometryKernel kernel;
    PaddedBox part{kernel, 60.0, 40.0};

    const std::optional<SketchFrame> onTop =
        SketchFrame::FromBasis(Vec3{0, 0, 40}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    ASSERT_TRUE(onTop.has_value());
    Sketch& holes = part.document.addSketch("Holes", *onTop);
    part.document.addSketchEntity(holes.id(), SketchPoint{Vec2{-15, 0}});
    part.document.addSketchEntity(holes.id(), SketchPoint{Vec2{15, 0}});

    Parameter& bore = part.document.addParameter("D", 10.0, UnitType::Millimeter);
    Parameter& deep = part.document.addParameter("Z", -20.0, UnitType::Millimeter);
    HoleFeature& drilled = part.document.addHoleFeature(*part.body, "Hole1", part.padId,
                                                        holes.id(), bore.id(), deep.id());

    ASSERT_TRUE(part.document.recompute().success);
    ASSERT_EQ(drilled.currentState(), ComputeState::Valid);

    const double expected = 60.0 * 60.0 * 40.0 - 2.0 * kPi * 25.0 * 20.0;
    EXPECT_NEAR(VolumeOfFeature(kernel, drilled), expected, 1e-6 * expected);
}

TEST(OcctRecomputeIntegrationTest, M20_FEAT_007_ADepthOfZEROGoesAllTheWayThrough) {
    // "Through all" asks how far the part reaches rather than guessing a deep
    // cylinder -- a guess works until somebody builds a part deeper than it.
    OcctGeometryKernel kernel;
    PaddedBox part{kernel, 60.0, 40.0};

    const std::optional<SketchFrame> onTop =
        SketchFrame::FromBasis(Vec3{0, 0, 40}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    ASSERT_TRUE(onTop.has_value());
    Sketch& holes = part.document.addSketch("Holes", *onTop);
    part.document.addSketchEntity(holes.id(), SketchPoint{Vec2{0, 0}});

    Parameter& bore = part.document.addParameter("D", 10.0, UnitType::Millimeter);
    Parameter& deep = part.document.addParameter("Z", 0.0, UnitType::Millimeter);
    HoleFeature& drilled = part.document.addHoleFeature(*part.body, "Hole1", part.padId,
                                                        holes.id(), bore.id(), deep.id());

    ASSERT_TRUE(part.document.recompute().success);
    // The bore goes the FULL 40 mm, not part way.
    const double expected = 60.0 * 60.0 * 40.0 - kPi * 25.0 * 40.0;
    EXPECT_NEAR(VolumeOfFeature(kernel, drilled), expected, 1e-6 * expected);
}

TEST(OcctRecomputeIntegrationTest, M20_FEAT_008_AHoleSketchWithNOPointsIsREFUSED) {
    // A hole feature that removes no material is one the user meant to do
    // something, and a chain that carries it forward unchanged looks exactly
    // like a chain that worked.
    OcctGeometryKernel kernel;
    PaddedBox part{kernel, 60.0, 40.0};
    Sketch& empty = part.document.addSketch("Holes");
    Parameter& bore = part.document.addParameter("D", 10.0, UnitType::Millimeter);
    Parameter& deep = part.document.addParameter("Z", -20.0, UnitType::Millimeter);
    HoleFeature& drilled = part.document.addHoleFeature(*part.body, "Hole1", part.padId,
                                                        empty.id(), bore.id(), deep.id());

    const DocumentRecomputeReport report = part.document.recompute();
    EXPECT_EQ(drilled.currentState(), ComputeState::Failed);
    EXPECT_NE(FailureMessageFor(report, drilled.id()).find("no points"), std::string::npos)
        << FailureMessageFor(report, drilled.id());
}

TEST(OcctRecomputeIntegrationTest, M20_FEAT_009_MovingAHolesPointMovesTheBore) {
    // The whole reason a hole is placed by sketch POINTS rather than by
    // coordinates: the point can be dimensioned, and the bore follows.
    OcctGeometryKernel kernel;
    PaddedBox part{kernel, 60.0, 40.0};

    const std::optional<SketchFrame> onTop =
        SketchFrame::FromBasis(Vec3{0, 0, 40}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    ASSERT_TRUE(onTop.has_value());
    Sketch& holes = part.document.addSketch("Holes", *onTop);
    const SketchEntityId centre =
        part.document.addSketchEntity(holes.id(), SketchPoint{Vec2{0, 0}});

    Parameter& bore = part.document.addParameter("D", 10.0, UnitType::Millimeter);
    Parameter& deep = part.document.addParameter("Z", -20.0, UnitType::Millimeter);
    HoleFeature& drilled = part.document.addHoleFeature(*part.body, "Hole1", part.padId,
                                                        holes.id(), bore.id(), deep.id());
    ASSERT_TRUE(part.document.recompute().success);
    const KernelMassPropertiesResult centred =
        kernel.calculateMassProperties(drilled.currentShape());
    ASSERT_TRUE(centred) << centred.message;
    // A bore down the middle of a square pad leaves the centroid where it was.
    EXPECT_NEAR(std::hypot(centred.properties.centerOfMassMm.x,
                           centred.properties.centerOfMassMm.y),
                0.0, 1e-6);

    ASSERT_TRUE(part.document.setSketchEntityGeometry(holes.id(), centre,
                                                      SketchPoint{Vec2{20, 20}}));
    ASSERT_TRUE(part.document.recompute().success);
    const KernelMassPropertiesResult moved =
        kernel.calculateMassProperties(drilled.currentShape());
    ASSERT_TRUE(moved) << moved.message;

    // The SAME material is removed, so the volume is unchanged -- what moved is
    // WHERE it was removed from, and the centroid says exactly how far.
    //
    // Taking a bore of volume v out at (20, 20) shifts the centroid of a solid
    // of volume V to -v*20/(V - v) on each axis. That is arithmetic, not the
    // kernel's answer read back, which is what makes it a check: a bore that
    // ignored its point and stayed in the middle would leave the centroid at
    // nought, and one that moved somewhere else would not land on this number.
    const double solid = 60.0 * 60.0 * 40.0;
    const double bored = kPi * 25.0 * 20.0;
    const double offset = bored * 20.0 / (solid - bored);
    EXPECT_NEAR(moved.properties.centerOfMassMm.x, -offset, 1e-6 * offset)
        << "the bore did not follow its point";
    EXPECT_NEAR(moved.properties.centerOfMassMm.y, -offset, 1e-6 * offset);
    EXPECT_NEAR(moved.properties.volumeMm3, centred.properties.volumeMm3,
                1e-6 * centred.properties.volumeMm3);
}

TEST(OcctRecomputeIntegrationTest, M20_FEAT_010_AThroughHoleReachesEvenOnATALLPart) {
    // "Through all" ASKS how far the part reaches. The alternative -- a
    // generous fixed depth -- works on every part anyone happens to test with
    // and then stops part-way on the first one that is bigger, leaving a blind
    // hole that looks exactly like a through hole from the top.
    //
    // This part is 2 metres tall on purpose: a guess would have to be at least
    // that to survive it, and whatever number it was, some part is taller.
    OcctGeometryKernel kernel;
    PaddedBox part{kernel, 60.0, 2000.0};

    const std::optional<SketchFrame> onTop =
        SketchFrame::FromBasis(Vec3{0, 0, 2000}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    ASSERT_TRUE(onTop.has_value());
    Sketch& holes = part.document.addSketch("Holes", *onTop);
    part.document.addSketchEntity(holes.id(), SketchPoint{Vec2{0, 0}});

    Parameter& bore = part.document.addParameter("D", 10.0, UnitType::Millimeter);
    Parameter& deep = part.document.addParameter("Z", 0.0, UnitType::Millimeter);
    HoleFeature& drilled = part.document.addHoleFeature(*part.body, "Hole1", part.padId,
                                                        holes.id(), bore.id(), deep.id());

    ASSERT_TRUE(part.document.recompute().success);
    ASSERT_EQ(drilled.currentState(), ComputeState::Valid);
    const double expected = 60.0 * 60.0 * 2000.0 - kPi * 25.0 * 2000.0;
    EXPECT_NEAR(VolumeOfFeature(kernel, drilled), expected, 1e-6 * expected);
}

// --- M21: BOOLEANS, MULTI-BODY and the pattern family ------------------------

TEST(OcctRecomputeIntegrationTest, M21_FEAT_001_ABooleanCombinesTWOSolids) {
    // The multi-body feature, and the first thing here to consume two.
    // Two 60 x 60 x 40 pads, one offset 40 in x: they share 20 x 60 x 40.
    OcctGeometryKernel kernel;
    PartDocument document{"BoolDoc"};
    document.setGeometryKernel(&kernel);

    Sketch& left = document.addSketch("Left");
    AddSquare(document, left.id(), 60.0);
    const std::optional<SketchFrame> shifted =
        SketchFrame::FromBasis(Vec3{40, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    ASSERT_TRUE(shifted.has_value());
    Sketch& right = document.addSketch("Right", *shifted);
    AddSquare(document, right.id(), 60.0);

    Parameter& tall = document.addParameter("H", 40.0, UnitType::Millimeter);
    Body& body = document.addBody("Body");
    const ObjectId a = document.addPadFeature(body, "PadA", left.id(), tall.id()).id();
    const ObjectId b = document.addPadFeature(body, "PadB", right.id(), tall.id()).id();

    BooleanFeature& shared = document.addBooleanFeature(body, "Common",
                                                        BooleanOperation::Intersect, a, b);
    ASSERT_TRUE(document.recompute().success);
    ASSERT_EQ(shared.currentState(), ComputeState::Valid);

    const double expected = 20.0 * 60.0 * 40.0;
    EXPECT_NEAR(VolumeOfFeature(kernel, shared), expected, 1e-6 * expected);
}

TEST(OcctRecomputeIntegrationTest, M21_FEAT_002_BOTHOperandsAreConsumed) {
    // The reason consumedSolidIds became plural. With one id a boolean could
    // declare only its target, and the tool would stay a live chain tail --
    // the viewer would draw the leftover alongside the result, so the part
    // would appear twice.
    OcctGeometryKernel kernel;
    PartDocument document{"BoolDoc"};
    document.setGeometryKernel(&kernel);

    Sketch& left = document.addSketch("Left");
    AddSquare(document, left.id(), 60.0);
    const std::optional<SketchFrame> shifted =
        SketchFrame::FromBasis(Vec3{40, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    ASSERT_TRUE(shifted.has_value());
    Sketch& right = document.addSketch("Right", *shifted);
    AddSquare(document, right.id(), 60.0);

    Parameter& tall = document.addParameter("H", 40.0, UnitType::Millimeter);
    Body& body = document.addBody("Body");
    const ObjectId a = document.addPadFeature(body, "PadA", left.id(), tall.id()).id();
    const ObjectId b = document.addPadFeature(body, "PadB", right.id(), tall.id()).id();
    BooleanFeature& united =
        document.addBooleanFeature(body, "Joined", BooleanOperation::Union, a, b);
    ASSERT_TRUE(document.recompute().success);

    const std::vector<ObjectId> eaten = united.consumedSolidIds();
    ASSERT_EQ(eaten.size(), 2u);
    EXPECT_EQ(eaten[0], a);
    EXPECT_EQ(eaten[1], b);
    // ...and the DOCUMENT agrees, which is the half that matters: the
    // consumed-once rule (ADR-M8-008) now refuses to let anything else eat the
    // tool. With a single-id declaration it would have said yes, because
    // nothing knew the boolean had taken it.
    Parameter& wall = document.addParameter("W", 5.0, UnitType::Millimeter);
    FaceQuery top;
    top.extremeTowards = Vec3{0, 0, 1};
    EXPECT_THROW(document.addShellFeature(body, "Late", b, {top}, wall.id()),
                 std::runtime_error);
}

TEST(OcctRecomputeIntegrationTest, M21_FEAT_003_SubtractKeepsItsORDER) {
    // "A minus B" and "B minus A" are different parts, and nothing here can
    // tell which the user meant -- so the order is stored, never normalised.
    OcctGeometryKernel kernel;
    const auto volumeFor = [&kernel](bool swapped) {
        PartDocument document{"BoolDoc"};
        document.setGeometryKernel(&kernel);
        Sketch& big = document.addSketch("Big");
        AddSquare(document, big.id(), 60.0);
        Sketch& small = document.addSketch("Small");
        AddSquare(document, small.id(), 30.0);
        Parameter& tall = document.addParameter("H", 40.0, UnitType::Millimeter);
        Parameter& shortOne = document.addParameter("S", 20.0, UnitType::Millimeter);
        Body& body = document.addBody("Body");
        const ObjectId a = document.addPadFeature(body, "PadA", big.id(), tall.id()).id();
        const ObjectId b = document.addPadFeature(body, "PadB", small.id(), shortOne.id()).id();
        BooleanFeature& cut = document.addBooleanFeature(
            body, "Cut", BooleanOperation::Subtract, swapped ? b : a, swapped ? a : b);
        EXPECT_TRUE(document.recompute().success);
        EXPECT_EQ(cut.currentState(), ComputeState::Valid);
        return VolumeOfFeature(kernel, cut);
    };

    // Big minus small: 60*60*40 less the 30*30*20 the small one occupies.
    const double drawn = volumeFor(false);
    EXPECT_NEAR(drawn, 60.0 * 60.0 * 40.0 - 30.0 * 30.0 * 20.0, 1e-3);
    // Small minus big is a DIFFERENT part -- the small one sits entirely
    // inside the big one, so almost nothing is left. A feature that sorted its
    // operands would return the same number twice.
    const double swapped = volumeFor(true);
    EXPECT_GT(std::fabs(drawn - swapped), 1.0)
        << "the two orders produced the same solid, so something normalised them";
}

TEST(OcctRecomputeIntegrationTest, M21_FEAT_004_ABooleanWithITSELFIsREFUSED) {
    // Union gives the same solid, intersect gives the same solid, subtract
    // gives nothing -- three answers, none of them what anybody meant, and two
    // of which look like success.
    OcctGeometryKernel kernel;
    PaddedBox part{kernel, 60.0, 40.0};
    BooleanFeature& silly = part.document.addBooleanFeature(
        *part.body, "Same", BooleanOperation::Union, part.padId, part.padId);

    const DocumentRecomputeReport report = part.document.recompute();
    EXPECT_EQ(silly.currentState(), ComputeState::Failed);
    EXPECT_NE(FailureMessageFor(report, silly.id()).find("two different solids"),
              std::string::npos)
        << FailureMessageFor(report, silly.id());
}

TEST(OcctRecomputeIntegrationTest, M21_FEAT_005_ACircularPatternMakesARing) {
    // Four instances at 90 degrees about the world Z: a full ring of four
    // disjoint blocks, so the volume is four times the one.
    OcctGeometryKernel kernel;
    PartDocument document{"RingDoc"};
    document.setGeometryKernel(&kernel);

    // A 20 mm block, offset from the axis so the copies do not overlap.
    const std::optional<SketchFrame> offset =
        SketchFrame::FromBasis(Vec3{80, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    ASSERT_TRUE(offset.has_value());
    Sketch& sketch = document.addSketch("Tooth", *offset);
    AddSquare(document, sketch.id(), 20.0);
    Parameter& tall = document.addParameter("H", 10.0, UnitType::Millimeter);
    Body& body = document.addBody("Body");
    const ObjectId pad = document.addPadFeature(body, "Pad1", sketch.id(), tall.id()).id();

    ReferenceFrame& axis = document.addFrame("Axis");
    Parameter& count = document.addParameter("N", 4.0, UnitType::Unitless);
    Parameter& step = document.addParameter("Step", kPi / 2.0, UnitType::Radian);
    CircularPatternFeature& ring = document.addCircularPatternFeature(
        body, "Ring", pad, axis.id(), count.id(), step.id());

    ASSERT_TRUE(document.recompute().success);
    ASSERT_EQ(ring.currentState(), ComputeState::Valid);
    const double one = 20.0 * 20.0 * 10.0;
    EXPECT_NEAR(VolumeOfFeature(kernel, ring), 4.0 * one, 1e-6 * one);
}

TEST(OcctRecomputeIntegrationTest, M21_FEAT_006_ACircularStepInMILLIMETRESIsREFUSED) {
    // 90 stored as millimetres reads as 90 RADIANS -- fourteen turns -- and
    // lands nowhere near where the drawing said, while looking like a number.
    OcctGeometryKernel kernel;
    PaddedBox part{kernel, 20.0, 10.0};
    ReferenceFrame& axis = part.document.addFrame("Axis");
    Parameter& count = part.document.addParameter("N", 4.0, UnitType::Unitless);
    Parameter& step = part.document.addParameter("Step", 90.0, UnitType::Millimeter);
    CircularPatternFeature& ring = part.document.addCircularPatternFeature(
        *part.body, "Ring", part.padId, axis.id(), count.id(), step.id());

    const DocumentRecomputeReport report = part.document.recompute();
    EXPECT_EQ(ring.currentState(), ComputeState::Failed);
    EXPECT_NE(FailureMessageFor(report, ring.id()).find("UnitType::Radian"), std::string::npos)
        << FailureMessageFor(report, ring.id());
}

TEST(OcctRecomputeIntegrationTest, M21_FEAT_007_ACurvePatternSpacesCopiesAlongASketchPath) {
    // Three instances along a 200 mm straight path: the base at one end, one at
    // the far end, one exactly in the middle -- so they do not touch and the
    // volume is three times one.
    OcctGeometryKernel kernel;
    PartDocument document{"CurveDoc"};
    document.setGeometryKernel(&kernel);

    Sketch& block = document.addSketch("Block");
    AddSquare(document, block.id(), 20.0);
    Parameter& tall = document.addParameter("H", 10.0, UnitType::Millimeter);
    Body& body = document.addBody("Body");
    const ObjectId pad = document.addPadFeature(body, "Pad1", block.id(), tall.id()).id();

    // The path starts where the block is and runs 200 mm along +X.
    Sketch& path = document.addSketch("Path");
    document.addSketchEntity(path.id(), SketchLine{Vec2{0, 0}, Vec2{200, 0}});

    Parameter& count = document.addParameter("N", 3.0, UnitType::Unitless);
    CurvePatternFeature& spread =
        document.addCurvePatternFeature(body, "Along", pad, path.id(), count.id());

    ASSERT_TRUE(document.recompute().success);
    ASSERT_EQ(spread.currentState(), ComputeState::Valid);
    const double one = 20.0 * 20.0 * 10.0;
    EXPECT_NEAR(VolumeOfFeature(kernel, spread), 3.0 * one, 1e-6 * one);

    // ...and they really are spread out, not stacked: the bounds reach the far
    // end of the path.
    const KernelBoundsResult bounds = kernel.boundsOfShape(spread.currentShape());
    ASSERT_TRUE(bounds.ok);
    EXPECT_NEAR(bounds.max.x, 210.0, 1e-6);
}

TEST(OcctRecomputeIntegrationTest, M21_FEAT_008_MovingThePATHMovesTheCopies) {
    // The path sketch is a dependency, so editing the curve rebuilds the
    // pattern. Without that edge the copies would stay where the old curve put
    // them, and nothing would say so.
    OcctGeometryKernel kernel;
    PartDocument document{"CurveDoc"};
    document.setGeometryKernel(&kernel);

    Sketch& block = document.addSketch("Block");
    AddSquare(document, block.id(), 20.0);
    Parameter& tall = document.addParameter("H", 10.0, UnitType::Millimeter);
    Body& body = document.addBody("Body");
    const ObjectId pad = document.addPadFeature(body, "Pad1", block.id(), tall.id()).id();

    Sketch& path = document.addSketch("Path");
    const SketchEntityId line =
        document.addSketchEntity(path.id(), SketchLine{Vec2{0, 0}, Vec2{200, 0}});
    Parameter& count = document.addParameter("N", 3.0, UnitType::Unitless);
    CurvePatternFeature& spread =
        document.addCurvePatternFeature(body, "Along", pad, path.id(), count.id());
    ASSERT_TRUE(document.recompute().success);

    ASSERT_TRUE(document.setSketchEntityGeometry(path.id(), line,
                                                 SketchLine{Vec2{0, 0}, Vec2{400, 0}}));
    ASSERT_TRUE(document.recompute().success);
    const KernelBoundsResult bounds = kernel.boundsOfShape(spread.currentShape());
    ASSERT_TRUE(bounds.ok);
    EXPECT_NEAR(bounds.max.x, 410.0, 1e-6) << "the copies did not follow the path";
}

TEST(OcctRecomputeIntegrationTest, M21_FEAT_009_AFaceQuerySURVIVESABoolean) {
    // THE RISK THE PLAN NAMED FIRST, under pressure for the first time.
    //
    // `CreatedBy` rides OCCT's modelling history, and history is what a boolean
    // is worst at keeping. If it breaks, a shell that named "the top face of
    // what the pad made" stops finding it -- and in an assembly the same break
    // is a mate on a face that no longer resolves, which is the failure the
    // parity plan calls the one that takes the whole machine apart.
    //
    // So: build two pads, union them, and then shell the RESULT through a
    // query that names a direction -- the one kind of query that must survive
    // any history at all, because it describes shape rather than provenance.
    OcctGeometryKernel kernel;
    PartDocument document{"HistoryDoc"};
    document.setGeometryKernel(&kernel);

    Sketch& left = document.addSketch("Left");
    AddSquare(document, left.id(), 60.0);
    const std::optional<SketchFrame> shifted =
        SketchFrame::FromBasis(Vec3{40, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    ASSERT_TRUE(shifted.has_value());
    Sketch& right = document.addSketch("Right", *shifted);
    AddSquare(document, right.id(), 60.0);

    Parameter& tall = document.addParameter("H", 40.0, UnitType::Millimeter);
    Parameter& wall = document.addParameter("W", 5.0, UnitType::Millimeter);
    Body& body = document.addBody("Body");
    const ObjectId a = document.addPadFeature(body, "PadA", left.id(), tall.id()).id();
    const ObjectId b = document.addPadFeature(body, "PadB", right.id(), tall.id()).id();
    const ObjectId joined =
        document.addBooleanFeature(body, "Joined", BooleanOperation::Union, a, b).id();

    FaceQuery top;
    top.extremeTowards = Vec3{0, 0, 1};
    ShellFeature& hollow =
        document.addShellFeature(body, "Shell1", joined, {top}, wall.id());

    const DocumentRecomputeReport report = document.recompute();
    EXPECT_EQ(hollow.currentState(), ComputeState::Valid)
        << "a face query did not survive the boolean: " << FailureMessageFor(report, hollow.id());
    ASSERT_TRUE(report.success) << FailureMessageFor(report, hollow.id());
    // ...and it really hollowed it: less material than the union it came from.
    const double union_ = 2.0 * 60.0 * 60.0 * 40.0 - 20.0 * 60.0 * 40.0;
    EXPECT_LT(VolumeOfFeature(kernel, hollow), union_ * 0.9);
}

TEST(OcctRecomputeIntegrationTest, M21_FEAT_011_EVERYChainFeatureRefusesAnAlreadyEatenBase) {
    // ADR-M8-008's consumed-once rule is enforced by a CALL AT EACH SITE, and
    // five features added in M20 and M21 were written without it. Nothing
    // noticed, because every test that exercised the rule used one of the older
    // features that had it.
    //
    // So this test walks EVERY chain feature the document can build. It is a
    // list, and a list can drift -- but it is a list in the one test whose
    // whole job is to catch that drift, and a new chain feature that forgets
    // the guard fails here on the day it is written rather than the day
    // somebody double-consumes a solid and gets two parts drawn on top of each
    // other.
    OcctGeometryKernel kernel;

    // Each entry: build a fresh part, take something else that already ate the
    // pad, then try to add THIS feature on the same pad.
    const auto expectRefused = [&kernel](const char* what,
                                         const std::function<void(PartDocument&, Body&,
                                                                  ObjectId)>& addOnTop) {
        PaddedBox part{kernel, 60.0, 40.0};
        Parameter& wall = part.document.addParameter("W", 5.0, UnitType::Millimeter);
        FaceQuery top;
        top.extremeTowards = Vec3{0, 0, 1};
        // The first consumer, which is allowed.
        part.document.addShellFeature(*part.body, "First", part.padId, {top}, wall.id());
        // The second, which is not.
        EXPECT_THROW(addOnTop(part.document, *part.body, part.padId), std::runtime_error)
            << what << " let a second feature consume the same solid";
    };

    expectRefused("Shell", [](PartDocument& d, Body& b, ObjectId base) {
        FaceQuery top;
        top.extremeTowards = Vec3{0, 0, 1};
        Parameter& w = d.addParameter("W2", 5.0, UnitType::Millimeter);
        d.addShellFeature(b, "X", base, {top}, w.id());
    });
    expectRefused("Draft", [](PartDocument& d, Body& b, ObjectId base) {
        FaceQuery wall;
        wall.facing = Vec3{0, 1, 0};
        FaceQuery neutral;
        neutral.extremeTowards = Vec3{0, 0, -1};
        Parameter& a = d.addParameter("A2", 0.1, UnitType::Radian);
        d.addDraftFeature(b, "X", base, {wall}, neutral, a.id());
    });
    expectRefused("Hole", [](PartDocument& d, Body& b, ObjectId base) {
        Sketch& s = d.addSketch("Pts2");
        d.addSketchEntity(s.id(), SketchPoint{Vec2{0, 0}});
        Parameter& dia = d.addParameter("D2", 5.0, UnitType::Millimeter);
        Parameter& dep = d.addParameter("Z2", -5.0, UnitType::Millimeter);
        d.addHoleFeature(b, "X", base, s.id(), dia.id(), dep.id());
    });
    expectRefused("Pocket", [](PartDocument& d, Body& b, ObjectId base) {
        Sketch& s = d.addSketch("Cut2");
        AddSquare(d, s.id(), 10.0);
        Parameter& dep = d.addParameter("Z3", 5.0, UnitType::Millimeter);
        d.addPocketFeature(b, "X", base, s.id(), dep.id());
    });
    expectRefused("Fillet", [](PartDocument& d, Body& b, ObjectId base) {
        Parameter& r = d.addParameter("R2", 2.0, UnitType::Millimeter);
        d.addFilletFeature(b, "X", base, r.id());
    });
    expectRefused("Chamfer", [](PartDocument& d, Body& b, ObjectId base) {
        Parameter& r = d.addParameter("C2", 2.0, UnitType::Millimeter);
        d.addChamferFeature(b, "X", base, r.id());
    });
    expectRefused("Mirror", [](PartDocument& d, Body& b, ObjectId base) {
        ReferenceFrame& f = d.addFrame("F2");
        d.addMirrorFeature(b, "X", base, f.id());
    });
    expectRefused("Pattern", [](PartDocument& d, Body& b, ObjectId base) {
        ReferenceFrame& f = d.addFrame("F3");
        Parameter& n = d.addParameter("N2", 2.0, UnitType::Unitless);
        Parameter& s = d.addParameter("S2", 20.0, UnitType::Millimeter);
        d.addPatternFeature(b, "X", base, f.id(), n.id(), s.id());
    });
    expectRefused("CircularPattern", [](PartDocument& d, Body& b, ObjectId base) {
        ReferenceFrame& f = d.addFrame("F4");
        Parameter& n = d.addParameter("N3", 2.0, UnitType::Unitless);
        Parameter& s = d.addParameter("S3", 1.0, UnitType::Radian);
        d.addCircularPatternFeature(b, "X", base, f.id(), n.id(), s.id());
    });
    expectRefused("CurvePattern", [](PartDocument& d, Body& b, ObjectId base) {
        Sketch& p = d.addSketch("Path2");
        d.addSketchEntity(p.id(), SketchLine{Vec2{0, 0}, Vec2{100, 0}});
        Parameter& n = d.addParameter("N4", 2.0, UnitType::Unitless);
        d.addCurvePatternFeature(b, "X", base, p.id(), n.id());
    });
    expectRefused("Boolean (target)", [](PartDocument& d, Body& b, ObjectId base) {
        Sketch& s = d.addSketch("Other");
        AddSquare(d, s.id(), 20.0);
        Parameter& h = d.addParameter("H2", 10.0, UnitType::Millimeter);
        const ObjectId other = d.addPadFeature(b, "PadOther", s.id(), h.id()).id();
        d.addBooleanFeature(b, "X", BooleanOperation::Union, base, other);
    });
    expectRefused("Boolean (tool)", [](PartDocument& d, Body& b, ObjectId base) {
        Sketch& s = d.addSketch("Other2");
        AddSquare(d, s.id(), 20.0);
        Parameter& h = d.addParameter("H3", 10.0, UnitType::Millimeter);
        const ObjectId other = d.addPadFeature(b, "PadOther2", s.id(), h.id()).id();
        // The TOOL side, which a guard on the target alone would let through.
        d.addBooleanFeature(b, "X", BooleanOperation::Union, other, base);
    });
}

// --- M22: IMPORT as a feature, through a real document -----------------------

TEST(OcctRecomputeIntegrationTest, M22_FEAT_001_AnImportedSolidIsAChainBase) {
    // The whole path: build a part, write it, read it back as a feature, and
    // dress the result. An import that could not be a base would be a picture
    // rather than a part.
    OcctGeometryKernel kernel;
    const std::string path =
        (std::filesystem::temp_directory_path() / "ep3d-feature-import.step").string();
    std::remove(path.c_str());

    {
        PartDocument source{"Source"};
        source.setGeometryKernel(&kernel);
        Sketch& sketch = source.addSketch("Base");
        AddSquare(source, sketch.id(), 60.0);
        Parameter& tall = source.addParameter("H", 40.0, UnitType::Millimeter);
        Body& body = source.addBody("Body");
        const ObjectId pad = source.addPadFeature(body, "Pad1", sketch.id(), tall.id()).id();
        ASSERT_TRUE(source.recompute().success);
        const ISolidFeature* solid = dynamic_cast<const ISolidFeature*>(
            source.bodies().front()->features().front().get());
        ASSERT_NE(solid, nullptr);
        (void)pad;
        ASSERT_TRUE(kernel.exportStep(solid->currentShape(), path));
    }

    PartDocument document{"Imported"};
    document.setGeometryKernel(&kernel);
    Body& body = document.addBody("Body");
    ImportFeature& brought = document.addImportFeature(body, "Ghost", path);

    // ...and something downstream of it, to prove it is a base like any other.
    Parameter& wall = document.addParameter("W", 5.0, UnitType::Millimeter);
    FaceQuery top;
    top.extremeTowards = Vec3{0, 0, 1};
    ShellFeature& hollow =
        document.addShellFeature(body, "Shell1", brought.id(), {top}, wall.id());

    ASSERT_TRUE(document.recompute().success);
    EXPECT_EQ(brought.currentState(), ComputeState::Valid);
    EXPECT_EQ(hollow.currentState(), ComputeState::Valid);

    const double expected = 60.0 * 60.0 * 40.0 - 50.0 * 50.0 * 35.0;
    EXPECT_NEAR(VolumeOfFeature(kernel, hollow), expected, 1e-6 * expected);
    std::remove(path.c_str());
}

TEST(OcctRecomputeIntegrationTest, M22_FEAT_002_AMissingSourceFileFAILSLoudly) {
    // The consequence of storing the path rather than the geometry, and it is
    // the point rather than a cost: a part that quietly kept working after its
    // source vanished is a part nobody can reproduce.
    OcctGeometryKernel kernel;
    PartDocument document{"Imported"};
    document.setGeometryKernel(&kernel);
    Body& body = document.addBody("Body");
    ImportFeature& brought = document.addImportFeature(body, "Ghost", "no-such-file.step");

    const DocumentRecomputeReport report = document.recompute();
    EXPECT_EQ(brought.currentState(), ComputeState::Failed);
    EXPECT_NE(FailureMessageFor(report, brought.id()).find("could not read"), std::string::npos)
        << FailureMessageFor(report, brought.id());
}

TEST(OcctRecomputeIntegrationTest, M22_FEAT_003_ReExportingTheSourceCHANGESTheModel) {
    // The other consequence, and the reason the file is re-read every rebuild
    // rather than cached: a user who fixed the source expects the model to
    // follow it.
    OcctGeometryKernel kernel;
    const std::string path =
        (std::filesystem::temp_directory_path() / "ep3d-reexport.step").string();
    std::remove(path.c_str());

    const auto writeCube = [&](double side) {
        PartDocument source{"Source"};
        source.setGeometryKernel(&kernel);
        Sketch& sketch = source.addSketch("Base");
        AddSquare(source, sketch.id(), side);
        Parameter& tall = source.addParameter("H", side, UnitType::Millimeter);
        Body& body = source.addBody("Body");
        source.addPadFeature(body, "Pad1", sketch.id(), tall.id());
        EXPECT_TRUE(source.recompute().success);
        const ISolidFeature* solid = dynamic_cast<const ISolidFeature*>(
            source.bodies().front()->features().front().get());
        EXPECT_NE(solid, nullptr);
        EXPECT_TRUE(kernel.exportStep(solid->currentShape(), path));
    };

    writeCube(40.0);
    PartDocument document{"Imported"};
    document.setGeometryKernel(&kernel);
    Body& body = document.addBody("Body");
    ImportFeature& brought = document.addImportFeature(body, "Ghost", path);
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(VolumeOfFeature(kernel, brought), 40.0 * 40.0 * 40.0, 1e-3);

    // The source changes under it, and the model follows on the next rebuild.
    writeCube(20.0);
    // NOTHING DIRTIES AN IMPORT ON ITS OWN: its input is a file, and the
    // graph has no node for something outside the document. Saying so here
    // is the honest version of what wireImportFeature records.
    ASSERT_TRUE(document.markDirty(brought.id()));
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(VolumeOfFeature(kernel, brought), 20.0 * 20.0 * 20.0, 1e-3);
    std::remove(path.c_str());
}


TEST(OcctRecomputeIntegrationTest, M22_FEAT_004_AnImportsFacesAreTaggedAsITSOwn) {
    // Every other chain base tags the faces it makes with its own id, so a
    // downstream feature can say "the face THIS made" and keep meaning it after
    // the geometry moves (ADR-M17-035). An import that skipped the tagging
    // would still measure, still round-trip and still take a `facing:` query --
    // and would silently be the ONE base in the tree that cannot be referred to
    // by provenance. A volume assertion cannot see that; only asking a
    // CreatedBy question can.
    OcctGeometryKernel kernel;
    const std::string path =
        (std::filesystem::temp_directory_path() / "ep3d-tagged-import.step").string();
    std::remove(path.c_str());

    {
        PartDocument source{"Source"};
        source.setGeometryKernel(&kernel);
        Sketch& sketch = source.addSketch("Base");
        AddSquare(source, sketch.id(), 60.0);
        Parameter& tall = source.addParameter("H", 40.0, UnitType::Millimeter);
        Body& body = source.addBody("Body");
        source.addPadFeature(body, "Pad1", sketch.id(), tall.id());
        ASSERT_TRUE(source.recompute().success);
        const auto* solid = dynamic_cast<const ISolidFeature*>(
            source.bodies().front()->features().front().get());
        ASSERT_NE(solid, nullptr);
        ASSERT_TRUE(kernel.exportStep(solid->currentShape(), path));
    }

    PartDocument document{"Imported"};
    document.setGeometryKernel(&kernel);
    Body& body = document.addBody("Body");
    ImportFeature& brought = document.addImportFeature(body, "Ghost", path);
    ASSERT_TRUE(document.recompute().success);

    // "The top face of what the import made" -- both halves, because createdBy
    // alone matches all six.
    FaceQuery mine;
    mine.createdBy = brought.id();
    mine.extremeTowards = Vec3{0, 0, 1};
    const FaceQueryResult found = kernel.resolveFace(brought.currentShape(), mine);
    EXPECT_TRUE(found.ok) << found.message;
    EXPECT_NEAR(found.face.point.z, 40.0, 1e-6) << found.message;

    // ...and the tag is the import's OWN id, not merely some id: a query naming
    // a different feature has to find nothing.
    FaceQuery somebodyElses;
    somebodyElses.createdBy = brought.id() + 1000;
    EXPECT_FALSE(kernel.resolveFace(brought.currentShape(), somebodyElses).ok)
        << "the import's faces answered to a feature that never touched them";

    std::remove(path.c_str());
}

// --- M23: an assembly instance, built and placed against real geometry -------

namespace {

// A scratch part on disk, removed when the test ends -- an instance names a
// FILE, so there has to be one.
struct ScratchPart {
    std::string path;
    explicit ScratchPart(const char* name) {
        path = (std::filesystem::temp_directory_path() / (std::string("ep3d-asm-") + name))
                   .string();
        std::remove(path.c_str());
    }
    ~ScratchPart() { std::remove(path.c_str()); }
};

// A cube of `side`, in a body called `bodyName`, written to `path`.
void WriteCubePart(const std::string& path, double side, const std::string& bodyName,
                   const std::string& secondBody = {}) {
    PartDocument part{"Source"};
    Sketch& sketch = part.addSketch("Base");
    AddSquare(part, sketch.id(), side);
    Parameter& tall = part.addParameter("H", side, UnitType::Millimeter);
    Body& body = part.addBody(bodyName);
    part.addPadFeature(body, "Pad1", sketch.id(), tall.id());
    if (!secondBody.empty()) {
        Sketch& other = part.addSketch("Other");
        AddSquare(part, other.id(), side / 2.0);
        Body& second = part.addBody(secondBody);
        part.addPadFeature(second, "Pad2", other.id(), tall.id());
    }
    ASSERT_TRUE(savePartDocumentToFile(part, path));
}

KernelMassProperties MassOf(OcctGeometryKernel& kernel, const KernelShape& shape) {
    const KernelMassPropertiesResult result = kernel.calculateMassProperties(shape);
    EXPECT_TRUE(result) << result.message;
    return result.properties;
}

Transform3D MovedTo(double x, double y, double z) {
    Transform3D t;
    t.translation = Vec3{x, y, z};
    return t;
}

} // namespace

TEST(OcctRecomputeIntegrationTest, M23_INST_001_AnInstanceBuildsThePartWhereItWasPut) {
    // The whole path in one test: a real part on disk, instanced, moved, and
    // measured where it ended up. A volume alone would pass on an instance
    // that ignored its placement entirely, so the CENTROID is what carries the
    // claim -- it is the only number that moves when the part does.
    OcctGeometryKernel kernel;
    ScratchPart file{"cube.ep3d"};
    WriteCubePart(file.path, 40.0, "Block");

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    PartInstance& one = assembly.addInstance("One", file.path);
    ASSERT_TRUE(assembly.setInstanceTransform(one.id(), MovedTo(100, 0, 0)));

    ASSERT_TRUE(assembly.recompute().success);
    EXPECT_EQ(one.currentState(), ComputeState::Valid);

    const KernelMassProperties placed = MassOf(kernel, one.currentShape());
    EXPECT_NEAR(placed.volumeMm3, 40.0 * 40.0 * 40.0, 1e-6 * 64000.0);
    // The part is drawn centred on its sketch origin and extruded upwards, so
    // an unplaced copy would sit at (0, 0, 20).
    EXPECT_NEAR(placed.centerOfMassMm.x, 100.0, 1e-6);
    EXPECT_NEAR(placed.centerOfMassMm.y, 0.0, 1e-6);
    EXPECT_NEAR(placed.centerOfMassMm.z, 20.0, 1e-6);
}

TEST(OcctRecomputeIntegrationTest, M23_INST_002_MovingAnInstanceMovesWhatItBuilt) {
    // Through the graph: nothing here tells the instance it is stale, the
    // frame -> instance edge does. An instance that cached its shape would
    // pass a first-build test and fail this one.
    OcctGeometryKernel kernel;
    ScratchPart file{"cube.ep3d"};
    WriteCubePart(file.path, 20.0, "Block");

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    PartInstance& one = assembly.addInstance("One", file.path);
    ASSERT_TRUE(assembly.recompute().success);
    EXPECT_NEAR(MassOf(kernel, one.currentShape()).centerOfMassMm.x, 0.0, 1e-6);

    ASSERT_TRUE(assembly.setInstanceTransform(one.id(), MovedTo(-75, 30, 0)));
    ASSERT_TRUE(assembly.recompute().success);
    const KernelMassProperties moved = MassOf(kernel, one.currentShape());
    EXPECT_NEAR(moved.centerOfMassMm.x, -75.0, 1e-6);
    EXPECT_NEAR(moved.centerOfMassMm.y, 30.0, 1e-6);
    EXPECT_NEAR(moved.volumeMm3, 8000.0, 1e-6 * 8000.0) << "moving it changed its size";
}

TEST(OcctRecomputeIntegrationTest, M23_INST_003_THREEInstancesOfOnePartAreThreeSolids) {
    // The M23 gate, geometry half: three parts in, each somewhere, each real.
    // Instancing the same file three times is the case a cache would break,
    // and the case an assembly exists for.
    OcctGeometryKernel kernel;
    ScratchPart file{"cube.ep3d"};
    WriteCubePart(file.path, 30.0, "Block");

    AssemblyDocument assembly{"Row"};
    assembly.setGeometryKernel(&kernel);
    const double places[3] = {0.0, 60.0, 120.0};
    std::vector<ObjectId> ids;
    for (int i = 0; i < 3; ++i) {
        PartInstance& one = assembly.addInstance("Cube" + std::to_string(i + 1), file.path);
        ASSERT_TRUE(assembly.setInstanceTransform(one.id(), MovedTo(places[i], 0, 0)));
        ids.push_back(one.id());
    }
    ASSERT_TRUE(assembly.recompute().success);

    for (int i = 0; i < 3; ++i) {
        const PartInstance* one = assembly.findInstance(ids[i]);
        ASSERT_NE(one, nullptr);
        ASSERT_EQ(one->currentState(), ComputeState::Valid);
        const KernelMassProperties placed = MassOf(kernel, one->currentShape());
        EXPECT_NEAR(placed.volumeMm3, 27000.0, 1e-6 * 27000.0);
        EXPECT_NEAR(placed.centerOfMassMm.x, places[i], 1e-6)
            << "instance " << (i + 1) << " did not go where it was put";
    }
}

TEST(OcctRecomputeIntegrationTest, M23_INST_004_AMissingPartFileFAILSLoudly) {
    // The cost of storing the sentence rather than the geometry, and the
    // point: an assembly that quietly kept showing a part whose file was
    // deleted is an assembly nobody can reproduce (ADR-M22-003, ADR-M23-002).
    OcctGeometryKernel kernel;
    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    PartInstance& ghost = assembly.addInstance("Ghost", "no-such-part.ep3d");

    const DocumentRecomputeReport report = assembly.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(ghost.currentState(), ComputeState::Failed);
    EXPECT_NE(FailureMessageFor(report, ghost.id()).find("no-such-part.ep3d"), std::string::npos)
        << FailureMessageFor(report, ghost.id());
}

TEST(OcctRecomputeIntegrationTest, M23_INST_005_APartWithTWOBodiesMustBeToldWhichOne) {
    // Taking the first would make this instance silently mean a different part
    // the day someone added a body to that file. Refused WITH THE NAMES,
    // because the reader's next move is to type one of them.
    OcctGeometryKernel kernel;
    ScratchPart file{"two-bodies.ep3d"};
    WriteCubePart(file.path, 20.0, "Left", "Right");

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    PartInstance& vague = assembly.addInstance("Vague", file.path);
    const DocumentRecomputeReport report = assembly.recompute();
    EXPECT_FALSE(report.success);
    const std::string why = FailureMessageFor(report, vague.id());
    EXPECT_NE(why.find("Left"), std::string::npos) << why;
    EXPECT_NE(why.find("Right"), std::string::npos) << why;

    // ...and naming one resolves it, to THAT one: the smaller body is a
    // quarter the footprint, so the number says which was chosen.
    ASSERT_TRUE(assembly.removeObject(vague.id()));
    PartInstance& named = assembly.addInstance("Named", file.path, "Right");
    ASSERT_TRUE(assembly.recompute().success);
    EXPECT_NEAR(MassOf(kernel, named.currentShape()).volumeMm3, 10.0 * 10.0 * 20.0,
                1e-6 * 2000.0);
}

TEST(OcctRecomputeIntegrationTest, M23_INST_006_EditingThePartCHANGESTheAssembly) {
    // The other consequence of storing the path, and the reason the file is
    // read again on every rebuild rather than cached: a user who changed the
    // part expects the assembly to follow it.
    OcctGeometryKernel kernel;
    ScratchPart file{"growing.ep3d"};
    WriteCubePart(file.path, 20.0, "Block");

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    PartInstance& one = assembly.addInstance("One", file.path);
    ASSERT_TRUE(assembly.recompute().success);
    EXPECT_NEAR(MassOf(kernel, one.currentShape()).volumeMm3, 8000.0, 1e-6 * 8000.0);

    WriteCubePart(file.path, 40.0, "Block");
    ASSERT_TRUE(assembly.markDirty(one.id()));
    ASSERT_TRUE(assembly.recompute().success);
    EXPECT_NEAR(MassOf(kernel, one.currentShape()).volumeMm3, 64000.0, 1e-6 * 64000.0);
}

TEST(OcctRecomputeIntegrationTest, M23_INST_007_APartThatDoesNotBuildNAMESWhatFailed) {
    // The reader's next move is to open that file and fix something, and which
    // feature it was is the difference between doing that and guessing.
    OcctGeometryKernel kernel;
    ScratchPart file{"broken.ep3d"};
    {
        PartDocument part{"Source"};
        Sketch& sketch = part.addSketch("Base");
        AddSquare(part, sketch.id(), 30.0);
        // A pad of ZERO length: the sketch is fine, the feature cannot build.
        Parameter& tall = part.addParameter("H", 0.0, UnitType::Millimeter);
        Body& body = part.addBody("Block");
        part.addPadFeature(body, "TooShort", sketch.id(), tall.id());
        ASSERT_TRUE(savePartDocumentToFile(part, file.path));
    }

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    PartInstance& one = assembly.addInstance("One", file.path);
    const DocumentRecomputeReport report = assembly.recompute();
    EXPECT_FALSE(report.success);
    const std::string why = FailureMessageFor(report, one.id());
    EXPECT_NE(why.find("TooShort"), std::string::npos)
        << "the assembly did not name the feature that failed: " << why;
}

TEST(OcctRecomputeIntegrationTest, M23_INST_008_APlacementRotatesFirstAndTHENTranslates) {
    // The order is a decision, and a caller that got it backwards would put a
    // part somewhere by an amount that looks like a modelling mistake. It is
    // decided once, in placeShape, and this is what pins it: the part's
    // centroid sits on its own axis, so rotate-then-move leaves it at
    // (100, 0, h/2) and move-then-rotate swings it round to (0, 100, h/2).
    OcctGeometryKernel kernel;
    ScratchPart file{"turned.ep3d"};
    WriteCubePart(file.path, 30.0, "Block");

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    PartInstance& one = assembly.addInstance("One", file.path);

    Transform3D placement = MovedTo(100, 0, 0);
    const double quarter = 3.14159265358979323846 / 2.0;
    placement.rotation = Quaternion{std::cos(quarter / 2.0), 0.0, 0.0, std::sin(quarter / 2.0)};
    ASSERT_TRUE(assembly.setInstanceTransform(one.id(), placement));
    ASSERT_TRUE(assembly.recompute().success);

    const KernelMassProperties placed = MassOf(kernel, one.currentShape());
    EXPECT_NEAR(placed.centerOfMassMm.x, 100.0, 1e-6) << "the placement translated before it turned";
    EXPECT_NEAR(placed.centerOfMassMm.y, 0.0, 1e-6) << "the placement translated before it turned";
    EXPECT_NEAR(placed.centerOfMassMm.z, 15.0, 1e-6);

    // ...and the part really did turn: a corner that was at (+15, +15) is now
    // at (-15, +15), which the bounding box cannot see on a square but the
    // volume confirms is still the same solid.
    EXPECT_NEAR(placed.volumeMm3, 27000.0, 1e-6 * 27000.0);
}

TEST(OcctRecomputeIntegrationTest, M23_INST_009_APlacementWithAScaleInItIsREFUSED) {
    // A non-unit quaternion is a SCALE hiding inside a rigid motion. Refused
    // rather than normalised: normalising would place the part correctly while
    // the caller believed it had asked for something else, and an assembly
    // that can silently resize a part is an assembly whose parts are not the
    // parts.
    OcctGeometryKernel kernel;
    Transform3D bad;
    bad.rotation = Quaternion{2.0, 0.0, 0.0, 0.0}; // twice as long as it should be
    BoxDefinition definition;
    definition.widthMm = 10.0;
    definition.heightMm = 10.0;
    definition.depthMm = 10.0;
    const ShapeResult box = kernel.createBox(definition);
    ASSERT_TRUE(box) << box.message;
    const ShapeResult refused = kernel.placeShape(box.shape, bad);
    EXPECT_FALSE(refused);
    EXPECT_NE(refused.message.find("unit quaternion"), std::string::npos) << refused.message;

    // ...and the ordinary identity placement is still accepted, so the guard
    // is a guard and not a wall.
    EXPECT_TRUE(kernel.placeShape(box.shape, Transform3D::Identity()));
}

TEST(OcctRecomputeIntegrationTest, M23_INST_010_AnInstanceTakesTheCHAINTIPNotTheFirstFeature) {
    // What an instance means by "that body" is the body AS IT STANDS -- after
    // everything that consumed anything. Taking the first solid feature would
    // instance the block a part was carved out of rather than the part, and
    // every test up to here missed it because every test part had exactly one
    // feature per body, where first and last are the same thing.
    //
    // So this part has a CHAIN: a pad, then a shell that eats it. The two
    // volumes are far apart, and the number says which one arrived.
    OcctGeometryKernel kernel;
    ScratchPart file{"chained.ep3d"};
    {
        PartDocument part{"Source"};
        Sketch& sketch = part.addSketch("Base");
        AddSquare(part, sketch.id(), 60.0);
        Parameter& tall = part.addParameter("H", 60.0, UnitType::Millimeter);
        Body& body = part.addBody("Case");
        const ObjectId pad = part.addPadFeature(body, "Pad1", sketch.id(), tall.id()).id();
        Parameter& wall = part.addParameter("W", 5.0, UnitType::Millimeter);
        FaceQuery top;
        top.extremeTowards = Vec3{0, 0, 1};
        part.addShellFeature(body, "Shell1", pad, {top}, wall.id());
        ASSERT_TRUE(savePartDocumentToFile(part, file.path));
    }

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    PartInstance& one = assembly.addInstance("One", file.path);
    ASSERT_TRUE(assembly.recompute().success);

    const double solidBlock = 60.0 * 60.0 * 60.0;
    const double hollowed = solidBlock - 50.0 * 50.0 * 55.0;
    const double measured = MassOf(kernel, one.currentShape()).volumeMm3;
    EXPECT_NEAR(measured, hollowed, 1e-6 * hollowed)
        << "the instance brought in the block, not the part carved out of it";
    EXPECT_LT(measured, 0.6 * solidBlock);
}
