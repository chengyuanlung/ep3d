#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Serialization/AssemblyDocumentSerializer.h"
#include "Solver/GaussNewtonAssemblySolver.h"
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
    KernelInterferenceResult measureInterference(const KernelShape& a,
                                                 const KernelShape& b) override {
        (void)a;
        (void)b;
        return KernelInterferenceResult{false, "this kernel does not measure interference", 0.0};
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
    IoResult exportIges(const KernelShape& shape, const std::string& path) override {
        return inner_.exportIges(shape, path);
    }
    ShapeResult importIges(const std::string& path) override {
        return inner_.importIges(path);
    }
    ShapeKind kindOfShape(const KernelShape& shape) override {
        return inner_.kindOfShape(shape);
    }
    ShapeResult importSurfaces(const std::string& path) override {
        return inner_.importSurfaces(path);
    }
    ShapeResult thickenSurface(const KernelShape& shape, double thicknessMm) override {
        return inner_.thickenSurface(shape, thicknessMm);
    }
    ShapeResult solidFromSkin(const KernelShape& shape) override {
        return inner_.solidFromSkin(shape);
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
    int countSolids(const KernelShape& shape) override {
        (void)shape;
        return 0;
    }
    ShapeResult compoundOf(const std::vector<KernelShape>& shapes) override {
        (void)shapes;
        return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                           "this kernel does not build compounds"};
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
    Instance& one = assembly.addInstance("One", file.path);
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
    Instance& one = assembly.addInstance("One", file.path);
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
        Instance& one = assembly.addInstance("Cube" + std::to_string(i + 1), file.path);
        ASSERT_TRUE(assembly.setInstanceTransform(one.id(), MovedTo(places[i], 0, 0)));
        ids.push_back(one.id());
    }
    ASSERT_TRUE(assembly.recompute().success);

    for (int i = 0; i < 3; ++i) {
        const Instance* one = assembly.findInstance(ids[i]);
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
    Instance& ghost = assembly.addInstance("Ghost", "no-such-part.ep3d");

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
    Instance& vague = assembly.addInstance("Vague", file.path);
    const DocumentRecomputeReport report = assembly.recompute();
    EXPECT_FALSE(report.success);
    const std::string why = FailureMessageFor(report, vague.id());
    EXPECT_NE(why.find("Left"), std::string::npos) << why;
    EXPECT_NE(why.find("Right"), std::string::npos) << why;

    // ...and naming one resolves it, to THAT one: the smaller body is a
    // quarter the footprint, so the number says which was chosen.
    ASSERT_TRUE(assembly.removeObject(vague.id()));
    Instance& named = assembly.addInstance("Named", file.path, "Right");
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
    Instance& one = assembly.addInstance("One", file.path);
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
    Instance& one = assembly.addInstance("One", file.path);
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
    Instance& one = assembly.addInstance("One", file.path);

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
    Instance& one = assembly.addInstance("One", file.path);
    ASSERT_TRUE(assembly.recompute().success);

    const double solidBlock = 60.0 * 60.0 * 60.0;
    const double hollowed = solidBlock - 50.0 * 50.0 * 55.0;
    const double measured = MassOf(kernel, one.currentShape()).volumeMm3;
    EXPECT_NEAR(measured, hollowed, 1e-6 * hollowed)
        << "the instance brought in the block, not the part carved out of it";
    EXPECT_LT(measured, 0.6 * solidBlock);
}

// --- M24: mates, against real parts on disk ----------------------------------

namespace {

constexpr double kPiM24 = 3.14159265358979323846;

// A part with a connector on it. `side` is the block, `at` where the connector
// sits in the part's own coordinates, `axis` where its +Z points -- which is
// the axis a revolute turns about and a slider slides along.
void WriteConnectedPart(const std::string& path, double side, const std::string& bodyName,
                        const std::string& connectorName, Vec3 at, Vec3 axis = Vec3{0, 0, 1}) {
    PartDocument part{"Source"};
    Sketch& sketch = part.addSketch("Base");
    AddSquare(part, sketch.id(), side);
    Parameter& tall = part.addParameter("H", side, UnitType::Millimeter);
    Body& body = part.addBody(bodyName);
    part.addPadFeature(body, "Pad1", sketch.id(), tall.id());

    ReferenceFrame& frame = part.addFrame(connectorName + " frame");
    Transform3D placement;
    placement.translation = at;
    // The shortest rotation taking +Z onto `axis`, built here rather than
    // borrowed from the script layer -- a test that used the production
    // helper would be checking that helper against itself.
    const double length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    EXPECT_GT(length, 1e-9);
    const Vec3 to{axis.x / length, axis.y / length, axis.z / length};
    if (to.z > 1.0 - 1e-12) {
        placement.rotation = Quaternion{1, 0, 0, 0};
    } else if (to.z < -1.0 + 1e-12) {
        placement.rotation = Quaternion{0, 1, 0, 0};
    } else {
        const double s = std::sqrt((1.0 + to.z) * 2.0);
        placement.rotation = Quaternion{s / 2.0, -to.y / s, to.x / s, 0.0};
    }
    part.setFrameTransform(frame.id(), placement);
    part.addConnector(connectorName, ConnectorRole::Generic, frame.id());
    ASSERT_TRUE(savePartDocumentToFile(part, path));
}

// How far apart two mate connectors ended up. THE measurement the hinge gate
// turns on: "does not fall apart" is exactly "this stays zero".
double ConnectorGap(const AssemblyDocument& assembly, ObjectId a, const std::string& aName,
                    ObjectId b, const std::string& bName) {
    bool foundA = false;
    bool foundB = false;
    const Transform3D at = assembly.mateConnectorWorldTransform(a, aName, &foundA);
    const Transform3D bt = assembly.mateConnectorWorldTransform(b, bName, &foundB);
    EXPECT_TRUE(foundA) << aName << " did not resolve";
    EXPECT_TRUE(foundB) << bName << " did not resolve";
    const double dx = at.translation.x - bt.translation.x;
    const double dy = at.translation.y - bt.translation.y;
    const double dz = at.translation.z - bt.translation.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::string SolveMessage(const AssemblyDocument& assembly) {
    return assembly.mateSolveReport().message;
}

} // namespace

TEST(OcctRecomputeIntegrationTest, M24_HINGE_001_AHingeTURNSAndDoesNotFALLAPART) {
    // THE M24 GATE, and the second half is the hard one.
    //
    // Turning needs one number to change. Not falling apart needs the solve to
    // put the arm where the mate actually says it goes, at EVERY angle -- and
    // that is checked by measuring the distance between the two mate
    // connectors, which is zero if and only if they are still joined.
    //
    // A centroid test alone would not do it: an arm that drifted along the
    // hinge axis would have a plausible-looking centroid and a joint that had
    // come apart.
    OcctGeometryKernel kernel;
    ScratchPart bracket{"hinge-bracket.ep3d"};
    ScratchPart arm{"hinge-arm.ep3d"};
    WriteConnectedPart(bracket.path, 40.0, "Bracket", "Pivot", Vec3{0, 0, 40});
    WriteConnectedPart(arm.path, 20.0, "Arm", "Eye", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Hinge"};
    assembly.setGeometryKernel(&kernel);
    Instance& base = assembly.addInstance("Base", bracket.path);
    Instance& swing = assembly.addInstance("Swing", arm.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(base.id(), true));
    Mate& elbow = assembly.addMate("Elbow", MateType::Revolute, base.id(), "Pivot", swing.id(),
                                   "Eye", 0.0);

    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    EXPECT_NEAR(ConnectorGap(assembly, base.id(), "Pivot", swing.id(), "Eye"), 0.0, 1e-9);

    // The arm's own centroid is 10 mm up its axis from the Eye, and the Eye
    // lands on the Pivot at (0, 0, 40) -- so at every angle the arm's centre
    // is at z = 50 and the joint is at z = 40.
    const KernelMassProperties closed = MassOf(kernel, swing.currentShape());
    EXPECT_NEAR(closed.centerOfMassMm.z, 50.0, 1e-6);

    // TURN IT. Every quarter, all the way round, and the joint has to hold at
    // each one -- a solve that were right only at zero would pass a test that
    // only ever looked at zero.
    for (int step = 1; step <= 4; ++step) {
        const double angle = step * kPiM24 / 2.0;
        ASSERT_TRUE(assembly.setMateValue(elbow.id(), angle));
        ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
        EXPECT_NEAR(ConnectorGap(assembly, base.id(), "Pivot", swing.id(), "Eye"), 0.0, 1e-9)
            << "the hinge came apart at step " << step;
        const KernelMassProperties turned = MassOf(kernel, swing.currentShape());
        EXPECT_NEAR(turned.centerOfMassMm.z, 50.0, 1e-6)
            << "the arm drifted along the hinge axis at step " << step;
        EXPECT_NEAR(turned.volumeMm3, closed.volumeMm3, 1e-6 * closed.volumeMm3)
            << "turning changed the arm";
    }

    // ...and the base never moved, because it is the ground.
    EXPECT_NEAR(assembly.instanceWorldTransform(base.id()).translation.x, 0.0, 1e-12);
    EXPECT_NEAR(assembly.instanceWorldTransform(base.id()).translation.z, 0.0, 1e-12);
}

TEST(OcctRecomputeIntegrationTest, M24_HINGE_002_ARevoluteReallyROTATESRatherThanJustMeeting) {
    // A solve that ignored the angle entirely would still hold the joint
    // together -- the connectors would coincide at every "angle" because the
    // angle was never applied. So the claim here is about a point that is NOT
    // on the hinge axis: the arm's far end has to swing.
    OcctGeometryKernel kernel;
    ScratchPart bracket{"swing-bracket.ep3d"};
    ScratchPart arm{"swing-arm.ep3d"};
    WriteConnectedPart(bracket.path, 40.0, "Bracket", "Pivot", Vec3{0, 0, 0});
    // The arm's connector is OFF its own centre, so the arm's centroid is not
    // on the hinge axis and has somewhere to swing to.
    WriteConnectedPart(arm.path, 20.0, "Arm", "Eye", Vec3{-30, 0, 0});

    AssemblyDocument assembly{"Swing"};
    assembly.setGeometryKernel(&kernel);
    Instance& base = assembly.addInstance("Base", bracket.path);
    Instance& swing = assembly.addInstance("Swing", arm.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(base.id(), true));
    Mate& elbow = assembly.addMate("Elbow", MateType::Revolute, base.id(), "Pivot", swing.id(),
                                   "Eye", 0.0);

    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    const KernelMassProperties at0 = MassOf(kernel, swing.currentShape());
    EXPECT_NEAR(at0.centerOfMassMm.x, 30.0, 1e-6);
    EXPECT_NEAR(at0.centerOfMassMm.y, 0.0, 1e-6);

    ASSERT_TRUE(assembly.setMateValue(elbow.id(), kPiM24 / 2.0));
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    const KernelMassProperties at90 = MassOf(kernel, swing.currentShape());
    EXPECT_NEAR(at90.centerOfMassMm.x, 0.0, 1e-6) << "a quarter turn did not move the arm";
    EXPECT_NEAR(at90.centerOfMassMm.y, 30.0, 1e-6);
    EXPECT_NEAR(at90.centerOfMassMm.z, at0.centerOfMassMm.z, 1e-6);
}

TEST(OcctRecomputeIntegrationTest, M24_HINGE_003_ASliderSlidesAlongTheSHAREDAxis) {
    // The connector's +Z is the axis for every mate kind, which is what makes
    // one formula enough. Here the axis is +X on both parts, so a slider has
    // to move the follower in X and in nothing else.
    OcctGeometryKernel kernel;
    ScratchPart railPart{"rail.ep3d"};
    ScratchPart shoePart{"shoe.ep3d"};
    WriteConnectedPart(railPart.path, 40.0, "Rail", "Track", Vec3{0, 0, 0}, Vec3{1, 0, 0});
    WriteConnectedPart(shoePart.path, 20.0, "Shoe", "Foot", Vec3{0, 0, 0}, Vec3{1, 0, 0});

    AssemblyDocument assembly{"Slide"};
    assembly.setGeometryKernel(&kernel);
    Instance& rail = assembly.addInstance("Rail", railPart.path);
    Instance& shoe = assembly.addInstance("Shoe", shoePart.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(rail.id(), true));
    Mate& slide =
        assembly.addMate("Slide", MateType::Slider, rail.id(), "Track", shoe.id(), "Foot", 0.0);

    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    const KernelMassProperties home = MassOf(kernel, shoe.currentShape());

    ASSERT_TRUE(assembly.setMateValue(slide.id(), 55.0));
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    const KernelMassProperties out = MassOf(kernel, shoe.currentShape());

    EXPECT_NEAR(out.centerOfMassMm.x - home.centerOfMassMm.x, 55.0, 1e-6)
        << "the slider did not slide along the connectors' shared axis";
    EXPECT_NEAR(out.centerOfMassMm.y, home.centerOfMassMm.y, 1e-6);
    EXPECT_NEAR(out.centerOfMassMm.z, home.centerOfMassMm.z, 1e-6);
    // A slider must not turn anything.
    EXPECT_NEAR(assembly.instanceWorldTransform(shoe.id()).rotation.w, 1.0, 1e-9);
}

TEST(OcctRecomputeIntegrationTest, M24_HINGE_004_AFastenedMatePutsTheConnectorsExactlyTogether) {
    OcctGeometryKernel kernel;
    ScratchPart a{"fast-a.ep3d"};
    ScratchPart b{"fast-b.ep3d"};
    WriteConnectedPart(a.path, 40.0, "A", "Face", Vec3{20, 5, 0});
    WriteConnectedPart(b.path, 20.0, "B", "Back", Vec3{-10, 0, 3});

    AssemblyDocument assembly{"Stack"};
    assembly.setGeometryKernel(&kernel);
    Instance& first = assembly.addInstance("First", a.path);
    Instance& second = assembly.addInstance("Second", b.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(first.id(), true));
    assembly.addMate("Bolt", MateType::Fastened, first.id(), "Face", second.id(), "Back");

    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    EXPECT_NEAR(ConnectorGap(assembly, first.id(), "Face", second.id(), "Back"), 0.0, 1e-9);
    // The follower moved to make that true rather than staying at the origin.
    const Transform3D placement = assembly.instanceWorldTransform(second.id());
    EXPECT_NEAR(placement.translation.x, 30.0, 1e-9);
    EXPECT_NEAR(placement.translation.y, 5.0, 1e-9);
    EXPECT_NEAR(placement.translation.z, -3.0, 1e-9);
}

TEST(OcctRecomputeIntegrationTest, M24_HINGE_005_MatesThatReachNoGroundAreREFUSED) {
    // "Somewhere" is not an answer. Grounding the first instance in the list
    // instead would make where everything ends up depend on the order things
    // were typed.
    OcctGeometryKernel kernel;
    ScratchPart a{"ungrounded-a.ep3d"};
    ScratchPart b{"ungrounded-b.ep3d"};
    WriteConnectedPart(a.path, 40.0, "A", "P", Vec3{0, 0, 0});
    WriteConnectedPart(b.path, 20.0, "B", "Q", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Floating"};
    assembly.setGeometryKernel(&kernel);
    Instance& first = assembly.addInstance("First", a.path);
    Instance& second = assembly.addInstance("Second", b.path);
    assembly.addMate("Join", MateType::Fastened, first.id(), "P", second.id(), "Q");

    EXPECT_FALSE(assembly.recompute().success);
    EXPECT_NE(SolveMessage(assembly).find("nothing in this assembly is grounded"),
              std::string::npos)
        << SolveMessage(assembly);

    // ...and grounding one fixes it, which is the evidence that the refusal
    // was about the ground and not about something else.
    ASSERT_TRUE(assembly.setInstanceGrounded(first.id(), true));
    EXPECT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
}

TEST(OcctRecomputeIntegrationTest, M24_HINGE_006_AnInstanceStrandedFromEveryGroundIsREFUSED) {
    // A grounded island and a floating pair. The floating pair has no answer,
    // and saying nothing about it would leave two parts sitting wherever they
    // were last dragged while the tree claimed the assembly was solved.
    OcctGeometryKernel kernel;
    ScratchPart part{"island.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Islands"};
    assembly.setGeometryKernel(&kernel);
    Instance& anchored = assembly.addInstance("Anchored", part.path);
    Instance& lost = assembly.addInstance("Lost", part.path);
    Instance& alsoLost = assembly.addInstance("AlsoLost", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(anchored.id(), true));
    assembly.addMate("Floating", MateType::Fastened, lost.id(), "P", alsoLost.id(), "P");

    EXPECT_FALSE(assembly.recompute().success);
    const std::string why = SolveMessage(assembly);
    EXPECT_NE(why.find("Lost"), std::string::npos) << why;
    EXPECT_NE(why.find("not connected to any ground"), std::string::npos) << why;
}

TEST(OcctRecomputeIntegrationTest, M25_LOOP_001_AClosedLoopIsSOLVEDNowRatherThanRefused) {
    // M24 refused this by name and said why: a tree solve cannot close a loop,
    // and approximating one would silently produce an assembly that does not
    // close. M25 buys the iterative solver, so the same three mates now solve
    // -- and the test that pinned the refusal is replaced rather than deleted,
    // because the behaviour it described is the behaviour that changed.
    OcctGeometryKernel kernel;
    GaussNewtonAssemblySolver solver;
    ScratchPart part{"loop.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Loop"};
    assembly.setGeometryKernel(&kernel);
    assembly.setAssemblySolver(&solver);
    Instance& a = assembly.addInstance("A", part.path);
    Instance& b = assembly.addInstance("B", part.path);
    Instance& c = assembly.addInstance("C", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(a.id(), true));
    assembly.addMate("AB", MateType::Fastened, a.id(), "P", b.id(), "P");
    assembly.addMate("BC", MateType::Fastened, b.id(), "P", c.id(), "P");
    assembly.addMate("CA", MateType::Fastened, c.id(), "P", a.id(), "P");

    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    // All three connectors coincide, which is what those three mates say.
    EXPECT_NEAR(ConnectorGap(assembly, a.id(), "P", c.id(), "P"), 0.0, 1e-9);
    EXPECT_NEAR(ConnectorGap(assembly, b.id(), "P", c.id(), "P"), 0.0, 1e-9);
    // Nothing is free: every mate in the loop is fastened.
    EXPECT_EQ(assembly.mateSolveReport().mechanismDegreesOfFreedom, 0);
}

TEST(OcctRecomputeIntegrationTest, M25_LOOP_002_ALoopThatCANNOTCloseSaysSo) {
    // The other half, and the one that makes the first mean something: a
    // mechanism with no configuration that satisfies its mates is REFUSED, not
    // approximated to the nearest miss. An assembly quietly 40 mm from closing
    // is worse than one that fails, because only one of them gets fixed.
    OcctGeometryKernel kernel;
    GaussNewtonAssemblySolver solver;
    ScratchPart part{"impossible.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Impossible"};
    assembly.setGeometryKernel(&kernel);
    assembly.setAssemblySolver(&solver);
    Instance& a = assembly.addInstance("A", part.path);
    Instance& b = assembly.addInstance("B", part.path);
    Instance& c = assembly.addInstance("C", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(a.id(), true));

    // Two DRIVEN sliders push C forty millimetres away from A...
    Mate& ab = assembly.addMate("AB", MateType::Slider, a.id(), "P", b.id(), "P", 20.0);
    Mate& bc = assembly.addMate("BC", MateType::Slider, b.id(), "P", c.id(), "P", 20.0);
    ASSERT_TRUE(assembly.setMateDriven(ab.id(), true));
    ASSERT_TRUE(assembly.setMateDriven(bc.id(), true));
    // ...and a fastened mate insists they are in the same place.
    assembly.addMate("CA", MateType::Fastened, c.id(), "P", a.id(), "P");

    EXPECT_FALSE(assembly.recompute().success);
    const std::string why = SolveMessage(assembly);
    EXPECT_NE(why.find("does not close"), std::string::npos) << why;
}

TEST(OcctRecomputeIntegrationTest, M24_HINGE_008_AConnectorNameThatDoesNotResolveIsNAMED) {
    // The connector lives in the PART file, so a part that was edited can take
    // one away. The reader's next move is to open that part, and which name
    // went missing is the difference between doing that and guessing.
    OcctGeometryKernel kernel;
    ScratchPart part{"named.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "Pivot", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    Instance& base = assembly.addInstance("Base", part.path);
    Instance& arm = assembly.addInstance("Arm", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(base.id(), true));
    assembly.addMate("Elbow", MateType::Revolute, base.id(), "Pivot", arm.id(), "NoSuchThing");

    EXPECT_FALSE(assembly.recompute().success);
    const std::string why = SolveMessage(assembly);
    EXPECT_NE(why.find("NoSuchThing"), std::string::npos) << why;
    EXPECT_NE(why.find("Arm"), std::string::npos) << why;
}

TEST(OcctRecomputeIntegrationTest, M24_HINGE_009_DOFIsReportedPERINSTANCE) {
    // Roadmap §20.3: "this assembly is under-constrained" is not something a
    // user can act on. "Swing has one rotation left, because of Elbow" is.
    OcctGeometryKernel kernel;
    ScratchPart part{"dof.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    Instance& base = assembly.addInstance("Base", part.path);
    Instance& swing = assembly.addInstance("Swing", part.path);
    Instance& bolted = assembly.addInstance("Bolted", part.path);
    Instance& loose = assembly.addInstance("Loose", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(base.id(), true));
    assembly.addMate("Elbow", MateType::Revolute, base.id(), "P", swing.id(), "P");
    assembly.addMate("Bolt", MateType::Fastened, swing.id(), "P", bolted.id(), "P");
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);

    const auto freedomOf = [&](ObjectId id) {
        for (const auto& one : assembly.mateSolveReport().freedoms)
            if (one.instanceId == id) return one;
        ADD_FAILURE() << "no freedom reported for an instance";
        return AssemblyDocument::MateSolveReport::InstanceFreedom{};
    };

    EXPECT_EQ(freedomOf(base.id()).rotational, 0);
    EXPECT_EQ(freedomOf(base.id()).translational, 0);
    EXPECT_EQ(freedomOf(base.id()).describedBy, "ground");

    EXPECT_EQ(freedomOf(swing.id()).rotational, 1) << "a revolute left no rotation";
    EXPECT_EQ(freedomOf(swing.id()).translational, 0);
    EXPECT_EQ(freedomOf(swing.id()).describedBy, "Elbow");

    EXPECT_EQ(freedomOf(bolted.id()).rotational, 0) << "a fastened mate left a freedom";
    EXPECT_EQ(freedomOf(bolted.id()).translational, 0);

    // Nothing mates it, so it is still exactly where it was put, with all six.
    EXPECT_EQ(freedomOf(loose.id()).rotational, 3);
    EXPECT_EQ(freedomOf(loose.id()).translational, 3);
    EXPECT_EQ(freedomOf(loose.id()).describedBy, "placed by hand");
}

TEST(OcctRecomputeIntegrationTest, M24_HINGE_010_AChainOfThreeIsPlacedAllTheWayDown) {
    // The solve walks outwards from the ground, so an instance two mates away
    // is placed from one that was itself placed by a mate. A solve that only
    // handled direct neighbours of the ground would pass every test above.
    OcctGeometryKernel kernel;
    ScratchPart part{"chain.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 20});

    AssemblyDocument assembly{"Tower"};
    assembly.setGeometryKernel(&kernel);
    Instance& a = assembly.addInstance("A", part.path);
    Instance& b = assembly.addInstance("B", part.path);
    Instance& c = assembly.addInstance("C", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(a.id(), true));
    // Each block's connector is 20 mm up its own body, so fastening the next
    // one's connector to it raises each by exactly nothing -- the connectors
    // coincide -- while a SLIDER of 20 stacks them.
    assembly.addMate("AB", MateType::Slider, a.id(), "P", b.id(), "P", 20.0);
    assembly.addMate("BC", MateType::Slider, b.id(), "P", c.id(), "P", 20.0);
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);

    const double zA = MassOf(kernel, a.currentShape()).centerOfMassMm.z;
    const double zB = MassOf(kernel, b.currentShape()).centerOfMassMm.z;
    const double zC = MassOf(kernel, c.currentShape()).centerOfMassMm.z;
    EXPECT_NEAR(zB - zA, 20.0, 1e-6) << "the first link did not place B";
    EXPECT_NEAR(zC - zB, 20.0, 1e-6) << "the chain stopped at the ground's neighbours";
    EXPECT_NEAR(ConnectorGap(assembly, b.id(), "P", c.id(), "P"), 20.0, 1e-9);
}

TEST(OcctRecomputeIntegrationTest, M24_HINGE_011_SolvingIsNotSomethingTheUserDID) {
    // A solved position is DERIVED. Pressing Undo after turning a hinge means
    // "put the angle back", not "put the arm back and leave the angle" -- so
    // the solve must move things without recording a single step.
    //
    // Every earlier test recomputed and then looked at geometry, which cannot
    // see this: an assembly whose solve pushed a Move onto the undo stack
    // produces exactly the same shapes.
    OcctGeometryKernel kernel;
    ScratchPart part{"undo-solve.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 20});

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    Instance& base = assembly.addInstance("Base", part.path);
    Instance& arm = assembly.addInstance("Arm", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(base.id(), true));
    Mate& elbow = assembly.addMate("Elbow", MateType::Revolute, base.id(), "P", arm.id(), "P");

    // A rebuild that really does move the arm -- confirmed below, so this is
    // not a test that passes because nothing happened.
    const std::size_t before = assembly.undoDepth();
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    EXPECT_EQ(assembly.undoDepth(), before) << "the solve recorded its own work";

    ASSERT_TRUE(assembly.setMateValue(elbow.id(), kPiM24 / 2.0));
    const std::size_t afterDrive = assembly.undoDepth();
    EXPECT_EQ(afterDrive, before + 1) << "driving a mate is one step, not more";
    const Transform3D wasAt = assembly.instanceWorldTransform(arm.id());
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    EXPECT_EQ(assembly.undoDepth(), afterDrive) << "the solve recorded its own work";

    // The rebuild DID move it, so the counts above mean something.
    const Transform3D nowAt = assembly.instanceWorldTransform(arm.id());
    EXPECT_GT(std::fabs(nowAt.rotation.z - wasAt.rotation.z), 1e-6)
        << "the solve moved nothing, so this test proved nothing";

    // ...and ONE undo puts the angle back, which is what the user meant.
    ASSERT_TRUE(assembly.undo());
    EXPECT_NEAR(elbow.value(), 0.0, 1e-12);
}

TEST(OcctRecomputeIntegrationTest, M24_HINGE_012_DrivingAMateMarksWhatItAffectsDIRTY) {
    // Observable BEFORE any rebuild: a tree that shows what is stale is a tree
    // a user can read. Checked here rather than in the Core suite because it
    // needs the instances to have been Valid first -- and in a Core test with
    // no kernel they are Dirty from birth, so the assertion cannot fail.
    OcctGeometryKernel kernel;
    ScratchPart part{"dirty-drive.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 20});

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    Instance& base = assembly.addInstance("Base", part.path);
    Instance& arm = assembly.addInstance("Arm", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(base.id(), true));
    Mate& elbow = assembly.addMate("Elbow", MateType::Revolute, base.id(), "P", arm.id(), "P");
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);

    ASSERT_EQ(assembly.dependencyGraph().state(arm.id()), ComputeState::Valid);
    ASSERT_EQ(assembly.dependencyGraph().state(base.id()), ComputeState::Valid);

    ASSERT_TRUE(assembly.setMateValue(elbow.id(), 0.4));
    // BOTH ends, because which one moves depends on the ground and the ground
    // is not consulted by an edit. Dirtying one would be a guess about a
    // direction that is not stored anywhere (ADR-M24-002).
    EXPECT_EQ(assembly.dependencyGraph().state(arm.id()), ComputeState::Dirty);
    EXPECT_EQ(assembly.dependencyGraph().state(base.id()), ComputeState::Dirty);
}

// --- M25: the four-bar linkage, and the rest of the mate family --------------

namespace {

// A link: a bar of `length` with a connector at EACH end, both pointing +Z so
// the pins are parallel and the linkage stays planar. `A` sits at the bar's
// near end and `B` at its far end.
void WriteLinkPart(const std::string& path, double length, const std::string& bodyName) {
    PartDocument part{"Link"};
    Sketch& sketch = part.addSketch("Bar");
    const double half = 4.0;
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, -half}, Vec2{length, -half}});
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{length, -half}, Vec2{length, half}});
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{length, half}, Vec2{0, half}});
    part.addSketchEntity(sketch.id(), SketchLine{Vec2{0, half}, Vec2{0, -half}});
    Parameter& thick = part.addParameter("T", 6.0, UnitType::Millimeter);
    Body& body = part.addBody(bodyName);
    part.addPadFeature(body, "Pad1", sketch.id(), thick.id());

    for (const auto& [name, x] : {std::pair<std::string, double>{"A", 0.0},
                                  std::pair<std::string, double>{"B", length}}) {
        ReferenceFrame& frame = part.addFrame(name + " frame");
        Transform3D at;
        at.translation = Vec3{x, 0.0, 0.0};
        part.setFrameTransform(frame.id(), at);
        part.addConnector(name, ConnectorRole::Generic, frame.id());
    }
    ASSERT_TRUE(savePartDocumentToFile(part, path));
}

} // namespace

