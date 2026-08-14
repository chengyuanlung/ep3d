// Regression tests for the five Major findings raised by the M3 independent
// review (docs/reviews/M3_IndependentReview.md). Each test here failed against
// the reviewed code and passes against the fix; without them the fixes could
// silently regress, since every one of these defects was invisible to the
// existing suite.
//
// Core-only (FakeGeometryKernel, no OCCT link) because every finding is about
// Core document/serializer semantics rather than about geometry itself.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/PlaceholderFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Fakes/FakeGeometryKernel.h"
#include <gtest/gtest.h>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

struct BoxDocFixture {
    PartDocument document{"ReviewDoc"};
    FakeGeometryKernel kernel;
    Material* material = nullptr;
    Parameter* width = nullptr;
    Parameter* height = nullptr;
    Parameter* depth = nullptr;
    BoxFeature* box = nullptr;

    BoxDocFixture(double w = 100.0, double h = 50.0, double d = 20.0, double density = 2700.0) {
        document.setGeometryKernel(&kernel);
        material = &document.addMaterial("Mat", density);
        width = &document.addParameter("Width", w, UnitType::Millimeter);
        height = &document.addParameter("Height", h, UnitType::Millimeter);
        depth = &document.addParameter("Depth", d, UnitType::Millimeter);
        Body& body = document.addBody("Body001");
        box = &document.addBoxFeature(body, "Box001", width->id(), height->id(), depth->id());
    }
};

// --- Major 1: downstream results must not falsely report success -----------
// Spec 2 Definition of Done ("downstream current results do not falsely
// succeed"), spec 13, spec 19-C. The pre-existing coverage only checked a
// document whose mass properties had NEVER been computed, so it could not
// catch a `valid` flag that is set once and then never cleared.

TEST(M3ReviewFindingTest, MAJOR1_GeometryFailureClearsMassPropertiesCurrency) {
    BoxDocFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.massProperties().valid);
    const double volumeBefore = fx.document.massProperties().volumeMm3;

    // Invalid width -> BoxFeature fails -> the graph BLOCKS the mass node, so
    // it is never invoked. Nothing inside the node can clear the flag; only
    // the document-level pass can.
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 0.0));
    EXPECT_FALSE(fx.document.recompute().success);

    EXPECT_FALSE(fx.document.massProperties().valid)
        << "stale mass properties still report themselves as current";
    // Retention is still the policy (ADR-M3-004): the numbers survive, only
    // their claim to being current is withdrawn.
    EXPECT_DOUBLE_EQ(fx.document.massProperties().volumeMm3, volumeBefore);
}

TEST(M3ReviewFindingTest, MAJOR1_RecoveryRestoresMassPropertiesCurrency) {
    BoxDocFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 0.0));
    ASSERT_FALSE(fx.document.recompute().success);
    ASSERT_FALSE(fx.document.massProperties().valid);

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 80.0));
    EXPECT_TRUE(fx.document.recompute().success);
    EXPECT_TRUE(fx.document.massProperties().valid);
    EXPECT_DOUBLE_EQ(fx.document.massProperties().volumeMm3, 80.0 * 50.0 * 20.0);
}

TEST(M3ReviewFindingTest, MAJOR1_InvalidDensityClearsMassPropertiesCurrency) {
    BoxDocFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.massProperties().valid);

    // Negative density fails by documented policy (ADR-M3-004); geometry is
    // untouched, so this exercises the node's own failure path rather than the
    // blocked path above.
    ASSERT_TRUE(fx.document.setMaterialDensity(-1.0));
    EXPECT_FALSE(fx.document.recompute().success);
    EXPECT_FALSE(fx.document.massProperties().valid);
}

TEST(M3ReviewFindingTest, MAJOR1_EditClearsCurrencyImmediatelyNotOnlyAtRecompute) {
    // Raised as a residual on re-review: currency was cleared at recompute time
    // but not at edit time, so between the two the document advertised the old
    // numbers as current while the feature already read Dirty. Both staleness
    // signals must flip at the same moment (ADR-M3-006).
    BoxDocFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.massProperties().valid);

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    EXPECT_FALSE(fx.document.massProperties().valid)
        << "mass properties still current after an edit that superseded them";
    EXPECT_NE(fx.box->state(), ComputeState::Valid); // the two signals agree

    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_TRUE(fx.document.massProperties().valid);
}

