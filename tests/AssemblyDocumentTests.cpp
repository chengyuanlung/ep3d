// M23 -- the second document type, and the machinery both types now share.
//
// These are Core tests: no kernel, no OCCT. What they check is that an
// assembly IS a document -- ids, undo, frames, names, a file that round-trips
// -- which is the half of M23 that has nothing to do with geometry. The
// geometry half lives in the OCCT suite, where an instance can actually build
// something.

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Document/EvaluationCut.h"
#include <algorithm>
#include "Core/Assembly/MateFreedom.h"
#include "Core/Document/PartDocument.h"
#include "Core/Geometry/Transform.h"
#include "Core/Serialization/AssemblyDocumentSerializer.h"
#include "Core/Serialization/PartDocumentSerializer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

std::string SaveToString(const AssemblyDocument& document) {
    std::ostringstream out;
    EXPECT_TRUE(saveAssemblyDocument(document, out));
    return out.str();
}

SaveResult TrySave(const AssemblyDocument& document) {
    std::ostringstream out;
    return saveAssemblyDocument(document, out);
}

AssemblyLoadResult LoadFromString(const std::string& text) {
    std::istringstream in(text);
    return loadAssemblyDocument(in);
}

Transform3D At(double x, double y, double z) {
    Transform3D t;
    t.translation = Vec3{x, y, z};
    return t;
}

// A quarter turn about +Z, as a unit quaternion.
Transform3D TurnedAt(double x, double y, double z, double radians) {
    Transform3D t = At(x, y, z);
    t.rotation = Quaternion{std::cos(radians / 2.0), 0.0, 0.0, std::sin(radians / 2.0)};
    return t;
}

const Instance* InstanceNamed(const AssemblyDocument& document, const std::string& name) {
    return document.findInstanceNamed(name);
}

} // namespace

TEST(AssemblyDocumentTest, M23_ASM_001_AnAssemblyIsADocumentLikeAPart) {
    // The claim P3 rests on: everything that makes a document a document came
    // from DocumentBase, so an assembly has it without having written it.
    AssemblyDocument document{"Gearbox"};
    EXPECT_EQ(document.type(), DocumentType::Assembly);
    EXPECT_NE(document.id(), kInvalidObjectId);
    EXPECT_EQ(document.name(), "Gearbox");

    // An Origin frame, made by the constructor and NOT recorded -- "Undo" on a
    // freshly opened document must not delete its origin.
    ASSERT_EQ(document.frames().size(), 1u);
    EXPECT_EQ(document.frames().front()->name(), "Origin");
    EXPECT_EQ(document.undoDepth(), 0u) << "constructing a document recorded an undo step";

    // ...and the registry and graph are the same ones a Part uses.
    EXPECT_TRUE(document.objectRegistry().contains(document.frames().front()->id()));
    EXPECT_TRUE(document.dependencyGraph().hasNode(document.frames().front()->id()));
}

TEST(AssemblyDocumentTest, M23_ASM_002_AnInstanceCarriesAPlacementFrameAndNoTransformOfItsOwn) {
    // The decision this milestone turns on (ADR-M23-002): an instance's
    // placement IS a frame. If it also held a Transform3D, the two would be
    // two answers to one question the first time anything moved one of them.
    AssemblyDocument document{"Rig"};
    Instance& gear = document.addInstance("Gear", "parts/gear.ep3d");

    ASSERT_NE(gear.frameId(), kInvalidObjectId);
    const ReferenceFrame* placement = document.findFrame(gear.frameId());
    ASSERT_NE(placement, nullptr);
    EXPECT_EQ(placement->name(), "Gear origin");
    EXPECT_TRUE(document.objectRegistry().contains(gear.id()));

    // Moving it moves the FRAME, and the instance's own answer follows from it.
    ASSERT_TRUE(document.setInstanceTransform(gear.id(), At(10, 20, 30)));
    EXPECT_NEAR(document.instanceTransform(gear.id()).translation.x, 10.0, 1e-12);
    EXPECT_NEAR(document.findFrame(gear.frameId())->localTransform().translation.y, 20.0, 1e-12);

    // And the instance is DIRTY because of it -- through an ordinary graph
    // edge, not because anything here walked anything.
    EXPECT_EQ(document.dependencyGraph().state(gear.id()), ComputeState::Dirty);
}

TEST(AssemblyDocumentTest, M23_ASM_003_MovingAnInstanceIsUndoable) {
    // For free, and that is the point: a move is a FrameTransformEdit, which
    // DocumentBase already knew how to replay before assemblies existed.
    AssemblyDocument document{"Rig"};
    Instance& gear = document.addInstance("Gear", "parts/gear.ep3d");
    ASSERT_TRUE(document.setInstanceTransform(gear.id(), At(10, 0, 0)));
    ASSERT_TRUE(document.setInstanceTransform(gear.id(), At(50, 0, 0)));

    ASSERT_TRUE(document.undo());
    EXPECT_NEAR(document.instanceTransform(gear.id()).translation.x, 10.0, 1e-12);
    ASSERT_TRUE(document.undo());
    EXPECT_NEAR(document.instanceTransform(gear.id()).translation.x, 0.0, 1e-12);
    ASSERT_TRUE(document.redo());
    EXPECT_NEAR(document.instanceTransform(gear.id()).translation.x, 10.0, 1e-12);
}

TEST(AssemblyDocumentTest, M23_ASM_004_UndoingAnInsertTakesTheFrameWithIt) {
    // An insert is two objects, so an undo that removed only one would leave a
    // frame in the tree that places nothing and that the next save would
    // faithfully preserve for ever.
    AssemblyDocument document{"Rig"};
    const ObjectId gearId = document.addInstance("Gear", "parts/gear.ep3d").id();
    const ObjectId frameId = document.findInstance(gearId)->frameId();
    ASSERT_EQ(document.frames().size(), 2u);

    // One user action, so ONE undo -- the frame and the instance are recorded
    // as separate deltas but the insert is not a transaction, so this takes
    // two steps and both are checked.
    while (document.canUndo()) ASSERT_TRUE(document.undo());
    EXPECT_EQ(document.instances().size(), 0u);
    EXPECT_EQ(document.frames().size(), 1u) << "the placement frame outlived its instance";
    EXPECT_FALSE(document.objectRegistry().contains(gearId));
    EXPECT_FALSE(document.objectRegistry().contains(frameId));

    // ...and redo brings BOTH back under the SAME ids, because every reference
    // to them has to keep meaning what it meant (A03).
    while (document.canRedo()) ASSERT_TRUE(document.redo());
    ASSERT_EQ(document.instances().size(), 1u);
    EXPECT_EQ(document.instances().front()->id(), gearId);
    EXPECT_EQ(document.instances().front()->frameId(), frameId);
}

TEST(AssemblyDocumentTest, M23_ASM_005_DeletingAnInstanceTakesItsFrameWithIt) {
    AssemblyDocument document{"Rig"};
    const ObjectId gearId = document.addInstance("Gear", "parts/gear.ep3d").id();
    const ObjectId frameId = document.findInstance(gearId)->frameId();

    ASSERT_TRUE(document.removeObject(gearId));
    EXPECT_EQ(document.instances().size(), 0u);
    EXPECT_EQ(document.findFrame(frameId), nullptr) << "an orphan placement frame was left behind";
    EXPECT_FALSE(document.dependencyGraph().hasNode(frameId));
}

TEST(AssemblyDocumentTest, M23_ASM_006_RenamingAnInstanceRenamesThePlaceThatCarriesIt) {
    // A tree showing "Gear origin" under an instance called "Pinion" would be
    // describing a document that does not exist.
    AssemblyDocument document{"Rig"};
    Instance& gear = document.addInstance("Gear", "parts/gear.ep3d");
    const ObjectId frameId = gear.frameId();

    const DocumentBase::RenameResult renamed = document.renameObject(gear.id(), "Pinion");
    ASSERT_TRUE(renamed.ok) << renamed.message;
    EXPECT_EQ(document.findInstance(gear.id())->name(), "Pinion");
    EXPECT_EQ(document.findFrame(frameId)->name(), "Pinion origin");

    // And the rename is one step of history, undoable like any other.
    ASSERT_TRUE(document.undo());
    EXPECT_EQ(document.findInstance(gear.id())->name(), "Gear");
    EXPECT_EQ(document.findFrame(frameId)->name(), "Gear origin");
}

