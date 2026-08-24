// M31 -- mate relations (roadmap §20.5).
//
// A relation couples two freedoms a mate solve would otherwise choose
// independently. Every test here asserts the COUPLING as an angle or a
// distance, never that the relation object exists: a relation that is stored
// and never applied is the shape this milestone exists to avoid.

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Assembly/Mate.h"
#include "Core/Assembly/Relation.h"

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <string>

namespace {

using namespace paramcad;

constexpr double kTwoPi = 6.283185307179586476925286766559;

// Three instances in a row, each with one connector, so mates can be made
// between them without a kernel: relations act on mate VALUES, and those are
// decided before any geometry is built.
struct Gearbox {
    AssemblyDocument document{"Gearbox"};
    ObjectId housing = kInvalidObjectId;
    ObjectId first = kInvalidObjectId;
    ObjectId second = kInvalidObjectId;

    Gearbox() {
        housing = document.addInstance("Housing", "housing.ep3d").id();
        first = document.addInstance("First", "wheel.ep3d").id();
        second = document.addInstance("Second", "wheel.ep3d").id();
        document.setInstanceGrounded(housing, true);
    }

    ObjectId mate(const std::string& name, MateType type, ObjectId a, ObjectId b) {
        return document.addMate(name, type, a, "Pin", b, "Bore").id();
    }
};

// WHAT THE FREEDOM ACTUALLY IS, relations included -- the same code path the
// solve places geometry with.
//
// Read here rather than through recompute() because recompute needs the part
// FILES to exist and a kernel to build them, and a relation decides a number
// long before any of that. Every mate-solve test that does need geometry lives
// in the Kernel suite; this milestone's subject is the coupling itself.
double ValueOf(const AssemblyDocument& document, ObjectId mateId, MateComponent component) {
    return document.valuesAfterRelations(mateId)[static_cast<std::size_t>(component)];
}

} // namespace

// =============================================================================
// What the four types will and will not accept
// =============================================================================

TEST(RelationTest, M31_REL_001_AGearNeedsTwoROTATIONS) {
    // §20.5: a gear couples two revolute freedoms. Offering it a translation
    // is refused BY NAME, not by producing a coupling that means nothing.
    const std::string why = WhyRelationIsRefused(
        RelationType::Gear, CoupledFreedom{1, MateComponent::RZ},
        CoupledFreedom{2, MateComponent::TZ});
    EXPECT_NE(why.find("ROTATIONS"), std::string::npos) << why;
}

TEST(RelationTest, M31_REL_002_AScrewCouplesONEMatesOwnTurnToItsOwnTravel) {
    // THE ONE-MATE CASE, and the whole reason this model couples FREEDOMS
    // rather than mates. §20.5 says Screw takes a single mate; a model built
    // on "a relation joins two things" needs it as a special case forever.
    EXPECT_TRUE(WhyRelationIsRefused(RelationType::Screw,
                                     CoupledFreedom{7, MateComponent::RZ},
                                     CoupledFreedom{7, MateComponent::TZ})
                    .empty());

    // ...and two DIFFERENT mates is a rack and pinion, which the message says.
    const std::string why = WhyRelationIsRefused(RelationType::Screw,
                                                 CoupledFreedom{7, MateComponent::RZ},
                                                 CoupledFreedom{8, MateComponent::TZ});
    EXPECT_NE(why.find("rack and pinion"), std::string::npos) << why;
}

TEST(RelationTest, M31_REL_003_ARelationCannotCoupleAFreedomToItself) {
    // An equation saying a number is a multiple of itself is true only at zero
    // or at a ratio of one, and means nothing either way.
    const std::string why = WhyRelationIsRefused(RelationType::Gear,
                                                 CoupledFreedom{1, MateComponent::RZ},
                                                 CoupledFreedom{1, MateComponent::RZ});
    EXPECT_FALSE(why.empty());
}

// =============================================================================
// What the coupling actually DOES
// =============================================================================

