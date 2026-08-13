// M4 UI: automated coverage of everything the UI decides (UI spec 20/21).
//
// DocumentOutline is free of Qt and of OCCT precisely so that "what is shown,
// in what state, with what diagnostic" is testable without a display. The
// widget layer is a renderer over these values; what it cannot be tested for
// here is recorded as NOT EXECUTED in the UI self-validation report rather than
// asserted.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Fakes/FakeGeometryKernel.h"
#include "Viewer/DocumentOutline.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <set>
#include <string>

namespace {

using namespace paramcad;

struct OutlineDoc {
    PartDocument document{"Part001"};
    FakeGeometryKernel kernel;
    Sketch* sketch = nullptr;
    Parameter* length = nullptr;
    PadFeature* pad = nullptr;
    SketchEntityId topEdge{kInvalidSketchEntityId};

    OutlineDoc() {
        document.setGeometryKernel(&kernel);
        document.addMaterial("Aluminium", 2700.0);
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

const OutlineNode* Find(const OutlineNode& node, const std::string& name) {
    if (node.name == name) return &node;
    for (const OutlineNode& child : node.children)
        if (const OutlineNode* hit = Find(child, name)) return hit;
    return nullptr;
}

const PropertyRow* Row(const std::vector<PropertyRow>& rows, const std::string& label) {
    const auto it = std::find_if(rows.begin(), rows.end(),
                                 [&](const PropertyRow& r) { return r.label == label; });
    return it != rows.end() ? &*it : nullptr;
}

// --- Tree structure and identity (UI spec 6/20) ----------------------------

TEST(DocumentOutlineTest, UI_TREE_001_ShowsTheDocumentStructure) {
    OutlineDoc doc;
    const OutlineNode root = DocumentOutline(doc.document).build();

    EXPECT_EQ(root.name, "Part001");
    EXPECT_EQ(root.kind, OutlineKind::Document);
    ASSERT_NE(Find(root, "Sketch001"), nullptr);
    ASSERT_NE(Find(root, "Pad001"), nullptr);
    ASSERT_NE(Find(root, "PadLength"), nullptr);
    ASSERT_NE(Find(root, "Aluminium"), nullptr);
    ASSERT_NE(Find(root, "MassProperties"), nullptr);
}

TEST(DocumentOutlineTest, UI_TREE_002_EveryRowCarriesItsStableObjectId) {
    // UI spec 20: tree items map to a stable ObjectId, never to a position.
    OutlineDoc doc;
    const OutlineNode root = DocumentOutline(doc.document).build();

    EXPECT_EQ(Find(root, "Sketch001")->id, doc.sketch->id());
    EXPECT_EQ(Find(root, "Pad001")->id, doc.pad->id());
    EXPECT_EQ(Find(root, "PadLength")->id, doc.length->id());
}

TEST(DocumentOutlineTest, UI_TREE_003_StatesAreDistinguishableWithoutColour) {
    // UI spec 11/19: state must never be conveyed by colour alone, and Failed
    // must not look like Dirty.
    const std::string valid = DocumentOutline::stateMarker(OutlineState::Valid);
    const std::string dirty = DocumentOutline::stateMarker(OutlineState::Dirty);
    const std::string failed = DocumentOutline::stateMarker(OutlineState::Failed);
    const std::string suppressed = DocumentOutline::stateMarker(OutlineState::Suppressed);

    EXPECT_NE(dirty, failed);
    EXPECT_NE(dirty, valid);
    EXPECT_NE(failed, valid);
    EXPECT_NE(suppressed, failed);
    EXPECT_FALSE(std::string(DocumentOutline::stateLabel(OutlineState::Failed)).empty());
    EXPECT_NE(std::string(DocumentOutline::stateLabel(OutlineState::Dirty)),
              std::string(DocumentOutline::stateLabel(OutlineState::Failed)));
}

TEST(DocumentOutlineTest, UI_TREE_004_PadShowsDirtyBeforeRecomputeAndValidAfter) {
    OutlineDoc doc;
    EXPECT_EQ(Find(DocumentOutline(doc.document).build(), "Pad001")->state,
              OutlineState::Dirty);

    ASSERT_TRUE(doc.document.recompute().success);
    EXPECT_EQ(Find(DocumentOutline(doc.document).build(), "Pad001")->state,
              OutlineState::Valid);
}

TEST(DocumentOutlineTest, UI_TREE_005_FailedProfileIsVisibleWithItsDiagnostic) {
    // UI spec 12: a failed feature must be identifiable from the tree, with a
    // useful reason, without opening a log.
    OutlineDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    ASSERT_TRUE(doc.sketch->removeEntity(doc.topEdge));
    ASSERT_TRUE(doc.document.markSketchDirty(doc.sketch->id()));
    ASSERT_FALSE(doc.document.recompute().success);

    const OutlineNode root = DocumentOutline(doc.document).build();
    const OutlineNode* sketch = Find(root, "Sketch001");
    ASSERT_NE(sketch, nullptr);
    EXPECT_EQ(sketch->state, OutlineState::Failed);
    EXPECT_FALSE(sketch->diagnostic.empty())
        << "the tree offers no reason the sketch is unusable";
    EXPECT_NE(sketch->diagnostic.find("closed"), std::string::npos)
        << "diagnostic does not describe the actual problem: " << sketch->diagnostic;

    EXPECT_EQ(Find(root, "Pad001")->state, OutlineState::Failed);
}

TEST(DocumentOutlineTest, M6_UI_001_AnOpenSketchNothingConsumesIsNotAFailure) {
    // Reported by the owner's manual UI validation of M6, on the very first
    // import: line.dxf imported one line, drew it correctly, and the tree said
    // "[Skt]! line". Nothing had failed.
    //
    // "Profile is not closed" describes a sketch that cannot be padded YET.
    // That is a failure when a Pad is asking for it -- UI_TREE_005 above, which
    // still holds -- and it is simply the state of the drawing when nothing is.
    // The rule was right; its scope was not. It was invisible before M6 because
    // every sketch the viewer could previously build closed its own profile.
    //
    // A user who sees "Failed" after a successful import learns to distrust the
    // marker, and then it cannot do its job when something really has failed.
    PartDocument document{"Part001"};
    Sketch& sketch = document.addSketch("line");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 50});
    ASSERT_TRUE(document.recompute().success)
        << "a document holding one open sketch failed to recompute";