TEST(OcctRecomputeIntegrationTest, M25_FOURBAR_001_DriveONELinkAndTheOtherTHREEFollow) {
    // THE M25 GATE.
    //
    // Four links in a closed loop: ground, crank, coupler, rocker. Turning the
    // crank has to move the other two to configurations nobody typed -- and
    // the loop has to STAY CLOSED, which is what a tree solve cannot do at all
    // and what M24 refused by name rather than approximate.
    //
    // The evidence is the gap at the closing joint. It is zero if and only if
    // the linkage is a linkage; an approximate solve would leave a millimetre
    // there and produce a picture that looks perfectly convincing.
    OcctGeometryKernel kernel;
    GaussNewtonAssemblySolver solver;
    ScratchPart groundPart{"fourbar-ground.ep3d"};
    ScratchPart crankPart{"fourbar-crank.ep3d"};
    ScratchPart couplerPart{"fourbar-coupler.ep3d"};
    ScratchPart rockerPart{"fourbar-rocker.ep3d"};
    // A GRASHOF crank-rocker: the shortest link plus the longest is less than
    // the other two, so the crank turns all the way round. Anything else and
    // "turn it a full circle" would be a demand the mechanism cannot meet, and
    // the test would be wrong rather than the solver.
    WriteLinkPart(groundPart.path, 100.0, "Ground");
    WriteLinkPart(crankPart.path, 30.0, "Crank");
    WriteLinkPart(couplerPart.path, 110.0, "Coupler");
    WriteLinkPart(rockerPart.path, 60.0, "Rocker");

    AssemblyDocument assembly{"FourBar"};
    assembly.setGeometryKernel(&kernel);
    assembly.setAssemblySolver(&solver);
    Instance& ground = assembly.addInstance("Ground", groundPart.path);
    Instance& crank = assembly.addInstance("Crank", crankPart.path);
    Instance& coupler = assembly.addInstance("Coupler", couplerPart.path);
    Instance& rocker = assembly.addInstance("Rocker", rockerPart.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(ground.id(), true));

    // Ground.A -- Crank.A -- Crank.B -- Coupler.A -- Coupler.B -- Rocker.A --
    // Rocker.B -- Ground.B, and round.
    Mate& j1 = assembly.addMate("J1", MateType::Revolute, ground.id(), "A", crank.id(), "A");
    assembly.addMate("J2", MateType::Revolute, crank.id(), "B", coupler.id(), "A");
    assembly.addMate("J3", MateType::Revolute, coupler.id(), "B", rocker.id(), "A");
    assembly.addMate("J4", MateType::Revolute, rocker.id(), "B", ground.id(), "B");
    // The crank is the one the user turns; the rest are what the solve moves.
    ASSERT_TRUE(assembly.setMateDriven(j1.id(), true));

    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);

    // A four-bar has exactly ONE freedom, and it is the crank's. Measured from
    // the rank of the Jacobian, not counted: this linkage writes fifteen
    // equations of which most are the identically-zero out-of-plane ones.
    EXPECT_EQ(assembly.mateSolveReport().mechanismDegreesOfFreedom, 0)
        << "the crank is driven, so nothing should be left over";

    std::vector<double> rockerAngles;
    for (int step = 0; step <= 8; ++step) {
        const double crankAngle = step * kPiM24 / 4.0;
        ASSERT_TRUE(assembly.setMateValue(j1.id(), crankAngle));
        ASSERT_TRUE(assembly.recompute().success)
            << "step " << step << ": " << SolveMessage(assembly);

        // EVERY JOINT STILL A JOINT. Four gaps, all zero, at every crank angle.
        EXPECT_NEAR(ConnectorGap(assembly, ground.id(), "A", crank.id(), "A"), 0.0, 1e-6)
            << "J1 came apart at step " << step;
        EXPECT_NEAR(ConnectorGap(assembly, crank.id(), "B", coupler.id(), "A"), 0.0, 1e-6)
            << "J2 came apart at step " << step;
        EXPECT_NEAR(ConnectorGap(assembly, coupler.id(), "B", rocker.id(), "A"), 0.0, 1e-6)
            << "J3 came apart at step " << step;
        EXPECT_NEAR(ConnectorGap(assembly, rocker.id(), "B", ground.id(), "B"), 0.0, 1e-6)
            << "J4 closed by " << ConnectorGap(assembly, rocker.id(), "B", ground.id(), "B")
            << " mm at step " << step;

        // ...and the crank really is where it was put, rather than having been
        // moved by the solve to make the numbers work.
        EXPECT_NEAR(assembly.findMate(j1.id())->value(), crankAngle, 1e-9)
            << "the solve moved the driven crank at step " << step;

        rockerAngles.push_back(assembly.findMateNamed("J4")->value());
    }

    // THE OTHER THREE FOLLOWED. A rocker that never moved would satisfy every
    // gap check above if the coupler happened to absorb everything, so the
    // claim that this is a MECHANISM and not four coincident points needs its
    // own evidence.
    const double lowest = *std::min_element(rockerAngles.begin(), rockerAngles.end());
    const double highest = *std::max_element(rockerAngles.begin(), rockerAngles.end());
    EXPECT_GT(highest - lowest, 0.5)
        << "the rocker barely moved, so nothing was following anything";
}

