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
#include "Core/Serialization/AssemblyDocumentSerializer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
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

std::string SaveToString(const AssemblyDocument& document) {
    std::ostringstream out;
    EXPECT_TRUE(saveAssemblyDocument(document, out));
    return out.str();
}

AssemblyLoadResult LoadFromString(const std::string& text) {
    std::istringstream in(text);
    return loadAssemblyDocument(in);
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
    rig.document.addRelation("Reduction", RelationType::Gear,
                             CoupledFreedom{a, MateComponent::RZ},
                             CoupledFreedom{b, MateComponent::RZ}, 2.0);

    // A DIFFERENT NAME, so what this refuses is the second DRIVER and not the
    // name -- "First" and "Second" are instances in this rig, and a test that
    // passed on the name check would be testing the wrong rule.
    EXPECT_THROW(rig.document.addRelation("Overdrive", RelationType::Gear,
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

// =============================================================================
// M31.2 -- a relation that survives the file, and one that can be taken back
// =============================================================================

TEST(RelationTest, M31_SER_001_ARelationSurvivesASaveAndAReopenAndSTILLDrives) {
    // NOT "the relation is in the file". A relation that comes back as an
    // object nobody applies is exactly the shape this milestone exists to
    // avoid, so the check is the DRIVEN ANGLE after the reopen.
    Gearbox rig;
    const ObjectId drive = rig.mate("Drive", MateType::Revolute, rig.housing, rig.first);
    const ObjectId driven = rig.mate("Driven", MateType::Revolute, rig.housing, rig.second);
    rig.document.addRelation("Reduction", RelationType::Gear,
                             CoupledFreedom{drive, MateComponent::RZ},
                             CoupledFreedom{driven, MateComponent::RZ}, 2.0, /*reversed=*/true);
    rig.document.setMateComponentValue(drive, MateComponent::RZ, 0.5);

    const std::string text = SaveToString(rig.document);
    const AssemblyLoadResult loaded = LoadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    const AssemblyDocument& back = *loaded.document;

    const Relation* relation = back.findRelationNamed("Reduction");
    ASSERT_NE(relation, nullptr) << "the relation did not survive the save";
    EXPECT_EQ(relation->type(), RelationType::Gear);
    EXPECT_NEAR(relation->ratio(), 2.0, 1e-12);
    EXPECT_TRUE(relation->reversed()) << "the direction was lost, so the gears now turn together";

    const Mate* driveBack = back.findMateNamed("Drive");
    const Mate* drivenBack = back.findMateNamed("Driven");
    ASSERT_NE(driveBack, nullptr);
    ASSERT_NE(drivenBack, nullptr);
    EXPECT_NEAR(ValueOf(back, drivenBack->id(), MateComponent::RZ), -1.0, 1e-9)
        << "the reopened relation is stored but not applied";

    // A loaded document has nothing to undo, and saving it again writes the
    // same bytes -- the two properties every other kind in this format has.
    EXPECT_EQ(back.undoDepth(), 0u);
    EXPECT_EQ(SaveToString(back), text);
}

TEST(RelationTest, M31_SER_002_ARelationNamingAMateThatIsGoneIsREFUSED) {
    // ADR-M3-008 at the loader's door: a reference that cannot resolve is
    // refused by name, not loaded as a relation that drives nothing.
    Gearbox rig;
    const ObjectId drive = rig.mate("Drive", MateType::Revolute, rig.housing, rig.first);
    const ObjectId driven = rig.mate("Driven", MateType::Revolute, rig.housing, rig.second);
    rig.document.addRelation("Reduction", RelationType::Gear,
                             CoupledFreedom{drive, MateComponent::RZ},
                             CoupledFreedom{driven, MateComponent::RZ}, 2.0);

    std::string text = SaveToString(rig.document);
    const std::string real = "\"driverMateId\": \"" + std::to_string(drive) + "\"";
    const std::size_t at = text.find(real);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, real.size(), "\"driverMateId\": \"777333\"");

    const AssemblyLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::UnknownDependencyId);
    EXPECT_NE(loaded.message.find("777333"), std::string::npos) << loaded.message;
}

TEST(RelationTest, M31_SER_003_AFileWhereTwoRelationsDriveOneFreedomIsREFUSED) {
    // The facade refuses this at creation; the loader has to refuse it too, or
    // a hand-edited file walks straight past the rule and the solve takes
    // whichever relation happened to be stored last.
    Gearbox rig;
    const ObjectId a = rig.mate("A", MateType::Revolute, rig.housing, rig.first);
    const ObjectId b = rig.mate("B", MateType::Revolute, rig.housing, rig.second);
    rig.document.addRelation("Reduction", RelationType::Gear,
                             CoupledFreedom{a, MateComponent::RZ},
                             CoupledFreedom{b, MateComponent::RZ}, 2.0);

    // Duplicate the single relation entry, with a fresh id and name.
    std::string text = SaveToString(rig.document);
    const Relation* only = rig.document.findRelationNamed("Reduction");
    ASSERT_NE(only, nullptr);
    const std::string entry = "\"id\": \"" + std::to_string(only->id()) + "\"";
    const std::size_t at = text.find("\"relations\":");
    ASSERT_NE(at, std::string::npos) << text;
    const std::size_t idAt = text.find(entry, at);
    ASSERT_NE(idAt, std::string::npos) << text;
    const std::size_t open = text.rfind('{', idAt);
    const std::size_t close = text.find('}', idAt);
    ASSERT_NE(open, std::string::npos);
    ASSERT_NE(close, std::string::npos);
    std::string copy = text.substr(open, close - open + 1);
    copy.replace(copy.find(entry), entry.size(), "\"id\": \"909091\"");
    copy.replace(copy.find("\"Reduction\""), 11, "\"Overdrive\"");
    text.insert(close + 1, ", " + copy);

    const AssemblyLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded) << "a file with two answers for one freedom was accepted";
    EXPECT_EQ(loaded.error, SerializationError::InvalidDependency);
}

