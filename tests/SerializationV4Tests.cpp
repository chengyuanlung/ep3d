// M4-I: schema v4 semantic persistence for Sketch and Pad (spec 16, spec 18
// "Persistence" matrix; ADR-M4-004).
//
// The governing rule is that identity is ObjectId / SketchEntityId and nothing
// else: no OCCT topology, no vector index, no pointer address ever reaches the
// file.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PlaceholderFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Fakes/FakeGeometryKernel.h"
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
    EXPECT_NE(SaveToString(doc.document).find("\"schemaVersion\": 8"), std::string::npos); // v8: M8.3 Fillet/Chamfer
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
    document.addPlaceholderFeature(body, "Ghost", "Loft");

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->bodies().front()->features().size(), 1u);
    EXPECT_EQ(loaded.document->bodies().front()->features().front()->typeName(), "Loft");
}

} // namespace