TEST(OcctRecomputeIntegrationTest, M25_FOURBAR_002_AnUndrivenLinkageReportsITSOneFreedom) {
    // Roadmap §20.3 wants freedom to be readable. For a mechanism the freedom
    // belongs to the LINKAGE, not to any one link -- a four-bar whose three
    // moving links each "have one rotation" reads as three when it has one.
    OcctGeometryKernel kernel;
    GaussNewtonAssemblySolver solver;
    ScratchPart groundPart{"free-ground.ep3d"};
    ScratchPart crankPart{"free-crank.ep3d"};
    ScratchPart couplerPart{"free-coupler.ep3d"};
    ScratchPart rockerPart{"free-rocker.ep3d"};
    WriteLinkPart(groundPart.path, 100.0, "Ground");
    WriteLinkPart(crankPart.path, 30.0, "Crank");
    WriteLinkPart(couplerPart.path, 110.0, "Coupler");
    WriteLinkPart(rockerPart.path, 60.0, "Rocker");

    AssemblyDocument assembly{"FourBar"};
    assembly.setGeometryKernel(&kernel);
    assembly.setAssemblySolver(&solver);
    Instance& ground = assembly.addInstance("Ground", groundPart.path);
    Instance& crank = assembly.addInstance("Crank", crankPart.path);
    Instance& coupler = assembly.addInstance("Coupler", couplerPart.path);
    Instance& rocker = assembly.addInstance("Rocker", rockerPart.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(ground.id(), true));
    assembly.addMate("J1", MateType::Revolute, ground.id(), "A", crank.id(), "A");
    assembly.addMate("J2", MateType::Revolute, crank.id(), "B", coupler.id(), "A");
    assembly.addMate("J3", MateType::Revolute, coupler.id(), "B", rocker.id(), "A");
    assembly.addMate("J4", MateType::Revolute, rocker.id(), "B", ground.id(), "B");

    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    EXPECT_EQ(assembly.mateSolveReport().mechanismDegreesOfFreedom, 1)
        << "a four-bar with nothing driven has exactly one freedom";

    // And the per-instance report says the freedom is the linkage's rather
    // than pretending to divide it up.
    const auto freedomOf = [&](ObjectId id) {
        for (const auto& one : assembly.mateSolveReport().freedoms)
            if (one.instanceId == id) return one;
        ADD_FAILURE() << "no freedom reported";
        return AssemblyDocument::MateSolveReport::InstanceFreedom{};
    };
    EXPECT_EQ(freedomOf(ground.id()).describedBy, "ground");
    EXPECT_EQ(freedomOf(crank.id()).describedBy, "in a closed loop");
    EXPECT_EQ(freedomOf(coupler.id()).describedBy, "in a closed loop");
    EXPECT_EQ(freedomOf(rocker.id()).describedBy, "in a closed loop");
}

