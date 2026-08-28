// M58.2 -- where the generator meets the mechanism.
//
// THE FAILURE THIS FILE IS FOR is not exotic and does not look like a bug.
//
// A pair of 20 and 40 teeth is coupled, and later somebody types into the ratio
// field -- 0.6, because that is what the pair they were sketching last week
// had. The gears still say 0.5. The mechanism turns at 0.6. Every number on the
// screen is a number somebody chose, nothing is dangling, nothing is invalid,
// and the machine is wrong.
//
// The FIRST draft of this file used a different scenario -- changing the pinion
// from 20 teeth to 24 -- and it turns out an instance's source path cannot be
// changed at all in EP3D: it is set when the instance is made and there is no
// setter. So that drift does not exist, and the one that does is the ratio
// field, which is exactly the field this milestone is about.

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Assembly/Mate.h"
#include "Core/Library/GearMesh.h"
#include "Core/Library/SpurGear.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace {

using namespace paramcad;

// Two gears on two shafts, mated to a housing so each has a rotation to
// couple. The connector names are what a part file would carry; nothing here
// needs them to resolve, because a relation couples FREEDOMS and a freedom is
// a mate and a component.
struct Drive {
    AssemblyDocument assembly{"Drive"};
    ObjectId housing = kInvalidObjectId;
    ObjectId pinion = kInvalidObjectId;
    ObjectId wheel = kInvalidObjectId;
    ObjectId pinionMate = kInvalidObjectId;
    ObjectId wheelMate = kInvalidObjectId;

    explicit Drive(const std::string& pinionPath = "gear:m2 z20 b10",
                   const std::string& wheelPath = "gear:m2 z40 b10") {
        housing = assembly.addInstance("Housing", "housing.ep3d", "").id();
        pinion = assembly.addInstance("Pinion", pinionPath, "").id();
        wheel = assembly.addInstance("Wheel", wheelPath, "").id();
        pinionMate =
            assembly.addMate("Pinion shaft", MateType::Revolute, housing, "ShaftA", pinion,
                             "Bore")
                .id();
        wheelMate = assembly.addMate("Wheel shaft", MateType::Revolute, housing, "ShaftB", wheel,
                                     "Bore")
                        .id();
    }

    CoupledFreedom driver() const { return CoupledFreedom{pinionMate, MateComponent::RZ}; }
    CoupledFreedom driven() const { return CoupledFreedom{wheelMate, MateComponent::RZ}; }
};

TEST(GearMeshTest, M58_MESH_001_TheRatioComesOffTheGearsRatherThanTheKeyboard) {
    Drive drive;
    const GearMeshResult mesh = MeshGears(drive.assembly, "Reduction", drive.pinion, drive.wheel,
                                          drive.driver(), drive.driven());
    ASSERT_TRUE(mesh.ok) << mesh.why;

    EXPECT_NEAR(mesh.ratio, -0.5, 1e-9);
    EXPECT_NEAR(mesh.centreDistanceMm, 60.0, 1e-9);
    EXPECT_GT(mesh.contactRatio, 1.2);

    // THE RELATION HOLDS A MAGNITUDE AND A FLAG, and the sign became the flag
    // in one place. Two conversions is how a pair comes to turn the same way in
    // the solve and opposite ways on the drawing.
    const Relation* relation = drive.assembly.findRelation(mesh.relationId);
    ASSERT_NE(relation, nullptr);
    EXPECT_NEAR(relation->ratio(), 0.5, 1e-9);
    EXPECT_TRUE(relation->reversed());
    // ...and the coupling reads back through the relation's own arithmetic:
    // a turn of the pinion is half a turn of the wheel, the other way.
    EXPECT_NEAR(relation->valueFor(1.0), -0.5, 1e-9);

    EXPECT_TRUE(WhyMeshDisagrees(drive.assembly, mesh.relationId).empty());
}