TEST(RelationTest, M31_SER_004_AFileCouplingAFreedomAMateDoesNotHaveIsREFUSED) {
    // A slider has no rotation. Accepting this would give the file a relation
    // writing a component the solve pins to zero -- a control with nothing
    // behind it, exactly as a limit on a pinned component would be.
    Gearbox rig;
    const ObjectId slide = rig.mate("Slide", MateType::Slider, rig.housing, rig.first);
    const ObjectId turn = rig.mate("Turn", MateType::Revolute, rig.housing, rig.second);
    rig.document.addRelation("Feed", RelationType::RackAndPinion,
                             CoupledFreedom{turn, MateComponent::RZ},
                             CoupledFreedom{slide, MateComponent::TZ}, 10.0);

    std::string text = SaveToString(rig.document);
    // Point the DRIVEN end at the slider's rotation, which it has not got.
    const std::size_t at = text.find("\"drivenComponent\": 2");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"drivenComponent\": 2").size(), "\"drivenComponent\": 5");

    const AssemblyLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::InvalidFieldType);
}

TEST(RelationTest, M31_UNDO_001_AddingARelationIsONEUndoStepAndGivesTheFreedomBack) {
    Gearbox rig;
    const ObjectId drive = rig.mate("Drive", MateType::Revolute, rig.housing, rig.first);
    const ObjectId driven = rig.mate("Driven", MateType::Revolute, rig.housing, rig.second);
    rig.document.setMateComponentValue(driven, MateComponent::RZ, 0.25);
    rig.document.setMateComponentValue(drive, MateComponent::RZ, 0.5);

    const std::size_t before = rig.document.undoDepth();
    rig.document.addRelation("Reduction", RelationType::Gear,
                             CoupledFreedom{drive, MateComponent::RZ},
                             CoupledFreedom{driven, MateComponent::RZ}, 2.0);
    EXPECT_EQ(rig.document.undoDepth(), before + 1);
    EXPECT_NEAR(ValueOf(rig.document, driven, MateComponent::RZ), 1.0, 1e-9);

    ASSERT_TRUE(rig.document.undo());
    EXPECT_EQ(rig.document.findRelationNamed("Reduction"), nullptr);
    // ...and the freedom is the MATE'S OWN NUMBER again, not the last thing
    // the relation wrote into it.
    EXPECT_NEAR(ValueOf(rig.document, driven, MateComponent::RZ), 0.25, 1e-9)
        << "the driven mate kept the angle a relation that no longer exists gave it";

    ASSERT_TRUE(rig.document.redo());
    ASSERT_NE(rig.document.findRelationNamed("Reduction"), nullptr);
    EXPECT_NEAR(ValueOf(rig.document, driven, MateComponent::RZ), 1.0, 1e-9);
}