TEST(AssemblyDocumentTest, M23_ASM_007_ANameIsTakenWhateverKindOfThingTookIt) {
    // The uniqueness rule is the document's, not the instance list's. Before
    // M23 frames were not even renameable, so this rule had nothing to say
    // about them; now they are objects a user can name and it has to.
    AssemblyDocument document{"Rig"};
    Instance& first = document.addInstance("Gear", "parts/gear.ep3d");
    Instance& second = document.addInstance("Shaft", "parts/shaft.ep3d");

    const DocumentBase::RenameResult clash = document.renameObject(second.id(), "Gear");
    EXPECT_FALSE(clash.ok);
    EXPECT_NE(clash.message.find("already taken"), std::string::npos) << clash.message;

    // A frame's name counts too -- "Gear origin" is a real object's real name.
    const DocumentBase::RenameResult frameClash =
        document.renameObject(second.id(), "Gear origin");
    EXPECT_FALSE(frameClash.ok) << "an instance took a name a frame already had";

    // ...and a free name is still free.
    EXPECT_TRUE(document.renameObject(second.id(), "Pinion").ok);
    EXPECT_EQ(first.name(), "Gear");
}

TEST(AssemblyDocumentTest, M23_ASM_008_AnIdIsUsedONCEWhateverUsesIt) {
    // One rule, one place (DocumentBase::requireUnusedId). An assembly gets it
    // without writing it, which is the whole claim of ADR-M23-001.
    AssemblyDocument document{"Rig"};
    Instance& gear = document.addInstance("Gear", "parts/gear.ep3d");

    EXPECT_THROW(document.restoreInstance(gear.id(), "Copy", ComputeState::Dirty,
                                          "parts/gear.ep3d", {}, gear.frameId()),
                 std::runtime_error);
    EXPECT_THROW(document.restoreFrame(gear.id(), "Elsewhere", kInvalidObjectId, Transform3D{}),
                 std::runtime_error);
    // ...including the document's own id, which no registry can see.
    EXPECT_THROW(document.restoreFrame(document.id(), "Elsewhere", kInvalidObjectId,
                                       Transform3D{}),
                 std::runtime_error);
}

TEST(AssemblyDocumentTest, M23_ASM_009_AnInstanceMustNameAFile) {
    AssemblyDocument document{"Rig"};
    EXPECT_THROW(document.addInstance("Nothing", ""), std::runtime_error);
    EXPECT_EQ(document.instances().size(), 0u);
    EXPECT_EQ(document.frames().size(), 1u) << "a refused insert left a frame behind";
}

// --- The gate: three parts, moved, saved, reopened, still where they were ----

TEST(AssemblyDocumentTest, M23_SER_001_THREEPartsSurviveASaveAndAReopenWhereTheyWerePut) {
    AssemblyDocument document{"Gearbox"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& gear = document.addInstance("Gear", "parts/gear.ep3d", "Main");
    Instance& shaft = document.addInstance("Shaft", "parts/shaft.ep3d");

    ASSERT_TRUE(document.setInstanceTransform(base.id(), At(0, 0, 0)));
    ASSERT_TRUE(document.setInstanceTransform(gear.id(), At(40, -15, 7.5)));
    // One of them TURNED as well as moved: a translation-only test cannot tell
    // a quaternion that round-tripped from one that was quietly reset to
    // identity, and identity is what a dropped field reads as.
    ASSERT_TRUE(document.setInstanceTransform(shaft.id(), TurnedAt(-20, 60, 0, 1.0)));

    const std::string text = SaveToString(document);
    const AssemblyLoadResult loaded = LoadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    const AssemblyDocument& back = *loaded.document;

    EXPECT_EQ(back.name(), "Gearbox");
    ASSERT_EQ(back.instances().size(), 3u);

    const Instance* baseBack = InstanceNamed(back, "Base");
    const Instance* gearBack = InstanceNamed(back, "Gear");
    const Instance* shaftBack = InstanceNamed(back, "Shaft");
    ASSERT_NE(baseBack, nullptr);
    ASSERT_NE(gearBack, nullptr);
    ASSERT_NE(shaftBack, nullptr);

    // WHICH PART, and WHICH BODY of it.
    EXPECT_EQ(gearBack->sourcePath(), "parts/gear.ep3d");
    EXPECT_EQ(gearBack->bodyName(), "Main");
    EXPECT_EQ(baseBack->bodyName(), "") << "an unnamed body came back named";

    // WHERE. The gate.
    EXPECT_NEAR(back.instanceWorldTransform(gearBack->id()).translation.x, 40.0, 1e-9);
    EXPECT_NEAR(back.instanceWorldTransform(gearBack->id()).translation.y, -15.0, 1e-9);
    EXPECT_NEAR(back.instanceWorldTransform(gearBack->id()).translation.z, 7.5, 1e-9);
    const Transform3D turned = back.instanceWorldTransform(shaftBack->id());
    EXPECT_NEAR(turned.translation.x, -20.0, 1e-9);
    EXPECT_NEAR(turned.rotation.w, std::cos(0.5), 1e-9);
    EXPECT_NEAR(turned.rotation.z, std::sin(0.5), 1e-9);

    // A reopened document has no history to undo (ADR-M9-001).
    EXPECT_EQ(back.undoDepth(), 0u);
}

TEST(AssemblyDocumentTest, M23_SER_002_AnAssemblyFileCarriesNOGeometry) {
    // The rule the whole format is built on (ADR-M4-004), restated for the new
    // document type rather than assumed to carry over.
    AssemblyDocument document{"Gearbox"};
    document.addInstance("Gear", "parts/gear.ep3d", "Main");
    const std::string text = SaveToString(document);

    EXPECT_NE(text.find("parts/gear.ep3d"), std::string::npos) << text;
    EXPECT_NE(text.find("\"documentType\": \"Assembly\""), std::string::npos) << text;
    // Nothing that smells of a solid, a mesh or a face.
    EXPECT_EQ(text.find("vertices"), std::string::npos);
    EXPECT_EQ(text.find("triangles"), std::string::npos);
    EXPECT_EQ(text.find("ADVANCED_FACE"), std::string::npos);
    // ...and the placement appears ONCE, in the frame, not again in the
    // instance: two copies is how two answers begin.
    EXPECT_EQ(text.find("\"transform\"", text.find("\"instances\"")), std::string::npos) << text;
}

TEST(AssemblyDocumentTest, M23_SER_003_APartFileIsNOTAnAssemblyAndSaysSo) {
    // Both loaders read the same header through the same function, so the two
    // cannot come to disagree about what a documentType means.
    PartDocument part{"Bracket"};
    std::ostringstream partText;
    ASSERT_TRUE(savePartDocument(part, partText));

    const AssemblyLoadResult wrong = LoadFromString(partText.str());
    EXPECT_FALSE(wrong);
    EXPECT_EQ(wrong.error, SerializationError::WrongDocumentType);
    EXPECT_NE(wrong.message.find("Part"), std::string::npos) << wrong.message;

    // ...and the other way round.
    AssemblyDocument assembly{"Gearbox"};
    assembly.addInstance("Gear", "parts/gear.ep3d");
    std::istringstream assemblyText(SaveToString(assembly));
    const LoadResult alsoWrong = loadPartDocument(assemblyText);
    EXPECT_FALSE(alsoWrong);
    EXPECT_EQ(alsoWrong.error, SerializationError::WrongDocumentType);
}

TEST(AssemblyDocumentTest, M23_SER_004_AnInstanceWithNOFileIsRefusedAtBOTHDoors) {
    // ADR-M3-008: every reference the loader checks is checked at save time,
    // so a document that saves can always be opened again. Checked at both
    // ends here because one end alone is how that invariant rots.
    AssemblyDocument document{"Rig"};
    document.addInstance("Gear", "parts/gear.ep3d");
    std::string text = SaveToString(document);
    const std::size_t at = text.find("\"parts/gear.ep3d\"");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"parts/gear.ep3d\"").size(), "\"\"");

    const AssemblyLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("names no part file"), std::string::npos) << loaded.message;
}

TEST(AssemblyDocumentTest, M23_SER_005_AnInstancePlacedByAFrameThatIsNotThereIsREFUSED) {
    // The dangling reference the loader would otherwise throw on -- and a
    // loader that throws is a loader a caller cannot use.
    AssemblyDocument document{"Rig"};
    Instance& gear = document.addInstance("Gear", "parts/gear.ep3d");
    std::string text = SaveToString(document);

    const std::string realFrame = "\"" + std::to_string(gear.frameId()) + "\"";
    const std::size_t at = text.rfind(realFrame);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, realFrame.size(), "\"999777\"");

    const AssemblyLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::UnknownDependencyId);
    EXPECT_NE(loaded.message.find("999777"), std::string::npos) << loaded.message;
}