TEST(OcctRecomputeIntegrationTest, M25_MATE_004_ACylindricalMateTurnsANDSlides) {
    // The first mate with TWO freedoms, which is what forced the value to
    // become one number per component. A revolute and a slider each free half
    // of what this frees, so the three together are the whole test of the
    // freedom table doing real work.
    OcctGeometryKernel kernel;
    ScratchPart part{"cyl.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Cyl"};
    assembly.setGeometryKernel(&kernel);
    Instance& base = assembly.addInstance("Base", part.path);
    Instance& shaft = assembly.addInstance("Shaft", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(base.id(), true));
    Mate& joint =
        assembly.addMate("Joint", MateType::Cylindrical, base.id(), "P", shaft.id(), "P");

    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    const KernelMassProperties home = MassOf(kernel, shaft.currentShape());

    // SLIDE it, without turning it.
    ASSERT_TRUE(assembly.setMateComponentValue(joint.id(), MateComponent::TZ, 30.0));
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    const KernelMassProperties slid = MassOf(kernel, shaft.currentShape());
    EXPECT_NEAR(slid.centerOfMassMm.z - home.centerOfMassMm.z, 30.0, 1e-6);
    EXPECT_NEAR(assembly.instanceWorldTransform(shaft.id()).rotation.w, 1.0, 1e-9)
        << "sliding a cylindrical mate turned it";

    // ...and TURN it, without losing the slide. Both at once is the point: a
    // model that kept one number would have lost the 30 here.
    ASSERT_TRUE(assembly.setMateComponentValue(joint.id(), MateComponent::RZ, kPiM24 / 2.0));
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    const KernelMassProperties both = MassOf(kernel, shaft.currentShape());
    EXPECT_NEAR(both.centerOfMassMm.z - home.centerOfMassMm.z, 30.0, 1e-6)
        << "turning it forgot how far it had slid";
    EXPECT_NEAR(assembly.instanceWorldTransform(shaft.id()).rotation.z, std::sin(kPiM24 / 4.0),
                1e-9);

    EXPECT_EQ(assembly.findMate(joint.id())->values()[2], 30.0);
    EXPECT_NEAR(assembly.findMate(joint.id())->values()[5], kPiM24 / 2.0, 1e-12);
}

