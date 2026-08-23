// M4-I: schema v4 semantic persistence for Sketch and Pad (spec 16, spec 18
// "Persistence" matrix; ADR-M4-004).
//
// The governing rule is that identity is ObjectId / SketchEntityId and nothing
// else: no OCCT topology, no vector index, no pointer address ever reaches the
// file.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PlaceholderFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Fakes/FakeGeometryKernel.h"
#include "Support/SchemaVersionText.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;
constexpr double kLengthAbsTol = 1e-6;
constexpr double kVolumeMassRelTol = 1e-9;

std::string SaveToString(const PartDocument& document) {
    std::ostringstream out;
    const SaveResult result = savePartDocument(document, out);
    EXPECT_TRUE(result) << result.message;
    return out.str();
}

LoadResult LoadFromString(const std::string& text) {
    std::istringstream in(text);
    return loadPartDocument(in);
}

struct PadDoc {
    PartDocument document{"PadDoc"};
    FakeGeometryKernel kernel;
    Sketch* sketch = nullptr;
    Parameter* length = nullptr;
    PadFeature* pad = nullptr;
    std::vector<SketchEntityId> entityIds;

    explicit PadDoc(SketchFrame frame = SketchFrame::WorldXY()) {
        document.setGeometryKernel(&kernel);
        document.addMaterial("Steel", 7850.0);
        length = &document.addParameter("PadLength", 20.0, UnitType::Millimeter);
        sketch = &document.addSketch("Sketch001", frame);
        entityIds = {sketch->addLine(Vec2{0, 0}, Vec2{100, 0}),
                     sketch->addLine(Vec2{100, 0}, Vec2{100, 50}),
                     sketch->addLine(Vec2{100, 50}, Vec2{0, 50}),
                     sketch->addLine(Vec2{0, 50}, Vec2{0, 0})};
        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", sketch->id(), length->id());
    }
};

// Renamed from M4_SER_001_SchemaVersionIsFour when M5 bumped the version to
// 5. What it checks is unchanged and still worth checking: save writes the
// CURRENT version, so a forgotten bump alongside a format change fails here
// rather than producing files an older loader silently mis-reads.
TEST(SerializationV4Test, M4_SER_001_SaveWritesTheCurrentSchemaVersion) {
    PadDoc doc;
    EXPECT_NE(SaveToString(doc.document).find(paramcad::testing::CurrentSchemaVersionField()), std::string::npos);
}

TEST(SerializationV4Test, M4_SER_002_SketchAndEntityIdsSurvive) {
    PadDoc doc;
    const ObjectId sketchId = doc.sketch->id();
    const std::vector<SketchEntityId> before = doc.entityIds;

    const LoadResult loaded = LoadFromString(SaveToString(doc.document));
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->sketches().size(), 1u);

    const Sketch& restored = *loaded.document->sketches().front();
    EXPECT_EQ(restored.id(), sketchId);
    EXPECT_EQ(restored.name(), "Sketch001");
    ASSERT_EQ(restored.entities().size(), before.size());
    for (SketchEntityId id : before)
        EXPECT_NE(restored.findEntity(id), nullptr)
            << "entity id " << ToObjectId(id) << " did not survive the round trip";
}

TEST(SerializationV4Test, M4_SER_003_EntityGeometrySurvivesExactly) {
    PartDocument document{"GeoDoc"};
    Sketch& sketch = document.addSketch("Sketch001");
    const SketchEntityId point = sketch.addPoint(Vec2{1.5, -2.25});
    const SketchEntityId line = sketch.addLine(Vec2{0, 0}, Vec2{100, 50});
    const SketchEntityId circle = sketch.addCircle(Vec2{7, 8}, 9.5);
    const SketchEntityId arc = sketch.addArc(Vec2{1, 2}, 12.0, 0.25, 2.75, false);

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& restored = *loaded.document->sketches().front();

    const auto* p = std::get_if<SketchPoint>(&restored.findEntity(point)->geometry);
    ASSERT_NE(p, nullptr);
    EXPECT_DOUBLE_EQ(p->position.x, 1.5);
    EXPECT_DOUBLE_EQ(p->position.y, -2.25);

    const auto* l = std::get_if<SketchLine>(&restored.findEntity(line)->geometry);
    ASSERT_NE(l, nullptr);
    EXPECT_DOUBLE_EQ(l->end.x, 100.0);
    EXPECT_DOUBLE_EQ(l->end.y, 50.0);

    const auto* c = std::get_if<SketchCircle>(&restored.findEntity(circle)->geometry);
    ASSERT_NE(c, nullptr);
    EXPECT_DOUBLE_EQ(c->radiusMm, 9.5);

    const auto* a = std::get_if<SketchArc>(&restored.findEntity(arc)->geometry);
    ASSERT_NE(a, nullptr);
    EXPECT_DOUBLE_EQ(a->radiusMm, 12.0);
    EXPECT_DOUBLE_EQ(a->startAngleRad, 0.25);
    EXPECT_DOUBLE_EQ(a->endAngleRad, 2.75);
    EXPECT_FALSE(a->counterClockwise) << "arc direction was not preserved";
}

