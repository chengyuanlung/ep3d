// M32.0 -- one list of named objects, three questions derived from it.
//
// "What is this called", "write this name onto it" and "is this name taken"
// used to be three hand-kept walks PER DOCUMENT TYPE, and a fourth copy sat
// dead in PartDocument.cpp. A kind added to some of them and not the others is
// this project's signature defect: M31 shipped a relation that was in NONE of
// the assembly's three, so it could take a name a mate already had and then
// could not be renamed to anything.
//
// These tests do not check a list. They check the PROPERTY the list exists to
// produce -- that every object a document names is answerable all three ways --
// which is a claim that stays true when a new kind arrives, or fails loudly.

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Assembly/Mate.h"
#include "Core/Assembly/Relation.h"
#include "Core/Document/PartDocument.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Geometry/Transform.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace paramcad;

// Every object the document says it names, as (id, name).
//
// Read through the PUBLIC readers rather than through the visitor, because a
// test that walked the visitor would be checking the visitor against itself.
struct Named {
    ObjectId id;
    std::string name;
};

// A part with one of every kind it can name.
struct PopulatedPart {
    PartDocument document{"NamedPart"};
    PopulatedPart() {
        document.addParameter("Width", 40.0, UnitType::Millimeter);
        Sketch& sketch = document.addSketch("Face");
        (void)sketch;
        Body& body = document.addBody("Solid");
        (void)body;
        document.addMaterial("Aluminium", 2700.0);
    }
};

// ...and an assembly with one of every kind IT can name, relations included.
struct PopulatedAssembly {
    AssemblyDocument document{"NamedRig"};
    PopulatedAssembly() {
        Instance& base = document.addInstance("Base", "parts/base.ep3d");
        Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
        Instance& other = document.addInstance("Other", "parts/arm.ep3d");
        document.setInstanceGrounded(base.id(), true);
        Mate& first = document.addMate("Hinge", MateType::Revolute, base.id(), "P", arm.id(), "Q");
        Mate& second =
            document.addMate("Wrist", MateType::Revolute, base.id(), "P", other.id(), "Q");
        document.addRelation("Reduction", RelationType::Gear,
                             CoupledFreedom{first.id(), MateComponent::RZ},
                             CoupledFreedom{second.id(), MateComponent::RZ}, 2.0);
        document.captureNamedPosition("Shut");
        ExplodeView& view = document.addExplodeView("Apart");
        document.addExplodeStep(view.id(), "Lift", arm.id(), Vec3{0, 0, 20});
    }
};

// THE PROPERTY, checked against a list of ids the test names explicitly.
//
// An explicit list, because "everything the visitor visits" would pass on a
// visitor that visits nothing -- which is precisely the hole this replaced.
void ExpectAnswerableThreeWays(DocumentBase& document, const std::vector<Named>& expected) {
    for (const Named& one : expected) {
        // 1. It has that name.
        EXPECT_EQ(document.objectName(one.id), one.name)
            << "the document cannot say what " << one.name << " is called";
        // 2. That name is taken -- by it, and not by anything else.
        EXPECT_FALSE(document.unusedNameLike(one.name) == one.name)
            << one.name << " is not seen as taken, so another object could take it";
        // 3. It can be renamed, and the new name is what comes back.
        const std::string renamed = one.name + " renamed";
        const auto result = document.renameObject(one.id, renamed);
        EXPECT_TRUE(result.ok) << one.name << ": " << result.message;
        EXPECT_EQ(document.objectName(one.id), renamed)
            << one.name << " reported a successful rename and kept its old name";
        // ...and put it back, so the ids stay describable for the next one.
        EXPECT_TRUE(document.renameObject(one.id, one.name).ok);
    }
}

} // namespace

TEST(NamedObjectTest, M32_NAME_001_EveryKindAPartNamesIsAnswerableThreeWays) {
    PopulatedPart rig;
    std::vector<Named> expected;
    for (const auto& parameter : rig.document.parameters().items())
        expected.push_back(Named{parameter->id(), parameter->name()});
    for (const Sketch* sketch : rig.document.sketches())
        expected.push_back(Named{sketch->id(), sketch->name()});
    for (const auto& body : rig.document.bodies()) {
        expected.push_back(Named{body->id(), body->name()});
        for (const auto& feature : body->features())
            expected.push_back(Named{feature->id(), feature->name()});
    }
    if (rig.document.material() != nullptr)
        expected.push_back(Named{rig.document.material()->id(), rig.document.material()->name()});

    ASSERT_GE(expected.size(), 4u) << "this rig stopped covering the kinds a part names";
    ExpectAnswerableThreeWays(rig.document, expected);
}

TEST(NamedObjectTest, M32_NAME_002_EveryKindAnAssemblyNamesIsAnswerableThreeWays) {
    // THE TEST M31 DID NOT HAVE. A relation is in this list, so a relation that
    // fell out of the walk fails here rather than the next time somebody tries
    // to rename one.
    PopulatedAssembly rig;
    std::vector<Named> expected;
    for (const Instance* one : rig.document.instances())
        expected.push_back(Named{one->id(), one->name()});
    for (const Mate* one : rig.document.mates())
        expected.push_back(Named{one->id(), one->name()});
    for (const Relation* one : rig.document.relations())
        expected.push_back(Named{one->id(), one->name()});
    for (const NamedPosition* one : rig.document.namedPositions())
        expected.push_back(Named{one->id(), one->name()});
    for (const ExplodeView* one : rig.document.explodeViews())
        expected.push_back(Named{one->id(), one->name()});

    ASSERT_GE(expected.size(), 8u) << "this rig stopped covering the kinds an assembly names";
    ExpectAnswerableThreeWays(rig.document, expected);
}

TEST(NamedObjectTest, M32_NAME_003_ANameIsUniqueACROSSKindsNotWithinOne) {
    // Uniqueness is a property of the DOCUMENT. A rule enforced per kind is a
    // rule that fails the first time two kinds meet -- which is how a relation
    // called "Hinge" could sit beside a mate called "Hinge".
    PopulatedAssembly rig;
    const auto clash = rig.document.renameObject(rig.document.findInstanceNamed("Arm")->id(),
                                                 "Hinge");
    EXPECT_FALSE(clash.ok) << "an instance took a mate's name";
    EXPECT_NE(clash.message.find("taken"), std::string::npos) << clash.message;
}

TEST(NamedObjectTest, M32_NAME_004_AnInstancesPlacementFrameFollowsItsName) {
    // The one rename that does more than write a string. It survived the
    // collapse into a single walk, which is the thing worth pinning: a
    // refactor that flattened every setter into `setName` would have lost it
    // silently, and the tree would show "Arm origin" under "Elbow".
    PopulatedAssembly rig;
    const Instance* arm = rig.document.findInstanceNamed("Arm");
    ASSERT_NE(arm, nullptr);
    ASSERT_TRUE(rig.document.renameObject(arm->id(), "Elbow").ok);
    const std::string frameName = rig.document.objectName(arm->frameId());
    EXPECT_NE(frameName.find("Elbow"), std::string::npos)
        << "the placement frame kept the old name: " << frameName;
}