TEST(RelationTest, M31_REL_010_AGearTurnsItsPartnerInRatio) {
    Gearbox rig;
    const ObjectId drive = rig.mate("Drive", MateType::Revolute, rig.housing, rig.first);
    const ObjectId driven = rig.mate("Driven", MateType::Revolute, rig.housing, rig.second);

    // 2:1 -- the driven wheel turns twice for every turn of the driver.
    rig.document.addRelation("Reduction", RelationType::Gear,
                             CoupledFreedom{drive, MateComponent::RZ},
                             CoupledFreedom{driven, MateComponent::RZ}, 2.0);

    rig.document.setMateComponentValue(drive, MateComponent::RZ, 0.5);

    EXPECT_NEAR(ValueOf(rig.document, driven, MateComponent::RZ), 1.0, 1e-9)
        << "the gear did not carry the driver's angle to its partner";
    // ...and the DRIVER is untouched: a relation writes one end, not both.
    EXPECT_NEAR(ValueOf(rig.document, drive, MateComponent::RZ), 0.5, 1e-9);
}

TEST(RelationTest, M31_REL_011_ReversedTurnsTheOtherWay) {
    Gearbox rig;
    const ObjectId drive = rig.mate("Drive", MateType::Revolute, rig.housing, rig.first);
    const ObjectId driven = rig.mate("Driven", MateType::Revolute, rig.housing, rig.second);
    rig.document.addRelation("Reduction", RelationType::Gear,
                             CoupledFreedom{drive, MateComponent::RZ},
                             CoupledFreedom{driven, MateComponent::RZ}, 2.0, /*reversed=*/true);

    rig.document.setMateComponentValue(drive, MateComponent::RZ, 0.5);
    EXPECT_NEAR(ValueOf(rig.document, driven, MateComponent::RZ), -1.0, 1e-9)
        << "two meshing gears turn opposite ways, and `reversed` is how that is said";
}

TEST(RelationTest, M31_REL_012_AScrewTravelsItsLeadPerTurn) {
    // A lead screw is quoted in MILLIMETRES PER TURN, which is what `ratio`
    // means for this type -- and the conversion to radians lives in exactly
    // one place so no call site can get 2*pi wrong on its own.
    Gearbox rig;
    const ObjectId spindle =
        rig.mate("Spindle", MateType::Cylindrical, rig.housing, rig.first);
    rig.document.addRelation("Lead", RelationType::Screw,
                             CoupledFreedom{spindle, MateComponent::RZ},
                             CoupledFreedom{spindle, MateComponent::TZ}, 4.0);

    // One full turn.
    rig.document.setMateComponentValue(spindle, MateComponent::RZ, kTwoPi);
    EXPECT_NEAR(ValueOf(rig.document, spindle, MateComponent::TZ), 4.0, 1e-9)
        << "one turn of a 4 mm lead screw advances 4 mm";

    // ...and half a turn is half the lead, which is the check that the
    // conversion is a scale and not a lookup.
    rig.document.setMateComponentValue(spindle, MateComponent::RZ, kTwoPi / 2.0);
    EXPECT_NEAR(ValueOf(rig.document, spindle, MateComponent::TZ), 2.0, 1e-9);
}

TEST(RelationTest, M31_REL_013_ARackAdvancesAsItsPinionTurns) {
    Gearbox rig;
    const ObjectId pinion = rig.mate("Pinion", MateType::Revolute, rig.housing, rig.first);
    const ObjectId rack = rig.mate("Rack", MateType::Slider, rig.housing, rig.second);
    rig.document.addRelation("Drive", RelationType::RackAndPinion,
                             CoupledFreedom{pinion, MateComponent::RZ},
                             CoupledFreedom{rack, MateComponent::TZ}, 10.0);

    rig.document.setMateComponentValue(pinion, MateComponent::RZ, kTwoPi);
    EXPECT_NEAR(ValueOf(rig.document, rack, MateComponent::TZ), 10.0, 1e-9);
}

