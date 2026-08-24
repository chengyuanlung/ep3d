// M27 -- the shell's tree and property panel, asked about an ASSEMBLY.
//
// AssemblyOutline is free of Qt and of OCCT for the same reason DocumentOutline
// is: what the UI decides -- what to show, in what state, with what diagnostic
// -- is testable without a display (UI spec 20). What cannot be tested here is
// whether any of it reaches the screen, and that is the viewer's --selftest.

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Assembly/Mate.h"
#include "Viewer/AssemblyOutline.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

const OutlineNode* Find(const OutlineNode& node, const std::string& name) {
    if (node.name == name) return &node;
    for (const OutlineNode& child : node.children)
        if (const OutlineNode* found = Find(child, name)) return found;
    return nullptr;
}

std::string ValueOf(const std::vector<PropertyRow>& rows, const std::string& label) {
    for (const PropertyRow& row : rows)
        if (row.label == label) return row.value;
    return std::string();
}

// Two instances and one mate: the smallest thing that is actually an assembly.
struct Rig {
    AssemblyDocument document{"Hinge"};
    ObjectId base = kInvalidObjectId;
    ObjectId swing = kInvalidObjectId;

    Rig() {
        base = document.addInstance("Base", "base.ep3d", "Body").id();
        swing = document.addInstance("Swing", "swing.ep3d", "Body").id();
        document.setInstanceGrounded(base, true);
    }
};

} // namespace

TEST(AssemblyOutlineTest, M27_TREE_001_TheTreeIsInstancesAndMatesNotAPartsRows) {
    Rig rig;
    const AssemblyOutline outline(rig.document);
    const OutlineNode root = outline.build();

    EXPECT_EQ(root.typeLabel, "Assembly");
    // [Asm], not [Part]. The tag a reader sees comes from the kind, and an
    // assembly whose root said "[Part]" contradicted the title bar, the
    // status bar and the menu that was enabled.
    EXPECT_EQ(root.kind, OutlineKind::Assembly);

    // The two things an assembly is made of, each under its own group.
    const OutlineNode* instances = Find(root, "Instances");
    ASSERT_NE(instances, nullptr) << "the tree has no Instances group";
    EXPECT_EQ(instances->children.size(), 2u);

    const OutlineNode* base = Find(root, "Base");
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(base->kind, OutlineKind::Instance);
    EXPECT_EQ(base->id, rig.base) << "the row does not carry the instance's id";

    ASSERT_NE(Find(root, "Mates"), nullptr) << "the tree has no Mates group";
}

TEST(AssemblyOutlineTest, M27_TREE_002_AGroundedInstanceSaysSoOnItsRow) {
    // Roadmap §20.3: freedom is reported PER INSTANCE, because "this assembly
    // is under-constrained" is not something a user can act on.
    Rig rig;
    const AssemblyOutline outline(rig.document);
    const OutlineNode root = outline.build();

    const OutlineNode* base = Find(root, "Base");
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(base->diagnostic, "grounded");

    // ...and the ungrounded one does NOT claim to be.
    const OutlineNode* swing = Find(root, "Swing");
    ASSERT_NE(swing, nullptr);
    EXPECT_NE(swing->diagnostic, "grounded");
}

TEST(AssemblyOutlineTest, M27_PROP_001_AnInstanceDescribesItsSOURCEAndItsPlacement) {
    Rig rig;
    const AssemblyOutline outline(rig.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(rig.swing);
    ASSERT_FALSE(rows.empty()) << "an instance describes nothing";

    EXPECT_EQ(ValueOf(rows, "Name"), "Swing");
    EXPECT_EQ(ValueOf(rows, "Type"), "Part instance");
    // THE SENTENCE, not the geometry (ADR-M22-003): what an instance stores is
    // a path re-read every rebuild, and the path is what a user needs when it
    // stops resolving.
    EXPECT_EQ(ValueOf(rows, "File"), "swing.ep3d");
    EXPECT_EQ(ValueOf(rows, "Body"), "Body");
    EXPECT_EQ(ValueOf(rows, "Grounded"), "no");
    EXPECT_FALSE(ValueOf(rows, "X").empty()) << "an instance does not say where it is";
}

TEST(AssemblyOutlineTest, M27_PROP_002_TheNameRowWritesToTheINSTANCENotAParameter) {
    // The panel writes through the document's facade, and `parameterId` on a
    // Name row carries the OBJECT's id -- the field has always meant "what to
    // write". A row pointing anywhere else would accept typing and rename
    // nothing.
    Rig rig;
    const AssemblyOutline outline(rig.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(rig.swing);

    bool sawName = false;
    for (const PropertyRow& row : rows) {
        if (row.label != "Name") continue;
        sawName = true;
        EXPECT_TRUE(row.editable);
        EXPECT_EQ(row.field, PropertyField::Name);
        EXPECT_EQ(row.parameterId, rig.swing);
    }
    EXPECT_TRUE(sawName);
}

TEST(AssemblyOutlineTest, M27_PROP_003_AnIdThatIsNotInThisAssemblyDescribesNothing) {
    // Empty, not a row of blanks. A panel full of empty cells looks like a
    // described object whose values failed to load.
    Rig rig;
    const AssemblyOutline outline(rig.document);
    EXPECT_TRUE(outline.propertiesOf(kInvalidObjectId).empty());
    EXPECT_TRUE(outline.propertiesOf(999999).empty());
}

TEST(AssemblyOutlineTest, M27_TREE_003_AnEmptyAssemblyStillHasItsGroups) {
    // A document with nothing in it must still read as an assembly. A tree that
    // showed only a bare root would look like a document that failed to load.
    AssemblyDocument empty{"Empty"};
    const AssemblyOutline outline(empty);
    const OutlineNode root = outline.build();

    EXPECT_EQ(root.name, "Empty");
    ASSERT_NE(Find(root, "Instances"), nullptr);
    ASSERT_NE(Find(root, "Mates"), nullptr);
    // ...and no named-position or exploded-view group, because there are none
    // and an empty group is a promise of something that is not there.
    EXPECT_EQ(Find(root, "Named positions"), nullptr);
    EXPECT_EQ(Find(root, "Exploded views"), nullptr);
}