TEST(RelationTest, M31_UNDO_002_ChangingTheRatioIsUndoableAndMOVESTheDrivenEndBack) {
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

    ASSERT_TRUE(rig.document.setRelationRatio(relation, 4.0));
    ASSERT_TRUE(rig.document.setRelationReversed(relation, true));
    EXPECT_NEAR(ValueOf(rig.document, driven, MateComponent::RZ), -2.0, 1e-9);

    ASSERT_TRUE(rig.document.undo()); // the direction
    EXPECT_NEAR(ValueOf(rig.document, driven, MateComponent::RZ), 2.0, 1e-9);
    ASSERT_TRUE(rig.document.undo()); // the ratio
    EXPECT_NEAR(ValueOf(rig.document, driven, MateComponent::RZ), 1.0, 1e-9)
        << "the ratio came back but the driven end stayed where the new one put it";
    ASSERT_NE(rig.document.findRelation(relation), nullptr);
    EXPECT_NEAR(rig.document.findRelation(relation)->ratio(), 2.0, 1e-12);
}

TEST(RelationTest, M31_UNDO_003_DeletingTheMateTakesTheRelationAndONEUndoBringsBothBack) {
    // A relation names a mate's freedom, so it cannot outlive that mate --
    // and the user deleted ONE thing, so one undo must put back everything
    // that went with it. Two steps would let the user stop halfway, at a
    // state the model was never in.
    Gearbox rig;
    const ObjectId drive = rig.mate("Drive", MateType::Revolute, rig.housing, rig.first);
    const ObjectId driven = rig.mate("Driven", MateType::Revolute, rig.housing, rig.second);
    rig.document.addRelation("Reduction", RelationType::Gear,
                             CoupledFreedom{drive, MateComponent::RZ},
                             CoupledFreedom{driven, MateComponent::RZ}, 2.0);

    const std::size_t before = rig.document.undoDepth();
    ASSERT_TRUE(rig.document.removeObject(drive));
    EXPECT_EQ(rig.document.findRelationNamed("Reduction"), nullptr)
        << "a relation outlived the mate whose freedom it reads";
    EXPECT_EQ(rig.document.undoDepth(), before + 1) << "one deletion, one undo step";

    ASSERT_TRUE(rig.document.undo());
    EXPECT_NE(rig.document.findMateNamed("Drive"), nullptr);
    const Relation* back = rig.document.findRelationNamed("Reduction");
    ASSERT_NE(back, nullptr) << "the relation did not come back with its mate";
    EXPECT_NEAR(back->ratio(), 2.0, 1e-12);

    // ...and it drives again, which is the part a restored-but-inert object
    // would fail.
    rig.document.setMateComponentValue(rig.document.findMateNamed("Drive")->id(),
                                       MateComponent::RZ, 0.5);
    EXPECT_NEAR(ValueOf(rig.document, driven, MateComponent::RZ), 1.0, 1e-9);
}