// --- Major 2: Feature::state() must not claim a validity it cannot have ----

TEST(M3ReviewFindingTest, MAJOR2_RestoredFeatureIsNotValidBeforeRecompute) {
    BoxDocFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_EQ(fx.box->state(), ComputeState::Valid);

    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(fx.document, out));
    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;

    ASSERT_EQ(loaded.document->bodies().size(), 1u);
    ASSERT_EQ(loaded.document->bodies().front()->features().size(), 1u);
    const Feature& restored = *loaded.document->bodies().front()->features().front();

    // ComputeState is persisted; KernelShape deliberately is not
    // (ADR-M3-005). A restored feature therefore holds no runtime shape and
    // must not claim to be Valid.
    EXPECT_NE(restored.state(), ComputeState::Valid)
        << "restored feature claims validity while holding no runtime shape";
}

TEST(M3ReviewFindingTest, MAJOR2_ParameterEditInvalidatesCachedFeatureState) {
    BoxDocFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_EQ(fx.box->state(), ComputeState::Valid);

    // The graph dirties the feature by propagation from Width. The cached
    // Feature::state() must follow, or it advertises superseded geometry as
    // current.
    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    EXPECT_NE(fx.box->state(), ComputeState::Valid);

    ASSERT_TRUE(fx.document.recompute().success);
    EXPECT_EQ(fx.box->state(), ComputeState::Valid);
}

// --- Major 3: removeObject must actually remove a Body-owned feature -------

TEST(M3ReviewFindingTest, MAJOR3_RemovingBoxFeatureDetachesItFromItsBody) {
    BoxDocFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    const ObjectId boxId = fx.box->id();
    ASSERT_NE(boxId, kInvalidObjectId);

    ASSERT_TRUE(fx.document.removeObject(boxId));

    ASSERT_EQ(fx.document.bodies().size(), 1u);
    EXPECT_TRUE(fx.document.bodies().front()->features().empty())
        << "removeObject reported success but the body still owns the feature";
    EXPECT_EQ(fx.document.objectRegistry().find(boxId), nullptr);
}

TEST(M3ReviewFindingTest, MAJOR3_RemovedBoxFeatureDoesNotBreakLaterRecomputes) {
    BoxDocFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.removeObject(fx.box->id()));

    // Previously every later pass failed forever with "no box feature
    // configured", because the mass node kept pointing at the removed source.
    EXPECT_TRUE(fx.document.recompute().success);
    EXPECT_FALSE(fx.document.massProperties().valid);
}

TEST(M3ReviewFindingTest, MAJOR3_RemovedBoxFeatureIsNotSerialized) {
    BoxDocFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.removeObject(fx.box->id()));

    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(fx.document, out));
    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->bodies().size(), 1u);
    EXPECT_TRUE(loaded.document->bodies().front()->features().empty())
        << "a removed feature came back on load";
}

// --- Major 4: save/load symmetry -------------------------------------------
// Anything savePartDocument accepts must be loadable. Removing a Parameter a
// BoxFeature still references used to produce a file that saved cleanly and
// could never be loaded again.

TEST(M3ReviewFindingTest, MAJOR4_SaveRejectsDanglingParameterReference) {
    BoxDocFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    ASSERT_TRUE(fx.document.removeObject(fx.width->id()));

    std::ostringstream out;
    const SaveResult saved = savePartDocument(fx.document, out);
    EXPECT_FALSE(saved) << "saved a document that can never be loaded back";
    EXPECT_EQ(saved.error, SerializationError::UnknownDependencyId);
}

TEST(M3ReviewFindingTest, MAJOR4_IntactDocumentStillSaves) {
    // Guard against the validation above rejecting healthy documents.
    BoxDocFixture fx;
    ASSERT_TRUE(fx.document.recompute().success);
    std::ostringstream out;
    EXPECT_TRUE(savePartDocument(fx.document, out));
}

// --- Critical 1: features without a graph node ----------------------------
// Raised by the M3 re-review against the fix for Major 2. Only BoxFeature ever
// gets a graph node; DependencyGraph::state() asserts on an unknown id. Any
// feature cached as Valid without a graph node therefore aborted the process in
// Debug and was silently demoted in Release. PlaceholderFeature is exactly that
// case, and it is the type every unrecognized persisted feature loads back as.

