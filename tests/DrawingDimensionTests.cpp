// M34 -- dimensions: the thing that makes a drawing a drawing.
//
// A drawing without them is a picture. What these tests protect is the one
// property that separates the two: THE NUMBER IS THE PART'S. Not a number
// somebody typed, not a number measured on paper, and never a number left over
// from a shape that has since changed.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

std::string SaveToString(const DrawingDocument& document) {
    std::ostringstream out;
    EXPECT_TRUE(saveDrawingDocument(document, out));
    return out.str();
}

DrawingLoadResult LoadFromString(const std::string& text) {
    std::istringstream in(text);
    return loadDrawingDocument(in);
}

} // namespace

// =============================================================================
// The style
// =============================================================================

TEST(DrawingDimensionTest, M34_STYLE_001_EveryDrawingStartsWithOneAndCannotLoseIt) {
    // A dimension has to have a style. A drawing that made the user create one
    // before they could put a size on anything is a drawing nobody finishes.
    DrawingDocument document{"Sheet"};
    const DimensionStyle* iso = document.findDimensionStyleNamed(kDefaultDimensionStyleName);
    ASSERT_NE(iso, nullptr);
    EXPECT_EQ(document.currentDimensionStyleId(), iso->id());
    EXPECT_EQ(document.undoDepth(), 0u) << "the seeded style arrived as an undoable edit";
    EXPECT_FALSE(document.removeObject(iso->id())) << "the default dimension style was deleted";
}

TEST(DrawingDimensionTest, M34_STYLE_002_ANumberIsFormattedTheSameWayEverywhere) {
    DimensionStyle style{"Test"};
    style.setDecimals(2);
    EXPECT_EQ(style.format(25.0), "25.00")
        << "trailing zeros were trimmed -- '25.00' and '25' claim different intent";
    EXPECT_EQ(style.format(-0.0001), "0.00")
        << "a negative zero was printed beside the positive one";
    style.setDecimals(0);
    EXPECT_EQ(style.format(25.4), "25");
    style.setSuffix(" mm");
    EXPECT_EQ(style.format(25.4), "25 mm");
}

TEST(DrawingDimensionTest, M34_STYLE_003_AStyleThatDrawsNothingReadableIsREFUSED) {
    // A zero text height would be found at plot time, a long way from whoever
    // typed it. The old value stays, which is a style that still works.
    DimensionStyle style{"Test"};
    style.setTextHeightMm(0.0);
    EXPECT_NEAR(style.textHeightMm(), 3.5, 1e-12) << "a zero text height was accepted";
    style.setArrowSizeMm(-1.0);
    EXPECT_NEAR(style.arrowSizeMm(), 3.5, 1e-12);
    // ...but a ZERO GAP is a real house style, and refusing it would be this
    // file having an opinion it is not entitled to.
    style.setExtensionGapMm(0.0);
    EXPECT_NEAR(style.extensionGapMm(), 0.0, 1e-12);
}

TEST(DrawingDimensionTest, M34_STYLE_004_TheOverallScaleMultipliesEveryPaperLength) {
    // A drawing plotted at half size keeps its text readable without every
    // field being retyped -- and the MEASUREMENT is untouched, which is the
    // half that matters.
    DimensionStyle style{"Test"};
    style.setTextHeightMm(3.5);
    style.setOverallScale(2.0);
    EXPECT_NEAR(style.scaledTextHeightMm(), 7.0, 1e-12);
    EXPECT_NEAR(style.scaledArrowSizeMm(), 7.0, 1e-12);
    EXPECT_EQ(style.format(25.0), "25.00") << "the overall scale reached the measurement";
}

// =============================================================================
// What it measures
// =============================================================================

TEST(DrawingDimensionTest, M34_DIM_001_ALinearDimensionMeasuresBetweenTwoPoints) {
    DrawingDocument document{"Sheet"};
    const DrawingDimension& dimension = document.addDimension(
        DimensionKind::Linear, DimensionAnchor::free(Vec2{10.0, 10.0}),
        DimensionAnchor::free(Vec2{40.0, 50.0}), Vec2{25.0, 70.0});

    // Aligned: the true distance, 3-4-5.
    const DimensionMeasurement aligned = document.measure(dimension);
    ASSERT_TRUE(aligned.ok) << aligned.why;
    EXPECT_NEAR(aligned.valueMm, 50.0, 1e-9);

    // ...and the same two points, measured the other two ways.
    ASSERT_TRUE(document.setDimensionDirection(dimension.id(), LinearDirection::Horizontal));
    EXPECT_NEAR(document.measure(dimension).valueMm, 30.0, 1e-9);
    ASSERT_TRUE(document.setDimensionDirection(dimension.id(), LinearDirection::Vertical));
    EXPECT_NEAR(document.measure(dimension).valueMm, 40.0, 1e-9);
}