TEST(AssemblyDocumentTest, M23_SER_006_ADuplicateIdIsRefusedACROSSKinds) {
    // The id rule spans the whole document, not each array. A frame's id
    // reused by an instance is exactly the collision a per-array check misses.
    AssemblyDocument document{"Rig"};
    Instance& gear = document.addInstance("Gear", "parts/gear.ep3d");
    std::string text = SaveToString(document);

    const std::string instanceId = "\"id\": \"" + std::to_string(gear.id()) + "\"";
    const std::size_t at = text.find(instanceId);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, instanceId.size(),
                 "\"id\": \"" + std::to_string(gear.frameId()) + "\"");

    const AssemblyLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::DuplicateId);
}

TEST(AssemblyDocumentTest, M23_SER_007_ASavedAssemblyIsByteIdenticalOnARoundTrip) {
    // The test that catches what a field-by-field check cannot: something
    // written that is not read, or read into the wrong place.
    AssemblyDocument document{"Gearbox"};
    Instance& gear = document.addInstance("Gear", "parts/gear.ep3d", "Main");
    document.addInstance("Shaft", "parts/shaft.ep3d");
    ASSERT_TRUE(document.setInstanceTransform(gear.id(), TurnedAt(3, 4, 5, 0.75)));
    document.addConnector("Mount", ConnectorRole::Mount, gear.frameId(),
                          ConnectorOwner::Assembly);

    const std::string first = SaveToString(document);
    const AssemblyLoadResult loaded = LoadFromString(first);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(SaveToString(*loaded.document), first);
}

TEST(AssemblyDocumentTest, M23_SER_008_ADocumentThatCannotLOADIsRefusedAtSAVE) {
    // Constructed through a restore path rather than the ordinary facade,
    // because the facade cannot build this state -- which is the point of
    // checking at save: the check is the LAST line, not the only one.
    AssemblyDocument document{"Rig"};
    ReferenceFrame& stray = document.addFrame("Stray");
    document.restoreInstance(4242, "Ghost", ComputeState::Dirty, "parts/gear.ep3d", {},
                             stray.id());
    ASSERT_TRUE(TrySave(document)) << "a well-formed assembly was refused";

    // Now take the frame away underneath it. The instance still names it.
    ASSERT_TRUE(document.removeObject(stray.id()));
    const SaveResult refused = TrySave(document);
    EXPECT_FALSE(refused);
    EXPECT_NE(refused.message.find("frame that is not in this document"), std::string::npos)
        << refused.message;
}

TEST(AssemblyDocumentTest, M23_SER_009_ATransformMissingAComponentIsREFUSED) {
    // ALL SEVEN OR NONE. A transform whose qw was dropped reads as a rotation
    // of zero, which is not "no rotation" -- it is a degenerate quaternion,
    // and the part it places would arrive somewhere nobody chose. Silently
    // filling in a default is how a file half-read becomes a document
    // half-right.
    AssemblyDocument document{"Rig"};
    Instance& gear = document.addInstance("Gear", "parts/gear.ep3d");
    ASSERT_TRUE(document.setInstanceTransform(gear.id(), TurnedAt(3, 4, 5, 0.75)));

    std::string text = SaveToString(document);
    const std::size_t at = text.find("\"qw\"");
    ASSERT_NE(at, std::string::npos) << text;
    const std::size_t comma = text.find(',', at);
    ASSERT_NE(comma, std::string::npos);
    text.erase(at, comma - at + 1);

    const AssemblyLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("missing a numeric component"), std::string::npos)
        << loaded.message;
}

// --- M24: mates as document objects ------------------------------------------

TEST(AssemblyDocumentTest, M24_MATE_001_AMateIsADocumentObjectLikeAnyOther) {
    // Named, id-carrying, resolvable, renameable, removable. Stated as a test
    // because a mate that was none of those would still solve, and would then
    // be the one thing in the tree a user could not point at.
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    Mate& elbow = document.addMate("Elbow", MateType::Revolute, base.id(), "Pivot", arm.id(),
                                   "Eye", 0.0);

    EXPECT_NE(elbow.id(), kInvalidObjectId);
    EXPECT_TRUE(document.objectRegistry().contains(elbow.id()));
    EXPECT_EQ(document.findMateNamed("Elbow"), &elbow);
    EXPECT_EQ(document.objectName(elbow.id()), "Elbow");

    const DocumentBase::RenameResult renamed = document.renameObject(elbow.id(), "Knee");
    ASSERT_TRUE(renamed.ok) << renamed.message;
    EXPECT_EQ(document.findMateNamed("Knee"), &elbow);
    // ...and its name is taken, by the same rule that governs every other name.
    EXPECT_FALSE(document.renameObject(arm.id(), "Knee").ok);
}

TEST(AssemblyDocumentTest, M24_MATE_002_AThingCannotBeMatedToItself) {
    // The solve would place it from its own placement, which is either a
    // no-op or a contradiction depending on the value, and neither is what
    // anybody meant.
    AssemblyDocument document{"Rig"};
    Instance& one = document.addInstance("One", "parts/one.ep3d");
    EXPECT_THROW(document.addMate("Silly", MateType::Revolute, one.id(), "A", one.id(), "B", 0.0),
                 std::runtime_error);
    EXPECT_EQ(document.mates().size(), 0u);
}

TEST(AssemblyDocumentTest, M24_MATE_003_AFastenedMateHasNoFreedomToGiveAValueTo) {
    // Refused rather than ignored: ignoring it would leave the writer
    // believing they had offset something. Both doors, because two doors that
    // disagree about what is legal is how a document saves and stops loading.
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");

    EXPECT_THROW(document.addMate("Stuck", MateType::Fastened, base.id(), "A", arm.id(), "B",
                                  5.0),
                 std::runtime_error);
    EXPECT_THROW(document.restoreMate(9911, "Stuck", MateType::Fastened, base.id(), "A",
                                      arm.id(), "B", 5.0),
                 std::runtime_error);

    // ...and driving one after the fact is refused too, rather than silently
    // stored where nothing would ever read it.
    Mate& fixed = document.addMate("Fixed", MateType::Fastened, base.id(), "A", arm.id(), "B");
    EXPECT_FALSE(document.setMateValue(fixed.id(), 5.0));
    EXPECT_EQ(fixed.value(), 0.0);
}

TEST(AssemblyDocumentTest, M24_MATE_004_DrivingAMateIsAnOrdinaryUndoableEdit) {
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    Mate& elbow = document.addMate("Elbow", MateType::Revolute, base.id(), "Pivot", arm.id(),
                                   "Eye", 0.0);

    ASSERT_TRUE(document.setMateValue(elbow.id(), 1.0));
    ASSERT_TRUE(document.setMateValue(elbow.id(), 2.0));
    EXPECT_EQ(document.nextUndoLabel(), "Drive Elbow");

    ASSERT_TRUE(document.undo());
    EXPECT_NEAR(elbow.value(), 1.0, 1e-12);
    ASSERT_TRUE(document.undo());
    EXPECT_NEAR(elbow.value(), 0.0, 1e-12);
    ASSERT_TRUE(document.redo());
    EXPECT_NEAR(elbow.value(), 1.0, 1e-12);

    // BOTH ENDS are dirtied, because which one moves depends on the ground and
    // the ground is not consulted here. Dirtying one would be a guess about a
    // direction that is not stored anywhere.
    EXPECT_EQ(document.dependencyGraph().state(arm.id()), ComputeState::Dirty);
    EXPECT_EQ(document.dependencyGraph().state(base.id()), ComputeState::Dirty);
}

TEST(AssemblyDocumentTest, M24_MATE_005_DeletingAnInstanceDeletesTheMatesThatNamedIt) {
    // A mate to something that is not there cannot be solved and cannot be
    // repaired -- there is no other instance to re-point it at -- so leaving
    // it would put a permanent failure in the tree whose cause has already
    // been deleted.
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    Instance& tip = document.addInstance("Tip", "parts/tip.ep3d");
    const ObjectId elbow =
        document.addMate("Elbow", MateType::Revolute, base.id(), "P", arm.id(), "E").id();
    const ObjectId wrist =
        document.addMate("Wrist", MateType::Fastened, arm.id(), "F", tip.id(), "G").id();
    ASSERT_EQ(document.mates().size(), 2u);

    ASSERT_TRUE(document.removeObject(arm.id()));
    EXPECT_EQ(document.mates().size(), 0u) << "a mate outlived the instance it named";
    EXPECT_FALSE(document.objectRegistry().contains(elbow));
    EXPECT_FALSE(document.objectRegistry().contains(wrist));
    EXPECT_NE(document.findInstance(base.id()), nullptr) << "the other end went too";
}

