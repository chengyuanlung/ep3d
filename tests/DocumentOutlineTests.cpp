// M4 UI: automated coverage of everything the UI decides (UI spec 20/21).
//
// DocumentOutline is free of Qt and of OCCT precisely so that "what is shown,
// in what state, with what diagnostic" is testable without a display. The
// widget layer is a renderer over these values; what it cannot be tested for
// here is recorded as NOT EXECUTED in the UI self-validation report rather than
// asserted.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/SweepFeature.h"
#include "Core/Reference/ReferenceFrame.h"
#include "Core/Feature/LoftFeature.h"
#include "Core/Feature/TransformFeatures.h"
#include "Core/Feature/ShellFeature.h"
#include "Core/Feature/HoleFeature.h"
#include "Core/Sketch/Sketch.h"
#include "Core/Body/Body.h"
#include "Core/Parameter/Parameter.h"
#include <vector>
#include "Fakes/FakeGeometryKernel.h"
#include "Viewer/DocumentOutline.h"
#include "Core/Feature/ISketchConsuming.h"
#include "Core/Sketch/Profile.h"
#include <functional>
#include <cstdio>
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


// =============================================================================
// M26.7 -- what a click on sketch geometry puts in the property panel
//
// The rows themselves, tested here where no widget is needed. The WIRING (a
// click reaching the panel at all) is checked in the viewer's --selftest,
// because that is the half no unit test can see.
// =============================================================================

namespace {

std::string ValueOf(const std::vector<PropertyRow>& rows, const std::string& label) {
    for (const PropertyRow& row : rows)
        if (row.label == label) return row.value;
    return std::string();
}

bool HasRow(const std::vector<PropertyRow>& rows, const std::string& label) {
    for (const PropertyRow& row : rows)
        if (row.label == label) return true;
    return false;
}

} // namespace

