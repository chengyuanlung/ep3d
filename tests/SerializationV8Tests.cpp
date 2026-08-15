// Schema v8 (M8.3): Fillet and Chamfer records, at the Core level.
//
// UNLIKE v6/v7, these tests do NOT recompute after load: the fake kernel
// deliberately models no fillets (a rounded box's closed form here would make
// Core tests agree with this file's arithmetic instead of with geometry).
// Round-trip IDENTITY is asserted here; recompute-after-load with the real
// kernel is GATE_FD in the integration suite. Both halves exist; neither
// pretends to be the other.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Feature/EdgeDressFeatures.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Feature/RevolveFeature.h"
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
    // round into a bevel and no id-level assertion notices. Pinned PER ID:
    // review (R2-m1/R3-m2) showed the original count-only version was
    // swap-blind -- a SYMMETRIC dispatch swap keeps one Fillet and one
    // Chamfer in the document and the counts pass. The id->type mapping is
    // what a swap cannot preserve.
    DressDoc doc;
    const LoadResult loaded = LoadFromString(SaveToString(doc.document));
    ASSERT_TRUE(loaded) << loaded.message;

    int fillets = 0;
    int chamfers = 0;
    for (const auto& body : loaded.document->bodies())
        for (const auto& feature : body->features()) {
            if (feature->typeName() == "Fillet") {
                ++fillets;
                EXPECT_EQ(feature->id(), doc.filletId) << "the Fillet id belongs to the chamfer";
            }
            if (feature->typeName() == "Chamfer") {
                ++chamfers;
                EXPECT_EQ(feature->id(), doc.chamferId) << "the Chamfer id belongs to the fillet";
            }
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
    EXPECT_NE(loaded.message.find("earlier solid feature"), std::string::npos) << loaded.message;
}

TEST(SerializationV8Test, M8_SER_204_SavingADressWhoseBaseIsGoneIsRefused) {
    DressDoc doc;
    // Remove the chamfer first (it consumes the FILLET -- round 2 caught this
    // comment claiming it consumed the pad); then removing the PAD leaves the
    // fillet's base dangling.
    ASSERT_TRUE(doc.document.removeObject(doc.chamferId));
    ASSERT_TRUE(doc.document.removeObject(doc.padId));

    std::ostringstream out;
    const SaveResult saved = savePartDocument(doc.document, out);
    EXPECT_FALSE(saved);
    EXPECT_NE(saved.message.find("base feature"), std::string::npos) << saved.message;
}

TEST(SerializationV8Test, M8_REV_322_EverySolidTypeRoundTripsAsAChainBase) {
    // Round 2's converging Major (R2R2-M2 and R2-R1-M1 found it
    // independently): the save door decides "solid" by ISolidFeature
    // capability, the load door by the kSolidFeatureTypeNames table -- and a
    // drifted table was invisible to every test. This is the drift guard:
    // one document in which EVERY solid type is consumed as a chain base
    // (Box<-Pocket, Pad<-Fillet, Revolve<-Chamfer). The day a name drops off
    // the loader's table -- or a new ISolidFeature type ships without a table
    // entry and a row here -- the load below refuses and this goes red.
    PartDocument document{"AllSolidBases"};
    document.addMaterial("Aluminium", 2700.0);
    Parameter& w = document.addParameter("W", 100.0, UnitType::Millimeter);
    Parameter& h = document.addParameter("H", 50.0, UnitType::Millimeter);
    Parameter& d = document.addParameter("D", 20.0, UnitType::Millimeter);
    Parameter& depth = document.addParameter("Depth", 10.0, UnitType::Millimeter);
    Parameter& radius = document.addParameter("Radius", 2.0, UnitType::Millimeter);
    Parameter& distance = document.addParameter("Distance", 1.0, UnitType::Millimeter);
    Parameter& angle = document.addParameter("Angle", 6.283185307179586, UnitType::Radian);

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
    Sketch& revolveSketch = document.addSketch("RevolveSketch");
    const SketchEntityId axis = revolveSketch.addLine(Vec2{0, -5}, Vec2{0, 45});
    revolveSketch.addLine(Vec2{10, 0}, Vec2{30, 0});
    revolveSketch.addLine(Vec2{30, 0}, Vec2{30, 40});
    revolveSketch.addLine(Vec2{30, 40}, Vec2{10, 40});
    revolveSketch.addLine(Vec2{10, 40}, Vec2{10, 0});

    Body& b1 = document.addBody("BoxBody");
    BoxFeature& box = document.addBoxFeature(b1, "Box001", w.id(), h.id(), d.id());
    document.addPocketFeature(b1, "Pocket001", box.id(), pocketSketch.id(), depth.id());
    Body& b2 = document.addBody("PadBody");
    PadFeature& pad = document.addPadFeature(b2, "Pad001", padSketch.id(), d.id());
    document.addFilletFeature(b2, "Fillet001", pad.id(), radius.id());
    Body& b3 = document.addBody("RevolveBody");
    RevolveFeature& revolve =
        document.addRevolveFeature(b3, "Revolve001", revolveSketch.id(), axis, angle.id());
    document.addChamferFeature(b3, "Chamfer001", revolve.id(), distance.id());

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;

    int concreteSolids = 0;
    for (const auto& body : loaded.document->bodies())
        for (const auto& feature : body->features())
            if (dynamic_cast<const ISolidFeature*>(feature.get()) != nullptr) ++concreteSolids;
    EXPECT_EQ(concreteSolids, 6) << "a solid type came back as something else (placeholder?)";
}

TEST(SerializationV8Test, M8_REV_321_RemovingTheSizeParameterMakesTheDocUnsavable) {
    // Round-1 R2-C1, probe A6: the last of the six save/load symmetry gaps
    // (ADR-M3-008) -- removing a dress feature's size parameter saved cleanly
    // and the loader refused the just-written bytes.
    DressDoc doc;
    ASSERT_TRUE(doc.document.removeObject(doc.radiusId));

    std::ostringstream out;
    const SaveResult saved = savePartDocument(doc.document, out);
    EXPECT_FALSE(saved);
    EXPECT_NE(saved.message.find("fillet/chamfer size parameter id"), std::string::npos)
        << saved.message;
}

} // namespace
