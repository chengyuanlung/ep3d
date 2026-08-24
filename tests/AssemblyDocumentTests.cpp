// M23 -- the second document type, and the machinery both types now share.
//
// These are Core tests: no kernel, no OCCT. What they check is that an
// assembly IS a document -- ids, undo, frames, names, a file that round-trips
// -- which is the half of M23 that has nothing to do with geometry. The
// geometry half lives in the OCCT suite, where an instance can actually build
// something.

#include "Core/Assembly/AssemblyDocument.h"
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

const PartInstance* InstanceNamed(const AssemblyDocument& document, const std::string& name) {
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
    PartInstance& gear = document.addInstance("Gear", "parts/gear.ep3d");

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
    PartInstance& gear = document.addInstance("Gear", "parts/gear.ep3d");
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
    PartInstance& gear = document.addInstance("Gear", "parts/gear.ep3d");
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
    PartInstance& first = document.addInstance("Gear", "parts/gear.ep3d");
    PartInstance& second = document.addInstance("Shaft", "parts/shaft.ep3d");

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
    PartInstance& gear = document.addInstance("Gear", "parts/gear.ep3d");

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
    PartInstance& base = document.addInstance("Base", "parts/base.ep3d");
    PartInstance& gear = document.addInstance("Gear", "parts/gear.ep3d", "Main");
    PartInstance& shaft = document.addInstance("Shaft", "parts/shaft.ep3d");

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

    const PartInstance* baseBack = InstanceNamed(back, "Base");
    const PartInstance* gearBack = InstanceNamed(back, "Gear");
    const PartInstance* shaftBack = InstanceNamed(back, "Shaft");
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
    PartInstance& gear = document.addInstance("Gear", "parts/gear.ep3d");
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
    PartInstance& gear = document.addInstance("Gear", "parts/gear.ep3d");
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
    PartInstance& gear = document.addInstance("Gear", "parts/gear.ep3d", "Main");
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
    PartInstance& gear = document.addInstance("Gear", "parts/gear.ep3d");
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