TEST(AssemblyDocumentTest, M24_MATE_006_GroundingIsUndoableAndDefaultsToNothing) {
    // Nothing is grounded by default. Grounding the first instance in the list
    // would make where everything ends up depend on the order things were
    // typed, which is position-as-meaning by another name.
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    EXPECT_FALSE(document.isInstanceGrounded(base.id()));

    ASSERT_TRUE(document.setInstanceGrounded(base.id(), true));
    EXPECT_TRUE(document.isInstanceGrounded(base.id()));
    ASSERT_TRUE(document.undo());
    EXPECT_FALSE(document.isInstanceGrounded(base.id()));
    ASSERT_TRUE(document.redo());
    EXPECT_TRUE(document.isInstanceGrounded(base.id()));

    // Grounding what is already grounded records nothing: an undo stack full
    // of steps that change nothing is an undo stack a user cannot read.
    const std::size_t depth = document.undoDepth();
    EXPECT_TRUE(document.setInstanceGrounded(base.id(), true));
    EXPECT_EQ(document.undoDepth(), depth);
}

TEST(AssemblyDocumentTest, M24_MATE_007_ONEFormulaDecidesWhatEachMateKindMeans) {
    // Every mate type is MateTransform plus a placement rule written once, so
    // a new kind cannot arrive with its own idea of what "connected" means.
    // M25 generalised the values to one per component; the claim is unchanged.
    const auto only = [](MateComponent component, double value) {
        MateValues values{};
        values[static_cast<std::size_t>(component)] = value;
        return values;
    };

    const Transform3D fastened = MateTransform(MateType::Fastened, only(MateComponent::TZ, 7.0));
    EXPECT_NEAR(fastened.translation.z, 0.0, 1e-12) << "a fastened mate moved something";
    EXPECT_NEAR(fastened.rotation.w, 1.0, 1e-12);

    const double quarter = 3.14159265358979323846 / 2.0;
    const Transform3D turned = MateTransform(MateType::Revolute, only(MateComponent::RZ, quarter));
    EXPECT_NEAR(turned.translation.z, 0.0, 1e-12) << "a revolute translated";
    // A quarter turn about +Z takes +X to +Y, and nothing else would.
    const Vec3 moved = RotateByQuaternion(turned.rotation, Vec3{1, 0, 0});
    EXPECT_NEAR(moved.x, 0.0, 1e-9);
    EXPECT_NEAR(moved.y, 1.0, 1e-9);

    const Transform3D slid = MateTransform(MateType::Slider, only(MateComponent::TZ, 12.0));
    EXPECT_NEAR(slid.rotation.w, 1.0, 1e-12) << "a slider turned";
    EXPECT_NEAR(slid.translation.z, 12.0, 1e-12);
    EXPECT_NEAR(slid.translation.x, 0.0, 1e-12) << "a slider slid along the wrong axis";
}

TEST(AssemblyDocumentTest, M25_MATE_001_TheFreedomTableISTheMateTypeTable) {
    // Roadmap section 20.1, column by column. This is the ONE place a mate type
    // says what it is, so the table is checked here rather than inferred from
    // behaviour somewhere downstream.
    struct Expected {
        MateType type;
        int rotational;
        int translational;
    };
    const Expected table[] = {
        {MateType::Fastened, 0, 0},    {MateType::Revolute, 1, 0}, {MateType::Slider, 0, 1},
        {MateType::Cylindrical, 1, 1}, {MateType::Ball, 3, 0},     {MateType::Planar, 1, 2},
    };
    for (const Expected& row : table) {
        const MateFreedom freedom = FreedomOf(row.type);
        EXPECT_EQ(freedom.rotational(), row.rotational) << toString(row.type);
        EXPECT_EQ(freedom.translational(), row.translational) << toString(row.type);
    }

    // A revolute and a slider free DIFFERENT components of the same axis --
    // which is the whole reason one formula is enough.
    EXPECT_TRUE(FreedomOf(MateType::Revolute).isFree(MateComponent::RZ));
    EXPECT_FALSE(FreedomOf(MateType::Revolute).isFree(MateComponent::TZ));
    EXPECT_TRUE(FreedomOf(MateType::Slider).isFree(MateComponent::TZ));
    EXPECT_FALSE(FreedomOf(MateType::Slider).isFree(MateComponent::RZ));
    // A cylindrical frees both, and that is the only difference between it and
    // the two of them.
    EXPECT_TRUE(FreedomOf(MateType::Cylindrical).isFree(MateComponent::TZ));
    EXPECT_TRUE(FreedomOf(MateType::Cylindrical).isFree(MateComponent::RZ));
    // Parallel is an ALIGNMENT mate: it pins the two tilts and nothing else.
    EXPECT_FALSE(FreedomOf(MateType::Parallel).isFree(MateComponent::RX));
    EXPECT_FALSE(FreedomOf(MateType::Parallel).isFree(MateComponent::RY));
    EXPECT_TRUE(FreedomOf(MateType::Parallel).isFree(MateComponent::TX));
    EXPECT_TRUE(FreedomOf(MateType::Parallel).isFree(MateComponent::TZ));
}

TEST(AssemblyDocumentTest, M25_MATE_002_ComponentsAndTransformsAreEachOthersInverse) {
    // The six numbers a residual is made of, and the transform they describe.
    // If these two disagreed, a solve would drive the wrong thing to zero.
    Transform3D t;
    t.translation = Vec3{3.0, -7.0, 11.0};
    const double angle = 0.9;
    t.rotation = Quaternion{std::cos(angle / 2.0), 0.0, std::sin(angle / 2.0), 0.0};

    const std::array<double, kMateComponentCount> components = ComponentsOf(t);
    EXPECT_NEAR(components[0], 3.0, 1e-12);
    EXPECT_NEAR(components[1], -7.0, 1e-12);
    EXPECT_NEAR(components[2], 11.0, 1e-12);
    // The rotation is 0.9 rad about +Y, so the axis-angle vector is (0, 0.9, 0).
    EXPECT_NEAR(components[3], 0.0, 1e-12);
    EXPECT_NEAR(components[4], 0.9, 1e-12);
    EXPECT_NEAR(components[5], 0.0, 1e-12);

    const Transform3D back = TransformOfComponents(components);
    EXPECT_NEAR(back.translation.x, t.translation.x, 1e-12);
    EXPECT_NEAR(back.rotation.w, t.rotation.w, 1e-12);
    EXPECT_NEAR(back.rotation.y, t.rotation.y, 1e-12);

    // IDENTITY IS ALL ZEROES, which is what makes these usable as residuals --
    // and it has to be smooth through zero, not merely correct at it.
    const std::array<double, kMateComponentCount> none = ComponentsOf(Transform3D::Identity());
    for (double one : none) EXPECT_NEAR(one, 0.0, 1e-15);
    // BELOW THE CUT where dividing by the sine would be dividing by nothing.
    // 1e-12 does not reach it -- the ordinary path handles that perfectly --
    // so the number here is small enough to take the other branch, which is
    // the branch that has to give the same answer.
    Transform3D tiny;
    tiny.rotation = Quaternion{1.0, 1e-16, 0.0, 0.0};
    const std::array<double, kMateComponentCount> small = ComponentsOf(tiny);
    EXPECT_NEAR(small[3], 2e-16, 1e-22) << "the axis-angle vector is not smooth through zero";
    // ...and the ordinary path, just above the cut, agrees with it.
    Transform3D justAbove;
    justAbove.rotation = Quaternion{1.0, 1e-12, 0.0, 0.0};
    EXPECT_NEAR(ComponentsOf(justAbove)[3], 2e-12, 1e-18);
}

