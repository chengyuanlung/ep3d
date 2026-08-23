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
#include "Core/Feature/PlaceholderFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Feature/RevolveFeature.h"
#include "Core/Feature/FeatureSnapshot.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include "Fakes/FakeGeometryKernel.h"
#include <gtest/gtest.h>
#include <cstdio>
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

TEST(SerializationV8Test, M8_SER_205_AChamferBaseListedLaterIsRefusedOnLoad) {
    // Round 4, found independently by R1 (R1R4-M2) and R2 (R2R4-M2): the
    // consumer table `kConsumingFeatureTypeNames` claimed "Fillet/Chamfer
    // pinned by M8_SER_203", and 203 swaps Pad and FILLET -- nothing anywhere
    // swapped Fillet and CHAMFER, so dropping "Chamfer" from the table survived
    // every shipped test. That is R3R3-M1's finding reproduced in miniature, in
    // the very commit that fixed it, in the very table that commit added.
    //
    // 203's twin, one link further down the chain: Sketch -> Pad -> Fillet ->
    // Chamfer, with the CHAMFER moved ahead of the fillet it consumes.
    DressDoc doc;
    std::string saved = SaveToString(doc.document);

    const std::size_t filletPos = saved.find("\"name\": \"Fillet001\"");
    const std::size_t chamferPos = saved.find("\"name\": \"Chamfer001\"");
    ASSERT_NE(filletPos, std::string::npos);
    ASSERT_NE(chamferPos, std::string::npos);
    const std::size_t filletStart = saved.rfind('{', filletPos);
    const std::size_t chamferStart = saved.rfind('{', chamferPos);
    ASSERT_LT(filletStart, chamferStart);

    // Brace-matched extents, not `find("},")`: the chamfer is the LAST feature
    // in the array, so its record ends "}" with no comma, and the shortcut
    // 203 can afford produced malformed JSON here -- which the loader then
    // refused for the wrong reason, passing a weaker version of this test.
    const auto recordEnd = [&saved](std::size_t start) {
        int depth = 0;
        for (std::size_t i = start; i < saved.size(); ++i) {
            if (saved[i] == '{') ++depth;
            if (saved[i] == '}' && --depth == 0) return i;
        }
        return std::string::npos;
    };
    const std::size_t filletEnd = recordEnd(filletStart);
    const std::size_t chamferEnd = recordEnd(chamferStart);
    ASSERT_NE(filletEnd, std::string::npos);
    ASSERT_NE(chamferEnd, std::string::npos);
    const std::string filletRecord = saved.substr(filletStart, filletEnd - filletStart + 1);
    const std::string chamferRecord = saved.substr(chamferStart, chamferEnd - chamferStart + 1);
    // Later one first, so the earlier replacement cannot shift its offsets.
    saved.replace(chamferStart, chamferRecord.size(), filletRecord);
    saved.replace(filletStart, filletRecord.size(), chamferRecord);

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
    // The CHAIN-WALK diagnostic, not the restore facade's generic one: with
    // "Chamfer" missing from the table the walk falls silent and the refusal
    // degrades to a restore throw carrying the wrong SerializationError, past
    // the id-generator advance. Pinning the wording is what tells the two apart.
    EXPECT_NE(loaded.message.find("earlier solid feature"), std::string::npos)
        << loaded.message;
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
    // Round 2's converging Major (R2R2-M2 and R2-R1-M1): the save door decides
    // "solid" by ISolidFeature capability, the load door by the
    // kSolidFeatureTypeNames table -- and a drifted table was invisible to
    // every test. This is the drift guard, and it must cover the WHOLE table:
    // round 3 (R3R3-M1) proved the first version pinned only half of it --
    // Pocket/Fillet/Chamfer appeared only as consumers, so dropping "Chamfer"
    // from the table survived all 763 executing tests. Now EVERY name is
    // consumed as a base at least once:
    //   b1: Box <- Pocket1 <- Fillet1 <- Chamfer1   (pins Box, Pocket, Fillet)
    //   b2: Pad <- Chamfer2 <- Fillet2              (pins Pad, Chamfer)
    //   b3: Revolve <- Pocket2                      (pins Revolve)
    // The day ANY name drops off the loader's table -- or a new ISolidFeature
    // type ships without a table entry and a row here -- the load refuses and
    // this goes red.
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
    PocketFeature& pocket1 =
        document.addPocketFeature(b1, "Pocket001", box.id(), pocketSketch.id(), depth.id());
    FilletFeature& fillet1 =
        document.addFilletFeature(b1, "Fillet001", pocket1.id(), radius.id());
    document.addChamferFeature(b1, "Chamfer001", fillet1.id(), distance.id());

    Body& b2 = document.addBody("PadBody");
    PadFeature& pad = document.addPadFeature(b2, "Pad001", padSketch.id(), d.id());
    ChamferFeature& chamfer2 =
        document.addChamferFeature(b2, "Chamfer002", pad.id(), distance.id());
    document.addFilletFeature(b2, "Fillet002", chamfer2.id(), radius.id());

    Body& b3 = document.addBody("RevolveBody");
    RevolveFeature& revolve =
        document.addRevolveFeature(b3, "Revolve001", revolveSketch.id(), axis, angle.id());
    document.addPocketFeature(b3, "Pocket002", revolve.id(), pocketSketch.id(), depth.id());

    const LoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;

    int concreteSolids = 0;
    for (const auto& body : loaded.document->bodies())
        for (const auto& feature : body->features())
            if (dynamic_cast<const ISolidFeature*>(feature.get()) != nullptr) ++concreteSolids;
    EXPECT_EQ(concreteSolids, 9) << "a solid type came back as something else (placeholder?)";
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


// --- M9.1: the shared FeatureSnapshot -----------------------------------------

TEST(SerializationV8Test, M9_UNDO_401_SnapshotCapturesEveryTypesOwnFields) {
    // `FeatureSnapshot.h` promises that a type this function does not know is a
    // silent hole, and points here. So this is the test that has to notice.
    //
    // The RESTORE half is already pinned elsewhere and deliberately not
    // re-pinned here: the serializer's loader now goes through
    // `RestoreFeatureFromSnapshot`, so dropping a field from it breaks
    // M8_REV_322 (every solid type round-tripped as a chain base) and the v6/v7/
    // v8 record tests. What had NO coverage at all is `SnapshotFeature`, which
    // only undo calls -- so every assertion below is on the snapshot's fields,
    // compared against the ids the document was built with rather than against
    // the snapshot of something else. Comparing two snapshots would pass
    // happily with the same field dropped from both.
    PartDocument document{"Snap"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    Material& material = document.addMaterial("Aluminium", 2700.0);
    Parameter& a = document.addParameter("A", 10.0, UnitType::Millimeter);
    Parameter& b = document.addParameter("B", 20.0, UnitType::Millimeter);
    Parameter& c = document.addParameter("C", 30.0, UnitType::Millimeter);
    Parameter& angle = document.addParameter("Angle", 1.5, UnitType::Radian);

    Sketch& sketch = document.addSketch("S");
    const SketchEntityId axis = sketch.addLine(Vec2{0, -5}, Vec2{0, 55});
    sketch.addLine(Vec2{10, 0}, Vec2{30, 0});
    sketch.addLine(Vec2{30, 0}, Vec2{30, 50});
    sketch.addLine(Vec2{30, 50}, Vec2{10, 50});
    sketch.addLine(Vec2{10, 50}, Vec2{10, 0});

    Body& body = document.addBody("Body001");

    const BoxFeature& box = document.addBoxFeature(body, "Box001", a.id(), b.id(), c.id());
    const FeatureSnapshot boxSnap = SnapshotFeature(box);
    EXPECT_EQ(boxSnap.typeName, "Box");
    EXPECT_EQ(boxSnap.id, box.id());
    EXPECT_EQ(boxSnap.name, "Box001");
    EXPECT_EQ(boxSnap.widthParameterId, a.id());
    EXPECT_EQ(boxSnap.heightParameterId, b.id());
    EXPECT_EQ(boxSnap.depthParameterId, c.id());
    EXPECT_EQ(boxSnap.materialId, material.id());

    // A Pocket on the box: the chain reference is the field that matters most,
    // because losing it is what would silently unchain an undone deletion.
    Sketch& pocketSketch = document.addSketch("P");
    pocketSketch.addLine(Vec2{1, 1}, Vec2{2, 1});
    pocketSketch.addLine(Vec2{2, 1}, Vec2{2, 2});
    pocketSketch.addLine(Vec2{2, 2}, Vec2{1, 2});
    pocketSketch.addLine(Vec2{1, 2}, Vec2{1, 1});
    const PocketFeature& pocket =
        document.addPocketFeature(body, "Pocket001", box.id(), pocketSketch.id(), a.id());
    const FeatureSnapshot pocketSnap = SnapshotFeature(pocket);
    EXPECT_EQ(pocketSnap.typeName, "Pocket");
    EXPECT_EQ(pocketSnap.baseFeatureId, box.id());
    EXPECT_EQ(pocketSnap.sketchId, pocketSketch.id());
    EXPECT_EQ(pocketSnap.depthParameterId, a.id());

    const FilletFeature& fillet =
        document.addFilletFeature(body, "Fillet001", pocket.id(), b.id());
    const FeatureSnapshot filletSnap = SnapshotFeature(fillet);
    EXPECT_EQ(filletSnap.typeName, "Fillet");
    EXPECT_EQ(filletSnap.baseFeatureId, pocket.id());
    EXPECT_EQ(filletSnap.sizeParameterId, b.id());

    const ChamferFeature& chamfer =
        document.addChamferFeature(body, "Chamfer001", fillet.id(), c.id());
    const FeatureSnapshot chamferSnap = SnapshotFeature(chamfer);
    EXPECT_EQ(chamferSnap.typeName, "Chamfer");
    EXPECT_EQ(chamferSnap.baseFeatureId, fillet.id());
    EXPECT_EQ(chamferSnap.sizeParameterId, c.id());

    // Pad and Revolve go in their own body: the one above already has a tail.
    Body& second = document.addBody("Body002");
    const PadFeature& pad = document.addPadFeature(second, "Pad001", sketch.id(), a.id());
    const FeatureSnapshot padSnap = SnapshotFeature(pad);
    EXPECT_EQ(padSnap.typeName, "Pad");
    EXPECT_EQ(padSnap.sketchId, sketch.id());
    EXPECT_EQ(padSnap.lengthParameterId, a.id());
    EXPECT_EQ(padSnap.materialId, material.id());

    Body& third = document.addBody("Body003");
    const RevolveFeature& revolve =
        document.addRevolveFeature(third, "Revolve001", sketch.id(), axis, angle.id());
    const FeatureSnapshot revolveSnap = SnapshotFeature(revolve);
    EXPECT_EQ(revolveSnap.typeName, "Revolve");
    EXPECT_EQ(revolveSnap.sketchId, sketch.id());
    EXPECT_EQ(revolveSnap.axisEntityId, ToObjectId(axis));
    EXPECT_EQ(revolveSnap.angleParameterId, angle.id());

    // A Placeholder carries only the common fields, and that is correct rather
    // than a hole -- it has no others. Asserted so the "unknown type" branch is
    // exercised rather than assumed.
    Body& fourth = document.addBody("Body004");
    const Feature& ghost = document.addPlaceholderFeature(fourth, "Ghost", "Loft");
    const FeatureSnapshot ghostSnap = SnapshotFeature(ghost);
    EXPECT_EQ(ghostSnap.typeName, "Loft");
    EXPECT_EQ(ghostSnap.name, "Ghost");
    EXPECT_EQ(ghostSnap.baseFeatureId, kInvalidObjectId);
    EXPECT_EQ(ghostSnap.sketchId, kInvalidObjectId);
}

} // namespace