    // The root is bound to a NAMED value. Find() returns a pointer into the
    // tree, so passing the temporary directly reads freed memory the instant
    // the full expression ends -- which is exactly what this test did first,
    // and is the same mistake M5's test helpers made.
    const OutlineNode root = DocumentOutline(document).build();
    const OutlineNode* node = Find(root, "line");
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->state, OutlineState::Valid)
        << "an open sketch with no consumer was marked failed";

    // The reason is still offered -- this is about the STATE channel, not about
    // hiding information. A user who wonders why they cannot pad it must still
    // be able to find out without opening a log.
    EXPECT_FALSE(node->diagnostic.empty())
        << "the state was corrected by throwing the explanation away";
    EXPECT_NE(node->diagnostic.find("closed"), std::string::npos) << node->diagnostic;
}

TEST(DocumentOutlineTest, UI_TREE_006_MassPropertiesNeverShowsCurrentWhenItIsNot) {
    // The display-layer form of ADR-M3-006 (UI spec 13): retained numbers must
    // not be presented as up to date.
    OutlineDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    EXPECT_EQ(Find(DocumentOutline(doc.document).build(), "MassProperties")->state,
              OutlineState::Valid);

    ASSERT_TRUE(doc.sketch->removeEntity(doc.topEdge));
    ASSERT_TRUE(doc.document.markSketchDirty(doc.sketch->id()));
    ASSERT_FALSE(doc.document.recompute().success);

    EXPECT_NE(Find(DocumentOutline(doc.document).build(), "MassProperties")->state,
              OutlineState::Valid);
}

TEST(DocumentOutlineTest, UI_TREE_007_RootSummarisesItsChildrenNotNotComputed) {
    // Major 4: the root Part row read "Not computed" forever, even with every
    // child up to date, and stayed that way when a child failed.
    OutlineDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    EXPECT_EQ(DocumentOutline(doc.document).build().state, OutlineState::Valid)
        << "root reported a state that ignores its children";

    ASSERT_TRUE(doc.sketch->removeEntity(doc.topEdge));
    ASSERT_TRUE(doc.document.markSketchDirty(doc.sketch->id()));
    ASSERT_FALSE(doc.document.recompute().success);
    EXPECT_EQ(DocumentOutline(doc.document).build().state, OutlineState::Failed)
        << "a failure below the root was invisible at the root";
}