TEST(GearMeshTest, M58_MESH_002_ATypedRatioThatNoLongerMatchesIsSAID) {
    // THE POINT OF THE MILESTONE. Relation stores its ratio -- it is saved,
    // serialized, and EDITABLE, because for the general case of two coupled
    // rotations there are no gears to read it off and typing is the only way.
    // So the number CAN drift away from the gears, and the answer is a named
    // question that says whether it has, with every relevant number in the
    // sentence.
    //
    // The drift is not hypothetical: setRelationRatio is what the ratio field
    // in the UI calls, and a user who is used to typing it will type it.
    Drive drive;
    const GearMeshResult mesh = MeshGears(drive.assembly, "Reduction", drive.pinion, drive.wheel,
                                          drive.driver(), drive.driven());
    ASSERT_TRUE(mesh.ok) << mesh.why;
    ASSERT_TRUE(WhyMeshDisagrees(drive.assembly, mesh.relationId).empty());

    // Somebody types 0.6 -- the ratio a 24-tooth pinion would have given, on a
    // pair that still has 20 teeth on it. Nothing is dangling and nothing is
    // invalid; the mechanism simply turns at a rate the gears do not.
    ASSERT_TRUE(drive.assembly.setRelationRatio(mesh.relationId, 0.6));

    const std::string why = WhyMeshDisagrees(drive.assembly, mesh.relationId);
    EXPECT_FALSE(why.empty()) << "a 20:40 pair went on turning at 0.6";
    EXPECT_NE(why.find("20"), std::string::npos) << why;
    EXPECT_NE(why.find("40"), std::string::npos) << why;
    // BOTH RATIOS, because "the ratio is wrong" sends a reader looking for
    // what it should be.
    EXPECT_NE(why.find("-0.5"), std::string::npos) << why;
    EXPECT_NE(why.find("-0.6"), std::string::npos) << why;

    // THE DIRECTION COUNTS TOO. Clearing `reversed` leaves the magnitude
    // right and turns the wheel the wrong way -- two external gears do not do
    // that, and a check on the magnitude alone would call it fine.
    ASSERT_TRUE(drive.assembly.setRelationRatio(mesh.relationId, 0.5));
    ASSERT_TRUE(WhyMeshDisagrees(drive.assembly, mesh.relationId).empty());
    ASSERT_TRUE(drive.assembly.setRelationReversed(mesh.relationId, false));
    EXPECT_FALSE(WhyMeshDisagrees(drive.assembly, mesh.relationId).empty())
        << "the wheel was turning the same way as the pinion and nothing said so";

    // Put back through the same function that worked it out the first time,
    // and it settles.
    ASSERT_TRUE(drive.assembly.setRelationReversed(mesh.relationId, true));
    EXPECT_TRUE(WhyMeshDisagrees(drive.assembly, mesh.relationId).empty());
}

TEST(GearMeshTest, M58_MESH_003_SomethingThatIsNotAGearIsRefusedBYNAME) {
    Drive drive;
    // The housing is a file, not a gear -- so there is no tooth count to take
    // a ratio from, and saying which of the two says so is the difference
    // between a message and a shrug.
    const GearMeshResult mesh = MeshGears(drive.assembly, "Nonsense", drive.pinion, drive.housing,
                                          drive.driver(), drive.driven());
    EXPECT_FALSE(mesh.ok);
    EXPECT_NE(mesh.why.find("Housing"), std::string::npos) << mesh.why;
    EXPECT_NE(mesh.why.find("not a gear"), std::string::npos) << mesh.why;
    EXPECT_EQ(mesh.relationId, kInvalidObjectId) << "a relation was made anyway";

    // A gear cannot mesh with itself: that is not a coupling, it is an
    // equation saying a number is a multiple of itself.
    const GearMeshResult itself = MeshGears(drive.assembly, "Itself", drive.pinion, drive.pinion,
                                            drive.driver(), drive.driven());
    EXPECT_FALSE(itself.ok);
    EXPECT_NE(itself.why.find("itself"), std::string::npos) << itself.why;
}

