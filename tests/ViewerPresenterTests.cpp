// M4-J: automated coverage of the viewer's document-facing half (spec 18
// "Viewer": automate ownership/association where practical; ADR-M4-006).
//
// DocumentPresenter is deliberately free of both Qt and OCCT, which is what
// makes these assertions possible without a display. The widget layer -- the
// part that genuinely needs a window -- is covered by the manual smoke test
// recorded in the self-validation report.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PlaceholderFeature.h"
#include "Fakes/FakeGeometryKernel.h"
#include "Viewer/DocumentOutline.h"
#include "Viewer/DocumentPresenter.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <set>

namespace {

using namespace paramcad;

struct ViewerDoc {
    PartDocument document{"ViewerDoc"};
    FakeGeometryKernel kernel;
    Sketch* sketch = nullptr;
    Parameter* length = nullptr;
    PadFeature* pad = nullptr;
    SketchEntityId topEdge{kInvalidSketchEntityId};

    ViewerDoc() {
        document.setGeometryKernel(&kernel);
        document.addMaterial("Mat", 2700.0);
        length = &document.addParameter("PadLength", 20.0, UnitType::Millimeter);
        sketch = &document.addSketch("Sketch001");
        sketch->addLine(Vec2{0, 0}, Vec2{100, 0});
        sketch->addLine(Vec2{100, 0}, Vec2{100, 50});
        topEdge = sketch->addLine(Vec2{100, 50}, Vec2{0, 50});
        sketch->addLine(Vec2{0, 50}, Vec2{0, 0});
        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", sketch->id(), length->id());
    }
};

TEST(ViewerPresenterTest, M4_VIEW_001_NothingIsDisplayableBeforeRecompute) {
    ViewerDoc doc;
    DocumentPresenter presenter(doc.document);
    EXPECT_TRUE(presenter.displayableSolids().empty())
        << "a feature with no runtime shape was offered for display";
}

TEST(ViewerPresenterTest, M4_VIEW_002_ValidSolidBecomesDisplayable) {
    ViewerDoc doc;
    DocumentPresenter presenter(doc.document);
    ASSERT_TRUE(presenter.recomputeForDisplay());

    const std::vector<ObjectId> ids = presenter.displayableSolids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids.front(), doc.pad->id())
        << "displayable solids must be identified by ObjectId, not by position";
}

TEST(ViewerPresenterTest, M4_VIEW_003_StaleGeometryIsNotOfferedForDisplay) {
    // The display-layer counterpart of ADR-M3-006: a failed rebuild RETAINS the
    // last valid shape, so a viewer that only checked "is there a shape?" would
    // keep drawing superseded geometry as if it were current.
    ViewerDoc doc;
    DocumentPresenter presenter(doc.document);
    ASSERT_TRUE(presenter.recomputeForDisplay());
    ASSERT_EQ(presenter.displayableSolids().size(), 1u);

    ASSERT_TRUE(doc.sketch->removeEntity(doc.topEdge)); // break the loop
    ASSERT_TRUE(doc.document.markSketchDirty(doc.sketch->id()));
    EXPECT_FALSE(presenter.recomputeForDisplay());

    EXPECT_TRUE(doc.pad->currentShape().isValid()) << "retention policy changed";
    EXPECT_TRUE(presenter.displayableSolids().empty())
        << "stale geometry was offered for display after a failed rebuild";
}

TEST(ViewerPresenterTest, M4_VIEW_004_RepairMakesItDisplayableAgain) {
    ViewerDoc doc;
    DocumentPresenter presenter(doc.document);
    ASSERT_TRUE(presenter.recomputeForDisplay());
    ASSERT_TRUE(doc.sketch->removeEntity(doc.topEdge));
    ASSERT_TRUE(doc.document.markSketchDirty(doc.sketch->id()));
    ASSERT_FALSE(presenter.recomputeForDisplay());

    doc.sketch->addLine(Vec2{100, 50}, Vec2{0, 50});
    ASSERT_TRUE(doc.document.markSketchDirty(doc.sketch->id()));
    EXPECT_TRUE(presenter.recomputeForDisplay());
    EXPECT_EQ(presenter.displayableSolids().size(), 1u);
}

// --- UI review findings ----------------------------------------------------