TEST(DocumentOutlineTest, UI_TREE_008_HiddenIsItsOwnStateNotSuppressed) {
    // UI spec 11: Hidden and Suppressed are semantically different and must not
    // be represented identically.
    EXPECT_NE(std::string(DocumentOutline::stateMarker(OutlineState::Hidden)),
              std::string(DocumentOutline::stateMarker(OutlineState::Suppressed)));
    EXPECT_NE(std::string(DocumentOutline::stateLabel(OutlineState::Hidden)),
              std::string(DocumentOutline::stateLabel(OutlineState::Suppressed)));

    OutlineDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    std::set<ObjectId> hidden;
    hidden.insert(doc.pad->id());
    EXPECT_EQ(Find(DocumentOutline(doc.document).build(hidden), "Pad001")->state,
              OutlineState::Hidden);
}

// --- Property panel (UI spec 7/8) ------------------------------------------

TEST(DocumentOutlineTest, UI_PROP_001_PadExposesAnEditableLengthWithAUnit) {
    OutlineDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    const std::vector<PropertyRow> rows = DocumentOutline(doc.document).propertiesOf(doc.pad->id());

    const PropertyRow* length = Row(rows, "Length");
    ASSERT_NE(length, nullptr);
    EXPECT_TRUE(length->editable);
    EXPECT_EQ(length->unitLabel, "mm") << "a length was offered without a unit (UI spec 8)";
    EXPECT_EQ(length->value, "20.000");
    // The editable row must target the PARAMETER, so the panel writes through
    // the document facade rather than into the feature (UI spec 20).
    EXPECT_EQ(length->parameterId, doc.length->id());
}

TEST(DocumentOutlineTest, UI_PROP_002_EveryDimensionalValueCarriesAUnit) {
    // UI spec 8 makes a naked engineering value a defect. Checked for every row
    // the panel can produce, not just the one we remembered to look at.
    OutlineDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    for (ObjectId id : {doc.pad->id(), doc.sketch->id(), doc.length->id()}) {
        for (const PropertyRow& row : DocumentOutline(doc.document).propertiesOf(id)) {
            const bool dimensional = row.label.find("Length") != std::string::npos ||
                                     row.label.find("Origin") != std::string::npos ||
                                     row.label == "Value" || row.label == "Density";
            if (!dimensional) continue;
            EXPECT_FALSE(row.unitLabel.empty())
                << "row '" << row.label << "' shows a dimensional value with no unit";
        }
    }
}

TEST(DocumentOutlineTest, UI_PROP_003_ReadOnlyRowsAreMarkedReadOnly) {
    OutlineDoc doc;
    const std::vector<PropertyRow> rows = DocumentOutline(doc.document).propertiesOf(doc.pad->id());
    const PropertyRow* name = Row(rows, "Name");
    ASSERT_NE(name, nullptr);
    EXPECT_FALSE(name->editable);
    EXPECT_EQ(name->parameterId, kInvalidObjectId)
        << "a read-only row still names something writable";
}

TEST(DocumentOutlineTest, UI_PROP_004_SketchShowsProfileStatusAndReason) {
    OutlineDoc doc;
    {
        const std::vector<PropertyRow> rows =
            DocumentOutline(doc.document).propertiesOf(doc.sketch->id());
        const PropertyRow* status = Row(rows, "Status");
        ASSERT_NE(status, nullptr);
        EXPECT_EQ(status->value, "Closed loop");
        EXPECT_EQ(Row(rows, "Diagnostic"), nullptr) << "a healthy sketch reported a diagnostic";
    }

    ASSERT_TRUE(doc.sketch->removeEntity(doc.topEdge));
    {
        const std::vector<PropertyRow> rows =
            DocumentOutline(doc.document).propertiesOf(doc.sketch->id());
        EXPECT_EQ(Row(rows, "Status")->value, "Invalid");
        const PropertyRow* diagnostic = Row(rows, "Diagnostic");
        ASSERT_NE(diagnostic, nullptr);
        EXPECT_FALSE(diagnostic->value.empty());
    }
}

TEST(DocumentOutlineTest, UI_PROP_005_MaterialIsShownAsUnassignedWhenItIs) {
    OutlineDoc doc;
    ASSERT_TRUE(doc.document.removeObject(doc.document.material()->id()));
    const std::vector<PropertyRow> rows = DocumentOutline(doc.document).propertiesOf(doc.pad->id());
    const PropertyRow* material = Row(rows, "Material");
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->value, "(none)")
        << "the panel still names a material this document no longer has";
}

TEST(DocumentOutlineTest, UI_PROP_006_UnknownIdYieldsNoRows) {
    OutlineDoc doc;
    EXPECT_TRUE(DocumentOutline(doc.document).propertiesOf(999999).empty());
    EXPECT_TRUE(DocumentOutline(doc.document).propertiesOf(kInvalidObjectId).empty());
}

} // namespace