TEST(SerializationV4Test, M4_SER_004_SketchFrameSurvives) {
    PadDoc doc{SketchFrame::Rotated(Vec3{1, 0, 0}, kPi / 2, Vec3{5, 6, 7})};
    const Vec3 expected = doc.sketch->frame().toWorld(Vec2{10, 20});

    const LoadResult loaded = LoadFromString(SaveToString(doc.document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Vec3 actual = loaded.document->sketches().front()->frame().toWorld(Vec2{10, 20});

    EXPECT_NEAR(actual.x, expected.x, kLengthAbsTol);
    EXPECT_NEAR(actual.y, expected.y, kLengthAbsTol);
    EXPECT_NEAR(actual.z, expected.z, kLengthAbsTol);
}

TEST(SerializationV4Test, M4_SER_005_PadReferencesSurvive) {
    PadDoc doc;
    const ObjectId padId = doc.pad->id();
    const ObjectId sketchId = doc.sketch->id();
    const ObjectId lengthId = doc.length->id();

    const LoadResult loaded = LoadFromString(SaveToString(doc.document));
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->bodies().size(), 1u);
    ASSERT_EQ(loaded.document->bodies().front()->features().size(), 1u);

    const auto* restored =
        dynamic_cast<const PadFeature*>(loaded.document->bodies().front()->features().front().get());
    ASSERT_NE(restored, nullptr) << "the Pad came back as a different feature type";
    EXPECT_EQ(restored->id(), padId);
    EXPECT_EQ(restored->sketchId(), sketchId);
    EXPECT_EQ(restored->lengthParameterId(), lengthId);
    ASSERT_TRUE(loaded.document->material());
    EXPECT_EQ(restored->materialId(), loaded.document->material()->id());
}

TEST(SerializationV4Test, M4_SER_006_NoRuntimeOrTopologyStateIsWritten) {
    PadDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    ASSERT_TRUE(doc.pad->currentShape().isValid()); // a real runtime shape exists

    const std::string saved = SaveToString(doc.document);
    for (const char* forbidden :
         {"TopoDS", "TShape", "BRep", "Geom_", "gp_", "AIS_", "KernelShape", "IShapeHandle",
          "OcctShape", "0x"}) {
        EXPECT_EQ(saved.find(forbidden), std::string::npos)
            << "serialized document leaks runtime/topology state: " << forbidden;
    }
    // Derived mass properties are computed, never persisted (ADR-009 D6).
    EXPECT_EQ(saved.find("massKg"), std::string::npos);
    EXPECT_EQ(saved.find("volumeMm3"), std::string::npos);
}

TEST(SerializationV4Test, M4_SER_007_LoadedDocumentRecomputesEquivalentGeometry) {
    PadDoc doc;
    ASSERT_TRUE(doc.document.recompute().success);
    const MassProperties before = doc.document.massProperties();

    const LoadResult loaded = LoadFromString(SaveToString(doc.document));
    ASSERT_TRUE(loaded) << loaded.message;

    FakeGeometryKernel loadedKernel;
    loaded.document->setGeometryKernel(&loadedKernel);
    ASSERT_TRUE(loaded.document->recompute().success);
    const MassProperties& after = loaded.document->massProperties();

    EXPECT_TRUE(after.valid);
    EXPECT_NEAR(after.volumeMm3, before.volumeMm3,
                kVolumeMassRelTol * std::max(1.0, before.volumeMm3));
    EXPECT_NEAR(after.massKg, before.massKg, kVolumeMassRelTol * std::max(1.0, before.massKg));
    EXPECT_NEAR(after.centerOfMassMm.x, before.centerOfMassMm.x, kLengthAbsTol);
    EXPECT_NEAR(after.centerOfMassMm.y, before.centerOfMassMm.y, kLengthAbsTol);
    EXPECT_NEAR(after.centerOfMassMm.z, before.centerOfMassMm.z, kLengthAbsTol);
    for (std::size_t i = 0; i < 9; ++i)
        EXPECT_NEAR(after.inertiaTensorKgM2.m[i], before.inertiaTensorKgM2.m[i], 1e-9);
}

TEST(SerializationV4Test, M4_SER_008_ReferencesSurviveIndependentOfStorageOrder) {
    // Removing an entity shifts every later entity's POSITION but must not
    // touch any identity (ADR-M4-001).
    PadDoc doc;
    ASSERT_TRUE(doc.sketch->removeEntity(doc.entityIds.front()));
    const SketchEntityId survivor = doc.entityIds.back();

    const LoadResult loaded = LoadFromString(SaveToString(doc.document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& restored = *loaded.document->sketches().front();
    EXPECT_EQ(restored.entities().size(), 3u);
    EXPECT_NE(restored.findEntity(survivor), nullptr);
    EXPECT_EQ(restored.findEntity(doc.entityIds.front()), nullptr);
}

TEST(SerializationV4Test, M4_SER_009_ByteIdenticalSecondSave) {
    PadDoc doc;
    const std::string first = SaveToString(doc.document);
    const LoadResult loaded = LoadFromString(first);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(SaveToString(*loaded.document), first);
}

TEST(SerializationV4Test, M4_SER_010_DuplicateEntityIdInFileRejected) {
    PadDoc doc;
    std::string saved = SaveToString(doc.document);
    // Duplicate the first entity id onto the second entity record.
    const std::string firstId = std::to_string(ToObjectId(doc.entityIds[0]));
    const std::string secondId = std::to_string(ToObjectId(doc.entityIds[1]));
    const std::size_t at = saved.find("\"" + secondId + "\"");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, secondId.size() + 2, "\"" + firstId + "\"");

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded) << "a duplicate sketch entity id was accepted";
    EXPECT_EQ(loaded.error, SerializationError::DuplicateId);
}

TEST(SerializationV4Test, M4_SER_011_PadReferencingAMissingSketchRejected) {
    PadDoc doc;
    std::string saved = SaveToString(doc.document);
    const std::string sketchId = std::to_string(doc.sketch->id());
    // Point the Pad at an id that is not a sketch in this file.
    const std::size_t at = saved.find("\"sketchId\": \"" + sketchId + "\"");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, ("\"sketchId\": \"" + sketchId + "\"").size(),
                  "\"sketchId\": \"999999999\"");

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::UnknownDependencyId);
}

