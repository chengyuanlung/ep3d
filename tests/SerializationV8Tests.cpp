// Schema v8 (M8.3): Fillet and Chamfer records, at the Core level.
//
// UNLIKE v6/v7, these tests do NOT recompute after load: the fake kernel
// deliberately models no fillets (a rounded box's closed form here would make
// Core tests agree with this file's arithmetic instead of with geometry).
// Round-trip IDENTITY is asserted here; recompute-after-load with the real
// kernel is GATE_FD in the integration suite. Both halves exist; neither
// pretends to be the other.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/EdgeDressFeatures.h"
#include "Core/Feature/PadFeature.h"
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

struct DressDoc {
    PartDocument document{"V8Doc"};
    ObjectId padId = kInvalidObjectId;
    ObjectId filletId = kInvalidObjectId;
    ObjectId chamferId = kInvalidObjectId;
    ObjectId radiusId = kInvalidObjectId;
    ObjectId distanceId = kInvalidObjectId;

    DressDoc() {
        document.addMaterial("Aluminium", 2700.0);
        Parameter& padLength = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
        Parameter& radius = document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
        Parameter& distance = document.addParameter("ChamferDistance", 1.0, UnitType::Millimeter);
        radiusId = radius.id();
        distanceId = distance.id();

        Sketch& sketch = document.addSketch("PadSketch");
        sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
        sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
        sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
        sketch.addLine(Vec2{0, 50}, Vec2{0, 0});

        Body& body = document.addBody("Body001");
        PadFeature& pad = document.addPadFeature(body, "Pad001", sketch.id(), padLength.id());
        padId = pad.id();
        // A fillet on the pad, and a chamfer CHAINED ON THE FILLET -- so the
        // file carries both record types AND a dress-on-dress chain.
        FilletFeature& fillet =
            document.addFilletFeature(body, "Fillet001", padId, radius.id());
        filletId = fillet.id();
        ChamferFeature& chamfer =
            document.addChamferFeature(body, "Chamfer001", filletId, distance.id());
        chamferId = chamfer.id();
    }
};

TEST(SerializationV8Test, M8_SER_201_FilletAndChamferRoundTripWithTheirChain) {
    DressDoc doc;
    const std::string saved = SaveToString(doc.document);
    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;

    const FilletFeature* fillet = nullptr;
    const ChamferFeature* chamfer = nullptr;
    for (const auto& body : loaded.document->bodies())
        for (const auto& feature : body->features()) {
            if (auto* f = dynamic_cast<FilletFeature*>(feature.get())) fillet = f;
            if (auto* c = dynamic_cast<ChamferFeature*>(feature.get())) chamfer = c;
        }
    ASSERT_NE(fillet, nullptr) << "the fillet did not survive as a Fillet";
    ASSERT_NE(chamfer, nullptr) << "the chamfer did not survive as a Chamfer";

    EXPECT_EQ(fillet->id(), doc.filletId);
    EXPECT_EQ(fillet->baseFeatureId(), doc.padId);
    EXPECT_EQ(fillet->sizeParameterId(), doc.radiusId);
    // The dress-on-dress chain: the chamfer's base is the FILLET.
    EXPECT_EQ(chamfer->id(), doc.chamferId);
    EXPECT_EQ(chamfer->baseFeatureId(), doc.filletId);
    EXPECT_EQ(chamfer->sizeParameterId(), doc.distanceId);
}

TEST(SerializationV8Test, M8_SER_202_TheTwoTypesDoNotSwapOnRoundTrip) {
    // Fillet and Chamfer share one record shape, discriminated only by the
    // type string -- exactly the situation where a dispatch typo turns every
    // round into a bevel and no id-level assertion notices. Pinned by TYPE.
    DressDoc doc;
    const LoadResult loaded = LoadFromString(SaveToString(doc.document));
    ASSERT_TRUE(loaded) << loaded.message;

    int fillets = 0;
    int chamfers = 0;
    for (const auto& body : loaded.document->bodies())
        for (const auto& feature : body->features()) {
            if (feature->typeName() == "Fillet") ++fillets;
            if (feature->typeName() == "Chamfer") ++chamfers;
        }
    EXPECT_EQ(fillets, 1);
    EXPECT_EQ(chamfers, 1);
}

TEST(SerializationV8Test, M8_SER_203_ADressBaseListedLaterIsRefusedOnLoad) {
    DressDoc doc;
    std::string saved = SaveToString(doc.document);

    // Swap the pad and fillet records: the fillet then precedes its base.
    const std::size_t padPos = saved.find("\"name\": \"Pad001\"");
    const std::size_t filletPos = saved.find("\"name\": \"Fillet001\"");
    ASSERT_NE(padPos, std::string::npos);
    ASSERT_NE(filletPos, std::string::npos);
    const std::size_t padStart = saved.rfind('{', padPos);
    const std::size_t filletStart = saved.rfind('{', filletPos);
    ASSERT_LT(padStart, filletStart);
    const std::size_t padEnd = saved.find("},", padStart);
    const std::string padRecord = saved.substr(padStart, padEnd - padStart + 1);
    const std::size_t filletEnd = saved.find("},", filletStart);
    const std::string filletRecord = saved.substr(filletStart, filletEnd - filletStart + 1);
    saved.replace(filletStart, filletRecord.size(), padRecord);
    saved.replace(padStart, padRecord.size(), filletRecord);

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
    EXPECT_NE(loaded.message.find("earlier feature"), std::string::npos) << loaded.message;
}

TEST(SerializationV8Test, M8_SER_204_SavingADressWhoseBaseIsGoneIsRefused) {
    DressDoc doc;
    // Remove the chamfer first so only the fillet consumes the pad; then
    // removing the PAD leaves the fillet's base dangling.
    ASSERT_TRUE(doc.document.removeObject(doc.chamferId));
    ASSERT_TRUE(doc.document.removeObject(doc.padId));

    std::ostringstream out;
    const SaveResult saved = savePartDocument(doc.document, out);
    EXPECT_FALSE(saved);
    EXPECT_NE(saved.message.find("base feature"), std::string::npos) << saved.message;
}

} // namespace