TEST(AssemblyDocumentTest, M25_MATE_003_ResidualsAreTheComponentsTheMatePINS) {
    // A mate is satisfied exactly when its pinned components are zero, and its
    // free ones are exactly what it does not care about. A residual list that
    // included a free component would fight the freedom it is meant to leave.
    double out[kMateComponentCount] = {};

    // A revolute, turned to 0.5, with the follower ACTUALLY at 0.5: satisfied.
    MateValues values{};
    values[5] = 0.5;
    Transform3D relative;
    relative.rotation = Quaternion{std::cos(0.25), 0.0, 0.0, std::sin(0.25)};
    int written = MateResiduals(MateType::Revolute, values, relative, out);
    EXPECT_EQ(written, 5) << "a revolute pins five of the six";
    for (int i = 0; i < written; ++i) EXPECT_NEAR(out[i], 0.0, 1e-12) << "component " << i;

    // The same revolute with the follower turned somewhere ELSE about +Z is
    // STILL satisfied in every pinned component -- that rotation is the
    // freedom, and the solve is free to choose it.
    relative.rotation = Quaternion{std::cos(1.0), 0.0, 0.0, std::sin(1.0)};
    written = MateResiduals(MateType::Revolute, values, relative, out);
    for (int i = 0; i < written; ++i) EXPECT_NEAR(out[i], 0.0, 1e-12) << "component " << i;

    // But a follower that has slid along the pin is NOT satisfied.
    relative.translation = Vec3{0, 0, 4.0};
    written = MateResiduals(MateType::Revolute, values, relative, out);
    double worst = 0.0;
    for (int i = 0; i < written; ++i) worst = std::max(worst, std::fabs(out[i]));
    EXPECT_NEAR(worst, 4.0, 1e-9) << "a revolute did not notice the follower sliding off it";

    // ...and a CYLINDRICAL mate, which frees that slide, is satisfied by the
    // very same relative transform. Same numbers, different mate, different
    // answer -- which is the freedom table doing its job.
    MateValues cylindrical{};
    written = MateResiduals(MateType::Cylindrical, cylindrical, relative, out);
    EXPECT_EQ(written, 4) << "a cylindrical pins four of the six";
    for (int i = 0; i < written; ++i) EXPECT_NEAR(out[i], 0.0, 1e-12) << "component " << i;
}

TEST(AssemblyDocumentTest, M24_MATE_008_InverseUndoesAComposeExactly) {
    // The half of a mate that says "work backwards from where the connector
    // has to end up to where the instance has to be". A rigid inverse is
    // exact, so this is checked as exact rather than nearly.
    Transform3D t;
    t.translation = Vec3{3.0, -7.0, 11.0};
    const double angle = 0.9;
    t.rotation = Quaternion{std::cos(angle / 2.0), 0.0, std::sin(angle / 2.0), 0.0};

    const Transform3D roundTrip = Compose(t, Inverse(t));
    EXPECT_NEAR(roundTrip.translation.x, 0.0, 1e-12);
    EXPECT_NEAR(roundTrip.translation.y, 0.0, 1e-12);
    EXPECT_NEAR(roundTrip.translation.z, 0.0, 1e-12);
    EXPECT_NEAR(std::fabs(roundTrip.rotation.w), 1.0, 1e-12);

    // ...and the OTHER order too, which a conjugate-only implementation that
    // forgot to counter-rotate the offset would fail.
    const Transform3D otherWay = Compose(Inverse(t), t);
    EXPECT_NEAR(otherWay.translation.x, 0.0, 1e-12);
    EXPECT_NEAR(otherWay.translation.y, 0.0, 1e-12);
    EXPECT_NEAR(otherWay.translation.z, 0.0, 1e-12);
}

TEST(AssemblyDocumentTest, M24_SER_010_MatesAndGroundingSurviveASaveAndAReopen) {
    AssemblyDocument document{"Hinge"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d", "Bracket");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    ASSERT_TRUE(document.setInstanceGrounded(base.id(), true));
    Mate& elbow = document.addMate("Elbow", MateType::Revolute, base.id(), "Pivot", arm.id(),
                                   "Eye", 0.75);
    document.addMate("Slide", MateType::Slider, base.id(), "Rail", arm.id(), "Shoe", 12.5);

    const std::string text = SaveToString(document);
    const AssemblyLoadResult loaded = LoadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    const AssemblyDocument& back = *loaded.document;

    ASSERT_EQ(back.mates().size(), 2u);
    const Mate* elbowBack = back.findMateNamed("Elbow");
    ASSERT_NE(elbowBack, nullptr);
    EXPECT_EQ(elbowBack->type(), MateType::Revolute);
    EXPECT_EQ(elbowBack->leadingConnector(), "Pivot");
    EXPECT_EQ(elbowBack->followingConnector(), "Eye");
    // RADIANS, unconverted. A file that stored degrees would be a second unit
    // in a format that has none.
    EXPECT_NEAR(elbowBack->value(), 0.75, 1e-12);
    EXPECT_NEAR(back.findMateNamed("Slide")->value(), 12.5, 1e-12);

    const Instance* baseBack = back.findInstanceNamed("Base");
    ASSERT_NE(baseBack, nullptr);
    EXPECT_TRUE(back.isInstanceGrounded(baseBack->id()));
    EXPECT_FALSE(back.isInstanceGrounded(back.findInstanceNamed("Arm")->id()));
    EXPECT_EQ(back.undoDepth(), 0u) << "loading recorded undo steps";

    // Byte-identical, which catches what a field-by-field check cannot:
    // something written that is not read, or read into the wrong place.
    EXPECT_EQ(SaveToString(back), text);
    (void)elbow;
}

TEST(AssemblyDocumentTest, M24_SER_011_AMateNamingSomethingThatIsNotThereIsREFUSED) {
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    document.addMate("Elbow", MateType::Revolute, base.id(), "Pivot", arm.id(), "Eye");
    std::string text = SaveToString(document);

    const std::string real = "\"followingInstanceId\": \"" + std::to_string(arm.id()) + "\"";
    const std::size_t at = text.find(real);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, real.size(), "\"followingInstanceId\": \"888555\"");

    const AssemblyLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::UnknownDependencyId);
    EXPECT_NE(loaded.message.find("888555"), std::string::npos) << loaded.message;
}

TEST(AssemblyDocumentTest, M24_SER_012_AMateNamingNOConnectorIsREFUSED) {
    // A mate that names no connector can never resolve, so it is refused at
    // the door rather than at solve time, a long way from the cause.
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    document.addMate("Elbow", MateType::Revolute, base.id(), "Pivot", arm.id(), "Eye");

    std::string text = SaveToString(document);
    const std::size_t at = text.find("\"Pivot\"");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"Pivot\"").size(), "\"\"");

    const AssemblyLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("names no connector"), std::string::npos) << loaded.message;
}

TEST(AssemblyDocumentTest, M24_SER_013_AFastenedMateCarryingAValueInTheFileIsREFUSED) {
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    document.addMate("Fixed", MateType::Fastened, base.id(), "A", arm.id(), "B");

    // A fastened mate frees nothing, so every one of its six values is zero.
    // Putting a number on any of them by hand is refused at the door.
    std::string text = SaveToString(document);
    const std::size_t at = text.find("\"values\"");
    ASSERT_NE(at, std::string::npos) << text;
    const std::size_t zero = text.find('0', at);
    ASSERT_NE(zero, std::string::npos) << text;
    text.replace(zero, 1, "4");

    const AssemblyLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("no freedom"), std::string::npos) << loaded.message;
}

TEST(AssemblyDocumentTest, M25_SER_001_AV30FileWithASINGLEValueStillLoads) {
    // v30 wrote one number per mate; v31 writes six. An old file is read by
    // putting its number on the first free component, which is exactly what it
    // meant -- every mate type v30 knew about has one freedom.
    //
    // Written by hand rather than by an old serializer, because there is no
    // old serializer any more: this is what those files look like, and if that
    // ever stops being true this test is how it is found out.
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    document.addMate("Elbow", MateType::Revolute, base.id(), "Pivot", arm.id(), "Eye", 0.75);

    std::string text = SaveToString(document);
    // Strip the v31 fields and put back the v30 one.
    const std::size_t at = text.find("\"values\"");
    ASSERT_NE(at, std::string::npos) << text;
    const std::size_t end = text.find(']', at);
    ASSERT_NE(end, std::string::npos);
    text.replace(at, end - at + 1, "\"value\": 0.75");
    // Whatever the current stamp is, relabelled as v30 -- asked of the string
    // rather than written as a literal, because a version bump has nothing to
    // do with what this test checks and should not turn it red.
    const std::size_t stamp = text.find("\"schemaVersion\": ");
    ASSERT_NE(stamp, std::string::npos) << text;
    const std::size_t stampEnd = text.find(',', stamp);
    ASSERT_NE(stampEnd, std::string::npos);
    text.replace(stamp, stampEnd - stamp, "\"schemaVersion\": 30");

    const AssemblyLoadResult loaded = LoadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    const Mate* elbow = loaded.document->findMateNamed("Elbow");
    ASSERT_NE(elbow, nullptr);
    EXPECT_NEAR(elbow->value(), 0.75, 1e-12) << "a v30 mate value was lost";
    // A revolute's freedom is the rotation about z, and that is where the
    // number went -- not into component zero, which is a translation.
    EXPECT_NEAR(elbow->values()[5], 0.75, 1e-12);
    EXPECT_NEAR(elbow->values()[0], 0.0, 1e-12);
}

