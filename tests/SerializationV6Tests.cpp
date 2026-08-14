// Schema v6 (M8): the Pocket record and the chain reference, at the Core level
// with the fake kernel -- no OCCT, no solver.
//
// The fake's subtract models FULL CONTAINMENT analytically (volume difference),
// which is exactly the shape these fixtures build, so the recompute-after-load
// checks are real arithmetic rather than agreement with a stub.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include "Fakes/FakeGeometryKernel.h"
#include <gtest/gtest.h>
#include <sstream>
#include <string>

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

struct PocketDoc {
    PartDocument document{"V6Doc"};
    FakeGeometryKernel kernel;
    ObjectId padId = kInvalidObjectId;
    ObjectId pocketId = kInvalidObjectId;
    ObjectId depthId = kInvalidObjectId;

    PocketDoc() {
        document.setGeometryKernel(&kernel);
        document.addMaterial("Aluminium", 2700.0);
        Parameter& padLength = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
        Parameter& depth = document.addParameter("PocketDepth", 10.0, UnitType::Millimeter);
        depthId = depth.id();

        Sketch& padSketch = document.addSketch("PadSketch");
        padSketch.addLine(Vec2{0, 0}, Vec2{100, 0});
        padSketch.addLine(Vec2{100, 0}, Vec2{100, 50});
        padSketch.addLine(Vec2{100, 50}, Vec2{0, 50});
        padSketch.addLine(Vec2{0, 50}, Vec2{0, 0});

        Sketch& pocketSketch = document.addSketch("PocketSketch");
        pocketSketch.addLine(Vec2{10, 10}, Vec2{30, 10});
        pocketSketch.addLine(Vec2{30, 10}, Vec2{30, 40});
        pocketSketch.addLine(Vec2{30, 40}, Vec2{10, 40});
        pocketSketch.addLine(Vec2{10, 40}, Vec2{10, 10});

        Body& body = document.addBody("Body001");
        PadFeature& pad =
            document.addPadFeature(body, "Pad001", padSketch.id(), padLength.id());
        padId = pad.id();
        PocketFeature& pocket =
            document.addPocketFeature(body, "Pocket001", padId, pocketSketch.id(), depthId);
        pocketId = pocket.id();
    }
};

TEST(SerializationV6Test, M8_SER_001_PocketRoundTripsWithItsChainReference) {
    PocketDoc doc;
    const std::string saved = SaveToString(doc.document);

    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;

    // The pocket came back as a POCKET -- not a placeholder -- with every
    // semantic reference intact, above all the chain reference.
    const PocketFeature* pocket = nullptr;
    for (const auto& body : loaded.document->bodies())
        for (const auto& feature : body->features())
            if (auto* p = dynamic_cast<PocketFeature*>(feature.get())) pocket = p;
    ASSERT_NE(pocket, nullptr) << "the pocket did not survive the round trip as a Pocket";
    EXPECT_EQ(pocket->id(), doc.pocketId);
    EXPECT_EQ(pocket->baseFeatureId(), doc.padId);
    EXPECT_EQ(pocket->depthParameterId(), doc.depthId);

    // And the chain WORKS after load: 100000 - 6000 = 94000 with the fake's
    // analytical subtract.
    FakeGeometryKernel kernel;
    loaded.document->setGeometryKernel(&kernel);
    ASSERT_TRUE(loaded.document->recompute().success);
    ASSERT_TRUE(loaded.document->massProperties().valid);
    EXPECT_NEAR(loaded.document->massProperties().volumeMm3, 94000.0, 1e-9);
}

TEST(SerializationV6Test, M8_SER_002_EditAfterLoadDrivesTheWholeChain) {
    PocketDoc doc;
    const std::string saved = SaveToString(doc.document);
    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;

    FakeGeometryKernel kernel;
    loaded.document->setGeometryKernel(&kernel);
    ASSERT_TRUE(loaded.document->recompute().success);

    // Deepen the pocket to a through cut: 100000 - 20*30*20 = 88000.
    ASSERT_TRUE(loaded.document->setParameterValue(doc.depthId, 20.0));
    ASSERT_TRUE(loaded.document->recomputeFrom(doc.depthId).success);
    EXPECT_NEAR(loaded.document->massProperties().volumeMm3, 88000.0, 1e-9);
}

TEST(SerializationV6Test, M8_SER_003_ABaseListedAfterItsPocketIsRefusedOnLoad) {
    PocketDoc doc;
    std::string saved = SaveToString(doc.document);

    // Swap the two feature records so the pocket precedes its base -- the file
    // shape the loader must refuse, because restore replays array order and
    // would wire an edge to a node that does not exist yet.
    const std::size_t padPos = saved.find("\"name\": \"Pad001\"");
    const std::size_t pocketPos = saved.find("\"name\": \"Pocket001\"");
    ASSERT_NE(padPos, std::string::npos);
    ASSERT_NE(pocketPos, std::string::npos);
    // Feature records are JSON objects in an array; swap by object boundaries.
    const std::size_t padStart = saved.rfind('{', padPos);
    const std::size_t pocketStart = saved.rfind('{', pocketPos);
    ASSERT_LT(padStart, pocketStart);
    const std::size_t padEnd = saved.find("},", padStart);
    ASSERT_NE(padEnd, std::string::npos);
    const std::string padRecord = saved.substr(padStart, padEnd - padStart + 1);
    const std::size_t pocketEnd = saved.find('}', pocketStart);
    const std::string pocketRecord = saved.substr(pocketStart, pocketEnd - pocketStart + 1);
    saved.replace(pocketStart, pocketRecord.size(), padRecord);
    saved.replace(padStart, padRecord.size(), pocketRecord);

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("earlier feature"), std::string::npos) << loaded.message;
}

TEST(SerializationV6Test, M8_SER_004_APocketDepthThatIsNotAParameterIsRefused) {
    PocketDoc doc;
    std::string saved = SaveToString(doc.document);

    // Point the pocket's depth at an id that is not a parameter in the file.
    const std::string needle = "\"depthParameterId\": \"" + std::to_string(doc.depthId) + "\"";
    const std::size_t pos = saved.find(needle);
    ASSERT_NE(pos, std::string::npos);
    saved.replace(pos, needle.size(), "\"depthParameterId\": \"999999\"");

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("depth parameter"), std::string::npos) << loaded.message;
}

} // namespace
