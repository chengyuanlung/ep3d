#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
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
    KernelMassPropertiesResult calculateMassProperties(const KernelShape& shape) override {
        ++calculateMassPropertiesCallCount;
        return inner_.calculateMassProperties(shape);
    }

    int createBoxCallCount = 0;
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