TEST(SerializationV4Test, M4_SER_012_SaveRejectsPlaceholderUsingAReservedTypeName) {
    // A placeholder carrying "Pad" would be written as a Pad record without any
    // Pad fields, producing a file the loader can never accept. Found by the M3
    // regression suite when M4 introduced PadFeature.
    PartDocument document{"Doc"};
    Body& body = document.addBody("Body001");
    document.addPlaceholderFeature(body, "Ghost", "Pad");

    std::ostringstream out;
    const SaveResult result = savePartDocument(document, out);
    EXPECT_FALSE(result) << "saved a document that could never be loaded back";
    EXPECT_EQ(result.error, SerializationError::InvalidFieldType);
}

TEST(SerializationV4Test, M4_SER_013_UnreservedPlaceholderTypesStillRoundTrip) {
    // The guard above must not break ADR-009 D4's lossless preservation of
    // genuinely unknown feature types.
    PartDocument document{"Doc"};
    Body& body = document.addBody("Body001");
    document.addPlaceholderFeature(body, "Ghost", "SomeFutureFeature");

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->bodies().front()->features().size(), 1u);
    EXPECT_EQ(loaded.document->bodies().front()->features().front()->typeName(),
              "SomeFutureFeature");
}

TEST(SerializationV4Test, M8_REV_341_RestorePlaceholderRefusesARegisteredId) {
    // Round 3, three reviewers independently: the round-2 placeholder facade
    // was the SEVENTH restore path and the only one without a duplicate-id
    // guard -- restoring a placeholder with an existing object's id saved
    // cleanly and the loader refused the just-written bytes (ADR-M3-008's
    // class, introduced by a fix). Both collision flavors are pinned; this is
    // the registered-object flavor.
    PartDocument document{"Doc"};
    Parameter& width = document.addParameter("Width", 100.0, UnitType::Millimeter);
    Body& body = document.addBody("Body001");
    EXPECT_THROW(document.restorePlaceholderFeature(body, width.id(), "Ghost",
                                                    ComputeState::Dirty, "Widget"),
                 std::runtime_error);
    // No residue: the throw happened before construction, the doc still saves.
    std::ostringstream out;
    EXPECT_TRUE(savePartDocument(document, out));
}

