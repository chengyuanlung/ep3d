// M41.2 -- the symbols as things ON A DRAWING.
//
// AnnotationTests pins the rules. This pins the part only a document can own:
// that a datum's LETTER is derived from where it sits in the drawing, that
// every frame naming it reads the same letter, and that neither of those two
// facts can be got at from any other direction.
//
// The failure being guarded is the one M38's section letters were: two places
// each holding a letter, agreeing on the day they were written and disagreeing
// the first time anything is deleted -- with both halves looking correct.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using namespace paramcad;

DimensionAnchor Somewhere(Vec2 at) {
    DimensionAnchor anchor;
    anchor.kind = DimensionAnchorKind::Free;
    anchor.at = at;
    return anchor;
}

// A drawing with three datums on it, placed in order.
struct Sheet {
    DrawingDocument document{"Plate"};
    ObjectId a = kInvalidObjectId;
    ObjectId b = kInvalidObjectId;
    ObjectId c = kInvalidObjectId;

    Sheet() {
        a = document.addAnnotation(DatumFeatureSpec{}, Somewhere(Vec2{10.0, 10.0}),
                                   Vec2{10.0, 20.0}).id();
        b = document.addAnnotation(DatumFeatureSpec{}, Somewhere(Vec2{50.0, 10.0}),
                                   Vec2{50.0, 20.0}).id();
        c = document.addAnnotation(DatumFeatureSpec{}, Somewhere(Vec2{90.0, 10.0}),
                                   Vec2{90.0, 20.0}).id();
    }

    ObjectId frameOn(std::vector<ObjectId> datums, double tolerance = 0.2) {
        FeatureControlFrameSpec spec;
        spec.characteristic = GeometricCharacteristic::Position;
        spec.toleranceMm = tolerance;
        spec.diametricZone = true;
        for (const ObjectId id : datums) spec.datums.push_back(DatumReference{id});
        return document.addAnnotation(spec, Somewhere(Vec2{30.0, 60.0}), Vec2{30.0, 70.0})
            .id();
    }
};

TEST(AnnotationDocumentTest, M41_DOC_001_TheLetterIsDerivedFromWHEREITSITSInTheDrawing) {
    Sheet sheet;
    EXPECT_EQ(sheet.document.datumLetterOf(sheet.a), "A");
    EXPECT_EQ(sheet.document.datumLetterOf(sheet.b), "B");
    EXPECT_EQ(sheet.document.datumLetterOf(sheet.c), "C");

    // A symbol that is not a datum has no letter, and asking for one gets
    // nothing rather than "A".
    const ObjectId frame = sheet.frameOn({sheet.a});
    EXPECT_TRUE(sheet.document.datumLetterOf(frame).empty());
    EXPECT_TRUE(sheet.document.datumLetterOf(999333).empty());
}

TEST(AnnotationDocumentTest, M41_DOC_002_AFrameReadsTheLetterItsDatumCURRENTLYCarries) {
    // THE WHOLE POINT OF DERIVING IT. Delete an unreferenced datum ahead of
    // the ones a frame names, and every letter after it moves up -- the frame
    // follows, because it never held a letter of its own. Stored, it would go
    // on saying B while the symbol on the face said A, and both would look
    // entirely reasonable.
    Sheet sheet;
    const ObjectId frame = sheet.frameOn({sheet.b, sheet.c});
    EXPECT_NE(sheet.document.annotationText(frame).find("| B"), std::string::npos)
        << sheet.document.annotationText(frame);
    EXPECT_NE(sheet.document.annotationText(frame).find("| C"), std::string::npos);

    // A is named by nothing, so it can go.
    ASSERT_TRUE(sheet.document.removeObject(sheet.a));
    EXPECT_EQ(sheet.document.datumLetterOf(sheet.b), "A");
    EXPECT_EQ(sheet.document.datumLetterOf(sheet.c), "B");
    const std::string after = sheet.document.annotationText(frame);
    EXPECT_NE(after.find("| A"), std::string::npos) << after;
    EXPECT_NE(after.find("| B"), std::string::npos) << after;
    EXPECT_EQ(after.find("| C"), std::string::npos)
        << "the frame kept a letter that no datum carries any more: " << after;
}