TEST(DrawingDimensionTest, M34_DIM_002_RadiusAndDiameterAreTheSameTwoPointsReadDifferently) {
    DrawingDocument document{"Sheet"};
    const DrawingDimension& radius = document.addDimension(
        DimensionKind::Radius, DimensionAnchor::free(Vec2{0.0, 0.0}),
        DimensionAnchor::free(Vec2{20.0, 0.0}), Vec2{10.0, 10.0});
    EXPECT_NEAR(document.measure(radius).valueMm, 20.0, 1e-9);
    EXPECT_EQ(document.dimensionText(radius), "R20.00")
        << "a radius without its R is a number a reader cannot place";

    const DrawingDimension& diameter = document.addDimension(
        DimensionKind::Diameter, DimensionAnchor::free(Vec2{0.0, 0.0}),
        DimensionAnchor::free(Vec2{20.0, 0.0}), Vec2{10.0, 10.0});
    EXPECT_NEAR(document.measure(diameter).valueMm, 40.0, 1e-9);
    // U+2300, the diameter sign.
    EXPECT_EQ(document.dimensionText(diameter), "\xE2\x8C\x80" "40.00");
}

TEST(DrawingDimensionTest, M34_DIM_003_AnAngleIsInDEGREESBecauseADrawingSaysDegrees) {
    DrawingDocument document{"Sheet"};
    // Vertex at the origin (the dimension line's position), arms along +X and
    // +Y: a right angle.
    const DrawingDimension& angle = document.addDimension(
        DimensionKind::Angular, DimensionAnchor::free(Vec2{10.0, 0.0}),
        DimensionAnchor::free(Vec2{0.0, 10.0}), Vec2{0.0, 0.0});
    EXPECT_NEAR(document.measure(angle).valueMm, 90.0, 1e-9);
    EXPECT_EQ(document.dimensionText(angle), "90.00\xC2\xB0");
}

TEST(DrawingDimensionTest, M34_DIM_003b_AnAngleIsTheSHORTWayROUND) {
    // M34-12 survived: nothing asked what an angle does past a half turn.
    //
    // Two arms 270 degrees apart the long way are 90 degrees apart the short
    // way, and 90 is what a drawing says -- an arc drawn the short way beside
    // the number 270 is a dimension contradicting its own picture. It also
    // makes the reading INDEPENDENT OF PICK ORDER, which matters because
    // nothing about clicking two lines says which came first.
    DrawingDocument document{"Sheet"};
    // THE ARMS HAVE TO STRADDLE THE atan2 CUT for the fold to be reached at
    // all. Up-left is +135 degrees and down-left is -135, so the raw
    // difference is 270 -- and the answer a drawing wants is the 90 the other
    // way. A first draft of this test used arms 90 apart and passed against
    // the folding code AND against code with the fold deleted, which is a test
    // that measures nothing.
    const DrawingDimension& reflex = document.addDimension(
        DimensionKind::Angular, DimensionAnchor::free(Vec2{-10.0, 10.0}),
        DimensionAnchor::free(Vec2{-10.0, -10.0}), Vec2{0.0, 0.0});
    EXPECT_NEAR(document.measure(reflex).valueMm, 90.0, 1e-9)
        << "an angle came back as its explement";

    // ...and the two orders agree, which is the same fact said the way a user
    // would meet it.
    const DrawingDimension& other = document.addDimension(
        DimensionKind::Angular, DimensionAnchor::free(Vec2{-10.0, -10.0}),
        DimensionAnchor::free(Vec2{-10.0, 10.0}), Vec2{0.0, 0.0});
    EXPECT_NEAR(document.measure(other).valueMm, document.measure(reflex).valueMm, 1e-9)
        << "which arm was clicked first changed the angle";

    // A straight angle is 180 and stays 180 -- the boundary the fold is on.
    const DrawingDimension& flat = document.addDimension(
        DimensionKind::Angular, DimensionAnchor::free(Vec2{10.0, 0.0}),
        DimensionAnchor::free(Vec2{-10.0, 0.0}), Vec2{0.0, 0.0});
    EXPECT_NEAR(document.measure(flat).valueMm, 180.0, 1e-9);
}