TEST(OcctRecomputeIntegrationTest, M25_MATE_005_ABallMateHoldsThePointAndFreesTheTurns) {
    // Three rotations, no translation. Checked by what it REFUSES to let move
    // as much as by what it allows: a ball joint whose centre drifted would be
    // a ball joint that had come out of its socket.
    OcctGeometryKernel kernel;
    ScratchPart part{"ball.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 30});

    AssemblyDocument assembly{"Ball"};
    assembly.setGeometryKernel(&kernel);
    Instance& socket = assembly.addInstance("Socket", part.path);
    Instance& arm = assembly.addInstance("Arm", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(socket.id(), true));
    Mate& joint = assembly.addMate("Joint", MateType::Ball, socket.id(), "P", arm.id(), "P");

    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    EXPECT_NEAR(ConnectorGap(assembly, socket.id(), "P", arm.id(), "P"), 0.0, 1e-9);

    // Turned about X and about Y -- neither of which a revolute would allow.
    ASSERT_TRUE(assembly.setMateComponentValue(joint.id(), MateComponent::RX, 0.4));
    ASSERT_TRUE(assembly.setMateComponentValue(joint.id(), MateComponent::RY, -0.3));
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    EXPECT_NEAR(ConnectorGap(assembly, socket.id(), "P", arm.id(), "P"), 0.0, 1e-9)
        << "the ball joint came apart when it turned";
    // It really did turn: an untouched arm would still be at identity.
    EXPECT_LT(assembly.instanceWorldTransform(arm.id()).rotation.w, 0.999)
        << "a ball mate given two angles did not turn";

    // A translation is not its to give.
    EXPECT_FALSE(assembly.setMateComponentValue(joint.id(), MateComponent::TZ, 5.0))
        << "a ball mate accepted a translation";
}

TEST(OcctRecomputeIntegrationTest, M25_LIMIT_001_ALimitSTOPSAMotionRatherThanRefusingIt) {
    // Roadmap §22 is explicit: a drag past a limit stops at the limit rather
    // than erroring. But the stop is REPORTED, because a control that silently
    // ignores what it was told is a control that appears to be broken.
    OcctGeometryKernel kernel;
    ScratchPart part{"limited.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Limited"};
    assembly.setGeometryKernel(&kernel);
    Instance& base = assembly.addInstance("Base", part.path);
    Instance& arm = assembly.addInstance("Arm", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(base.id(), true));
    Mate& hinge = assembly.addMate("Hinge", MateType::Revolute, base.id(), "P", arm.id(), "P");

    ASSERT_TRUE(assembly.setMateLimit(hinge.id(), MateComponent::RZ, 0.0, 1.0));
    double became = 0.0;
    EXPECT_TRUE(assembly.setMateComponentValue(hinge.id(), MateComponent::RZ, 5.0, &became));
    EXPECT_NEAR(became, 1.0, 1e-12) << "the drive was not stopped at the limit";
    EXPECT_NEAR(assembly.findMate(hinge.id())->value(), 1.0, 1e-12);

    // The other end of the range, and a value INSIDE it, which must pass
    // through untouched -- a clamp that always clamped would be a lock.
    EXPECT_TRUE(assembly.setMateComponentValue(hinge.id(), MateComponent::RZ, -3.0, &became));
    EXPECT_NEAR(became, 0.0, 1e-12);
    EXPECT_TRUE(assembly.setMateComponentValue(hinge.id(), MateComponent::RZ, 0.5, &became));
    EXPECT_NEAR(became, 0.5, 1e-12);

    // A limit on a freedom the mate does not have is refused: a bound on
    // something that cannot move is a control with nothing behind it.
    EXPECT_FALSE(assembly.setMateLimit(hinge.id(), MateComponent::TX, 0.0, 1.0));
    // ...and one whose minimum is above its maximum can never be obeyed.
    EXPECT_FALSE(assembly.setMateLimit(hinge.id(), MateComponent::RZ, 2.0, 1.0));

    // Setting a limit around a value that is already outside it brings the
    // value in AT ONCE. A limit that only took effect on the next drive would
    // leave the model in a state its own rules forbid.
    ASSERT_TRUE(assembly.setMateComponentValue(hinge.id(), MateComponent::RZ, 0.9));
    ASSERT_TRUE(assembly.setMateLimit(hinge.id(), MateComponent::RZ, 0.0, 0.2));
    EXPECT_NEAR(assembly.findMate(hinge.id())->value(), 0.2, 1e-12);

    ASSERT_TRUE(assembly.clearMateLimit(hinge.id(), MateComponent::RZ));
    EXPECT_TRUE(assembly.setMateComponentValue(hinge.id(), MateComponent::RZ, 3.0, &became));
    EXPECT_NEAR(became, 3.0, 1e-12) << "a cleared limit still clamped";
}

TEST(OcctRecomputeIntegrationTest, M25_INTERFERE_001_TwoPartsInsideEachOtherAreREPORTED) {
    // Roadmap §23: interference is SEPARATE from mates, because a perfectly
    // legal set of mates can still drive two parts through each other. This is
    // exactly that case -- a fastened mate that puts one block inside another.
    OcctGeometryKernel kernel;
    ScratchPart part{"clash.ep3d"};
    WriteConnectedPart(part.path, 40.0, "Block", "P", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Clash"};
    assembly.setGeometryKernel(&kernel);
    Instance& first = assembly.addInstance("First", part.path);
    Instance& second = assembly.addInstance("Second", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(first.id(), true));
    // Both connectors at their parts' origins, fastened -- so the two blocks
    // occupy exactly the same space. Every mate is satisfied.
    assembly.addMate("Bolt", MateType::Fastened, first.id(), "P", second.id(), "P");
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);

    const AssemblyDocument::InterferenceReport report = assembly.checkInterference();
    ASSERT_TRUE(report.ok) << report.message;
    ASSERT_EQ(report.overlaps.size(), 1u) << "two coincident blocks did not interfere";
    EXPECT_NEAR(report.overlaps.front().volumeMm3, 40.0 * 40.0 * 40.0, 1e-6 * 64000.0)
        << "the overlap was measured as something other than the whole block";

    // MOVE THEM APART and it goes away -- which is what makes the number above
    // a measurement rather than a constant.
    ASSERT_TRUE(assembly.setInstanceGrounded(first.id(), true));
    ASSERT_TRUE(assembly.removeObject(assembly.findMateNamed("Bolt")->id()));
    Transform3D away;
    away.translation = Vec3{500, 0, 0};
    ASSERT_TRUE(assembly.setInstanceTransform(second.id(), away));
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    EXPECT_TRUE(assembly.checkInterference().overlaps.empty())
        << "two blocks half a metre apart were reported as interfering";
}

TEST(OcctRecomputeIntegrationTest, M25_INTERFERE_002_TouchingIsNotInterfering) {
    // Two faces resting on each other share a surface and no volume. Reporting
    // that as interference would make every assembly that actually fits report
    // a problem, which is the fastest way to make a check ignored.
    OcctGeometryKernel kernel;
    ScratchPart part{"touch.ep3d"};
    WriteConnectedPart(part.path, 40.0, "Block", "P", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Touch"};
    assembly.setGeometryKernel(&kernel);
    Instance& lower = assembly.addInstance("Lower", part.path);
    Instance& upper = assembly.addInstance("Upper", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(lower.id(), true));
    // The part is 40 tall from z=0, so stacking the second exactly 40 up puts
    // its underside on the first's top face.
    Transform3D stacked;
    stacked.translation = Vec3{0, 0, 40};
    ASSERT_TRUE(assembly.setInstanceTransform(upper.id(), stacked));
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);

    const AssemblyDocument::InterferenceReport report = assembly.checkInterference();
    ASSERT_TRUE(report.ok) << report.message;
    EXPECT_TRUE(report.overlaps.empty()) << "two parts resting on each other were called a clash";

    // ...and one millimetre INTO each other is a clash, so the line is where
    // it should be rather than merely somewhere.
    Transform3D sunk;
    sunk.translation = Vec3{0, 0, 39.0};
    ASSERT_TRUE(assembly.setInstanceTransform(upper.id(), sunk));
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    const AssemblyDocument::InterferenceReport clash = assembly.checkInterference();
    ASSERT_EQ(clash.overlaps.size(), 1u);
    EXPECT_NEAR(clash.overlaps.front().volumeMm3, 40.0 * 40.0 * 1.0, 1e-3);
}

TEST(OcctRecomputeIntegrationTest, M25_INTERFERE_003_AnUnbuiltInstanceIsNotReportedAsCLEAR) {
    // The one thing this must never say by accident. "No interference" and "I
    // could not look" are different sentences, and only one of them is safe to
    // act on.
    OcctGeometryKernel kernel;
    AssemblyDocument assembly{"Unbuilt"};
    assembly.setGeometryKernel(&kernel);
    assembly.addInstance("Ghost", "no-such-part.ep3d");

    const AssemblyDocument::InterferenceReport report = assembly.checkInterference();
    EXPECT_FALSE(report.ok);
    EXPECT_NE(report.message.find("not been built"), std::string::npos) << report.message;
    EXPECT_TRUE(report.overlaps.empty());
}

TEST(OcctRecomputeIntegrationTest, M25_MATE_006_APlanarMateFreesTheConnectorsOwnXY) {
    // WHICH plane, not merely how many freedoms. The count alone -- two
    // translations and a turn -- is satisfied by a mate that frees Y and Z
    // instead of X and Y, which would let a part sink through the surface it
    // is supposed to be lying on.
    OcctGeometryKernel kernel;
    ScratchPart part{"planar.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Planar"};
    assembly.setGeometryKernel(&kernel);
    Instance& table = assembly.addInstance("Table", part.path);
    Instance& puck = assembly.addInstance("Puck", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(table.id(), true));
    Mate& lying = assembly.addMate("Lying", MateType::Planar, table.id(), "P", puck.id(), "P");

    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    const KernelMassProperties home = MassOf(kernel, puck.currentShape());

    // SLIDING IN X AND Y IS ALLOWED, and moves the part by exactly that.
    ASSERT_TRUE(assembly.setMateComponentValue(lying.id(), MateComponent::TX, 40.0));
    ASSERT_TRUE(assembly.setMateComponentValue(lying.id(), MateComponent::TY, -25.0));
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    const KernelMassProperties slid = MassOf(kernel, puck.currentShape());
    EXPECT_NEAR(slid.centerOfMassMm.x - home.centerOfMassMm.x, 40.0, 1e-6);
    EXPECT_NEAR(slid.centerOfMassMm.y - home.centerOfMassMm.y, -25.0, 1e-6);
    // AND Z DID NOT MOVE, which is the half a count cannot see: the puck stays
    // on the table.
    EXPECT_NEAR(slid.centerOfMassMm.z, home.centerOfMassMm.z, 1e-9)
        << "a planar mate let the part leave its plane";

    // Lifting it off is not this mate's to give.
    EXPECT_FALSE(assembly.setMateComponentValue(lying.id(), MateComponent::TZ, 5.0))
        << "a planar mate accepted a translation out of its plane";
    // ...and neither is tipping it over.
    EXPECT_FALSE(assembly.setMateComponentValue(lying.id(), MateComponent::RX, 0.2));
    // Spinning in the plane IS.
    EXPECT_TRUE(assembly.setMateComponentValue(lying.id(), MateComponent::RZ, 0.5));
}

TEST(OcctRecomputeIntegrationTest, M25_LOOP_003_ALoopCloserRespectsWhatItsMateASKSFor) {
    // A loop-closing mate's residual is how far the follower is from where THE
    // MATE WANTS IT -- not from where the leader is. With every mate value at
    // zero the two are the same sentence, which is why every earlier loop test
    // passes either way.
    //
    // So this loop closes only at a NON-ZERO offset: a fastened ring of three
    // whose closing mate is a driven slider 25 mm along its axis. A residual
    // that compared against the bare relative transform would demand zero and
    // report the mechanism impossible.
    OcctGeometryKernel kernel;
    GaussNewtonAssemblySolver solver;
    ScratchPart part{"offsetloop.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 0});

    AssemblyDocument assembly{"OffsetLoop"};
    assembly.setGeometryKernel(&kernel);
    assembly.setAssemblySolver(&solver);
    Instance& a = assembly.addInstance("A", part.path);
    Instance& b = assembly.addInstance("B", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(a.id(), true));

    // One tree step puts B 25 mm along A's axis...
    Mate& tree = assembly.addMate("Tree", MateType::Slider, a.id(), "P", b.id(), "P", 25.0);
    ASSERT_TRUE(assembly.setMateDriven(tree.id(), true));
    // ...and a second mate between the SAME pair closes the loop, asking for
    // the same 25 mm. Consistent, and only because the mate's own value is
    // part of the question.
    Mate& closer = assembly.addMate("Closer", MateType::Slider, a.id(), "P", b.id(), "P", 25.0);
    ASSERT_TRUE(assembly.setMateDriven(closer.id(), true));

    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    const KernelMassProperties placed = MassOf(kernel, b.currentShape());
    EXPECT_NEAR(placed.centerOfMassMm.z, 10.0 + 25.0, 1e-6)
        << "the loop closed somewhere its mates did not ask for";

    // ...and asking the closing mate for something ELSE makes it impossible,
    // which is the evidence that its value was really being read.
    ASSERT_TRUE(assembly.setMateValue(closer.id(), 60.0));
    EXPECT_FALSE(assembly.recompute().success);
    EXPECT_NE(SolveMessage(assembly).find("does not close"), std::string::npos)
        << SolveMessage(assembly);
}

TEST(OcctRecomputeIntegrationTest, M25_LOOP_004_AMateWalkedBACKWARDSIsInverted) {
    // A mate can be reached from either end -- the ground decides which -- and
    // its middle transform is stated from the LEADING end. Walking it the
    // other way therefore means inverting it.
    //
    // Getting this wrong places a hinge at minus its angle whenever the chain
    // happens to run the other way, which is a defect that shows up in some
    // assemblies and not others. Here the mate is declared Arm -> Base and the
    // GROUND is Base, so the walk is guaranteed to run backwards.
    OcctGeometryKernel kernel;
    ScratchPart part{"backwards.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Backwards"};
    assembly.setGeometryKernel(&kernel);
    Instance& base = assembly.addInstance("Base", part.path);
    Instance& arm = assembly.addInstance("Arm", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(base.id(), true));
    // LEADING is Arm, FOLLOWING is Base -- the opposite of the direction the
    // solve will walk.
    assembly.addMate("Slide", MateType::Slider, arm.id(), "P", base.id(), "P", 30.0);
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);

    // The mate says "Base's connector is 30 mm along Arm's", so the arm has to
    // be 30 mm the OTHER way: at -30, not +30. Both are the same distance, so
    // a test that measured the gap would pass either way -- the SIGN is the
    // whole claim.
    const KernelMassProperties placed = MassOf(kernel, arm.currentShape());
    EXPECT_NEAR(placed.centerOfMassMm.z, 10.0 - 30.0, 1e-6)
        << "a mate walked from its following end was not inverted";
    EXPECT_NEAR(ConnectorGap(assembly, base.id(), "P", arm.id(), "P"), 30.0, 1e-9);
}

TEST(OcctRecomputeIntegrationTest, M25_INTERFERE_004_ACheckThatCouldNotRunIsNotACLEARANswer) {
    // "No interference" and "I could not compare them" are different
    // sentences. An empty overlap list from a check that failed reads exactly
    // like an empty one from a check that passed, so `ok` has to carry it.
    OcctGeometryKernel kernel;
    ScratchPart part{"clear.ep3d"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 0});

    AssemblyDocument assembly{"Clear"};
    assembly.setGeometryKernel(&kernel);
    Instance& a = assembly.addInstance("A", part.path);
    Instance& b = assembly.addInstance("B", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(a.id(), true));
    Transform3D away;
    away.translation = Vec3{500, 0, 0};
    ASSERT_TRUE(assembly.setInstanceTransform(b.id(), away));
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);

    const AssemblyDocument::InterferenceReport report = assembly.checkInterference();
    EXPECT_TRUE(report.overlaps.empty());
    // THE OTHER HALF: the check ran. Without this the same assertion passes on
    // a kernel that refused every comparison.
    EXPECT_TRUE(report.ok) << report.message;
    EXPECT_TRUE(report.message.empty()) << report.message;
}