TEST(AnnotationDocumentTest, M41_DOC_003_ADatumFramesStillNameIsNotDeleted) {
    // The two alternatives are both worse and both silent-ish: cascading the
    // delete throws away frames the user did not ask to lose, and letting them
    // dangle leaves a drawing that will not SAVE -- which the user meets much
    // later, with no way to connect it to this delete.
    Sheet sheet;
    const ObjectId frame = sheet.frameOn({sheet.b});
    EXPECT_EQ(sheet.document.framesReferringToDatum(sheet.b), 1u);

    EXPECT_FALSE(sheet.document.removeObject(sheet.b))
        << "a datum that a frame still names was deleted";
    EXPECT_NE(sheet.document.findAnnotation(sheet.b), nullptr);

    // ...and once the frame has gone, so can the datum.
    ASSERT_TRUE(sheet.document.removeObject(frame));
    EXPECT_EQ(sheet.document.framesReferringToDatum(sheet.b), 0u);
    EXPECT_TRUE(sheet.document.removeObject(sheet.b));
}

TEST(AnnotationDocumentTest, M41_DOC_004_ASymbolThatCannotBeDrawnIsREFUSEDAtTheDoor) {
    Sheet sheet;
    // A form tolerance with a datum.
    FeatureControlFrameSpec flat;
    flat.characteristic = GeometricCharacteristic::Flatness;
    flat.toleranceMm = 0.05;
    flat.diametricZone = false;
    flat.datums.push_back(DatumReference{sheet.a});
    EXPECT_THROW(sheet.document.addAnnotation(flat, Somewhere(Vec2{}), Vec2{}),
                 std::invalid_argument);

    // A frame naming something that is not a datum at all.
    const ObjectId frame = sheet.frameOn({sheet.a});
    FeatureControlFrameSpec wrong;
    wrong.characteristic = GeometricCharacteristic::Position;
    wrong.toleranceMm = 0.2;
    wrong.datums.push_back(DatumReference{frame});
    EXPECT_THROW(sheet.document.addAnnotation(wrong, Somewhere(Vec2{}), Vec2{}),
                 std::invalid_argument);

    // A surface finish that contradicts itself.
    SurfaceFinishSpec cast;
    cast.symbol = SurfaceSymbol::AsCast;
    cast.machiningAllowanceMm = 2.0;
    EXPECT_THROW(sheet.document.addAnnotation(cast, Somewhere(Vec2{}), Vec2{}),
                 std::invalid_argument);

    // ...and the same refusals on an EDIT, which is the other way in.
    EXPECT_FALSE(sheet.document.setAnnotationBody(frame, flat));
    EXPECT_FALSE(sheet.document.setAnnotationBody(frame, cast));
}

TEST(AnnotationDocumentTest, M41_DOC_005_AddingMovingAndEditingAreEachONEUndoStep) {
    Sheet sheet;
    const std::size_t before = sheet.document.annotations().size();
    const ObjectId frame = sheet.frameOn({sheet.a}, 0.2);
    EXPECT_EQ(sheet.document.annotations().size(), before + 1);

    ASSERT_TRUE(sheet.document.undo());
    EXPECT_EQ(sheet.document.annotations().size(), before);
    ASSERT_TRUE(sheet.document.redo());
    ASSERT_NE(sheet.document.findAnnotation(frame), nullptr);

    ASSERT_TRUE(sheet.document.setAnnotationPosition(frame, Vec2{120.0, 140.0}));
    EXPECT_NEAR(sheet.document.findAnnotation(frame)->positionMm().x, 120.0, 1e-9);
    ASSERT_TRUE(sheet.document.undo());
    EXPECT_NEAR(sheet.document.findAnnotation(frame)->positionMm().x, 30.0, 1e-9);

    // An edit that changes what the frame SAYS comes back too -- and what it
    // says is the part a reader acts on.
    FeatureControlFrameSpec tighter;
    tighter.characteristic = GeometricCharacteristic::Position;
    tighter.toleranceMm = 0.05;
    tighter.diametricZone = true;
    tighter.datums.push_back(DatumReference{sheet.a});
    ASSERT_TRUE(sheet.document.setAnnotationBody(frame, tighter));
    EXPECT_NE(sheet.document.annotationText(frame).find("0.05"), std::string::npos);
    ASSERT_TRUE(sheet.document.undo());
    EXPECT_NE(sheet.document.annotationText(frame).find("0.2"), std::string::npos)
        << sheet.document.annotationText(frame);
}