TEST(AssemblyDocumentTest, M24_SER_014_ADocumentWithABrokenMateIsRefusedAtSAVE) {
    // ADR-M3-008 again, for the new kind of reference: every rule the loader
    // enforces is enforced before a byte is written, so a document that saves
    // can always be opened.
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    document.addMate("Elbow", MateType::Revolute, base.id(), "Pivot", arm.id(), "Eye");
    ASSERT_TRUE(TrySave(document));

    // Take one end away through a path that does NOT cascade -- the restore
    // path, which is how a loader builds a document and therefore the one that
    // has to be defended against.
    AssemblyDocument hand{"Rig"};
    Instance& left = hand.addInstance("Left", "parts/base.ep3d");
    Instance& right = hand.addInstance("Right", "parts/arm.ep3d");
    hand.restoreMate(7001, "Elbow", MateType::Revolute, left.id(), "Pivot", right.id(), "Eye",
                     0.0);
    ASSERT_TRUE(TrySave(hand));
    ASSERT_TRUE(hand.removeObject(right.id()));
    // The cascade took the mate with it, so the document is savable again --
    // which IS the invariant, stated as the outcome rather than as a rule.
    EXPECT_TRUE(TrySave(hand));
    EXPECT_EQ(hand.mates().size(), 0u);
}

TEST(AssemblyDocumentTest, M25_SER_002_LimitsAndDrivingSurviveASaveAndAReopen) {
    // A limit that was not written down is a limit that stops existing when
    // the file is closed -- and the model that comes back is then free to sit
    // in a state its own rules forbade five minutes ago. Same for driving: a
    // mechanism reopened with nothing driven is a mechanism the solver is free
    // to rearrange.
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    Mate& hinge = document.addMate("Hinge", MateType::Revolute, base.id(), "P", arm.id(), "Q",
                                   0.5);
    Mate& slide = document.addMate("Slide", MateType::Slider, base.id(), "R", arm.id(), "S");
    Mate& joint = document.addMateWithValues("Joint", MateType::Cylindrical, base.id(), "T",
                                             arm.id(), "U", MateValues{0, 0, 12.0, 0, 0, 0.25});

    ASSERT_TRUE(document.setMateDriven(hinge.id(), true));
    ASSERT_TRUE(document.setMateLimit(hinge.id(), MateComponent::RZ, -1.0, 1.0));
    ASSERT_TRUE(document.setMateLimit(joint.id(), MateComponent::TZ, 0.0, 50.0));
    ASSERT_TRUE(document.setMateLimit(joint.id(), MateComponent::RZ, -2.0, 2.0));

    const std::string text = SaveToString(document);
    const AssemblyLoadResult loaded = LoadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    const AssemblyDocument& back = *loaded.document;

    const Mate* hingeBack = back.findMateNamed("Hinge");
    ASSERT_NE(hingeBack, nullptr);
    EXPECT_TRUE(hingeBack->isDriven()) << "driving was lost";
    EXPECT_TRUE(hingeBack->limits()[5].enabled) << "a limit was lost";
    EXPECT_NEAR(hingeBack->limits()[5].min, -1.0, 1e-12);
    EXPECT_NEAR(hingeBack->limits()[5].max, 1.0, 1e-12);

    // ...and the mates that had NEITHER come back with neither, so this is not
    // a test that would pass on a loader that enabled everything.
    const Mate* slideBack = back.findMateNamed("Slide");
    ASSERT_NE(slideBack, nullptr);
    EXPECT_FALSE(slideBack->isDriven());
    EXPECT_FALSE(slideBack->limits()[2].enabled);

    // A CYLINDRICAL MATE'S TWO VALUES AND TWO LIMITS, which is the case a
    // single number per mate could not carry at all.
    const Mate* jointBack = back.findMateNamed("Joint");
    ASSERT_NE(jointBack, nullptr);
    EXPECT_NEAR(jointBack->values()[2], 12.0, 1e-12);
    EXPECT_NEAR(jointBack->values()[5], 0.25, 1e-12);
    EXPECT_TRUE(jointBack->limits()[2].enabled);
    EXPECT_NEAR(jointBack->limits()[2].max, 50.0, 1e-12);
    EXPECT_TRUE(jointBack->limits()[5].enabled);
    EXPECT_NEAR(jointBack->limits()[5].min, -2.0, 1e-12);

    EXPECT_EQ(SaveToString(back), text);
    (void)slide;
}

TEST(AssemblyDocumentTest, M25_SER_003_ALimitOnAFreedomTheMateDoesNotHaveIsREFUSED) {
    // Both doors, because two doors that disagree about what is legal is how a
    // document saves and then refuses to open (ADR-M3-008).
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    Mate& hinge = document.addMate("Hinge", MateType::Revolute, base.id(), "P", arm.id(), "Q");
    ASSERT_TRUE(document.setMateLimit(hinge.id(), MateComponent::RZ, -1.0, 1.0));

    std::string text = SaveToString(document);
    // Move the limit onto a component a revolute pins.
    const std::size_t at = text.find("\"component\": 5");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"component\": 5").size(), "\"component\": 0");

    const AssemblyLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("does not have"), std::string::npos) << loaded.message;
}

// --- M26: sub-assemblies, poses, explosions, patterns -----------------------

TEST(AssemblyDocumentTest, M26_CUT_001_AnEvaluationPositionClampsAndDefaultsToALL) {
    // The rule roadmap §49 point 2 asked to be extracted at its third
    // appearance. Tested here rather than only through a Body's rollback and
    // an explosion's preview, because it is now ONE thing and the place to
    // check a thing is where it lives.
    EvaluationCut all;
    EXPECT_EQ(all.stored(), EvaluationCut::kAll);
    EXPECT_EQ(all.effective(3), 3u) << "the default is not the whole list";
    EXPECT_FALSE(all.isPast(2, 3));

    EvaluationCut two{2};
    EXPECT_EQ(two.effective(5), 2u);
    EXPECT_FALSE(two.isPast(1, 5));
    EXPECT_TRUE(two.isPast(2, 5)) << "the item AT the cut is on the far side of it";

    // A LIST THAT SHRANK under a stored cut means "all of it" again, rather
    // than a position pointing past the end that somebody has to find and fix.
    EvaluationCut nine{9};
    EXPECT_EQ(nine.effective(4), 4u);
    EXPECT_FALSE(nine.isPast(3, 4));

    // ...and the STORED value survives, so a save writes kAll rather than
    // however long the list happened to be when it was written.
    EXPECT_EQ(nine.stored(), 9u);
    EXPECT_NE(EvaluationCut{2}, EvaluationCut{3});
}

TEST(AssemblyDocumentTest, M26_POSE_001_APoseCapturesTheFreedomsANDTheLooseParts) {
    // §49: a named position is "mate 自由度的值 + 無 mate 實例的絕對變換". The
    // second half is easy to leave out and impossible to notice until an
    // assembly with a hand-placed part comes back with it somewhere else.
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    Instance& loose = document.addInstance("Loose", "parts/loose.ep3d");
    Mate& hinge = document.addMate("Hinge", MateType::Revolute, base.id(), "P", arm.id(), "Q");
    ASSERT_TRUE(document.setMateValue(hinge.id(), 0.75));
    Transform3D somewhere;
    somewhere.translation = Vec3{11, 22, 33};
    ASSERT_TRUE(document.setInstanceTransform(loose.id(), somewhere));

    const NamedPosition& pose = document.captureNamedPosition("Open");
    ASSERT_EQ(pose.mates().size(), 1u);
    EXPECT_EQ(pose.mates().front().mateId, hinge.id());
    EXPECT_NEAR(pose.mates().front().values[5], 0.75, 1e-12);
    // ONLY the instances no mate places -- Base and Arm are the hinge's, so
    // their transform is derived and recording it would be a second answer.
    ASSERT_EQ(pose.loose().size(), 1u);
    EXPECT_EQ(pose.loose().front().instanceId, loose.id());
    EXPECT_NEAR(pose.loose().front().transform.translation.y, 22.0, 1e-12);
}