TEST(OcctRecomputeIntegrationTest, M25_INTERFERE_005_TheBroadPhaseIsAFilterAndNotTheANSWER) {
    // Two round bars whose BOXES overlap and whose SOLIDS do not.
    //
    // Every other interference test has axis-aligned blocks, and for those the
    // boxes and the solids agree -- so they pass whether the precise phase
    // runs or not, and whether it reports "empty" as an answer or as an error.
    // Round corners are where the two stop agreeing, and that is the whole
    // reason roadmap §23 puts a precise stage after the cheap one.
    //
    // Radius 10 at the origin and at (18, 18): the centres are 25.5 mm apart
    // and the bars are 20 mm across, so they miss. Their boxes -- [-10,10] and
    // [8,28] on each axis -- overlap in a 2 mm corner.
    OcctGeometryKernel kernel;
    ScratchPart part{"round.ep3d"};
    {
        PartDocument source{"Round"};
        Sketch& sketch = source.addSketch("Circle");
        source.addSketchEntity(sketch.id(), SketchCircle{Vec2{0, 0}, 10.0});
        Parameter& tall = source.addParameter("H", 30.0, UnitType::Millimeter);
        Body& body = source.addBody("Bar");
        source.addPadFeature(body, "Pad1", sketch.id(), tall.id());
        ReferenceFrame& frame = source.addFrame("P frame");
        source.addConnector("P", ConnectorRole::Generic, frame.id());
        ASSERT_TRUE(savePartDocumentToFile(source, part.path));
    }

    AssemblyDocument assembly{"Round"};
    assembly.setGeometryKernel(&kernel);
    Instance& first = assembly.addInstance("First", part.path);
    Instance& second = assembly.addInstance("Second", part.path);
    ASSERT_TRUE(assembly.setInstanceGrounded(first.id(), true));
    Transform3D across;
    across.translation = Vec3{18.0, 18.0, 0.0};
    ASSERT_TRUE(assembly.setInstanceTransform(second.id(), across));
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);

    const AssemblyDocument::InterferenceReport report = assembly.checkInterference();
    EXPECT_TRUE(report.overlaps.empty()) << "two bars that miss were called a clash";
    // AND THE CHECK RAN. The broad phase let this pair through -- the boxes do
    // overlap -- so the precise phase was asked, and its empty answer has to
    // come back as "they do not touch" rather than as "I could not tell".
    EXPECT_TRUE(report.ok) << report.message;
    EXPECT_TRUE(report.message.empty()) << report.message;

    // ...and sliding them together IS a clash, so the line is where it should
    // be rather than merely somewhere.
    Transform3D closer;
    closer.translation = Vec3{12.0, 0.0, 0.0};
    ASSERT_TRUE(assembly.setInstanceTransform(second.id(), closer));
    ASSERT_TRUE(assembly.recompute().success) << SolveMessage(assembly);
    const AssemblyDocument::InterferenceReport clash = assembly.checkInterference();
    ASSERT_EQ(clash.overlaps.size(), 1u) << "two overlapping bars were not reported";
    EXPECT_GT(clash.overlaps.front().volumeMm3, 100.0);
}

