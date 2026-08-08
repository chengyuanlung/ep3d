#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Fakes/FakeGeometryKernel.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using namespace paramcad;

// Sets up a document with Width/Height/Depth parameters, a Material, and a
// FakeGeometryKernel injected -- the Core-only harness the M3 architect
// contract prescribes for exercising BoxFeature/MassPropertiesNode wiring
// without linking OCCT.
struct BoxFeatureFixture {
    PartDocument document{"BoxDoc"};
    FakeGeometryKernel kernel;
    Parameter* width = nullptr;
    Parameter* height = nullptr;
    Parameter* depth = nullptr;
    Body* body = nullptr;
    BoxFeature* box = nullptr;

    BoxFeatureFixture(double w = 100.0, double h = 50.0, double d = 20.0) {
        document.setGeometryKernel(&kernel);
        width = &document.addParameter("Width", w, UnitType::Millimeter);
        height = &document.addParameter("Height", h, UnitType::Millimeter);
        depth = &document.addParameter("Depth", d, UnitType::Millimeter);
        body = &document.addBody("Body001");
        box = &document.addBoxFeature(*body, "Box001", width->id(), height->id(), depth->id());
    }
};

TEST(BoxFeatureTest, M3_ARCH_001_RequiredDependencyGraphShape) {
    // Spec 11: Width/Height/Depth -> BoxFeature -> MassPropertiesNode.
    BoxFeatureFixture fx;
    const DependencyGraph& graph = fx.document.dependencyGraph();

    EXPECT_EQ(graph.dependentsOf(fx.width->id()), std::vector<ObjectId>{fx.box->id()});
    EXPECT_EQ(graph.dependentsOf(fx.height->id()), std::vector<ObjectId>{fx.box->id()});
    EXPECT_EQ(graph.dependentsOf(fx.depth->id()), std::vector<ObjectId>{fx.box->id()});

    const auto boxPrereqs = graph.prerequisitesOf(fx.box->id());
    EXPECT_TRUE(std::find(boxPrereqs.begin(), boxPrereqs.end(), fx.width->id()) != boxPrereqs.end());
    EXPECT_TRUE(std::find(boxPrereqs.begin(), boxPrereqs.end(), fx.height->id()) != boxPrereqs.end());
    EXPECT_TRUE(std::find(boxPrereqs.begin(), boxPrereqs.end(), fx.depth->id()) != boxPrereqs.end());

    // BoxFeature has exactly one dependent: the (singleton) MassPropertiesNode.
    const auto boxDependents = graph.dependentsOf(fx.box->id());
    ASSERT_EQ(boxDependents.size(), 1u);
    const ObjectId massNodeId = boxDependents[0];
    EXPECT_NE(massNodeId, fx.box->id());

    // No material assigned in this fixture: MassPropertiesNode has exactly
    // one prerequisite (the box).
    EXPECT_EQ(graph.prerequisitesOf(massNodeId), std::vector<ObjectId>{fx.box->id()});
}

TEST(BoxFeatureTest, M3_ARCH_002_MaterialWiresDirectEdgeToMassPropertiesNode) {
    PartDocument document("Doc");
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    Material& material = document.addMaterial("Aluminum", 2700.0);
    Parameter& width = document.addParameter("Width", 10.0, UnitType::Millimeter);
    Parameter& height = document.addParameter("Height", 10.0, UnitType::Millimeter);
    Parameter& depth = document.addParameter("Depth", 10.0, UnitType::Millimeter);
    Body& body = document.addBody("Body001");
    BoxFeature& box =
        document.addBoxFeature(body, "Box001", width.id(), height.id(), depth.id());

    const DependencyGraph& graph = document.dependencyGraph();
    const auto boxDependents = graph.dependentsOf(box.id());
    ASSERT_EQ(boxDependents.size(), 1u);
    const ObjectId massNodeId = boxDependents[0];

    // Material -> MassPropertiesNode is a DIRECT edge (spec 11 diagram),
    // separate from the BoxFeature -> MassPropertiesNode edge.
    EXPECT_EQ(graph.dependentsOf(material.id()), std::vector<ObjectId>{massNodeId});
    const auto massPrereqs = graph.prerequisitesOf(massNodeId);
    EXPECT_EQ(massPrereqs.size(), 2u);
    EXPECT_TRUE(std::find(massPrereqs.begin(), massPrereqs.end(), box.id()) != massPrereqs.end());
    EXPECT_TRUE(std::find(massPrereqs.begin(), massPrereqs.end(), material.id()) !=
                massPrereqs.end());
}

