#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Feature/SweepFeature.h"
#include "Core/Feature/LoftFeature.h"
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