// --- M26: a sub-assembly, against real geometry ------------------------------

namespace {

// A two-part assembly on disk: a block, and a second block fastened to it.
// Its total volume is the sum, and its own connector is at the origin.
void WriteSubAssembly(const std::string& path, const std::string& partPath, double side,
                      OcctGeometryKernel& kernel) {
    WriteConnectedPart(partPath, side, "Block", "P", Vec3{0, 0, 0});
    AssemblyDocument sub{"Sub"};
    sub.setGeometryKernel(&kernel);
    Instance& a = sub.addInstance("A", partPath);
    Instance& b = sub.addInstance("B", partPath);
    ASSERT_TRUE(sub.setInstanceGrounded(a.id(), true));
    Transform3D over;
    // Clear of each other, so the compound's volume is the plain sum and a
    // fuse would be visibly different.
    over.translation = Vec3{side * 2.0, 0, 0};
    ASSERT_TRUE(sub.setInstanceTransform(b.id(), over));
    ASSERT_TRUE(sub.recompute().success);
    ASSERT_TRUE(saveAssemblyDocumentToFile(sub, path));
}

} // namespace

TEST(OcctRecomputeIntegrationTest, M26_SUB_001_AnAssemblyCanBeInstancedLikeAPart) {
    // THE M26 STRUCTURAL CLAIM: `insert` does not care which kind of file it
    // was given. It reads the file's own documentType and asks the right
    // question, and there is ONE instance type either way -- because a mate
    // names an instance by id, and two kinds would mean every mate lookup,
    // rename, deletion and save had to ask which.
    OcctGeometryKernel kernel;
    ScratchPart part{"sub-part.ep3d"};
    ScratchPart subFile{"sub-assembly.ep3da"};
    WriteSubAssembly(subFile.path, part.path, 20.0, kernel);

    AssemblyDocument rig{"Rig"};
    rig.setGeometryKernel(&kernel);
    Instance& nested = rig.addInstance("Nested", subFile.path);
    ASSERT_TRUE(rig.setInstanceGrounded(nested.id(), true));
    ASSERT_TRUE(rig.recompute().success) << SolveMessage(rig);

    EXPECT_TRUE(nested.isSubAssembly()) << "the file's own type was not read";
    // BOTH BLOCKS, and no material invented between them: a compound, not a
    // fuse. Two 20 mm cubes are 16000 mm^3.
    const KernelMassProperties whole = MassOf(kernel, nested.currentShape());
    EXPECT_NEAR(whole.volumeMm3, 2.0 * 8000.0, 1e-6 * 16000.0);
    // The block is drawn CENTRED on its sketch origin, so one sits at x=0 and
    // the other 40 mm along; the pair's centroid is midway between them.
    EXPECT_NEAR(whole.centerOfMassMm.x, 20.0, 1e-6);
}