TEST(DrawingDimensionTest, M34_SER_006_SavingIsREFUSEDWhenADimensionNamesAMissingStyle) {
    // M34-13 survived: the LOADER refused this and the SAVER was never asked.
    //
    // ADR-M3-008's rule is that a document must not save cleanly and then
    // refuse to load, so the save has to check exactly what the load checks.
    // `restoreDimension` is the raw path the loader uses and it validates
    // nothing on purpose -- which is precisely the state a future migration,
    // or a bug in a reader, would leave behind.
    DrawingDocument document{"Sheet"};
    const ObjectId layerId = document.currentLayerId();
    document.restoreDimension(999001u, DimensionKind::Linear,
                              DimensionAnchor::free(Vec2{0.0, 0.0}),
                              DimensionAnchor::free(Vec2{30.0, 0.0}), LinearDirection::Aligned,
                              Vec2{15.0, 10.0}, /*styleId=*/777333u, layerId, std::string());

    std::ostringstream out;
    const SaveResult saved = saveDrawingDocument(document, out);
    EXPECT_FALSE(saved) << "a dimension naming a style that is not here saved cleanly";
    EXPECT_EQ(saved.error, SerializationError::UnknownDependencyId);
}

TEST(DrawingDimensionTest, M34_SER_007_SavingIsREFUSEDWhenADimensionIsOnAMissingLayer) {
    // The same check, one field along -- and it has its own test because the
    // pair of them is exactly the "two things that must agree, each tested
    // alone" shape, except here neither was tested at all.
    DrawingDocument document{"Sheet"};
    // Well clear of 999001 above: restoring an id advances the shared
    // generator past it, and the whole suite runs in one process.
    document.restoreDimension(999501u, DimensionKind::Linear,
                              DimensionAnchor::free(Vec2{0.0, 0.0}),
                              DimensionAnchor::free(Vec2{30.0, 0.0}), LinearDirection::Aligned,
                              Vec2{15.0, 10.0}, document.currentDimensionStyleId(),
                              /*layerId=*/777334u, std::string());

    std::ostringstream out;
    const SaveResult saved = saveDrawingDocument(document, out);
    EXPECT_FALSE(saved) << "a dimension on a layer that is not here saved cleanly";
    EXPECT_EQ(saved.error, SerializationError::UnknownDependencyId);
}

TEST(DrawingDimensionTest, M34_DIM_004_ADimensionOnAnEntityFOLLOWSItWhenItMoves) {
    // The whole point. A dimension that stored its coordinates would keep
    // reading 100 after the line became 200 long, and it would look right.
    DrawingDocument document{"Sheet"};
    const ObjectId line = document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}}).id();
    const DrawingDimension& dimension = document.addDimension(
        DimensionKind::Linear, DimensionAnchor::onEntity(line, 0),
        DimensionAnchor::onEntity(line, 1), Vec2{50.0, 20.0});
    EXPECT_NEAR(document.measure(dimension).valueMm, 100.0, 1e-9);

    // The line doubles. The dimension has to say so.
    ASSERT_TRUE(document.transformEntities({line}, Matrix2D::scaleAbout(Vec2{0, 0}, 2.0)));
    EXPECT_NEAR(document.measure(dimension).valueMm, 200.0, 1e-9)
        << "the dimension kept reading the size the line used to be";
}

TEST(DrawingDimensionTest, M34_DIM_005_ADimensionThatLostItsGeometryDANGLESLoudly) {
    // The one failure a drawing must never hide. Showing the last number it
    // read would be a drawing stating a size nothing on it measures.
    DrawingDocument document{"Sheet"};
    const ObjectId line = document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}}).id();
    const ObjectId dimension =
        document
            .addDimension(DimensionKind::Linear, DimensionAnchor::onEntity(line, 0),
                          DimensionAnchor::onEntity(line, 1), Vec2{50.0, 20.0})
            .id();
    ASSERT_TRUE(document.danglingDimensions().empty());

    ASSERT_TRUE(document.removeObject(line));
    const DimensionMeasurement lost = document.measure(*document.findDimension(dimension));
    EXPECT_FALSE(lost.ok);
    EXPECT_FALSE(lost.why.empty());
    EXPECT_EQ(document.dimensionText(*document.findDimension(dimension)), "<?>")
        << "a dangling dimension is still showing a number";
    ASSERT_EQ(document.danglingDimensions().size(), 1u);
    EXPECT_EQ(document.danglingDimensions().front(), dimension);
}