TEST(SerializationV4Test, M8_REV_342_RestorePlaceholderRefusesAnotherPlaceholdersId) {
    // The unregistered flavor: placeholders never enter the registry, so a
    // registry-only guard is blind to a placeholder-vs-placeholder collision
    // (the same blindness round 3 used to defeat the sibling guards). The
    // feature scan is the half that catches this.
    PartDocument document{"Doc"};
    Body& body = document.addBody("Body001");
    PlaceholderFeature& first = document.addPlaceholderFeature(body, "Ghost1", "Widget");
    EXPECT_THROW(document.restorePlaceholderFeature(body, first.id(), "Ghost2",
                                                    ComputeState::Dirty, "Widget"),
                 std::runtime_error);
    std::ostringstream out;
    EXPECT_TRUE(savePartDocument(document, out));
}

// --- Round 4, R1R4-C1: the guard was one-directional ------------------------
//
// Round 3 closed the placeholder path and its own comment named the general
// fact -- a placeholder is unregistered, so a placeholder-held id is invisible
// to `registry_.contains` and defeated every SIBLING guard too -- but changed
// no sibling. R1 built the consequence through public calls only: two features
// carrying one ObjectId in one Body, a document that would not save, and a
// "repair" that destroyed the wrong one and left the survivor unregistered,
// graph-less and unremovable -- which then saved and loaded cleanly as a
// healthy Pad. Every restore path now shares `requireUnusedId`.
//
// One test per path, because round 3 proved (R3R3-M1) that a fixture covering
// SOME members of a set and a comment claiming it covers all of them is how
// this project's tables drift.

namespace {

// The collision id is always a placeholder's -- the flavor that defeated the
// registry-only guards. A registered-id collision was already refused before
// round 4; these pin the half that was not.
ObjectId PlaceholderIdIn(PartDocument& document, Body& body) {
    return document.addPlaceholderFeature(body, "Ghost", "Widget").id();
}

} // namespace

TEST(SerializationV4Test, M8_REV_351_EveryFeatureRestorePathRefusesAPlaceholdersId) {
    PartDocument document{"Doc"};
    Body& body = document.addBody("Body001");
    Sketch& sketch = document.addSketch("Sketch001");
    Parameter& length = document.addParameter("Length", 20.0, UnitType::Millimeter);
    const ObjectId taken = PlaceholderIdIn(document, body);

    EXPECT_THROW(document.restorePadFeature(body, taken, "Pad", ComputeState::Dirty,
                                            sketch.id(), length.id(), kInvalidObjectId),
                 std::runtime_error);
    EXPECT_THROW(document.restoreBoxFeature(body, taken, "Box", ComputeState::Dirty, length.id(),
                                            length.id(), length.id(), kInvalidObjectId),
                 std::runtime_error);
    EXPECT_THROW(document.restoreRevolveFeature(body, taken, "Rev", ComputeState::Dirty,
                                                sketch.id(), SketchEntityId{1}, length.id(),
                                                kInvalidObjectId),
                 std::runtime_error);
    EXPECT_THROW(document.restorePlaceholderFeature(body, taken, "Ghost2", ComputeState::Dirty,
                                                    "Widget"),
                 std::runtime_error);

    // Exactly one feature still carries the id: nothing was half-built.
    std::size_t carrying = 0;
    for (const auto& feature : body.features())
        if (feature->id() == taken) ++carrying;
    EXPECT_EQ(carrying, 1u);
}