TEST(OcctRecomputeIntegrationTest, M26_SUB_002_MovingTheSubAssemblyMovesEverythingINSIDEIt) {
    // The reason M23 made a placement a FRAME rather than a Transform3D, cashed
    // in: the frame hierarchy already composes, so nothing here had to be told
    // about the parts inside.
    OcctGeometryKernel kernel;
    ScratchPart part{"sub-move-part.ep3d"};
    ScratchPart subFile{"sub-move.ep3da"};
    WriteSubAssembly(subFile.path, part.path, 20.0, kernel);

    AssemblyDocument rig{"Rig"};
    rig.setGeometryKernel(&kernel);
    Instance& nested = rig.addInstance("Nested", subFile.path);
    ASSERT_TRUE(rig.setInstanceGrounded(nested.id(), true));
    ASSERT_TRUE(rig.recompute().success) << SolveMessage(rig);
    const KernelMassProperties home = MassOf(kernel, nested.currentShape());

    Transform3D across;
    across.translation = Vec3{0, 200, 0};
    ASSERT_TRUE(rig.setInstanceTransform(nested.id(), across));
    ASSERT_TRUE(rig.recompute().success) << SolveMessage(rig);
    const KernelMassProperties moved = MassOf(kernel, nested.currentShape());

    EXPECT_NEAR(moved.centerOfMassMm.y - home.centerOfMassMm.y, 200.0, 1e-6);
    EXPECT_NEAR(moved.centerOfMassMm.x, home.centerOfMassMm.x, 1e-6);
    EXPECT_NEAR(moved.volumeMm3, home.volumeMm3, 1e-6 * home.volumeMm3)
        << "moving the sub-assembly changed what is in it";
}

TEST(OcctRecomputeIntegrationTest, M26_SUB_003_AnAssemblyThatCONTAINSITSELFIsRefused) {
    // Running off the end of the stack is not a failure a message can be
    // attached to. The chain of files open above an instance is carried in the
    // recompute context, and a source already in it is refused by name.
    OcctGeometryKernel kernel;
    ScratchPart part{"cycle-part.ep3d"};
    ScratchPart subFile{"cycle.ep3da"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 0});

    // An assembly that instances ITSELF. Written by hand, because no facade
    // would let this be built -- the file it names does not exist yet when the
    // instance is added.
    {
        AssemblyDocument sub{"Sub"};
        sub.setGeometryKernel(&kernel);
        Instance& block = sub.addInstance("Block", part.path);
        sub.addInstance("Me", subFile.path);
        ASSERT_TRUE(sub.setInstanceGrounded(block.id(), true));
        ASSERT_TRUE(saveAssemblyDocumentToFile(sub, subFile.path));
    }

    AssemblyLoadResult loaded = loadAssemblyDocumentFromFile(subFile.path);
    ASSERT_TRUE(loaded) << loaded.message;
    loaded.document->setGeometryKernel(&kernel);
    const DocumentRecomputeReport report = loaded.document->recompute();
    EXPECT_FALSE(report.success);
    bool named = false;
    for (const RecomputeItemReport& item : report.items)
        if (item.message.find("contains itself") != std::string::npos) named = true;
    EXPECT_TRUE(named) << "an assembly containing itself was not refused by name";
}

TEST(OcctRecomputeIntegrationTest, M26_SUB_004_ASubAssemblyOffersITSOwnConnectorsToMateBy) {
    // Roadmap §21's reuse rule, one level up: what the level above mates to is
    // the SUB-ASSEMBLY's own connectors, not the ones its parts brought in.
    // Those belong to the parts and are already spoken for by the mates inside.
    //
    // It falls out for free because connectors live on DocumentBase -- an
    // assembly has them for the same reason a part does (ADR-M23-001).
    OcctGeometryKernel kernel;
    ScratchPart part{"sub-conn-part.ep3d"};
    ScratchPart subFile{"sub-conn.ep3da"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 0});
    {
        AssemblyDocument sub{"Sub"};
        sub.setGeometryKernel(&kernel);
        Instance& a = sub.addInstance("A", part.path);
        ASSERT_TRUE(sub.setInstanceGrounded(a.id(), true));
        // The assembly's OWN connector, 50 mm up.
        ReferenceFrame& frame = sub.addFrame("Mount frame");
        Transform3D up;
        up.translation = Vec3{0, 0, 50};
        sub.setFrameTransform(frame.id(), up);
        sub.addConnector("Mount", ConnectorRole::Mount, frame.id(), ConnectorOwner::Assembly);
        ASSERT_TRUE(sub.recompute().success);
        ASSERT_TRUE(saveAssemblyDocumentToFile(sub, subFile.path));
    }

    AssemblyDocument rig{"Rig"};
    rig.setGeometryKernel(&kernel);
    Instance& base = rig.addInstance("Base", part.path);
    Instance& nested = rig.addInstance("Nested", subFile.path);
    ASSERT_TRUE(rig.setInstanceGrounded(base.id(), true));
    // Mating to the SUB-ASSEMBLY's connector by name.
    rig.addMate("Bolt", MateType::Fastened, base.id(), "P", nested.id(), "Mount");
    ASSERT_TRUE(rig.recompute().success) << SolveMessage(rig);

    EXPECT_NEAR(ConnectorGap(rig, base.id(), "P", nested.id(), "Mount"), 0.0, 1e-9);
    // The sub-assembly hangs 50 mm BELOW the base's connector, because its own
    // Mount is 50 mm above its contents.
    const KernelMassProperties placed = MassOf(kernel, nested.currentShape());
    EXPECT_NEAR(placed.centerOfMassMm.z, 10.0 - 50.0, 1e-6);

    // ...and the parts' connectors are NOT offered up: "P" is a name inside
    // the sub-assembly, and a mate to it from out here has to fail rather than
    // reach in.
    //
    // A FRESH rig, because adding a second mate to the same pair would be a
    // closed loop and the message would be about that instead -- which would
    // make this assertion pass for the wrong reason.
    AssemblyDocument reaching{"Reaching"};
    reaching.setGeometryKernel(&kernel);
    Instance& other = reaching.addInstance("Base", part.path);
    Instance& inner = reaching.addInstance("Nested", subFile.path);
    ASSERT_TRUE(reaching.setInstanceGrounded(other.id(), true));
    reaching.addMate("Reach", MateType::Fastened, other.id(), "P", inner.id(), "P");
    EXPECT_FALSE(reaching.recompute().success);
    EXPECT_NE(SolveMessage(reaching).find("no mate connector called 'P'"), std::string::npos)
        << SolveMessage(reaching);
}

TEST(OcctRecomputeIntegrationTest, M26_SUB_005_ACompoundIsNotAFUSE) {
    // Two parts that TOUCH. A fuse would make them one solid whose volume is
    // the union -- less than the sum wherever they overlap, and a single solid
    // where the assembly says there are two. `compoundOf` joins nothing.
    OcctGeometryKernel kernel;
    ScratchPart part{"touching-part.ep3d"};
    ScratchPart subFile{"touching.ep3da"};
    WriteConnectedPart(part.path, 20.0, "Block", "P", Vec3{0, 0, 0});
    {
        AssemblyDocument sub{"Sub"};
        sub.setGeometryKernel(&kernel);
        Instance& a = sub.addInstance("A", part.path);
        Instance& b = sub.addInstance("B", part.path);
        ASSERT_TRUE(sub.setInstanceGrounded(a.id(), true));
        // EXACTLY touching: the part is 20 tall from z=0, so 20 up rests the
        // second on the first.
        Transform3D stacked;
        stacked.translation = Vec3{0, 0, 20};
        ASSERT_TRUE(sub.setInstanceTransform(b.id(), stacked));
        ASSERT_TRUE(sub.recompute().success);
        ASSERT_TRUE(saveAssemblyDocumentToFile(sub, subFile.path));
    }

    AssemblyDocument rig{"Rig"};
    rig.setGeometryKernel(&kernel);
    Instance& nested = rig.addInstance("Nested", subFile.path);
    ASSERT_TRUE(rig.setInstanceGrounded(nested.id(), true));
    ASSERT_TRUE(rig.recompute().success) << SolveMessage(rig);

    // THE PLAIN SUM. A fuse of two touching blocks gives the same volume here
    // -- so the claim that separates them is the SHAPE COUNT, which a fused
    // pair would have as one.
    const KernelMassProperties whole = MassOf(kernel, nested.currentShape());
    EXPECT_NEAR(whole.volumeMm3, 2.0 * 8000.0, 1e-6 * 16000.0);
    // Two solids in there, not one.
    EXPECT_EQ(kernel.countSolids(nested.currentShape()), 2)
        << "the sub-assembly's parts were fused into one solid";
}