TEST(DrawingDimensionTest, M34_DIM_006_AnOverrideIsShownAndTheMEASUREMENTIsStillTaken) {
    // A drafter has to be able to say "2x" or "TYP". What must not happen is
    // the measurement being replaced -- a drawing full of typed numbers is a
    // drawing that has stopped tracking its model.
    DrawingDocument document{"Sheet"};
    const DrawingDimension& dimension = document.addDimension(
        DimensionKind::Linear, DimensionAnchor::free(Vec2{0.0, 0.0}),
        DimensionAnchor::free(Vec2{30.0, 0.0}), Vec2{15.0, 10.0});
    ASSERT_TRUE(document.setDimensionTextOverride(dimension.id(), "2x 30"));
    EXPECT_EQ(document.dimensionText(dimension), "2x 30");
    EXPECT_NEAR(document.measure(dimension).valueMm, 30.0, 1e-9)
        << "an override replaced the measurement instead of the text";
}

TEST(DrawingDimensionTest, M34_DIM_007_EditingAStyleChangesEveryDimensionUsingIt) {
    // That is what a style is FOR. A drawing where each dimension carried its
    // own text height is a drawing nobody can restyle.
    DrawingDocument document{"Sheet"};
    const DrawingDimension& dimension = document.addDimension(
        DimensionKind::Linear, DimensionAnchor::free(Vec2{0.0, 0.0}),
        DimensionAnchor::free(Vec2{30.0, 0.0}), Vec2{15.0, 10.0});
    EXPECT_EQ(document.dimensionText(dimension), "30.00");

    DimensionStyle wanted{"tmp"};
    wanted.setDecimals(0);
    ASSERT_TRUE(document.editDimensionStyle(document.currentDimensionStyleId(), wanted));
    EXPECT_EQ(document.dimensionText(dimension), "30")
        << "editing the style did not change what the dimension prints";
}

// =============================================================================
// Undo and the file
// =============================================================================

TEST(DrawingDimensionTest, M34_UNDO_001_EveryDimensionEditComesBack) {
    DrawingDocument document{"Sheet"};
    const ObjectId dimension =
        document
            .addDimension(DimensionKind::Linear, DimensionAnchor::free(Vec2{0.0, 0.0}),
                          DimensionAnchor::free(Vec2{30.0, 0.0}), Vec2{15.0, 10.0})
            .id();
    ASSERT_TRUE(document.setDimensionLinePosition(dimension, Vec2{15.0, 25.0}));
    ASSERT_TRUE(document.setDimensionTextOverride(dimension, "TYP"));

    ASSERT_TRUE(document.undo()); // the text
    EXPECT_TRUE(document.findDimension(dimension)->textOverride().empty());
    ASSERT_TRUE(document.undo()); // the move
    EXPECT_NEAR(document.findDimension(dimension)->linePositionMm().y, 10.0, 1e-9);
    ASSERT_TRUE(document.undo()); // the dimension
    EXPECT_EQ(document.findDimension(dimension), nullptr);

    while (document.canRedo()) ASSERT_TRUE(document.redo());
    ASSERT_NE(document.findDimension(dimension), nullptr);
    EXPECT_EQ(document.findDimension(dimension)->textOverride(), "TYP");
}

TEST(DrawingDimensionTest, M34_UNDO_002_AStyleEditIsRestoredWHOLE) {
    // A half-restored style would leave a drawing whose text is one size and
    // whose arrows are another -- and nothing would say so.
    DrawingDocument document{"Sheet"};
    const ObjectId styleId = document.currentDimensionStyleId();
    DimensionStyle wanted{"tmp"};
    wanted.setTextHeightMm(7.0);
    wanted.setArrowSizeMm(9.0);
    wanted.setDecimals(3);
    wanted.setSuffix(" mm");
    ASSERT_TRUE(document.editDimensionStyle(styleId, wanted));
    ASSERT_TRUE(document.undo());

    const DimensionStyle* back = document.findDimensionStyle(styleId);
    ASSERT_NE(back, nullptr);
    EXPECT_NEAR(back->textHeightMm(), 3.5, 1e-12);
    EXPECT_NEAR(back->arrowSizeMm(), 3.5, 1e-12);
    EXPECT_EQ(back->decimals(), 2);
    EXPECT_TRUE(back->suffix().empty()) << "only some of the style came back";
}