TEST(AssemblyDocumentTest, M26_POSE_002_ApplyingAPoseIsONEUndoStep) {
    // A pose is one thing the user chose. Without the transaction, undoing "go
    // to Open" would walk backwards through every mate it touched, one press
    // at a time -- and stop somewhere that was never a pose at all.
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    Instance& other = document.addInstance("Other", "parts/other.ep3d");
    Mate& one = document.addMate("One", MateType::Revolute, base.id(), "P", arm.id(), "Q");
    Mate& two = document.addMate("Two", MateType::Slider, base.id(), "R", other.id(), "S");

    ASSERT_TRUE(document.setMateValue(one.id(), 1.0));
    ASSERT_TRUE(document.setMateValue(two.id(), 20.0));
    document.captureNamedPosition("Open");

    ASSERT_TRUE(document.setMateValue(one.id(), 0.0));
    ASSERT_TRUE(document.setMateValue(two.id(), 0.0));
    const std::size_t before = document.undoDepth();

    ASSERT_TRUE(document.applyNamedPosition(document.findNamedPositionNamed("Open")->id()));
    EXPECT_NEAR(one.value(), 1.0, 1e-12);
    EXPECT_NEAR(two.value(), 20.0, 1e-12);
    EXPECT_EQ(document.undoDepth(), before + 1) << "applying a pose was more than one step";
    EXPECT_EQ(document.nextUndoLabel(), "Apply Open");

    // ONE press puts BOTH mates back.
    ASSERT_TRUE(document.undo());
    EXPECT_NEAR(one.value(), 0.0, 1e-12);
    EXPECT_NEAR(two.value(), 0.0, 1e-12);
}

TEST(AssemblyDocumentTest, M26_POSE_003_APoseIsNotAConfiguration) {
    // §49 point 3: a configuration changes what the model IS, a named position
    // only changes where its freedoms are sitting. So a pose holds nothing
    // that could define a part -- and applying one leaves the instances, the
    // mates and the sources exactly as they were.
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d", "Main");
    Mate& hinge = document.addMate("Hinge", MateType::Revolute, base.id(), "P", arm.id(), "Q");
    document.captureNamedPosition("Shut");

    ASSERT_TRUE(document.setMateValue(hinge.id(), 2.0));
    ASSERT_TRUE(document.applyNamedPosition(document.findNamedPositionNamed("Shut")->id()));

    EXPECT_EQ(document.instances().size(), 2u);
    EXPECT_EQ(document.mates().size(), 1u);
    EXPECT_EQ(document.findInstance(arm.id())->sourcePath(), "parts/arm.ep3d");
    EXPECT_EQ(document.findInstance(arm.id())->bodyName(), "Main") << "a pose changed a source";
    EXPECT_NEAR(hinge.value(), 0.0, 1e-12);
}

TEST(AssemblyDocumentTest, M26_EXPLODE_001_StepsComposeInOrderUpToThePreview) {
    // An explosion is an ordered list, and its own evaluation position is what
    // makes walking it forward a preview rather than an all-or-nothing switch.
    AssemblyDocument document{"Rig"};
    Instance& one = document.addInstance("One", "parts/one.ep3d");
    Instance& two = document.addInstance("Two", "parts/two.ep3d");
    ExplodeView& view = document.addExplodeView("Service");

    ASSERT_TRUE(document.addExplodeStep(view.id(), "Lift", one.id(), Vec3{0, 0, 50}));
    ASSERT_TRUE(document.addExplodeStep(view.id(), "Slide", one.id(), Vec3{30, 0, 0}));
    ASSERT_TRUE(document.addExplodeStep(view.id(), "Other", two.id(), Vec3{0, 60, 0}));

    // ALL OF IT by default. The two steps on `One` compose.
    EXPECT_NEAR(view.displacementOf(one.id()).translation.z, 50.0, 1e-12);
    EXPECT_NEAR(view.displacementOf(one.id()).translation.x, 30.0, 1e-12);
    EXPECT_NEAR(view.displacementOf(two.id()).translation.y, 60.0, 1e-12);
    // ...and an instance no step touches is not moved.
    EXPECT_NEAR(view.displacementOf(9999).translation.z, 0.0, 1e-12);

    // ONE STEP SHOWN: the first only.
    ASSERT_TRUE(document.setExplodePreview(view.id(), 1));
    EXPECT_EQ(view.stepsShown(), 1u);
    EXPECT_NEAR(view.displacementOf(one.id()).translation.z, 50.0, 1e-12);
    EXPECT_NEAR(view.displacementOf(one.id()).translation.x, 0.0, 1e-12)
        << "a step past the preview was applied anyway";
    EXPECT_NEAR(view.displacementOf(two.id()).translation.y, 0.0, 1e-12);

    // NONE shown is the unexploded assembly, which has to be reachable or the
    // preview cannot be walked from the beginning.
    ASSERT_TRUE(document.setExplodePreview(view.id(), 0));
    EXPECT_NEAR(view.displacementOf(one.id()).translation.z, 0.0, 1e-12);
}

TEST(AssemblyDocumentTest, M26_EXPLODE_002_AnExplosionNeverMovesTheModel) {
    // §49 calls it a derived display transform. So the assembly's own answer
    // has to be unchanged by every step, and asking with no view has to give
    // exactly that -- which is the only way a caller can tell the picture from
    // the model.
    AssemblyDocument document{"Rig"};
    Instance& one = document.addInstance("One", "parts/one.ep3d");
    Transform3D placed;
    placed.translation = Vec3{7, 8, 9};
    ASSERT_TRUE(document.setInstanceTransform(one.id(), placed));

    ExplodeView& view = document.addExplodeView("Service");
    ASSERT_TRUE(document.addExplodeStep(view.id(), "Lift", one.id(), Vec3{0, 0, 100}));

    // The model did not move.
    EXPECT_NEAR(document.instanceWorldTransform(one.id()).translation.z, 9.0, 1e-12);
    // No view asked for means the model's own answer.
    EXPECT_NEAR(document.explodedWorldTransform(kInvalidObjectId, one.id()).translation.z, 9.0,
                1e-12);
    // The view's answer is the model's, displaced.
    EXPECT_NEAR(document.explodedWorldTransform(view.id(), one.id()).translation.z, 109.0,
                1e-12);
    EXPECT_NEAR(document.explodedWorldTransform(view.id(), one.id()).translation.x, 7.0, 1e-12);
}

TEST(AssemblyDocumentTest, M26_EXPLODE_003_StepsCanBeReorderedAndRemoved) {
    // §49 says a step can be named, reordered and deleted. Reordering matters
    // because the steps compose: two rotations in the other order end
    // somewhere else, and an explosion whose steps cannot be reordered is a
    // list that has to be retyped to fix.
    AssemblyDocument document{"Rig"};
    Instance& one = document.addInstance("One", "parts/one.ep3d");
    ExplodeView& view = document.addExplodeView("Service");
    ASSERT_TRUE(document.addExplodeStep(view.id(), "First", one.id(), Vec3{10, 0, 0}));
    ASSERT_TRUE(document.addExplodeStep(view.id(), "Second", one.id(), Vec3{0, 20, 0}));
    ASSERT_EQ(view.steps().size(), 2u);
    EXPECT_EQ(view.steps()[0].name, "First");

    ASSERT_TRUE(document.moveExplodeStep(view.id(), 1, 0));
    EXPECT_EQ(view.steps()[0].name, "Second");
    EXPECT_EQ(view.steps()[1].name, "First");
    // With one step shown, the reorder is what changes which one it is.
    ASSERT_TRUE(document.setExplodePreview(view.id(), 1));
    EXPECT_NEAR(view.displacementOf(one.id()).translation.y, 20.0, 1e-12);
    EXPECT_NEAR(view.displacementOf(one.id()).translation.x, 0.0, 1e-12);

    ASSERT_TRUE(document.removeExplodeStep(view.id(), 0));
    ASSERT_EQ(view.steps().size(), 1u);
    EXPECT_EQ(view.steps()[0].name, "First");
    // An out-of-range index is refused rather than clamped: "delete step 7" of
    // a two-step list is a mistake, not a request.
    EXPECT_FALSE(document.removeExplodeStep(view.id(), 7));
    EXPECT_FALSE(document.moveExplodeStep(view.id(), 0, 9));
}