TEST(SerializationV4Test, M8_REV_352_ConsumingRestorePathsRefuseAPlaceholdersId) {
    // The three consumers are separated because each also calls
    // requireConsumableBase, and a guard that ran in the WRONG ORDER would
    // report the base problem and mask the id collision.
    PartDocument document{"Doc"};
    Body& body = document.addBody("Body001");
    Sketch& sketch = document.addSketch("Sketch001");
    Parameter& length = document.addParameter("Length", 20.0, UnitType::Millimeter);
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());
    const ObjectId taken = PlaceholderIdIn(document, body);

    EXPECT_THROW(document.restorePocketFeature(body, taken, "Pocket", ComputeState::Dirty,
                                               pad.id(), sketch.id(), length.id(),
                                               kInvalidObjectId),
                 std::runtime_error);
    EXPECT_THROW(document.restoreFilletFeature(body, taken, "Fillet", ComputeState::Dirty,
                                               pad.id(), length.id(), kInvalidObjectId),
                 std::runtime_error);
    EXPECT_THROW(document.restoreChamferFeature(body, taken, "Chamfer", ComputeState::Dirty,
                                                pad.id(), length.id(), kInvalidObjectId),
                 std::runtime_error);
    // The pad is still consumable -- no refused restore left a phantom consumer
    // behind that would make the NEXT one fail for the wrong reason.
    EXPECT_NO_THROW(document.addPocketFeature(body, "Pocket001", pad.id(), sketch.id(),
                                              length.id()));
}

TEST(SerializationV4Test, M8_REV_353_NonFeatureRestorePathsRefuseAPlaceholdersId) {
    PartDocument document{"Doc"};
    Body& body = document.addBody("Body001");
    const ObjectId taken = PlaceholderIdIn(document, body);

    EXPECT_THROW(document.restoreParameter(taken, "P", 1.0, UnitType::Millimeter, "",
                                           ParameterState::Valid),
                 std::runtime_error);
    EXPECT_THROW(document.restoreBody(taken, "Body002"), std::runtime_error);
    EXPECT_THROW(document.restoreSketch(taken, "Sketch002", SketchFrame::WorldXY()),
                 std::runtime_error);
    // Nothing was half-built by any refusal.
    EXPECT_EQ(document.bodies().size(), 1u);
    EXPECT_EQ(document.sketches().size(), 0u);
    std::ostringstream out;
    EXPECT_TRUE(savePartDocument(document, out));
}

TEST(SerializationV4Test, M8_REV_355_RestorePathsRefuseTheDocumentsOwnId) {
    // Round 4, R2R4-m1: the registry's THIRD blind spot. A PartDocument does
    // not register itself, so its own id passed both halves of the guard --
    // restoring anything onto it built cleanly and the document then refused to
    // save, permanently, with the collision invisible.
    PartDocument document{"Doc"};
    Body& body = document.addBody("Body001");
    const ObjectId taken = document.id();

    EXPECT_THROW(document.restorePlaceholderFeature(body, taken, "Ghost", ComputeState::Dirty,
                                                    "Widget"),
                 std::runtime_error);
    EXPECT_THROW(document.restoreBody(taken, "Body002"), std::runtime_error);
    EXPECT_THROW(document.restoreSketch(taken, "Sketch001", SketchFrame::WorldXY()),
                 std::runtime_error);
    EXPECT_THROW(document.restoreParameter(taken, "P", 1.0, UnitType::Millimeter, "",
                                           ParameterState::Valid),
                 std::runtime_error);

    // The document is still savable: every refusal happened before anything
    // was stored.
    std::ostringstream out;
    EXPECT_TRUE(savePartDocument(document, out));
}