TEST(DrawingDimensionTest, M34_SER_001_DimensionsAndStylesSurviveASaveAndAReopen) {
    DrawingDocument document{"Sheet"};
    DimensionStyle wanted{"tmp"};
    wanted.setTextHeightMm(5.0);
    wanted.setDecimals(1);
    wanted.setSuffix(" mm");
    wanted.setOverallScale(2.0);
    ASSERT_TRUE(document.editDimensionStyle(document.currentDimensionStyleId(), wanted));

    const ObjectId line = document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}}).id();
    document.addDimension(DimensionKind::Linear, DimensionAnchor::onEntity(line, 0),
                          DimensionAnchor::onEntity(line, 1), Vec2{50.0, 20.0});
    document.addDimension(DimensionKind::Radius, DimensionAnchor::free(Vec2{0.0, 0.0}),
                          DimensionAnchor::free(Vec2{12.0, 0.0}), Vec2{6.0, 6.0});

    const std::string saved = SaveToString(document);
    const DrawingLoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    const DrawingDocument& back = *loaded.document;

    ASSERT_EQ(back.dimensions().size(), 2u);
    EXPECT_EQ(SaveToString(back), saved);
    EXPECT_EQ(back.undoDepth(), 0u);

    // ...and it still MEASURES, which a restored-but-unhooked dimension would
    // fail: the anchor has to have come back pointing at the same entity.
    for (const DrawingDimension* one : back.dimensions()) {
        if (one->kind() != DimensionKind::Linear) continue;
        const DimensionMeasurement measured = back.measure(*one);
        ASSERT_TRUE(measured.ok) << measured.why;
        EXPECT_NEAR(measured.valueMm, 100.0, 1e-9)
            << "the reopened dimension is not measuring what it did";
        EXPECT_EQ(back.dimensionText(*one), "100.0 mm");
    }
}

TEST(DrawingDimensionTest, M34_SER_002_TheMEASUREMENTIsNotInTheFile) {
    // A file carrying it would hold a second, stale answer about the size of
    // the part -- the exact failure a drawing must not have.
    DrawingDocument document{"Sheet"};
    const ObjectId line = document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{123.456, 0.0}}).id();
    document.addDimension(DimensionKind::Linear, DimensionAnchor::onEntity(line, 0),
                          DimensionAnchor::onEntity(line, 1), Vec2{50.0, 20.0});
    const std::string saved = SaveToString(document);
    EXPECT_EQ(saved.find("123.46"), std::string::npos)
        << "the measured value was written into the file";
}

TEST(DrawingDimensionTest, M34_SER_003_ADimensionNamingAStyleThatIsGoneIsREFUSED) {
    DrawingDocument document{"Sheet"};
    DimensionStyle& extra = document.addDimensionStyle("Detail");
    ASSERT_TRUE(document.setCurrentDimensionStyle(extra.id()));
    document.addDimension(DimensionKind::Linear, DimensionAnchor::free(Vec2{0.0, 0.0}),
                          DimensionAnchor::free(Vec2{30.0, 0.0}), Vec2{15.0, 10.0});

    std::string text = SaveToString(document);
    const std::string real = "\"styleId\": \"" + std::to_string(extra.id()) + "\"";
    const std::size_t at = text.find(real);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, real.size(), "\"styleId\": \"777333\"");

    const DrawingLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::UnknownDependencyId);
}

TEST(DrawingDimensionTest, M34_SER_004_AStyleWithNoTextHeightIsREFUSEDByTheLoader) {
    DrawingDocument document{"Sheet"};
    std::string text = SaveToString(document);
    const std::size_t at = text.find("\"textHeightMm\": 3.5");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"textHeightMm\": 3.5").size(), "\"textHeightMm\": 0.0");

    const DrawingLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded) << "a style that draws text nobody can read was accepted";
}

TEST(DrawingDimensionTest, M34_SER_005_AStyleInUseCannotBeDeleted) {
    // A dimension whose style is gone has no way to be drawn.
    DrawingDocument document{"Sheet"};
    DimensionStyle& extra = document.addDimensionStyle("Detail");
    const ObjectId extraId = extra.id();
    ASSERT_TRUE(document.setCurrentDimensionStyle(extraId));
    const ObjectId dimension =
        document
            .addDimension(DimensionKind::Linear, DimensionAnchor::free(Vec2{0.0, 0.0}),
                          DimensionAnchor::free(Vec2{30.0, 0.0}), Vec2{15.0, 10.0})
            .id();
    ASSERT_TRUE(document.setCurrentDimensionStyle(
        document.findDimensionStyleNamed(kDefaultDimensionStyleName)->id()));

    EXPECT_FALSE(document.removeObject(extraId)) << "a style in use was deleted";
    ASSERT_TRUE(document.removeObject(dimension));
    EXPECT_TRUE(document.removeObject(extraId));
}