TEST(RelationTest, M31_UNDO_004_DeletingARelationOnItsOwnIsUndoable) {
    Gearbox rig;
    const ObjectId drive = rig.mate("Drive", MateType::Revolute, rig.housing, rig.first);
    const ObjectId driven = rig.mate("Driven", MateType::Revolute, rig.housing, rig.second);
    const ObjectId relation =
        rig.document
            .addRelation("Reduction", RelationType::Gear,
                         CoupledFreedom{drive, MateComponent::RZ},
                         CoupledFreedom{driven, MateComponent::RZ}, 2.0, /*reversed=*/true)
            .id();

    ASSERT_TRUE(rig.document.removeObject(relation));
    EXPECT_EQ(rig.document.findRelation(relation), nullptr);
    ASSERT_TRUE(rig.document.undo());
    const Relation* back = rig.document.findRelation(relation);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->name(), "Reduction");
    EXPECT_TRUE(back->reversed()) << "the relation came back turning the wrong way";
}

// =============================================================================
// A relation is a NAMED OBJECT, like every other one
// =============================================================================
//
// This project's signature defect: a new kind added to SOME of the parallel
// lists that all have to agree. `ownObjectName`, `applyOwnName` and
// `ownNameIsTaken` are three such lists, and M31's first draft was in none of
// them -- so a relation could take a name a mate already had, could not be
// renamed, and had no name for its own deletion label to print.

TEST(RelationTest, M31_NAME_001_TwoRelationsCannotShareAName) {
    Gearbox rig;
    const ObjectId a = rig.mate("A", MateType::Revolute, rig.housing, rig.first);
    const ObjectId b = rig.mate("B", MateType::Revolute, rig.housing, rig.second);
    const ObjectId third = rig.document.addInstance("Third", "wheel.ep3d").id();
    const ObjectId c = rig.mate("C", MateType::Revolute, rig.housing, third);

    rig.document.addRelation("Reduction", RelationType::Gear,
                             CoupledFreedom{a, MateComponent::RZ},
                             CoupledFreedom{b, MateComponent::RZ}, 2.0);
    EXPECT_THROW(rig.document.addRelation("Reduction", RelationType::Gear,
                                          CoupledFreedom{a, MateComponent::RZ},
                                          CoupledFreedom{c, MateComponent::RZ}, 3.0),
                 std::invalid_argument);
}

TEST(RelationTest, M31_NAME_002_ARelationCannotTakeANameAMateAlreadyHas) {
    // Names are unique across the WHOLE document, not within a kind. A rule
    // enforced per-kind is a rule that fails the first time somebody renames
    // across kinds.
    Gearbox rig;
    const ObjectId a = rig.mate("Drive", MateType::Revolute, rig.housing, rig.first);
    const ObjectId b = rig.mate("Driven", MateType::Revolute, rig.housing, rig.second);
    EXPECT_THROW(rig.document.addRelation("Drive", RelationType::Gear,
                                          CoupledFreedom{a, MateComponent::RZ},
                                          CoupledFreedom{b, MateComponent::RZ}, 2.0),
                 std::invalid_argument);
}

TEST(RelationTest, M31_NAME_003_ARelationCanBeRenamedAndTheRenameIsUndoable) {
    Gearbox rig;
    const ObjectId a = rig.mate("Drive", MateType::Revolute, rig.housing, rig.first);
    const ObjectId b = rig.mate("Driven", MateType::Revolute, rig.housing, rig.second);
    const ObjectId relation =
        rig.document
            .addRelation("Reduction", RelationType::Gear,
                         CoupledFreedom{a, MateComponent::RZ},
                         CoupledFreedom{b, MateComponent::RZ}, 2.0)
            .id();

    // ...and the document can SAY its name, which is what the tree row, the
    // property panel and the deletion label all read.
    EXPECT_EQ(rig.document.objectName(relation), "Reduction");

    const auto renamed = rig.document.renameObject(relation, "Step down");
    ASSERT_TRUE(renamed.ok) << renamed.message;
    EXPECT_EQ(rig.document.findRelation(relation)->name(), "Step down");
    ASSERT_TRUE(rig.document.undo());
    EXPECT_EQ(rig.document.findRelation(relation)->name(), "Reduction");
}