TEST(SerializationV4Test, M8_REV_354_RemoveObjectRemovesTheRightFeatureAlongsideAPlaceholder) {
    // NOT a guard for the by-identity change, and named for what it does test.
    //
    // The other half of R1R4-C1: `Body::removeFeature` took an id and erased
    // the FIRST match, so in a duplicate-id state removeObject unregistered one
    // feature and destroyed a different one. `requireUnusedId` now makes that
    // state unconstructible through the public API -- `Body::addFeature` is
    // private with PartDocument as its only friend, and all eleven restore
    // paths are guarded -- so reverting removeFeature to first-id-match fails
    // NOTHING (round 4 mutation F, verified UNGUARDED and recorded as such).
    // The by-identity form is defense in depth with no reachable failure, in
    // the V8/X2 tradition of stating that plainly instead of claiming a pin.
    //
    // What this test does pin is the ordinary case that runs every day: a body
    // holding a placeholder and a pad, removing the pad, and the placeholder
    // surviving intact.
    PartDocument document{"Doc"};
    Body& body = document.addBody("Body001");
    Sketch& sketch = document.addSketch("Sketch001");
    Parameter& length = document.addParameter("Length", 20.0, UnitType::Millimeter);
    document.addPlaceholderFeature(body, "Ghost", "Widget");
    PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), length.id());
    ASSERT_EQ(body.features().size(), 2u);

    ASSERT_TRUE(document.removeObject(pad.id()));
    ASSERT_EQ(body.features().size(), 1u);
    // The PAD went; the placeholder stayed. Erasing by id could not tell them
    // apart in the duplicate case, and this asserts the object, not the count.
    EXPECT_EQ(body.features().front()->typeName(), "Widget");
}

// --- Round 4, R2R4-C1: the dependency edges nothing checked -----------------

TEST(SerializationV4Test, M8_REV_361_ASaveRefusesAnEdgeTheLoaderCannotRead) {
    // ADR-M3-008's named worst class, sixth recurrence, found by execution:
    // the WRITER persists an edge whose prerequisite is a FEATURE and whose
    // dependent is a parameter; the LOADER accepts parameter endpoints only.
    // Four public facade calls produced a file that saved OK and would not load.
    PartDocument document{"Doc"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    Body& body = document.addBody("Body001");
    Parameter& width = document.addParameter("Width", 100.0, UnitType::Millimeter);
    Parameter& height = document.addParameter("Height", 50.0, UnitType::Millimeter);
    Parameter& depth = document.addParameter("Depth", 20.0, UnitType::Millimeter);
    BoxFeature& box =
        document.addBoxFeature(body, "Box001", width.id(), height.id(), depth.id());
    // An UNRELATED parameter: making the box's own Width depend on the box
    // would be a cycle and is refused by the graph, which is correct and is
    // not what this test is about.
    Parameter& measured = document.addParameter("Measured", 0.0, UnitType::Millimeter);

    // The document is savable before the edge exists...
    {
        std::ostringstream out;
        ASSERT_TRUE(savePartDocument(document, out));
    }

    // ...and refused after, rather than writing bytes it could not read back.
    ASSERT_TRUE(document.addDependency(measured.id(), box.id()));
    std::ostringstream out;
    const SaveResult saved = savePartDocument(document, out);
    EXPECT_FALSE(saved);
    EXPECT_NE(saved.message.find("could never be loaded back"), std::string::npos)
        << saved.message;
    EXPECT_NE(saved.message.find("dependency edge"), std::string::npos) << saved.message;

    // And the refusal is the SAVE side doing its job, not a side effect: with
    // the edge removed the document saves and loads again.
    ASSERT_TRUE(document.removeDependency(measured.id(), box.id()));
    std::ostringstream out2;
    ASSERT_TRUE(savePartDocument(document, out2));
    const LoadResult loaded = LoadFromString(out2.str());
    EXPECT_TRUE(loaded) << loaded.message;
}

TEST(SerializationV4Test, M8_REV_362_ParameterToParameterEdgesStillRoundTrip) {
    // The negative control: the check must refuse ONLY what the loader refuses.
    // An ordinary parameter-to-parameter edge is the Option-A case the format
    // exists to carry, and it must still save, load and arrive intact.
    PartDocument document{"Doc"};
    Parameter& a = document.addParameter("A", 1.0, UnitType::Millimeter);
    Parameter& b = document.addParameter("B", 2.0, UnitType::Millimeter);
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));

    std::ostringstream out;
    ASSERT_TRUE(savePartDocument(document, out));
    const LoadResult loaded = LoadFromString(out.str());
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->dependencyGraph().dependentsOf(a.id()).size(), 1u);
}

} // namespace