TEST(BoxFeatureTest, M3_ARCH_003_BoxFeatureDoesNotInstantiateConcreteKernel) {
    // Architectural property, not a runtime one: BoxFeature only ever reads
    // context.kernel (an IGeometryKernel*), never names a concrete kernel
    // type. Exercised here by proving recompute behaves purely through the
    // injected interface -- swap the injected kernel and behavior changes,
    // which would be impossible if BoxFeature hard-coded a concrete type.
    PartDocument document("Doc");
    Parameter& width = document.addParameter("Width", 10.0, UnitType::Millimeter);
    Parameter& height = document.addParameter("Height", 10.0, UnitType::Millimeter);
    Parameter& depth = document.addParameter("Depth", 10.0, UnitType::Millimeter);
    Body& body = document.addBody("Body001");
    BoxFeature& box =
        document.addBoxFeature(body, "Box001", width.id(), height.id(), depth.id());

    // No kernel injected: a normal, controlled failure path (ADR-M3-003).
    const DocumentRecomputeReport noKernel = document.recompute();
    EXPECT_FALSE(noKernel.success);
    EXPECT_EQ(box.state(), ComputeState::Failed);

    // Inject a kernel afterward and retry: same BoxFeature code, different
    // injected kernel, now succeeds.
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    const DocumentRecomputeReport withKernel = document.recomputeFrom(box.id());
    EXPECT_TRUE(withKernel.success);
    EXPECT_EQ(box.state(), ComputeState::Valid);
    EXPECT_TRUE(box.currentShape().isValid());
}

TEST(BoxFeatureTest, M3_FAKE_001_ValidBoxRecomputeSucceeds) {
    BoxFeatureFixture fx;
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_TRUE(report.success);
    EXPECT_EQ(fx.box->state(), ComputeState::Valid);
    EXPECT_TRUE(fx.box->currentShape().isValid());
    EXPECT_EQ(fx.kernel.createBoxCallCount, 1);
}

TEST(BoxFeatureTest, M3_FAKE_002_DimensionChangeRecomputesBoxAndMass) {
    // spec 11: Width/Height/Depth change -> BoxFeature + MassPropertiesNode
    // recompute; unrelated parameter recomputes neither.
    BoxFeatureFixture fx;
    Parameter& unrelated = fx.document.addParameter("Unrelated", 1.0, UnitType::Unitless);
    ASSERT_TRUE(fx.document.recompute().success);
    const int createBoxAfterFirst = fx.kernel.createBoxCallCount;
    const int massAfterFirst = fx.kernel.calculateMassPropertiesCallCount;

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_TRUE(report.success);
    EXPECT_EQ(fx.kernel.createBoxCallCount, createBoxAfterFirst + 1);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massAfterFirst + 1);

    // Unrelated parameter change: neither BoxFeature nor MassPropertiesNode
    // recompute.
    ASSERT_TRUE(fx.document.setParameterValue(unrelated.id(), 2.0));
    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.kernel.createBoxCallCount, createBoxAfterFirst + 1);
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, massAfterFirst + 1);
}

TEST(BoxFeatureTest, M3_FAKE_003_DensityOnlyChangeDoesNotRebuildGeometry) {
    PartDocument document("Doc");
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    Material& material = document.addMaterial("Aluminum", 2700.0);
    Parameter& width = document.addParameter("Width", 100.0, UnitType::Millimeter);
    Parameter& height = document.addParameter("Height", 50.0, UnitType::Millimeter);
    Parameter& depth = document.addParameter("Depth", 20.0, UnitType::Millimeter);
    Body& body = document.addBody("Body001");
    BoxFeature& box =
        document.addBoxFeature(body, "Box001", width.id(), height.id(), depth.id());
    ASSERT_TRUE(document.recompute().success);
    const int createBoxAfterFirst = kernel.createBoxCallCount;
    const int massAfterFirst = kernel.calculateMassPropertiesCallCount;

    ASSERT_TRUE(document.setMaterialDensity(7850.0));
    const DocumentRecomputeReport report = document.recompute();
    EXPECT_TRUE(report.success);
    // BoxFeature does NOT recompute (geometry untouched by density).
    EXPECT_EQ(kernel.createBoxCallCount, createBoxAfterFirst);
    // MassPropertiesNode DOES recompute.
    EXPECT_EQ(kernel.calculateMassPropertiesCallCount, massAfterFirst + 1);
    EXPECT_EQ(box.state(), ComputeState::Valid);
}