TEST(M3ReviewFindingTest, CRITICAL1_RecomputeWithValidPlaceholderFeatureDoesNotAbort) {
    PartDocument document{"MixedDoc"};
    Body& body = document.addBody("Body001");
    PlaceholderFeature& pad = body.addFeature<PlaceholderFeature>("Pad001", "Loft");
    ASSERT_TRUE(pad.recompute()); // -> Valid, but never joins the graph
    ASSERT_EQ(pad.state(), ComputeState::Valid);

    EXPECT_TRUE(document.recompute().success);
    // A feature outside the graph is outside the sync's remit: its state is
    // owned by whoever drives it, and must not be rewritten here.
    EXPECT_EQ(pad.state(), ComputeState::Valid);
}

TEST(M3ReviewFindingTest, CRITICAL1_ParameterEditWithValidPlaceholderFeatureDoesNotAbort) {
    BoxDocFixture fx;
    Body& body = *fx.document.bodies().front();
    PlaceholderFeature& pad = body.addFeature<PlaceholderFeature>("Pad001", "Loft");
    ASSERT_TRUE(pad.recompute());
    ASSERT_EQ(pad.state(), ComputeState::Valid);

    ASSERT_TRUE(fx.document.setParameterValue(fx.width->id(), 120.0));
    EXPECT_EQ(pad.state(), ComputeState::Valid);
    EXPECT_TRUE(fx.document.recompute().success);
}

TEST(M3ReviewFindingTest, CRITICAL1_LoadingDocumentWithValidPlaceholderPreservesItsState) {
    // Round-tripping a Valid PlaceholderFeature must be lossless: its state is
    // persisted, it has no graph node, and nothing may rewrite it on load. This
    // crashed inside loadPartDocument itself for a document that also held a
    // BoxFeature, because restoreBoxFeature runs the sync over every feature.
    BoxDocFixture fx;
    Body& body = *fx.document.bodies().front();
    PlaceholderFeature& pad = body.addFeature<PlaceholderFeature>("Pad001", "Loft");
    ASSERT_TRUE(pad.recompute());
    ASSERT_TRUE(fx.document.recompute().success);

    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(fx.document, out));
    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;

    const Body& restoredBody = *loaded.document->bodies().front();
    ASSERT_EQ(restoredBody.features().size(), 2u);
    const Feature* restoredPad = nullptr;
    for (const auto& feature : restoredBody.features())
        if (feature->typeName() == "Loft") restoredPad = feature.get();
    ASSERT_NE(restoredPad, nullptr);
    EXPECT_EQ(restoredPad->state(), ComputeState::Valid)
        << "a persisted feature state was silently rewritten on load";

    // Recomputing a restored mixed document must not abort, and must apply the
    // sync only to the graph-scheduled feature: the Box recomputes to Valid,
    // the graph-less placeholder keeps the state it was restored with.
    FakeGeometryKernel loadedKernel;
    loaded.document->setGeometryKernel(&loadedKernel);
    EXPECT_TRUE(loaded.document->recompute().success);
    EXPECT_EQ(restoredPad->state(), ComputeState::Valid);
    for (const auto& feature : restoredBody.features())
        if (feature->typeName() == "Box") EXPECT_EQ(feature->state(), ComputeState::Valid);
}

// --- Minor: dimension range (spec 23 adversarial checks) -------------------

TEST(M3ReviewFindingTest, MINOR_DegenerateSmallDimensionFailsAsInvalidDimension) {
    // Positive but degenerate: must be reported as a structured invalid
    // dimension, not as a kernel-internal error.
    FakeGeometryKernel kernel;
    const ShapeResult result = kernel.createBox(BoxDefinition{1e-12, 50.0, 20.0});
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, KernelError::InvalidDimension);
}

TEST(M3ReviewFindingTest, MINOR_SmallButUsableDimensionSucceeds) {
    FakeGeometryKernel kernel;
    const ShapeResult result = kernel.createBox(BoxDefinition{1e-3, 50.0, 20.0});
    EXPECT_TRUE(result) << result.message;
}

TEST(M3ReviewFindingTest, MINOR_LargeDimensionSucceeds) {
    FakeGeometryKernel kernel;
    const ShapeResult result = kernel.createBox(BoxDefinition{1e6, 1e6, 1e6});
    EXPECT_TRUE(result) << result.message;
}

} // namespace