TEST(ViewerPresenterTest, M4_VIEW_010_HiddenSolidIsComputedButNotDrawn) {
    // Major 3: no Show/Hide existed. Visibility is VIEW state (ADR-M4-014):
    // hiding changes what is drawn, never what is computed or saved.
    ViewerDoc doc;
    DocumentPresenter presenter(doc.document);
    ASSERT_TRUE(presenter.recomputeForDisplay());
    ASSERT_EQ(presenter.displayableSolids().size(), 1u);

    presenter.setHidden(doc.pad->id(), true);
    EXPECT_TRUE(presenter.isHidden(doc.pad->id()));
    EXPECT_TRUE(presenter.displayableSolids().empty()) << "a hidden solid was still drawn";

    // Hiding must not touch the document: geometry and mass stay computed.
    EXPECT_EQ(doc.pad->state(), ComputeState::Valid);
    EXPECT_TRUE(doc.document.massProperties().valid);
    EXPECT_DOUBLE_EQ(doc.document.massProperties().volumeMm3, 100000.0);

    presenter.setHidden(doc.pad->id(), false);
    EXPECT_EQ(presenter.displayableSolids().size(), 1u);
}

TEST(ViewerPresenterTest, M4_VIEW_011_ToggleHiddenRoundTrips) {
    ViewerDoc doc;
    DocumentPresenter presenter(doc.document);
    ASSERT_TRUE(presenter.recomputeForDisplay());

    presenter.toggleHidden(doc.pad->id());
    EXPECT_TRUE(presenter.isHidden(doc.pad->id()));
    presenter.toggleHidden(doc.pad->id());
    EXPECT_FALSE(presenter.isHidden(doc.pad->id()));
    EXPECT_EQ(presenter.displayableSolids().size(), 1u);
}

TEST(ViewerPresenterTest, M4_VIEW_012_HiddenNeverMasksFailed) {
    // A hidden object that FAILS must still report Failed, not Hidden --
    // otherwise hiding something would conceal that it is broken.
    ViewerDoc doc;
    DocumentPresenter presenter(doc.document);
    ASSERT_TRUE(presenter.recomputeForDisplay());
    presenter.setHidden(doc.pad->id(), true);

    ASSERT_TRUE(doc.sketch->removeEntity(doc.topEdge));
    ASSERT_TRUE(doc.document.markSketchDirty(doc.sketch->id()));
    ASSERT_FALSE(presenter.recomputeForDisplay());

    std::set<ObjectId> hidden;
    hidden.insert(doc.pad->id());
    const OutlineNode root = DocumentOutline(doc.document).build(hidden);
    const OutlineNode* pad = nullptr;
    for (const OutlineNode& child : root.children)
        if (child.name == "Pad001") pad = &child;
    ASSERT_NE(pad, nullptr);
    EXPECT_EQ(pad->state, OutlineState::Failed)
        << "hiding an object concealed that it had failed";
}

TEST(ViewerPresenterTest, M4_VIEW_005_PresenterDoesNotOwnTheDocument) {
    // Ownership check (ADR-M4-006): the presenter holds a non-owning reference,
    // so the document it reports on is the caller's, and destroying presenters
    // leaves it untouched.
    ViewerDoc doc;
    {
        DocumentPresenter first(doc.document);
        ASSERT_TRUE(first.recomputeForDisplay());
        EXPECT_EQ(&first.document(), &doc.document);
    }
    DocumentPresenter second(doc.document);
    EXPECT_EQ(&second.document(), &doc.document);
    EXPECT_EQ(second.displayableSolids().size(), 1u)
        << "document state did not survive the presenter that observed it";
}

TEST(ViewerPresenterTest, M4_VIEW_006_ParameterEditFlowsThroughTheDocumentFacade) {
    // The viewer's edit path: change a Parameter through the facade, recompute,
    // then re-read. The viewer never writes document state directly.
    ViewerDoc doc;
    DocumentPresenter presenter(doc.document);
    ASSERT_TRUE(presenter.recomputeForDisplay());
    const double before = doc.document.massProperties().volumeMm3;

    ASSERT_TRUE(doc.document.setParameterValue(doc.length->id(), 30.0));
    ASSERT_TRUE(presenter.recomputeForDisplay());

    EXPECT_GT(doc.document.massProperties().volumeMm3, before);
    EXPECT_EQ(presenter.displayableSolids().size(), 1u);
}

TEST(ViewerPresenterTest, M4_VIEW_007_NonSolidFeaturesAreNotDisplayable) {
    // Depends on capability, not concrete type (ADR-M3-007): a feature that
    // produces no solid is simply absent, not a special case.
    PartDocument document{"Doc"};
    Body& body = document.addBody("Body001");
    document.addPlaceholderFeature(body, "Ghost", "Revolve");

    DocumentPresenter presenter(document);
    EXPECT_TRUE(presenter.displayableSolids().empty());
}

} // namespace