TEST(DocumentOutlineTest, M26_PROP_001_ALineReportsItsEndsItsLengthAndItsAngle) {
    PartDocument document{"Part"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId line = sketch.addLine(Vec2{10.0, 20.0}, Vec2{40.0, 60.0});

    const DocumentOutline outline(document);
    const std::vector<PropertyRow> rows =
        outline.propertiesOfSketchElement(sketch.id(), SketchElementRef{line});

    EXPECT_EQ(ValueOf(rows, "Type"), "Line");
    EXPECT_EQ(ValueOf(rows, "Start u"), "10.000");
    EXPECT_EQ(ValueOf(rows, "Start v"), "20.000");
    EXPECT_EQ(ValueOf(rows, "End u"), "40.000");
    EXPECT_EQ(ValueOf(rows, "End v"), "60.000");
    // 3-4-5: the length is 50, and the angle is atan2(40, 30).
    EXPECT_EQ(ValueOf(rows, "Length"), "50.000");
    EXPECT_EQ(ValueOf(rows, "Angle"), "53.13");

    // EVERY ROW IS READ-ONLY. Sketch geometry is what the solver writes, and a
    // cell that took a typed coordinate would be overwritten by the next solve
    // that disagreed with it.
    for (const PropertyRow& row : rows)
        EXPECT_FALSE(row.editable) << row.label << " offers to be typed into";
}

TEST(DocumentOutlineTest, M26_PROP_002_ACircleGivesBOTHRadiusAndDiameter) {
    // A drawing quotes one or the other, and a user should not have to double
    // a number in their head to check it against one.
    PartDocument document{"Part"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId circle = sketch.addCircle(Vec2{5.0, -5.0}, 12.5);

    const DocumentOutline outline(document);
    const std::vector<PropertyRow> rows =
        outline.propertiesOfSketchElement(sketch.id(), SketchElementRef{circle});

    EXPECT_EQ(ValueOf(rows, "Type"), "Circle");
    EXPECT_EQ(ValueOf(rows, "Centre u"), "5.000");
    EXPECT_EQ(ValueOf(rows, "Radius"), "12.500");
    EXPECT_EQ(ValueOf(rows, "Diameter"), "25.000");
}

TEST(DocumentOutlineTest, M26_PROP_003_AnArcSaysWHICHOfTheTwoArcsItIs) {
    // Its two angles describe two different shapes equally well. The direction
    // is the flag SketchArc stores to resolve that, so leaving it out of the
    // panel would show a user three rows that do not pin down what they picked.
    PartDocument document{"Part"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId arc = sketch.addArc(Vec2{0.0, 0.0}, 20.0, 0.0, 1.5707963267948966, false);

    const DocumentOutline outline(document);
    const std::vector<PropertyRow> rows =
        outline.propertiesOfSketchElement(sketch.id(), SketchElementRef{arc});

    EXPECT_EQ(ValueOf(rows, "Type"), "Arc");
    EXPECT_EQ(ValueOf(rows, "Radius"), "20.000");
    EXPECT_EQ(ValueOf(rows, "Start angle"), "0.00");
    EXPECT_EQ(ValueOf(rows, "End angle"), "90.00");
    EXPECT_EQ(ValueOf(rows, "Direction"), "clockwise");
}

TEST(DocumentOutlineTest, M26_PROP_004_PickingAnENDPOINTSaysSoAndGivesThatPoint) {
    // "Line1" and "Line1's end" are different answers to "what did I click",
    // and the sub-element is the half an id-only lookup would throw away.
    PartDocument document{"Part"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId line = sketch.addLine(Vec2{0.0, 0.0}, Vec2{30.0, 40.0});

    const DocumentOutline outline(document);
    const std::vector<PropertyRow> whole =
        outline.propertiesOfSketchElement(sketch.id(), SketchElementRef{line});
    EXPECT_FALSE(HasRow(whole, "Picked")) << "the whole line claimed a sub-element was picked";

    const std::vector<PropertyRow> end = outline.propertiesOfSketchElement(
        sketch.id(), SketchElementRef{line, SketchSubElement::EndPoint});
    EXPECT_NE(ValueOf(end, "Picked").find("end point"), std::string::npos)
        << ValueOf(end, "Picked");
    EXPECT_EQ(ValueOf(end, "Picked point u"), "30.000");
    EXPECT_EQ(ValueOf(end, "Picked point v"), "40.000");
}

TEST(DocumentOutlineTest, M26_PROP_005_ItCountsWhatHoldsTheEntity) {
    // The row a user wants when geometry will not drag: an entity that refuses
    // to move is over-constrained, and counting what holds it is the first
    // question anyone asks.
    PartDocument document{"Part"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId held = sketch.addLine(Vec2{0.0, 0.0}, Vec2{50.0, 0.0});
    const SketchEntityId loose = sketch.addLine(Vec2{0.0, 30.0}, Vec2{50.0, 35.0});
    document.addSketchConstraint(sketch.id(), HorizontalConstraint{held});
    document.addSketchConstraint(
        sketch.id(), FixConstraint{SketchElementRef{held, SketchSubElement::StartPoint}});

    const DocumentOutline outline(document);
    EXPECT_EQ(ValueOf(outline.propertiesOfSketchElement(sketch.id(), SketchElementRef{held}),
                      "On this"),
              "2");
    EXPECT_EQ(ValueOf(outline.propertiesOfSketchElement(sketch.id(), SketchElementRef{loose}),
                      "On this"),
              "0");
}

TEST(DocumentOutlineTest, M26_PROP_006_ItSaysWhetherTheGeometryIsCONSTRUCTION) {
    // The flag that decides whether a pad will sweep it. A user staring at a
    // profile that refuses to extrude needs to be able to read this somewhere.
    PartDocument document{"Part"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId line = sketch.addLine(Vec2{0.0, 0.0}, Vec2{50.0, 0.0});

    const DocumentOutline outline(document);
    EXPECT_EQ(
        ValueOf(outline.propertiesOfSketchElement(sketch.id(), SketchElementRef{line}),
                "Construction"),
        "no");

    ASSERT_EQ(document.setSketchEntitiesConstruction(sketch.id(), {line}, true), 1u);
    EXPECT_EQ(
        ValueOf(outline.propertiesOfSketchElement(sketch.id(), SketchElementRef{line}),
                "Construction"),
        "yes");
}

TEST(DocumentOutlineTest, M26_PROP_007_AnUnknownRefDescribesNothing) {
    // Empty, not a row of blanks. A panel full of empty cells looks like a
    // described object whose values failed to load.
    PartDocument document{"Part"};
    Sketch& sketch = document.addSketch("Sketch001");

    const DocumentOutline outline(document);
    EXPECT_TRUE(outline.propertiesOfSketchElement(sketch.id(), SketchElementRef{}).empty());
    EXPECT_TRUE(
        outline.propertiesOfSketchElement(kInvalidObjectId, SketchElementRef{}).empty());
}


TEST(DocumentOutlineTest, M26_PROP_008_ACoordinateOfMinusNothingIsNotShownAsMinusZero) {
    // A solver returns -1e-17 for "on the axis" about half the time, and
    // "%.3f" prints that as "-0.000". A panel showing "-0.000 mm" beside
    // "0.000 mm" is showing two numbers where the model has one.
    //
    // Found by LOOKING at the panel, not by any assertion -- so this is the
    // assertion.
    PartDocument document{"Part"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId line = sketch.addLine(Vec2{0.0, -1e-17}, Vec2{80.0, -1e-17});

    const DocumentOutline outline(document);
    const std::vector<PropertyRow> rows =
        outline.propertiesOfSketchElement(sketch.id(), SketchElementRef{line});
    EXPECT_EQ(ValueOf(rows, "Start v"), "0.000");
    EXPECT_EQ(ValueOf(rows, "End v"), "0.000");

    // ...and a real negative still keeps its sign.
    const SketchEntityId below = sketch.addLine(Vec2{0.0, -4.25}, Vec2{10.0, -4.25});
    EXPECT_EQ(ValueOf(outline.propertiesOfSketchElement(sketch.id(), SketchElementRef{below}),
                      "Start v"),
              "-4.250");
}


// =============================================================================
// M26.8 -- a sketch of POINTS feeding a Hole is not a failed sketch
// =============================================================================

namespace {

const OutlineNode* FindNode(const OutlineNode& node, const std::string& name) {
    if (node.name == name) return &node;
    for (const OutlineNode& child : node.children)
        if (const OutlineNode* found = FindNode(child, name)) return found;
    return nullptr;
}

} // namespace

TEST(DocumentOutlineTest, M26_TREE_001_APointSketchFeedingAHoleIsNotMarkedFailed) {
    // What examples/stepper-motor.ep3ds showed in the tree: `[Skt] ! Mounts
    // Failed`, beside a hole feature that had drilled four correct bores.
    //
    // The sketch is four POINTS. It has no closed loop and is not supposed to
    // have one -- that is the drawing a hole wants. The old rule asked only
    // whether ANYTHING depended on the sketch, which is a different question
    // from whether anything wanted a profile out of it.
    PartDocument document{"Part"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);

    Sketch& profile = document.addSketch("Profile");
    profile.addLine(Vec2{0, 0}, Vec2{50, 0});
    profile.addLine(Vec2{50, 0}, Vec2{50, 50});
    profile.addLine(Vec2{50, 50}, Vec2{0, 50});
    profile.addLine(Vec2{0, 50}, Vec2{0, 0});
    Parameter& thickness = document.addParameter("T", 10.0, UnitType::Millimeter);
    Body& body = document.addBody("Block");
    const Feature& pad = document.addPadFeature(body, "Pad", profile.id(), thickness.id());

    Sketch& mounts = document.addSketch("Mounts");
    mounts.addPoint(Vec2{10, 10});
    mounts.addPoint(Vec2{40, 40});
    Parameter& bore = document.addParameter("Bore", 5.0, UnitType::Millimeter);
    Parameter& deep = document.addParameter("Deep", 0.0, UnitType::Millimeter);
    document.addHoleFeature(body, "Holes", pad.id(), mounts.id(), bore.id(), deep.id());
    (void)document.recompute();

    const DocumentOutline outline(document);
    const OutlineNode root = outline.build();

    const OutlineNode* mountsNode = FindNode(root, "Mounts");
    ASSERT_NE(mountsNode, nullptr);
    EXPECT_NE(mountsNode->state, OutlineState::Failed)
        << "a sketch of points feeding a hole was marked Failed: " << mountsNode->diagnostic;
    // THE REASON IS STILL THERE. Only the failure MARKER is withheld -- a user
    // who wonders why it will not pad can still read why.
    EXPECT_FALSE(mountsNode->diagnostic.empty())
        << "the profile's complaint stopped being reported at all";
}

TEST(DocumentOutlineTest, M26_TREE_002_AnOpenSketchFeedingAPadIsStillMarkedFailed) {
    // The other half, and the reason the rule exists at all. A pad sweeps an
    // AREA, so a sketch that does not close really is a failure for it -- and
    // relaxing the hole case must not relax this one.
    PartDocument document{"Part"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);

    Sketch& open = document.addSketch("OpenProfile");
    open.addLine(Vec2{0, 0}, Vec2{50, 0}); // one line: no loop
    Parameter& thickness = document.addParameter("T", 10.0, UnitType::Millimeter);
    Body& body = document.addBody("Block");
    document.addPadFeature(body, "Pad", open.id(), thickness.id());
    (void)document.recompute();

    const DocumentOutline outline(document);
    // The root is HELD. FindNode returns a pointer INTO the tree, so reading it
    // out of a temporary is a use-after-free -- which is what the first draft of
    // this test did, and it reported the production code as broken.
    const OutlineNode root = outline.build();
    const OutlineNode* node = FindNode(root, "OpenProfile");
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->state, OutlineState::Failed)
        << "an open sketch a pad is waiting for stopped being a failure";
}

TEST(DocumentOutlineTest, M26_TREE_003_AnOpenSketchNOTHINGDependsOnIsNotAFailure) {
    // The M6 case this rule was written for in the first place: one imported
    // line, drawn correctly, with nothing waiting for it. Kept as a test so
    // the M26.8 change cannot quietly undo it.
    PartDocument document{"Part"};
    Sketch& lonely = document.addSketch("JustALine");
    lonely.addLine(Vec2{0, 0}, Vec2{50, 0});

    const DocumentOutline outline(document);
    const OutlineNode root = outline.build();
    const OutlineNode* node = FindNode(root, "JustALine");
    ASSERT_NE(node, nullptr);
    EXPECT_NE(node->state, OutlineState::Failed);
}


// =============================================================================
// M26.9 -- every feature's numbers reach the property panel
// =============================================================================

TEST(DocumentOutlineTest, M26_PROP_010_AHoleShowsItsDiameterAndItsDepth) {
    // Reported from the stepper-motor example: selecting [Sld] MotorHole showed
    // no diameter and no depth. Both were stored, solved, saved and reloaded
    // correctly -- the panel had a hand-written branch per feature type and
    // Hole never got one.
    PartDocument document{"Part"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);

    Sketch& profile = document.addSketch("Profile");
    profile.addLine(Vec2{0, 0}, Vec2{50, 0});
    profile.addLine(Vec2{50, 0}, Vec2{50, 50});
    profile.addLine(Vec2{50, 50}, Vec2{0, 50});
    profile.addLine(Vec2{0, 50}, Vec2{0, 0});
    Parameter& thickness = document.addParameter("T", 10.0, UnitType::Millimeter);
    Body& body = document.addBody("Block");
    const Feature& pad = document.addPadFeature(body, "Pad", profile.id(), thickness.id());

    Sketch& mounts = document.addSketch("Mounts");
    mounts.addPoint(Vec2{10, 10});
    Parameter& bore = document.addParameter("Bore", 5.5, UnitType::Millimeter);
    Parameter& deep = document.addParameter("Deep", 12.0, UnitType::Millimeter);
    const Feature& hole =
        document.addHoleFeature(body, "Holes", pad.id(), mounts.id(), bore.id(), deep.id());
    (void)document.recompute();

    const DocumentOutline outline(document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(hole.id());

    EXPECT_EQ(ValueOf(rows, "Diameter"), "5.500");
    EXPECT_EQ(ValueOf(rows, "Depth"), "12.000");

    // EDITABLE, and pointing at the PARAMETER -- a row that pointed at the
    // feature would accept typing and change nothing.
    for (const PropertyRow& row : rows) {
        if (row.label != "Diameter" && row.label != "Depth") continue;
        EXPECT_TRUE(row.editable) << row.label << " is not editable";
        EXPECT_NE(row.parameterId, kInvalidObjectId) << row.label << " writes to nothing";
        EXPECT_EQ(row.field, PropertyField::Value);
    }

    // ...AND NO "Reversed" BOX. A diameter has no other way to go, and a
    // checkbox that means nothing is worse than no checkbox.
    EXPECT_FALSE(HasRow(rows, "Reversed"))
        << "a hole was offered a direction it does not have";
}

TEST(DocumentOutlineTest, M26_PROP_011_APadStillShowsItsLengthAndItsDirection) {
    // The four kinds that DID work must keep working: the capability replaced
    // their branches, it did not merely get added alongside them.
    PartDocument document{"Part"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    Sketch& profile = document.addSketch("Profile");
    profile.addLine(Vec2{0, 0}, Vec2{50, 0});
    Parameter& thickness = document.addParameter("T", 10.0, UnitType::Millimeter);
    Body& body = document.addBody("Block");
    const Feature& pad = document.addPadFeature(body, "Pad", profile.id(), thickness.id());

    const DocumentOutline outline(document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(pad.id());
    EXPECT_EQ(ValueOf(rows, "Length"), "10.000");
    // A pad's direction lives in the SIGN of its length, so the box belongs.
    EXPECT_TRUE(HasRow(rows, "Reversed"));
}

TEST(DocumentOutlineTest, M26_PROP_012_EveryFeatureWithANumberExposesIt) {
    // THE GUARD AGAINST THE NEXT ONE. The defect was never "Hole was
    // forgotten" -- it was that forgetting is silent. A feature that stores a
    // Parameter and exposes none is the shape to catch, whoever adds it.
    PartDocument document{"Part"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);

    Sketch& profile = document.addSketch("Profile");
    profile.addLine(Vec2{0, 0}, Vec2{50, 0});
    profile.addLine(Vec2{50, 0}, Vec2{50, 50});
    profile.addLine(Vec2{50, 50}, Vec2{0, 50});
    profile.addLine(Vec2{0, 50}, Vec2{0, 0});
    Parameter& thickness = document.addParameter("T", 10.0, UnitType::Millimeter);
    Body& body = document.addBody("Block");
    const Feature& pad = document.addPadFeature(body, "Pad", profile.id(), thickness.id());
    Parameter& wall = document.addParameter("Wall", 2.0, UnitType::Millimeter);
    const Feature& shell =
        document.addShellFeature(body, "Shell", pad.id(), FaceSelection{}, wall.id());
    Parameter& count = document.addParameter("N", 3.0, UnitType::Unitless);
    Parameter& gap = document.addParameter("Gap", 20.0, UnitType::Millimeter);
    ReferenceFrame& along = document.addFrame("Along");
    const Feature& row = document.addPatternFeature(body, "Row", shell.id(), along.id(),
                                                    count.id(), gap.id());
    (void)document.recompute();

    const DocumentOutline outline(document);
    // Shell's thickness and the pattern's count and spacing: none of these had
    // a branch in the panel before M26.9.
    EXPECT_EQ(ValueOf(outline.propertiesOf(shell.id()), "Thickness"), "2.000");
    EXPECT_EQ(ValueOf(outline.propertiesOf(row.id()), "Count"), "3.000");
    EXPECT_EQ(ValueOf(outline.propertiesOf(row.id()), "Spacing"), "20.000");
}

// =============================================================================
// M26.8 -- a feature names EVERY sketch it reads
// =============================================================================

TEST(DocumentOutlineTest, M26_TREE_004_ACurvePatternsPathIsProtectedAndNotCalledFailed) {
    // CurvePatternFeature held a path sketch and did not implement
    // ISketchConsuming at all. So the path was DELETABLE while the pattern
    // still walked it, and the tree marked it Failed for not closing into a
    // loop a path is never meant to close into.
    PartDocument document{"Part"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);

    Sketch& profile = document.addSketch("Profile");
    profile.addLine(Vec2{0, 0}, Vec2{10, 0});
    profile.addLine(Vec2{10, 0}, Vec2{10, 10});
    profile.addLine(Vec2{10, 10}, Vec2{0, 10});
    profile.addLine(Vec2{0, 10}, Vec2{0, 0});
    Parameter& thickness = document.addParameter("T", 5.0, UnitType::Millimeter);
    Body& body = document.addBody("Block");
    const Feature& pad = document.addPadFeature(body, "Pad", profile.id(), thickness.id());

    Sketch& path = document.addSketch("Path");
    path.addLine(Vec2{0, 0}, Vec2{100, 0}); // an open curve, as a path is
    Parameter& count = document.addParameter("N", 4.0, UnitType::Unitless);
    document.addCurvePatternFeature(body, "Along", pad.id(), path.id(), count.id());
    (void)document.recompute();

    // THE DELETION GATE now sees it. Empty here is exactly the condition under
    // which the sketch may be deleted, and it was empty for this path.
    EXPECT_FALSE(document.featuresReferencingSketch(path.id()).empty())
        << "a curve pattern's path could be deleted out from under it";

    const DocumentOutline outline(document);
    const OutlineNode root = outline.build();
    const OutlineNode* node = FindNode(root, "Path");
    ASSERT_NE(node, nullptr);
    EXPECT_NE(node->state, OutlineState::Failed)
        << "a curve pattern's path was marked Failed for being an open curve";
}

TEST(DocumentOutlineTest, M26_TREE_005_ALoftsLATERSectionsAreProtectedToo) {
    // consumedSketchId() named the FIRST section and nothing else, so the
    // deletion gate protected section one and let the others be deleted out
    // from under the loft that runs through them.
    PartDocument document{"Part"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);

    Body& body = document.addBody("Block");
    std::vector<ObjectId> sections;
    for (int i = 0; i < 3; ++i) {
        Sketch& section = document.addSketch("Section" + std::to_string(i));
        section.addLine(Vec2{0, 0}, Vec2{10, 0});
        section.addLine(Vec2{10, 0}, Vec2{10, 10});
        section.addLine(Vec2{10, 10}, Vec2{0, 10});
        section.addLine(Vec2{0, 10}, Vec2{0, 0});
        sections.push_back(section.id());
    }
    document.addLoftFeature(body, "Loft", sections);

    for (std::size_t i = 0; i < sections.size(); ++i)
        EXPECT_FALSE(document.featuresReferencingSketch(sections[i]).empty())
            << "section " << i << " could be deleted out from under the loft";
}


TEST(DocumentOutlineTest, M26_TREE_006_ASweepsPATHIsProtectedAndNotCalledFailed) {
    // A sweep reads TWO sketches and the capability let it name one. The path
    // was therefore deletable out from under it, and the tree marked the path
    // Failed for not closing into a loop a path is never meant to close into.
    PartDocument document{"Part"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);

    Sketch& profile = document.addSketch("Profile");
    profile.addLine(Vec2{0, 0}, Vec2{10, 0});
    profile.addLine(Vec2{10, 0}, Vec2{10, 10});
    profile.addLine(Vec2{10, 10}, Vec2{0, 10});
    profile.addLine(Vec2{0, 10}, Vec2{0, 0});
    Sketch& path = document.addSketch("Spine");
    path.addLine(Vec2{0, 0}, Vec2{80, 0}); // an open curve, as a path is

    Body& body = document.addBody("Block");
    document.addSweepFeature(body, "Sweep", profile.id(), path.id());
    (void)document.recompute();

    EXPECT_FALSE(document.featuresReferencingSketch(path.id()).empty())
        << "a sweep's path could be deleted out from under it";

    const DocumentOutline outline(document);
    const OutlineNode root = outline.build();
    const OutlineNode* node = FindNode(root, "Spine");
    ASSERT_NE(node, nullptr);
    EXPECT_NE(node->state, OutlineState::Failed)
        << "a sweep's path was marked Failed for being an open curve";

    // ...and the PROFILE is still the primary: it is what became the outline.
    const OutlineNode* profileNode = FindNode(root, "Profile");
    ASSERT_NE(profileNode, nullptr);
}
