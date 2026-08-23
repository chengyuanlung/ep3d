// M4 UI: automated coverage of everything the UI decides (UI spec 20/21).
//
// DocumentOutline is free of Qt and of OCCT precisely so that "what is shown,
// in what state, with what diagnostic" is testable without a display. The
// widget layer is a renderer over these values; what it cannot be tested for
// here is recorded as NOT EXECUTED in the UI self-validation report rather than
// asserted.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Sketch/Sketch.h"
#include "Core/Body/Body.h"
#include "Core/Parameter/Parameter.h"
#include <vector>
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
    // "Name" used to be the example here, and stopped being one at M17.16 when
    // renaming arrived (ADR-M17-039). "Type" is genuinely read-only: a Pad is
    // a Pad, and there is nothing to type over.
    OutlineDoc doc;
    const std::vector<PropertyRow> rows = DocumentOutline(doc.document).propertiesOf(doc.pad->id());
    const PropertyRow* type = Row(rows, "Type");
    ASSERT_NE(type, nullptr);
    EXPECT_FALSE(type->editable);
    EXPECT_EQ(type->parameterId, kInvalidObjectId)
        << "a read-only row still names something writable";
    EXPECT_EQ(type->field, PropertyField::None);
}

TEST(DocumentOutlineTest, M17_PROP_030_NameIsEditableAndWritesTheOBJECTNotAParameter) {
    // The Name row's `parameterId` carries the OBJECT's id. That field has
    // always meant "what to write" -- but everything else that used it wrote a
    // Parameter, so a rename row handed to the wrong branch would be refused
    // with "that parameter no longer exists" on every sketch and feature.
    OutlineDoc doc;
    const std::vector<PropertyRow> rows = DocumentOutline(doc.document).propertiesOf(doc.pad->id());
    const PropertyRow* name = Row(rows, "Name");
    ASSERT_NE(name, nullptr);
    EXPECT_TRUE(name->editable);
    EXPECT_EQ(name->field, PropertyField::Name);
    EXPECT_EQ(name->parameterId, doc.pad->id());
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

// --- M17.10: the tree is a TIMELINE, and sketches are absorbed ---------------
//
// The tree used to list every sketch and then every feature. Sketch on a face,
// pad, sketch on the new face, pad again -- and the result was three sketches
// in a row followed by three pads: the ORDER was gone, and nothing said which
// sketch made which pad. Both facts were in the model the whole time.
//
// This is SolidWorks' arrangement rather than Onshape's, and the reason is the
// second half: absorption puts the lineage where a user reads it, instead of
// behind a dependency dialog nobody opens.

namespace {

// The root's children, by name, in order -- what a user sees down the left.
std::vector<std::string> Spine(const OutlineNode& root) {
    std::vector<std::string> names;
    for (const OutlineNode& child : root.children) names.push_back(child.name);
    return names;
}

std::vector<std::string> ChildNames(const OutlineNode& node) {
    std::vector<std::string> names;
    for (const OutlineNode& child : node.children) names.push_back(child.name);
    return names;
}

int IndexOf(const std::vector<std::string>& names, const std::string& name) {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<int>(i);
    return -1;
}

} // namespace

TEST(DocumentOutlineTest, M17_TREE_020_APadABSORBSTheSketchItWasBuiltFrom) {
    OutlineDoc doc;
    const OutlineNode root = DocumentOutline(doc.document).build();

    // NOT on the spine any more -- it is inside the feature that consumed it.
    EXPECT_EQ(IndexOf(Spine(root), "Sketch001"), -1)
        << "the consumed sketch is still listed as a document-level peer";

    const OutlineNode* pad = Find(root, "Pad001");
    ASSERT_NE(pad, nullptr);
    EXPECT_NE(IndexOf(ChildNames(*pad), "Sketch001"), -1)
        << "the pad does not contain its own sketch";

    // And it is still THE SAME OBJECT: absorbing a sketch must not cost it its
    // identity, or selecting the row would stop reaching the sketch and "Edit
    // Selected Sketch" would go grey on a row that plainly is one.
    const OutlineNode* sketch = Find(root, "Sketch001");
    ASSERT_NE(sketch, nullptr);
    EXPECT_EQ(sketch->id, doc.sketch->id());
    EXPECT_EQ(sketch->kind, OutlineKind::Sketch);
}

TEST(DocumentOutlineTest, M17_TREE_021_TheSpineIsInCREATIONOrder) {
    // The workflow this exists for: draw, pad, draw on the result, pad again.
    // Listed by type that reads Sketch1 Sketch2 Pad1 Pad2, which says nothing
    // about what followed what.
    OutlineDoc doc;
    Parameter& second = doc.document.addParameter("Boss", 5.0, UnitType::Millimeter);
    Sketch& later = doc.document.addSketch("Sketch002");
    later.addLine(Vec2{0, 0}, Vec2{10, 0});
    later.addLine(Vec2{10, 0}, Vec2{10, 10});
    later.addLine(Vec2{10, 10}, Vec2{0, 10});
    later.addLine(Vec2{0, 10}, Vec2{0, 0});
    doc.document.addPadFeature(*doc.document.bodies().front(), "Pad002", later.id(), second.id());

    const OutlineNode root = DocumentOutline(doc.document).build();
    const std::vector<std::string> spine = Spine(root);
    const int first = IndexOf(spine, "Pad001");
    const int next = IndexOf(spine, "Pad002");
    ASSERT_NE(first, -1);
    ASSERT_NE(next, -1);
    EXPECT_LT(first, next) << "the second pad is listed before the first";

    // Each pad carries its OWN sketch, which is the whole point: the lineage is
    // readable without opening anything.
    const OutlineNode* padOne = Find(root, "Pad001");
    const OutlineNode* padTwo = Find(root, "Pad002");
    ASSERT_NE(padOne, nullptr);
    ASSERT_NE(padTwo, nullptr);
    EXPECT_NE(IndexOf(ChildNames(*padOne), "Sketch001"), -1);
    EXPECT_NE(IndexOf(ChildNames(*padTwo), "Sketch002"), -1);
    EXPECT_EQ(IndexOf(ChildNames(*padOne), "Sketch002"), -1) << "the pads swapped sketches";
}

TEST(DocumentOutlineTest, M17_TREE_022_AnUnusedSketchStaysOnTheSpine) {
    // A sketch nothing consumes belongs to nobody, and hiding it inside a
    // feature would be a claim about a relationship that does not exist.
    OutlineDoc doc;
    Sketch& loose = doc.document.addSketch("Scratch");
    loose.addLine(Vec2{0, 0}, Vec2{5, 0});

    const OutlineNode root = DocumentOutline(doc.document).build();
    EXPECT_NE(IndexOf(Spine(root), "Scratch"), -1);
}

TEST(DocumentOutlineTest, M17_TREE_023_ASketchWithTWOConsumersIsAbsorbedByNEITHER) {
    // Nesting it under one of them would say it belongs to that feature, which
    // is false for the other -- and the user would have no way to reach the
    // relationship the tree chose to hide. It stays on the spine, visibly
    // belonging to neither.
    OutlineDoc doc;
    Parameter& depth = doc.document.addParameter("PocketDepth", 5.0, UnitType::Millimeter);
    doc.document.addPocketFeature(*doc.document.bodies().front(), "Pocket001", doc.pad->id(),
                                  doc.sketch->id(), depth.id());

    const OutlineNode root = DocumentOutline(doc.document).build();
    EXPECT_NE(IndexOf(Spine(root), "Sketch001"), -1)
        << "a sketch two features share was absorbed by one of them";
    const OutlineNode* pad = Find(root, "Pad001");
    ASSERT_NE(pad, nullptr);
    EXPECT_EQ(IndexOf(ChildNames(*pad), "Sketch001"), -1);
}

TEST(DocumentOutlineTest, M17_TREE_024_ARowWithAStateOfItsOwnKeepsItWhenItsChildFails) {
    // The rule absorption made load-bearing. A Pad whose sketch has conflicting
    // dimensions is BLOCKED: it never ran, and the thing that broke is the
    // sketch. Rolling the child's Failed upward would make the Pad report that
    // IT failed -- pointing the user at the wrong object, which is the exact
    // distinction M5_DEF_012 exists to protect.
    OutlineDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    Parameter& alt = doc.document.addParameter("Alt", 70.0, UnitType::Millimeter);
    ASSERT_NE(doc.document.addSketchConstraint(doc.sketch->id(),
                                               LengthConstraint{doc.topEdge, alt.id()}),
              kInvalidSketchConstraintId);
    ASSERT_NE(doc.document.addSketchConstraint(
                  doc.sketch->id(), LengthConstraint{doc.topEdge, doc.length->id()}),
              kInvalidSketchConstraintId);
    doc.document.recompute();

    const OutlineNode root = DocumentOutline(doc.document).build();
    const OutlineNode* pad = Find(root, "Pad001");
    ASSERT_NE(pad, nullptr);
    const OutlineNode* sketch = Find(root, "Sketch001");
    ASSERT_NE(sketch, nullptr);

    EXPECT_EQ(sketch->state, OutlineState::Failed) << "the sketch is what broke";
    EXPECT_EQ(pad->state, OutlineState::Blocked)
        << "the pad reported its child's failure as its own: " << pad->diagnostic;
    // The ROOT still summarises: rolling up must still travel, it just stops
    // overwriting rows that had something of their own to say.
    EXPECT_NE(root.state, OutlineState::Valid);
}