TEST(BoxFeatureTest, M3_FAKE_004_TransactionalShapeRetentionOnFailure) {
    // ADR-M3-001/004: a failed build leaves currentShape_ byte-for-byte
    // unchanged; staleness is carried entirely by ComputeState.
    BoxFeatureFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const IShapeHandle* validHandle = fx.box->currentShape().handle();
    ASSERT_NE(validHandle, nullptr);

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 0.0)); // invalid: zero
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(fx.box->state(), ComputeState::Failed);
    // The shape handle is untouched -- same pointer, still "valid" in the
    // sense of holding data, but staleness must be read from state(), never
    // inferred from currentShape().
    EXPECT_EQ(fx.box->currentShape().handle(), validHandle);
    EXPECT_TRUE(fx.box->currentShape().isValid());
}

// --- Invalid geometry (spec 13) ---------------------------------------------

TEST(BoxFeatureTest, M3_FAKE_005_ZeroDimensionFails) {
    BoxFeatureFixture fx(0.0, 50.0, 20.0);
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(fx.box->state(), ComputeState::Failed);
}

TEST(BoxFeatureTest, M3_FAKE_006_NegativeDimensionFails) {
    BoxFeatureFixture fx(-5.0, 50.0, 20.0);
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(fx.box->state(), ComputeState::Failed);
}

TEST(BoxFeatureTest, M3_FAKE_007_NaNDimensionFails) {
    BoxFeatureFixture fx(std::numeric_limits<double>::quiet_NaN(), 50.0, 20.0);
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(fx.box->state(), ComputeState::Failed);
}

TEST(BoxFeatureTest, M3_FAKE_008_InfiniteDimensionFails) {
    BoxFeatureFixture fx(std::numeric_limits<double>::infinity(), 50.0, 20.0);
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(fx.box->state(), ComputeState::Failed);
}

TEST(BoxFeatureTest, M3_FAKE_009_DownstreamCannotReportSuccessAfterGeometryFailure) {
    BoxFeatureFixture fx(0.0, 50.0, 20.0);
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
    bool foundMassItem = false;
    // MassPropertiesNode must be present as Failed or BlockedByDependency,
    // never Success, and its callback must never have run.
    EXPECT_EQ(fx.kernel.calculateMassPropertiesCallCount, 0);
    for (const auto& item : report.items) {
        if (item.status == RecomputeStatus::BlockedByDependency ||
            (item.status == RecomputeStatus::Failed && item.id != fx.box->id())) {
            foundMassItem = true;
        }
    }
    EXPECT_TRUE(foundMassItem);
    EXPECT_FALSE(fx.document.massProperties().valid); // never computed at all
}

TEST(BoxFeatureTest, M3_FAKE_010_RecoveryAfterInvalidDimension) {
    BoxFeatureFixture fx(0.0, 50.0, 20.0);
    ASSERT_FALSE(fx.document.recompute().success);
    ASSERT_EQ(fx.box->state(), ComputeState::Failed);

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 80.0));
    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_TRUE(report.success);
    EXPECT_EQ(fx.box->state(), ComputeState::Valid);
    EXPECT_TRUE(fx.document.massProperties().valid);
    EXPECT_DOUBLE_EQ(fx.document.massProperties().volumeMm3, 80.0 * 50.0 * 20.0);
}

TEST(BoxFeatureTest, M3_FAKE_011_DeleteReferencedParameterFailsCleanlyNoCrash) {
    // Adversarial reviewer check (spec 23): deleting a Parameter a BoxFeature
    // still references must never dereference anything unsafely. removeObject
    // dirties the box (its input set changed, ADR-007); BoxFeature keeps the
    // now-stale widthParameterId, so the NEXT recompute resolves it through
    // the registry, finds nothing, and fails in a controlled way.
    BoxFeatureFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_EQ(fx.box->state(), ComputeState::Valid);
    const ObjectId widthId = fx.width->id();

    ASSERT_TRUE(fx.document.removeObject(widthId));
    EXPECT_FALSE(fx.document.objectRegistry().contains(widthId));
    EXPECT_EQ(fx.box->widthParameterId(), widthId); // BoxFeature still holds the stale id

    const DocumentRecomputeReport report = fx.document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(fx.box->state(), ComputeState::Failed);
    const RecomputeItemReport* boxItem = nullptr;
    for (const auto& item : report.items)
        if (item.id == fx.box->id()) boxItem = &item;
    ASSERT_NE(boxItem, nullptr);
    EXPECT_EQ(boxItem->status, RecomputeStatus::Failed);
    EXPECT_FALSE(boxItem->message.empty()); // diagnostic exists, no crash occurred to reach here
}

} // namespace