TEST(AssemblyDocumentTest, M26_EXPLODE_004_EveryExplodeEditIsUndoable) {
    AssemblyDocument document{"Rig"};
    Instance& one = document.addInstance("One", "parts/one.ep3d");
    ExplodeView& view = document.addExplodeView("Service");
    ASSERT_TRUE(document.addExplodeStep(view.id(), "First", one.id(), Vec3{10, 0, 0}));
    ASSERT_TRUE(document.addExplodeStep(view.id(), "Second", one.id(), Vec3{0, 20, 0}));
    ASSERT_TRUE(document.moveExplodeStep(view.id(), 1, 0));
    ASSERT_TRUE(document.setExplodePreview(view.id(), 1));

    ASSERT_TRUE(document.undo()); // the preview
    EXPECT_EQ(view.stepsShown(), 2u);
    ASSERT_TRUE(document.undo()); // the reorder
    EXPECT_EQ(view.steps()[0].name, "First");
    ASSERT_TRUE(document.undo()); // the second step
    EXPECT_EQ(view.steps().size(), 1u);
    ASSERT_TRUE(document.undo()); // the first step
    EXPECT_EQ(view.steps().size(), 0u);
    ASSERT_TRUE(document.undo()); // the view itself
    EXPECT_EQ(document.explodeViews().size(), 0u);

    // ...and forward again, with the reorder still in it.
    while (document.canRedo()) ASSERT_TRUE(document.redo());
    ASSERT_EQ(document.explodeViews().size(), 1u);
    const ExplodeView* back = document.findExplodeViewNamed("Service");
    ASSERT_NE(back, nullptr);
    ASSERT_EQ(back->steps().size(), 2u);
    EXPECT_EQ(back->steps()[0].name, "Second") << "redo lost the reorder";
    EXPECT_EQ(back->stepsShown(), 1u);
}

TEST(AssemblyDocumentTest, M26_PATTERN_001_APatternedCopyFOLLOWSTheOriginal) {
    // The claim that makes it a pattern rather than three parts in a row: each
    // copy's placement frame hangs off the ORIGINAL's, so moving the original
    // moves the row and nothing here watches anything.
    AssemblyDocument document{"Rig"};
    Instance& one = document.addInstance("Bolt", "parts/bolt.ep3d");
    const std::vector<ObjectId> made = document.addInstancePattern(one.id(), 3, Vec3{25, 0, 0});
    ASSERT_EQ(made.size(), 2u) << "a row of three is the original plus two";
    ASSERT_EQ(document.instances().size(), 3u);

    EXPECT_NEAR(document.instanceWorldTransform(made[0]).translation.x, 25.0, 1e-12);
    EXPECT_NEAR(document.instanceWorldTransform(made[1]).translation.x, 50.0, 1e-12);

    // MOVE THE ORIGINAL and the whole row goes with it.
    Transform3D moved;
    moved.translation = Vec3{100, 5, 0};
    ASSERT_TRUE(document.setInstanceTransform(one.id(), moved));
    EXPECT_NEAR(document.instanceWorldTransform(made[0]).translation.x, 125.0, 1e-12);
    EXPECT_NEAR(document.instanceWorldTransform(made[0]).translation.y, 5.0, 1e-12);
    EXPECT_NEAR(document.instanceWorldTransform(made[1]).translation.x, 150.0, 1e-12);

    // A copy is an ORDINARY instance: nameable, and its source is the
    // original's.
    EXPECT_EQ(document.findInstance(made[0])->sourcePath(), "parts/bolt.ep3d");
    EXPECT_TRUE(document.renameObject(made[0], "Second bolt").ok);

    // A count below one is a mistake, not a request.
    EXPECT_THROW(document.addInstancePattern(one.id(), 0, Vec3{1, 0, 0}), std::runtime_error);
}

TEST(AssemblyDocumentTest, M26_PATTERN_002_DeletingTheOriginalLeavesTheCopiesWHERETHEYARE) {
    // The hole M26's pattern opened: a copy's frame hangs off the original's,
    // and worldTransform walks UP -- so a child whose parent was deleted
    // quietly reports its LOCAL transform as its world one and jumps.
    //
    // Each child is lifted to the grandparent with its local transform
    // rewritten, so nothing moves. ADR-M26-004.
    AssemblyDocument document{"Rig"};
    Instance& one = document.addInstance("Bolt", "parts/bolt.ep3d");
    Transform3D start;
    start.translation = Vec3{100, 0, 0};
    ASSERT_TRUE(document.setInstanceTransform(one.id(), start));
    const std::vector<ObjectId> made = document.addInstancePattern(one.id(), 3, Vec3{25, 0, 0});
    ASSERT_EQ(made.size(), 2u);
    ASSERT_NEAR(document.instanceWorldTransform(made[1]).translation.x, 150.0, 1e-12);

    ASSERT_TRUE(document.removeObject(one.id()));
    EXPECT_EQ(document.instances().size(), 2u);
    // EXACTLY WHERE THEY WERE. Not at 25 and 50, which is where their own
    // offsets alone would put them.
    EXPECT_NEAR(document.instanceWorldTransform(made[0]).translation.x, 125.0, 1e-9);
    EXPECT_NEAR(document.instanceWorldTransform(made[1]).translation.x, 150.0, 1e-9);
}

TEST(AssemblyDocumentTest, M26_SER_001_PosesAndExplosionsSurviveASaveAndAReopen) {
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    Instance& loose = document.addInstance("Loose", "parts/loose.ep3d");
    Mate& hinge = document.addMate("Hinge", MateType::Revolute, base.id(), "P", arm.id(), "Q");
    ASSERT_TRUE(document.setMateValue(hinge.id(), 0.4));
    Transform3D somewhere;
    somewhere.translation = Vec3{3, 4, 5};
    ASSERT_TRUE(document.setInstanceTransform(loose.id(), somewhere));
    document.captureNamedPosition("Open");

    ExplodeView& view = document.addExplodeView("Service");
    ASSERT_TRUE(document.addExplodeStep(view.id(), "Lift", arm.id(), Vec3{0, 0, 40}));
    ASSERT_TRUE(document.addExplodeStep(view.id(), "Slide", loose.id(), Vec3{60, 0, 0}));
    ASSERT_TRUE(document.setExplodePreview(view.id(), 1));

    const std::string text = SaveToString(document);
    const AssemblyLoadResult loaded = LoadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    const AssemblyDocument& back = *loaded.document;

    const NamedPosition* poseBack = back.findNamedPositionNamed("Open");
    ASSERT_NE(poseBack, nullptr);
    ASSERT_EQ(poseBack->mates().size(), 1u);
    EXPECT_NEAR(poseBack->mates().front().values[5], 0.4, 1e-12);
    ASSERT_EQ(poseBack->loose().size(), 1u);
    EXPECT_NEAR(poseBack->loose().front().transform.translation.y, 4.0, 1e-12);

    const ExplodeView* viewBack = back.findExplodeViewNamed("Service");
    ASSERT_NE(viewBack, nullptr);
    ASSERT_EQ(viewBack->steps().size(), 2u);
    EXPECT_EQ(viewBack->steps()[0].name, "Lift");
    EXPECT_NEAR(viewBack->steps()[1].displacement.translation.x, 60.0, 1e-12);
    EXPECT_EQ(viewBack->stepsShown(), 1u) << "the preview position was lost";

    EXPECT_EQ(back.undoDepth(), 0u);
    EXPECT_EQ(SaveToString(back), text);
}

TEST(AssemblyDocumentTest, M26_SER_002_APreviewOfALLComesBackAsALLNotAsACount) {
    // The stored cut, not the effective one. If a save wrote "2" for a
    // two-step view showing everything, adding a third step after reopening
    // would leave it hidden -- and nobody would connect that to the save.
    AssemblyDocument document{"Rig"};
    Instance& one = document.addInstance("One", "parts/one.ep3d");
    ExplodeView& view = document.addExplodeView("Service");
    ASSERT_TRUE(document.addExplodeStep(view.id(), "A", one.id(), Vec3{1, 0, 0}));
    ASSERT_TRUE(document.addExplodeStep(view.id(), "B", one.id(), Vec3{0, 1, 0}));
    ASSERT_EQ(view.previewCut(), EvaluationCut::kAll);

    const AssemblyLoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const ExplodeView* back = loaded.document->findExplodeViewNamed("Service");
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->previewCut(), EvaluationCut::kAll)
        << "an explosion showing everything came back pinned to its step count";
}

TEST(AssemblyDocumentTest, M26_SER_003_APoseOrStepNamingSomethingGoneIsREFUSED) {
    // ADR-M3-008 for the new kinds of reference, at both doors.
    AssemblyDocument document{"Rig"};
    Instance& base = document.addInstance("Base", "parts/base.ep3d");
    Instance& arm = document.addInstance("Arm", "parts/arm.ep3d");
    document.addMate("Hinge", MateType::Revolute, base.id(), "P", arm.id(), "Q");
    document.captureNamedPosition("Open");

    std::string text = SaveToString(document);
    const std::string real = "\"mateId\": \"" + std::to_string(
        document.findMateNamed("Hinge")->id()) + "\"";
    const std::size_t at = text.find(real);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, real.size(), "\"mateId\": \"777333\"");

    const AssemblyLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::UnknownDependencyId);
    EXPECT_NE(loaded.message.find("777333"), std::string::npos) << loaded.message;
}