TEST(AnnotationDocumentTest, M41_DOC_006_TheSymbolsSurviveASaveAndKeepTheirLetters) {
    Sheet sheet;
    const ObjectId frame = sheet.frameOn({sheet.b, sheet.c});
    SurfaceFinishSpec finish;
    finish.symbol = SurfaceSymbol::Machined;
    finish.raMicrometres = 0.8;
    finish.process = "ground";
    finish.lay = SurfaceLay::Perpendicular;
    const ObjectId surface =
        sheet.document.addAnnotation(finish, Somewhere(Vec2{70.0, 40.0}), Vec2{70.0, 55.0})
            .id();

    const std::string said = sheet.document.annotationText(frame);
    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(sheet.document, out));
    const std::string text = out.str();
    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;

    EXPECT_EQ(loaded.document->annotations().size(), 5u);
    EXPECT_EQ(loaded.document->datumLetterOf(sheet.b), "B")
        << "the datums came back in a different order, so the letters moved";
    EXPECT_EQ(loaded.document->annotationText(frame), said);
    EXPECT_EQ(loaded.document->annotationText(surface), "ground Ra 0.8 " +
                                                           SymbolOfLay(SurfaceLay::Perpendicular));
    // What the file writes, it reads.
    std::ostringstream again;
    ASSERT_TRUE(saveDrawingDocument(*loaded.document, again));
    EXPECT_EQ(again.str(), text);
}

TEST(AnnotationDocumentTest, M41_DOC_007_AFileWhoseFrameNamesAMissingDatumIsREFUSED) {
    // ADR-M3-008, and by the SAME call: the saver asks the document whether a
    // symbol can be drawn, and so does the loader. A file written by hand or
    // by an older build is where this becomes reachable.
    Sheet sheet;
    sheet.frameOn({sheet.b});
    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(sheet.document, out));
    std::string text = out.str();

    const std::string real = "\"datumId\": \"" + std::to_string(sheet.b) + "\"";
    const std::size_t at = text.find(real);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, real.size(), "\"datumId\": \"606060\"");

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded) << "a frame pointing at a datum that is not there was accepted";
    EXPECT_NE(loaded.message.find("datum"), std::string::npos) << loaded.message;
}

TEST(AnnotationDocumentTest, M41_DOC_008_AFileNamingASymbolThisBuildDoesNotKNOWIsREFUSED) {
    // Defaulted instead, a characteristic this build has never heard of would
    // become position -- a different specification that draws as an ordinary
    // frame. The same for a surface symbol, where the two ends of the range
    // are "must be machined" and "must not be".
    Sheet sheet;
    sheet.frameOn({sheet.a});
    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(sheet.document, out));
    const std::string good = out.str();

    std::string text = good;
    std::size_t at = text.find("\"characteristic\": \"position\"");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"characteristic\": \"position\"").size(),
                 "\"characteristic\": \"squareness\"");
    std::istringstream in(text);
    EXPECT_FALSE(loadDrawingDocument(in)) << "an unknown characteristic became position";

    SurfaceFinishSpec finish;
    finish.raMicrometres = 1.6;
    sheet.document.addAnnotation(finish, Somewhere(Vec2{}), Vec2{});
    std::ostringstream second;
    ASSERT_TRUE(saveDrawingDocument(sheet.document, second));
    text = second.str();
    at = text.find("\"symbol\": \"machined\"");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"symbol\": \"machined\"").size(),
                 "\"symbol\": \"whatever\"");
    std::istringstream third(text);
    EXPECT_FALSE(loadDrawingDocument(third))
        << "an unknown surface symbol was read as one of the three";
}

} // namespace