TEST(RelationTest, M31_REL_014_AGearTRAINResolvesEndToEnd) {
    // A drives B and B drives C. Applied once in storage order, C would read a
    // stale B whenever C's relation happened to be stored first -- so the
    // order relations were CREATED in would change the answer, which is the
    // kind of dependence nobody can see and everybody trips over.
    Gearbox rig;
    const ObjectId third = rig.document.addInstance("Third", "wheel.ep3d").id();
    const ObjectId a = rig.mate("A", MateType::Revolute, rig.housing, rig.first);
    const ObjectId b = rig.mate("B", MateType::Revolute, rig.housing, rig.second);
    const ObjectId c = rig.mate("C", MateType::Revolute, rig.housing, third);

    // DELIBERATELY OUT OF ORDER: the second link is created first.
    rig.document.addRelation("BtoC", RelationType::Gear, CoupledFreedom{b, MateComponent::RZ},
                             CoupledFreedom{c, MateComponent::RZ}, 3.0);
    rig.document.addRelation("AtoB", RelationType::Gear, CoupledFreedom{a, MateComponent::RZ},
                             CoupledFreedom{b, MateComponent::RZ}, 2.0);

    rig.document.setMateComponentValue(a, MateComponent::RZ, 0.1);
    EXPECT_NEAR(ValueOf(rig.document, b, MateComponent::RZ), 0.2, 1e-9);
    EXPECT_NEAR(ValueOf(rig.document, c, MateComponent::RZ), 0.6, 1e-9)
        << "the far end of the train did not follow, so the chain resolved in storage order";
}

TEST(RelationTest, M31_REL_015_TwoRelationsCannotDriveTheSameFreedom) {
    // Two answers to one question: the solve would take whichever ran last,
    // and which that is depends on storage order.
    Gearbox rig;
    const ObjectId a = rig.mate("A", MateType::Revolute, rig.housing, rig.first);
    const ObjectId b = rig.mate("B", MateType::Revolute, rig.housing, rig.second);
    rig.document.addRelation("First", RelationType::Gear, CoupledFreedom{a, MateComponent::RZ},
                             CoupledFreedom{b, MateComponent::RZ}, 2.0);

    EXPECT_THROW(rig.document.addRelation("Second", RelationType::Gear,
                                          CoupledFreedom{a, MateComponent::RZ},
                                          CoupledFreedom{b, MateComponent::RZ}, 3.0),
                 std::invalid_argument);
}

TEST(RelationTest, M31_REL_016_AFreedomTheMateDoesNotHaveIsRefused) {
    // A slider has no rotation to gear to. Caught at creation with a message
    // naming the mate type, rather than as a coupling that quietly writes a
    // pinned component the solve then ignores.
    Gearbox rig;
    const ObjectId slide = rig.mate("Slide", MateType::Slider, rig.housing, rig.first);
    const ObjectId turn = rig.mate("Turn", MateType::Revolute, rig.housing, rig.second);

    EXPECT_THROW(rig.document.addRelation("Bad", RelationType::Gear,
                                          CoupledFreedom{slide, MateComponent::RZ},
                                          CoupledFreedom{turn, MateComponent::RZ}, 1.0),
                 std::invalid_argument);
}

TEST(RelationTest, M31_REL_017_ChangingTheRatioMovesTheDrivenEnd) {
    // A ratio nobody can change is a constant. This is what makes a relation
    // parametric rather than a one-off number typed at creation.
    Gearbox rig;
    const ObjectId drive = rig.mate("Drive", MateType::Revolute, rig.housing, rig.first);
    const ObjectId driven = rig.mate("Driven", MateType::Revolute, rig.housing, rig.second);
    const ObjectId relation =
        rig.document
            .addRelation("Reduction", RelationType::Gear,
                         CoupledFreedom{drive, MateComponent::RZ},
                         CoupledFreedom{driven, MateComponent::RZ}, 2.0)
            .id();

    rig.document.setMateComponentValue(drive, MateComponent::RZ, 0.5);
    EXPECT_NEAR(ValueOf(rig.document, driven, MateComponent::RZ), 1.0, 1e-9);

    ASSERT_TRUE(rig.document.setRelationRatio(relation, 4.0));
    EXPECT_NEAR(ValueOf(rig.document, driven, MateComponent::RZ), 2.0, 1e-9);
}
