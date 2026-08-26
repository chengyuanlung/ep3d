// M47.2 -- the weld symbol as a thing ON A DRAWING, and in a file.
//
// WeldSymbolTests pins the ISO 2553 rules. This pins the part only a document
// and a serializer can own: that a weld reaches the paper through the same
// refusal gate every other symbol does, that the side survives being written
// down, and -- the one that matters -- that the saver cannot quietly write a
// weld as something else.
//
// Before M47 the annotation saver was an if / else-if chain whose last branch
// was "otherwise, write a datum". A fifth body would have gone down it. The
// file would have written, loaded and opened; the welding instruction would
// have become a letter. That branch is now a visit with no default, so the
// sixth body is a build error rather than a drawing.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace {

using namespace paramcad;

DimensionAnchor Somewhere(Vec2 at) {
    DimensionAnchor anchor;
    anchor.kind = DimensionAnchorKind::Free;
    anchor.at = at;
    return anchor;
}

WeldSymbolSpec DoubleFillet() {
    WeldBead arrow;
    arrow.type = WeldType::Fillet;
    arrow.sizeMm = 5.0;
    arrow.sizeKind = FilletSizeKind::Throat;

    WeldBead other;
    other.type = WeldType::Fillet;
    other.sizeMm = 6.0;
    other.sizeKind = FilletSizeKind::Leg;
    WeldRun run;
    run.count = 4;
    run.lengthMm = 40.0;
    run.gapMm = 60.0;
    other.run = run;

    WeldSymbolSpec spec;
    spec.arrowSide = arrow;
    spec.otherSide = other;
    spec.allAround = true;
    spec.tail = "ISO 4063-135";
    return spec;
}

std::string Save(const DrawingDocument& document) {
    std::ostringstream out;
    const SaveResult result = saveDrawingDocument(document, out);
    EXPECT_EQ(result.error, SerializationError::None) << result.message;
    return out.str();
}

TEST(WeldDocumentTest, M47_DOC_001_AWeldIsSavedAsAWeldAndNotAsWhateverCameLast) {
    DrawingDocument document{"Bracket"};
    const ObjectId id =
        document.addAnnotation(DoubleFillet(), Somewhere(Vec2{40.0, 30.0}), Vec2{60.0, 50.0})
            .id();

    const std::string saved = Save(document);
    // THE BRANCH THAT USED TO BE A FALL-THROUGH. If a weld were written as a
    // datum this would still be a valid file -- which is exactly why the
    // assertion is on the name and not on whether it loads.
    EXPECT_NE(saved.find("\"kind\": \"weld\""), std::string::npos);
    EXPECT_NE(saved.find("arrowSide"), std::string::npos);
    EXPECT_NE(saved.find("otherSide"), std::string::npos);

    std::istringstream in(saved);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const Annotation* back = loaded.document->findAnnotation(id);
    ASSERT_NE(back, nullptr);
    ASSERT_TRUE(back->isWeld()) << "a weld came back as some other symbol";
    EXPECT_EQ(loaded.document->annotationText(id), document.annotationText(id));
}

TEST(WeldDocumentTest, M47_DOC_002_TheSideSurvivesTheFile) {
    // THE FAILURE THIS FILE EXISTS FOR. A weld on the far side of a joint,
    // written and read back onto the near side, is a drawing that tells the
    // shop to weld a face they can reach instead of one they cannot -- and
    // both drawings look right.
    DrawingDocument document{"Bracket"};
    WeldSymbolSpec farSideOnly;
    WeldBead bead;
    bead.type = WeldType::Fillet;
    bead.sizeMm = 5.0;
    bead.sizeKind = FilletSizeKind::Leg;
    farSideOnly.otherSide = bead;

    const ObjectId id =
        document.addAnnotation(farSideOnly, Somewhere(Vec2{10.0, 10.0}), Vec2{20.0, 20.0}).id();

    std::istringstream in(Save(document));
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const Annotation* back = loaded.document->findAnnotation(id);
    ASSERT_NE(back, nullptr);
    const auto* spec = std::get_if<WeldSymbolSpec>(&back->body());
    ASSERT_NE(spec, nullptr);
    EXPECT_FALSE(spec->arrowSide.has_value()) << "an empty near side came back with a weld on it";
    ASSERT_TRUE(spec->otherSide.has_value()) << "the far side's weld was lost";
    EXPECT_EQ(spec->otherSide->sizeKind, FilletSizeKind::Leg);
    // AND THE INTERMITTENT RUN CAME WITH IT. A far side that quietly lost its
    // run reads as a continuous weld: more metal, more distortion, and the
    // side nobody checks.
    EXPECT_FALSE(spec->otherSide->run.has_value());
}