// =============================================================================
// The two ways of saying which freedom each end is, and that they AGREE
// =============================================================================

TEST(RelationTest, M31_KIND_001_EveryTypesOwnFreEdomKindsAreOnesItWillACCEPT) {
    // TWO THINGS THAT MUST AGREE, and this project's whole defect class.
    //
    // `WhyRelationIsRefused` says which pairs are acceptable, backwards -- you
    // offer it a pair and it tells you no. `RelationDriverIsRotation` and
    // `RelationDrivenIsRotation` say the same table FORWARDS, so a caller that
    // has to CHOOSE a pair does not have to guess and be refused. Two readings
    // of one rule is exactly the shape that drifts.
    //
    // Found by mutation: making a rack and pinion drive a ROTATION was killed
    // by nothing, because every test that built a relation through the menu or
    // the script used a gear -- and for a gear the two readings agree.
    struct Case {
        RelationType type;
        bool oneMate;
    };
    const Case kCases[] = {{RelationType::Gear, false},
                           {RelationType::RackAndPinion, false},
                           {RelationType::Screw, true},
                           {RelationType::Linear, false}};
    for (const Case& one : kCases) {
        const auto pick = [](bool rotation, ObjectId mateId) {
            return CoupledFreedom{mateId, rotation ? MateComponent::RZ : MateComponent::TZ};
        };
        const ObjectId driverMate = 41;
        const ObjectId drivenMate = one.oneMate ? driverMate : 42;
        const std::string why = WhyRelationIsRefused(
            one.type, pick(RelationDriverIsRotation(one.type), driverMate),
            pick(RelationDrivenIsRotation(one.type), drivenMate));
        EXPECT_TRUE(why.empty())
            << toString(one.type) << " refuses the very freedoms its own table chooses: " << why;
    }
}

TEST(RelationTest, M31_KIND_002_FirstFreeComponentOfKindPicksTheKindItWasAskedFor) {
    // A slider has a translation and no rotation; a revolute the other way
    // round. Asking for the kind a mate has not got returns the count, which
    // the callers report rather than working around.
    const MateFreedom slider = FreedomOf(MateType::Slider);
    EXPECT_EQ(FirstFreeComponentOfKind(slider, /*rotation=*/true), kMateComponentCount);
    EXPECT_LT(FirstFreeComponentOfKind(slider, /*rotation=*/false), kMateComponentCount);
    EXPECT_FALSE(IsRotation(static_cast<MateComponent>(
        FirstFreeComponentOfKind(slider, /*rotation=*/false))));

    const MateFreedom revolute = FreedomOf(MateType::Revolute);
    EXPECT_EQ(FirstFreeComponentOfKind(revolute, /*rotation=*/false), kMateComponentCount);
    EXPECT_TRUE(IsRotation(static_cast<MateComponent>(
        FirstFreeComponentOfKind(revolute, /*rotation=*/true))));

    // A CYLINDRICAL mate has BOTH, which is the case a screw needs -- and the
    // reason this asks for a kind rather than for "the first free one".
    const MateFreedom cylindrical = FreedomOf(MateType::Cylindrical);
    const std::size_t turns = FirstFreeComponentOfKind(cylindrical, /*rotation=*/true);
    const std::size_t slides = FirstFreeComponentOfKind(cylindrical, /*rotation=*/false);
    EXPECT_LT(turns, kMateComponentCount);
    EXPECT_LT(slides, kMateComponentCount);
    EXPECT_NE(turns, slides);
}