TEST(GearMeshTest, M58_MESH_004_TwoGearsThatCannotMeshAreRefusedBeforeARelationExists) {
    // Different modules is the mistake nearly every time, and no centre
    // distance fixes it. A relation made anyway would turn two gears that
    // cannot touch.
    Drive drive{"gear:m2 z20 b10", "gear:m2.5 z40 b10"};
    const GearMeshResult mesh = MeshGears(drive.assembly, "Reduction", drive.pinion, drive.wheel,
                                          drive.driver(), drive.driven());
    EXPECT_FALSE(mesh.ok);
    EXPECT_NE(mesh.why.find("will not mesh"), std::string::npos) << mesh.why;
    EXPECT_NE(mesh.why.find("different size"), std::string::npos) << mesh.why;
    EXPECT_TRUE(drive.assembly.relations().empty()) << "a relation was made for a pair that cannot run";
}

TEST(GearMeshTest, M58_MESH_005_NOTHINGToCompareIsNotTheSameAsAGREEMENT) {
    // A relation between two parts that are not gears is a typed ratio and
    // always will be -- EP3D has coupled arbitrary rotations since M31 and that
    // does not change. Reporting it as checked would be a green light nobody
    // earned, so the check keeps quiet rather than approving.
    AssemblyDocument assembly{"Linkage"};
    const ObjectId base = assembly.addInstance("Base", "base.ep3d", "").id();
    const ObjectId armA = assembly.addInstance("ArmA", "arm.ep3d", "").id();
    const ObjectId armB = assembly.addInstance("ArmB", "arm.ep3d", "").id();
    const ObjectId mateA =
        assembly.addMate("A", MateType::Revolute, base, "P1", armA, "Pin").id();
    const ObjectId mateB =
        assembly.addMate("B", MateType::Revolute, base, "P2", armB, "Pin").id();
    const Relation& typed =
        assembly.addRelation("Coupled", RelationType::Gear,
                             CoupledFreedom{mateA, MateComponent::RZ},
                             CoupledFreedom{mateB, MateComponent::RZ}, 3.0, false);

    EXPECT_TRUE(WhyMeshDisagrees(assembly, typed.id()).empty())
        << "a coupling with no gears behind it was judged against gears it does not have";

    // An id that is not a relation at all is quiet too, rather than throwing
    // at a caller that is asking a question about something else.
    EXPECT_TRUE(WhyMeshDisagrees(assembly, base).empty());
}

TEST(GearMeshTest, M58_MESH_006_AMateWithAGearAtBOTHEndsIsAmbiguousAndIsLeftAlone) {
    // FOUND BY THE MUTATION GATE, and the case is an ordinary one: a gear pair
    // positioned by mating the two gears directly to each other, with no
    // housing in the assembly at all.
    //
    // A freedom names a MATE, and a mate has two instances. Which of them
    // turns is not written down anywhere. Where one side is a gear and the
    // other is a housing the answer is obvious and this reads it; where BOTH
    // sides are gears there is no answer, and picking the one that happens to
    // be stored first would mean comparing a gear against itself -- a ratio of
    // -1, which would then be reported as a disagreement on a pair that is
    // perfectly set up.
    //
    // Quiet is the honest output. It is not approval: nothing was checked, and
    // MESH_005 says why that matters.
    AssemblyDocument assembly{"Open pair"};
    const ObjectId pinion = assembly.addInstance("Pinion", "gear:m2 z20 b10", "").id();
    const ObjectId wheel = assembly.addInstance("Wheel", "gear:m2 z40 b10", "").id();
    const ObjectId first =
        assembly.addMate("Mesh", MateType::Revolute, pinion, "Axis", wheel, "Axis").id();
    const ObjectId second =
        assembly.addMate("Backing", MateType::Revolute, pinion, "Face", wheel, "Face").id();

    const Relation& coupled =
        assembly.addRelation("Reduction", RelationType::Gear,
                             CoupledFreedom{first, MateComponent::RZ},
                             CoupledFreedom{second, MateComponent::RZ}, 0.5, true);

    EXPECT_TRUE(WhyMeshDisagrees(assembly, coupled.id()).empty())
        << "a mate with a gear at both ends was resolved by guessing which one turns, and "
           "the guess compared a gear with itself";
}

} // namespace
