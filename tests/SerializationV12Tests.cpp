// Schema v12 (M16): where the user dragged each dimension's value.
//
// This is the test that decides whether dimension placement is a FEATURE or a
// toy. A user who spends a minute arranging a drawing and finds it rearranged
// on reopen has lost real work -- so the position has to survive the file, and
// a file written before v12 has to keep loading without one.

#include "Core/Document/PartDocument.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"

#include "Support/SchemaVersionText.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

std::string SaveToString(const PartDocument& document) {
    std::ostringstream out;
    const SaveResult saved = savePartDocument(document, out);
    EXPECT_TRUE(saved) << saved.message;
    return out.str();
}

LoadResult LoadFromString(const std::string& text) {
    std::istringstream in(text);
    return loadPartDocument(in);
}

// A sketch with one dimensioned line, and the dimension moved.
struct PlacedDoc {
    PartDocument document{"V12Doc"};
    ObjectId sketchId = kInvalidObjectId;
    SketchConstraintId placed{kInvalidSketchConstraintId};
    SketchConstraintId automatic{kInvalidSketchConstraintId};

    PlacedDoc() {
        Parameter& width = document.addParameter("Width", 100.0, UnitType::Millimeter);
        Parameter& height = document.addParameter("Height", 50.0, UnitType::Millimeter);
        Sketch& sketch = document.addSketch("Sketch001");
        sketchId = sketch.id();
        const SketchEntityId bottom = sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
        const SketchEntityId right = sketch.addLine(Vec2{100, 0}, Vec2{100, 50});

        placed = document.addSketchConstraint(sketchId, LengthConstraint{bottom, width.id()});
        automatic = document.addSketchConstraint(sketchId, LengthConstraint{right, height.id()});
        EXPECT_TRUE(document.setSketchDimensionPlacement(sketchId, placed, Vec2{50.0, -30.0}));
    }
};

const Sketch& OnlySketch(const PartDocument& document) {
    return *document.sketches().front();
}

} // namespace

TEST(SerializationV12Test, M16_SER_001_TheSchemaVersionIsStamped) {
    PlacedDoc source;
    EXPECT_NE(SaveToString(source.document).find(paramcad::testing::CurrentSchemaVersionField()), std::string::npos);
}

TEST(SerializationV12Test, M16_SER_002_APlacedDimensionKeepsItsPositionAcrossTheRoundTrip) {
    PlacedDoc source;
    const LoadResult loaded = LoadFromString(SaveToString(source.document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& sketch = OnlySketch(*loaded.document);

    const Vec2* position = sketch.dimensionPlacement(source.placed);
    ASSERT_NE(position, nullptr);
    EXPECT_NEAR(position->x, 50.0, 1e-9);
    EXPECT_NEAR(position->y, -30.0, 1e-9);
}

TEST(SerializationV12Test, M16_SER_003_AnAutomaticDimensionWritesNoPositionAtAll) {
    PlacedDoc source;
    const std::string text = SaveToString(source.document);
    const LoadResult loaded = LoadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& sketch = OnlySketch(*loaded.document);

    // Only the MOVED one is written. Writing a position for an automatically
    // placed dimension would freeze today's layout rule into every file, so
    // that improving the layout later would silently not apply to old work.
    EXPECT_EQ(sketch.dimensionPlacement(source.automatic), nullptr);
    EXPECT_EQ(sketch.dimensionPlacements().size(), 1u);
}

TEST(SerializationV12Test, M16_SER_004_AFileWithNoPlacementsArrayStillLoads) {
    PlacedDoc source;
    std::string text = SaveToString(source.document);
    // Rename the key, as a pre-v12 file simply would not have it. The JSON
    // stays well formed, so the loader has to tolerate the ABSENT field rather
    // than fail on malformed input and look like it passed.
    const std::string key = "\"dimensionPlacements\"";
    std::size_t at = text.find(key);
    ASSERT_NE(at, std::string::npos);
    while (at != std::string::npos) {
        text.replace(at, key.size(), "\"legacyIgnoredField\"");
        at = text.find(key);
    }

    const LoadResult loaded = LoadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    // Every dimension is placed automatically -- exactly what such a file did
    // when it was written.
    EXPECT_TRUE(OnlySketch(*loaded.document).dimensionPlacements().empty());
}

TEST(SerializationV12Test, M16_SER_005_APlacementNamingNoConstraintIsDropped) {
    PlacedDoc source;
    std::string text = SaveToString(source.document);
    // Point the placement at an id the file does not contain.
    const std::string idText = std::to_string(static_cast<unsigned long long>(
        ToObjectId(source.placed)));
    const std::size_t at = text.find("\"constraintId\": \"" + idText + "\"");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, ("\"constraintId\": \"" + idText + "\"").size(),
                 "\"constraintId\": \"999999\"");

    const LoadResult loaded = LoadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    // DROPPED, not kept: an orphan would be written back out on the next save
    // and could re-attach itself to whatever later reused that id.
    EXPECT_TRUE(OnlySketch(*loaded.document).dimensionPlacements().empty());
}

TEST(SerializationV12Test, M16_SER_006_ANonFinitePositionIsRefused) {
    PlacedDoc source;
    std::string text = SaveToString(source.document);
    const std::size_t at = text.find("\"u\": 50");
    ASSERT_NE(at, std::string::npos);
    // A number the JSON reader accepts but the model must not: 1e999 parses to
    // infinity, and an infinite label position would put a dimension nowhere.
    text.replace(at, std::string("\"u\": 50").size(), "\"u\": 1e999");

    const LoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
}