TEST(WeldDocumentTest, M47_DOC_003_TheRunSurvivesWithItsGapUnderItsOwnName) {
    DrawingDocument document{"Bracket"};
    const ObjectId id =
        document.addAnnotation(DoubleFillet(), Somewhere(Vec2{40.0, 30.0}), Vec2{60.0, 50.0})
            .id();
    std::istringstream in(Save(document));
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const auto* spec = std::get_if<WeldSymbolSpec>(&loaded.document->findAnnotation(id)->body());
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(spec->otherSide.has_value());
    ASSERT_TRUE(spec->otherSide->run.has_value());
    EXPECT_EQ(spec->otherSide->run->count, 4);
    EXPECT_NEAR(spec->otherSide->run->lengthMm, 40.0, 1e-9);
    EXPECT_NEAR(spec->otherSide->run->gapMm, 60.0, 1e-9);
    EXPECT_NEAR(RunExtentMm(*spec->otherSide->run), 340.0, 1e-9);
}

TEST(WeldDocumentTest, M47_DOC_004_TheDocumentRefusesAWeldTheStandardRefuses) {
    // ONE GATE FOR EVERY SYMBOL. A weld that WhyWeldRefused turns down has to
    // reach the drawing the same way a bad frame does: no text, and a reason.
    DrawingDocument document{"Bracket"};
    WeldSymbolSpec unsized;
    WeldBead bead;
    bead.type = WeldType::Fillet;
    bead.sizeMm = 5.0;
    bead.sizeKind = FilletSizeKind::Unspecified;   // throat or leg? nobody said
    unsized.arrowSide = bead;
    const ObjectId id =
        document.addAnnotation(unsized, Somewhere(Vec2{10.0, 10.0}), Vec2{20.0, 20.0}).id();

    EXPECT_FALSE(document.whyAnnotationRefused(id).empty());
    EXPECT_TRUE(document.annotationText(id).empty())
        << "a refused weld still put a size on the paper";
}

TEST(WeldDocumentTest, M47_DOC_005_WhatTheSaverRefusesTheLoaderRefuses) {
    // ADR-M3-008, by calling the same function. A file hand-edited to carry a
    // fillet with no throat-or-leg letter is not something this build will
    // draw, so it is not something it will read either -- the alternative is a
    // drawing that opens with a size nobody can act on.
    DrawingDocument document{"Bracket"};
    document.addAnnotation(DoubleFillet(), Somewhere(Vec2{40.0, 30.0}), Vec2{60.0, 50.0});
    std::string saved = Save(document);

    const std::string::size_type at = saved.find("\"sizeKind\": \"throat\"");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, std::string("\"sizeKind\": \"throat\"").size(),
                  "\"sizeKind\": \"unspecified\"");

    std::istringstream in(saved);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded) << "a fillet with no throat-or-leg letter loaded anyway";
}

TEST(WeldDocumentTest, M47_DOC_006_AWeldTypeThisBuildDoesNotKnowIsRefusedNotDefaulted) {
    // A TYPE THAT FELL BACK WOULD BECOME A FILLET, and a butt weld read as a
    // fillet is a joint with no penetration -- drawn, and welded, and never
    // questioned until it opens.
    DrawingDocument document{"Bracket"};
    document.addAnnotation(DoubleFillet(), Somewhere(Vec2{40.0, 30.0}), Vec2{60.0, 50.0});
    std::string saved = Save(document);
    const std::string::size_type at = saved.find("\"type\": \"fillet\"");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, std::string("\"type\": \"fillet\"").size(), "\"type\": \"double-v\"");

    std::istringstream in(saved);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded) << "an unknown weld type loaded as something else";
}

} // namespace
