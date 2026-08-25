// M35.1 -- the frame and the title block.
//
// What these protect is one property: THE SHEET IS THE SINGLE ANSWER. A frame
// that outlived a resize, or a title block printing a scale nothing was
// plotted at, is the drawing contradicting itself -- and a drawing that
// contradicts itself is worse than no drawing, because somebody will make the
// part from it.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Drawing/SheetFrame.h"
#include "Core/Drawing/TitleBlock.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace {

using namespace paramcad;

std::string SaveToString(const DrawingDocument& document) {
    std::ostringstream out;
    EXPECT_TRUE(saveDrawingDocument(document, out));
    return out.str();
}

} // namespace

// =============================================================================
// The frame
// =============================================================================

TEST(SheetFrameTest, M35_FRAME_001_TheFrameIsTheSheetMinusItsMargins) {
    Sheet sheet{SheetSize::A3, SheetOrientation::Landscape};
    const SheetFrameGeometry frame = FrameOf(sheet, FrameMargins::standard());
    ASSERT_TRUE(frame.ok) << frame.why;

    // A3 landscape is 420 x 297; 20 mm on the binding edge and 10 elsewhere.
    EXPECT_NEAR(frame.innerMinMm.x, 20.0, 1e-9);
    EXPECT_NEAR(frame.innerMinMm.y, 10.0, 1e-9);
    EXPECT_NEAR(frame.innerMaxMm.x, 410.0, 1e-9);
    EXPECT_NEAR(frame.innerMaxMm.y, 287.0, 1e-9);
    EXPECT_NEAR(frame.innerWidthMm(), 390.0, 1e-9);
    EXPECT_NEAR(frame.innerHeightMm(), 277.0, 1e-9);
}

TEST(SheetFrameTest, M35_FRAME_002_ResizingTheSheetRESIZESTheFrame) {
    // THE POINT OF DERIVING IT. A frame made of entities would still be A4
    // sized after this, and it would look completely plausible.
    Sheet sheet{SheetSize::A4, SheetOrientation::Portrait};
    const double smallWidth = FrameOf(sheet, FrameMargins::standard()).innerWidthMm();

    sheet.setSize(SheetSize::A2);
    const SheetFrameGeometry bigger = FrameOf(sheet, FrameMargins::standard());
    ASSERT_TRUE(bigger.ok);
    EXPECT_GT(bigger.innerWidthMm(), smallWidth)
        << "the sheet grew and the frame did not";
    EXPECT_NEAR(bigger.innerWidthMm(), 420.0 - 20.0 - 10.0, 1e-9);
}

TEST(SheetFrameTest, M35_FRAME_003_TheBindingEdgeStaysOnTheLeftWhenTheSheetIsTurned) {
    // A sheet that moved its binding edge when it was turned would file the
    // wrong way up half the time.
    Sheet portrait{SheetSize::A3, SheetOrientation::Portrait};
    Sheet landscape{SheetSize::A3, SheetOrientation::Landscape};
    EXPECT_NEAR(FrameOf(portrait, FrameMargins::standard()).innerMinMm.x, 20.0, 1e-9);
    EXPECT_NEAR(FrameOf(landscape, FrameMargins::standard()).innerMinMm.x, 20.0, 1e-9);
    EXPECT_NEAR(FrameOf(portrait, FrameMargins::standard()).innerMinMm.y, 10.0, 1e-9);
    EXPECT_NEAR(FrameOf(landscape, FrameMargins::standard()).innerMinMm.y, 10.0, 1e-9);
}

TEST(SheetFrameTest, M35_FRAME_004_MarginsWiderThanThePaperAreREFUSEDAndSayWhy) {
    // NOT silently shrunk to fit. A frame quietly narrowed would print a
    // border measuring something other than what was asked for, and nobody
    // would know which sheets it happened on.
    Sheet tiny;
    ASSERT_TRUE(tiny.setCustomSize(25.0, 25.0));
    FrameMargins wide;
    wide.bindingMm = 20.0;
    wide.otherMm = 10.0;
    const SheetFrameGeometry frame = FrameOf(tiny, wide);
    EXPECT_FALSE(frame.ok);
    EXPECT_FALSE(frame.why.empty());
    EXPECT_TRUE(frame.zones.empty());
}

TEST(SheetFrameTest, M35_ZONE_001_ZonesDivideTheBorderEVENLY) {
    // A half zone in the corner is a reference nobody can use, so the target
    // decides only HOW MANY -- and they then share the edge exactly.
    Sheet sheet{SheetSize::A2, SheetOrientation::Landscape};
    const SheetFrameGeometry frame = FrameOf(sheet, FrameMargins::standard(), 100.0);
    ASSERT_TRUE(frame.ok) << frame.why;

    double columnSpan = 0.0;
    double rowSpan = 0.0;
    std::size_t columns = 0;
    std::size_t rows = 0;
    for (const SheetZone& zone : frame.zones) {
        EXPECT_GT(zone.toMm, zone.fromMm) << "a zone of no width is not a zone";
        if (zone.isRow) {
            ++rows;
            rowSpan += zone.toMm - zone.fromMm;
        } else {
            ++columns;
            columnSpan += zone.toMm - zone.fromMm;
        }
    }
    EXPECT_GE(columns, 2u) << "one zone across the whole edge says nothing";
    EXPECT_GE(rows, 2u);
    EXPECT_NEAR(columnSpan, frame.innerWidthMm(), 1e-9)
        << "the columns do not add up to the edge they divide";
    EXPECT_NEAR(rowSpan, frame.innerHeightMm(), 1e-9);
}

TEST(SheetFrameTest, M35_ZONE_004_EvenATinySheetGetsMoreThanOneDivision) {
    // M35-12 survived: every sheet the other tests use is big enough that the
    // rounding lands on four or more zones either way, so nothing asked what
    // happens when the target is wider than the paper.
    //
    // One zone spanning the whole edge is a reference that tells a reader
    // nothing they did not already know -- "it is somewhere on the drawing".
    Sheet tiny;
    ASSERT_TRUE(tiny.setCustomSize(60.0, 60.0));
    const SheetFrameGeometry frame = FrameOf(tiny, FrameMargins::standard(), 100.0);
    ASSERT_TRUE(frame.ok) << frame.why;

    std::size_t columns = 0;
    std::size_t rows = 0;
    for (const SheetZone& zone : frame.zones) (zone.isRow ? rows : columns)++;
    EXPECT_GE(columns, 2u) << "a 30 mm edge came out as one single zone";
    EXPECT_GE(rows, 2u);
}

TEST(SheetFrameTest, M35_ZONE_002_LettersRunUPTheSideAndNumbersAlongTheBottom) {
    // ISO 5457's direction, which is the OPPOSITE of a spreadsheet's. Upside
    // down, every zone reference already on the drawing points somewhere else.
    Sheet sheet{SheetSize::A2, SheetOrientation::Landscape};
    const SheetFrameGeometry frame = FrameOf(sheet, FrameMargins::standard());
    ASSERT_TRUE(frame.ok);

    // The bottom-left corner of the frame is A1.
    EXPECT_EQ(ZoneAt(frame, Vec2{frame.innerMinMm.x + 1.0, frame.innerMinMm.y + 1.0}), "A1");
    // ...and moving RIGHT changes the number while moving UP changes the letter.
    const std::string right =
        ZoneAt(frame, Vec2{frame.innerMaxMm.x - 1.0, frame.innerMinMm.y + 1.0});
    const std::string up =
        ZoneAt(frame, Vec2{frame.innerMinMm.x + 1.0, frame.innerMaxMm.y - 1.0});
    ASSERT_FALSE(right.empty());
    ASSERT_FALSE(up.empty());
    EXPECT_EQ(right.front(), 'A') << "moving across the sheet changed the ROW letter";
    EXPECT_NE(up.front(), 'A') << "moving up the sheet did not change the row letter";
    EXPECT_EQ(up.substr(1), "1") << "moving up the sheet changed the COLUMN number";
}

TEST(SheetFrameTest, M35_ZONE_003_APointOutsideTheFrameIsInNoZone) {
    // Clamping to the nearest one would hand back a reference to a place the
    // thing is not.
    Sheet sheet{SheetSize::A3, SheetOrientation::Landscape};
    const SheetFrameGeometry frame = FrameOf(sheet, FrameMargins::standard());
    ASSERT_TRUE(frame.ok);
    EXPECT_TRUE(ZoneAt(frame, Vec2{0.0, 0.0}).empty()) << "the paper's corner is inside the frame";
    EXPECT_TRUE(ZoneAt(frame, Vec2{-50.0, 100.0}).empty());
    EXPECT_TRUE(ZoneAt(frame, Vec2{100000.0, 100.0}).empty());
    // ...and the frame's own far corner IS in a zone, rather than falling off
    // the end of the last division.
    EXPECT_FALSE(ZoneAt(frame, frame.innerMaxMm).empty())
        << "the frame's top-right corner is in no zone";
}

// =============================================================================
// The title block
// =============================================================================

TEST(TitleBlockTest, M35_TITLE_001_EveryDrawingStartsWithTheMandatoryFields) {
    // A user who has to build a title block before they can name the thing
    // they are drawing builds it once and copies that file forever.
    TitleBlock block;
    ASSERT_NE(block.findField(kTitleBlockTitleLabel), nullptr);
    ASSERT_NE(block.findField(kTitleBlockNumberLabel), nullptr);
    EXPECT_NE(block.findField("Scale"), nullptr);
    EXPECT_NE(block.findField("Projection"), nullptr);
}

TEST(TitleBlockTest, M35_TITLE_002_TheSCALEFieldCannotBeTypedInto) {
    // THE WHOLE POINT OF THE FILE. The scale printed in the block and the
    // scale the views are drawn at are ONE fact, and the only way to keep them
    // one is for there to be no way to type the second.
    TitleBlock block;
    const TitleBlockField* scale = block.findField("Scale");
    ASSERT_NE(scale, nullptr);
    EXPECT_TRUE(scale->isDerived());
    EXPECT_FALSE(block.setField("Scale", "1:100"))
        << "a scale was typed into the title block";

    Sheet sheet{SheetSize::A3, SheetOrientation::Landscape};
    sheet.setScale(DrawingScale{1, 2});
    EXPECT_EQ(block.valueOf(*block.findField("Scale"), sheet), "1:2");
}

TEST(TitleBlockTest, M35_TITLE_003_TheDerivedFieldsFOLLOWTheSheet) {
    TitleBlock block;
    Sheet sheet{SheetSize::A3, SheetOrientation::Landscape};
    sheet.setScale(DrawingScale{1, 2});
    sheet.setProjectionAngle(ProjectionAngle::First);
    EXPECT_EQ(block.valueOf(*block.findField("Size"), sheet), "A3");
    EXPECT_EQ(block.valueOf(*block.findField("Projection"), sheet), "First angle");

    // The sheet changes. Nothing is told; the block simply reads it again.
    sheet.setSize(SheetSize::A1);
    sheet.setScale(DrawingScale{2, 1});
    sheet.setProjectionAngle(ProjectionAngle::Third);
    EXPECT_EQ(block.valueOf(*block.findField("Size"), sheet), "A1");
    EXPECT_EQ(block.valueOf(*block.findField("Scale"), sheet), "2:1");
    EXPECT_EQ(block.valueOf(*block.findField("Projection"), sheet), "Third angle");
}

TEST(TitleBlockTest, M35_TITLE_004_ACustomSheetPrintsItsSIZEAndNotTheWordCustom) {
    // "Custom" tells a reader holding the paper nothing they did not already
    // know, and loses the one number they might want.
    TitleBlock block;
    Sheet sheet;
    ASSERT_TRUE(sheet.setCustomSize(500.0, 250.0));
    const std::string printed = block.valueOf(*block.findField("Size"), sheet);
    EXPECT_NE(printed, "Custom");
    EXPECT_NE(printed.find("500"), std::string::npos) << printed;
    EXPECT_NE(printed.find("250"), std::string::npos) << printed;
}

TEST(TitleBlockTest, M35_TITLE_005_TheTypedFieldsAreRememberedAndTheDerivedOnesAreNot) {
    TitleBlock block;
    EXPECT_TRUE(block.setField(kTitleBlockTitleLabel, "Bearing Housing"));
    EXPECT_TRUE(block.setField(kTitleBlockNumberLabel, "EP3D-1042-A"));
    EXPECT_EQ(block.findField(kTitleBlockTitleLabel)->value, "Bearing Housing");
    // A derived field carries no value of its own, so there is nothing to save
    // and nothing that can come back stale.
    EXPECT_TRUE(block.findField("Scale")->value.empty());
}

TEST(TitleBlockTest, M35_TITLE_006_TheTitleAndTheNumberCannotBeRemoved) {
    // A drawing that cannot be identified is not a drawing -- it is a picture
    // somebody will have to guess about.
    TitleBlock block;
    EXPECT_FALSE(block.removeField(kTitleBlockTitleLabel));
    EXPECT_FALSE(block.removeField(kTitleBlockNumberLabel));
    EXPECT_TRUE(block.removeField("Material"));
    EXPECT_EQ(block.findField("Material"), nullptr);
}

TEST(TitleBlockTest, M35_TITLE_007_TwoFieldsCannotShareOneLabel) {
    // Two rows saying different things under one name, and a reader with no
    // way to tell which is meant.
    TitleBlock block;
    EXPECT_TRUE(block.addField("Weight", TitleBlockSource::Free));
    EXPECT_FALSE(block.addField("Weight", TitleBlockSource::Free));
    EXPECT_FALSE(block.addField(kTitleBlockTitleLabel, TitleBlockSource::Free));
    EXPECT_FALSE(block.addField("", TitleBlockSource::Free));
}

TEST(TitleBlockTest, M35_TITLE_008_TheHeightFOLLOWSTheRowCount) {
    // Stored, it would be a second answer that disagrees the moment somebody
    // adds a field.
    TitleBlock block;
    const double before = block.heightMm();
    ASSERT_TRUE(block.addField("Weight", TitleBlockSource::Free));
    EXPECT_NEAR(block.heightMm(), before + block.rowHeightMm(), 1e-9);
    ASSERT_TRUE(block.removeField("Weight"));
    EXPECT_NEAR(block.heightMm(), before, 1e-9);
}

TEST(TitleBlockTest, M35_TITLE_009_ABlockNobodyCanReadIsREFUSED) {
    // Found at plot time otherwise, a long way from whoever typed it. The old
    // value stays, which is a block that still works -- the rule
    // DimensionStyle follows for text height.
    TitleBlock block;
    EXPECT_FALSE(block.setWidthMm(0.0));
    EXPECT_NEAR(block.widthMm(), 180.0, 1e-9);
    EXPECT_FALSE(block.setRowHeightMm(-1.0));
    EXPECT_NEAR(block.rowHeightMm(), 8.0, 1e-9);
}

TEST(TitleBlockTest, M35_TITLE_011_TheFIRSTFieldIsTheTOPRow) {
    // M35-21 survived, and the defect it describes SHIPPED once: the painter
    // worked the row position out inline, counted up instead of down, and put
    // Title at the bottom of the block with Sheet above it -- exactly
    // reversing ISO 7200's order. It was caught by looking at a screenshot,
    // which does not scale, so the arithmetic moved here.
    TitleBlock block;
    ASSERT_TRUE(block.setRowHeightMm(10.0));
    const std::size_t rowCount = block.fields().size();
    ASSERT_GE(rowCount, 2u);

    // The block's bottom is at y = 100; its top is 100 + rowCount * 10.
    const double bottom = 100.0;
    EXPECT_NEAR(block.rowBottomMm(0, bottom),
                bottom + static_cast<double>(rowCount - 1) * 10.0, 1e-9)
        << "the first field is not the top row";
    EXPECT_NEAR(block.rowBottomMm(rowCount - 1, bottom), bottom, 1e-9)
        << "the last field is not the bottom row";
    // ...and they descend, one row height at a time, with no gaps or overlaps.
    for (std::size_t i = 1; i < rowCount; ++i)
        EXPECT_NEAR(block.rowBottomMm(i - 1, bottom) - block.rowBottomMm(i, bottom), 10.0, 1e-9)
            << "rows " << i - 1 << " and " << i << " do not sit one above the other";
    // The whole stack fills the block exactly.
    EXPECT_NEAR(block.rowBottomMm(0, bottom) + 10.0, bottom + block.heightMm(), 1e-9);
}

TEST(TitleBlockTest, M35_TITLE_010_EverySourceSurvivesBeingWrittenAndReadBack) {
    // The pair `toString` / `ParseTitleBlockSource` is two things that must
    // agree, and this is the test that makes disagreeing impossible to ship:
    // a new source added to one and not the other fails here.
    for (const TitleBlockSource source :
         {TitleBlockSource::Free, TitleBlockSource::SheetScale, TitleBlockSource::SheetSize,
          TitleBlockSource::ProjectionSymbol, TitleBlockSource::SheetCount}) {
        TitleBlockSource back = TitleBlockSource::Free;
        ASSERT_TRUE(ParseTitleBlockSource(toString(source), back))
            << "a source this build writes, it cannot read: " << toString(source);
        EXPECT_EQ(back, source);
    }
    // ...and something this build does not know is REFUSED, not quietly turned
    // into a typed field holding whatever string was beside it.
    TitleBlockSource unknown = TitleBlockSource::SheetScale;
    EXPECT_FALSE(ParseTitleBlockSource("SheetWeather", unknown));
    EXPECT_EQ(unknown, TitleBlockSource::SheetScale) << "a failed parse still wrote";
}

// =============================================================================
// On a drawing: undo, the file, and the one property that matters
// =============================================================================

TEST(TitleBlockTest, M35_DOC_001_TheBlockSitsInTheFramesBottomRightAndMOVESWithIt) {
    // DERIVED, so a resize moves it. Stored, the block would stay where an A4
    // sheet's corner used to be and hang off the side of an A2 one.
    DrawingDocument document{"Sheet"};
    ASSERT_TRUE(document.setSheetSize(SheetSize::A4));
    const Vec2 small = document.titleBlockOriginMm();
    ASSERT_TRUE(document.setSheetSize(SheetSize::A2));
    const Vec2 big = document.titleBlockOriginMm();
    EXPECT_GT(big.x, small.x) << "the sheet grew and the title block stayed put";

    // Its right edge is the frame's right edge, and its bottom is the frame's
    // bottom -- which is what "in the corner" means.
    const SheetFrameGeometry frame = document.frame();
    ASSERT_TRUE(frame.ok) << frame.why;
    EXPECT_NEAR(big.x + document.titleBlock().widthMm(), frame.innerMaxMm.x, 1e-9);
    EXPECT_NEAR(big.y, frame.innerMinMm.y, 1e-9);
}

TEST(TitleBlockTest, M35_DOC_002_TheBLOCKAndTheVIEWSCannotDisagreeAboutTheScale) {
    // THE FAILURE THIS WHOLE DESIGN EXISTS TO RULE OUT. There is no code path
    // that types a scale into the title block, so there is no state in which
    // the corner says 1:2 and the views are plotted at 1:5.
    DrawingDocument document{"Sheet"};
    ASSERT_TRUE(document.setSheetScale(DrawingScale{1, 2}));
    const TitleBlockField* scale = document.titleBlock().findField("Scale");
    ASSERT_NE(scale, nullptr);
    EXPECT_EQ(document.titleBlock().valueOf(*scale, document.sheet()), "1:2");
    EXPECT_FALSE(document.setTitleBlockField("Scale", "1:5"))
        << "a scale was typed into the title block";

    ASSERT_TRUE(document.setSheetScale(DrawingScale{1, 5}));
    EXPECT_EQ(document.titleBlock().valueOf(*document.titleBlock().findField("Scale"),
                                            document.sheet()),
              "1:5")
        << "the sheet was replotted and the corner still claims the old scale";
}

TEST(TitleBlockTest, M35_DOC_003_EveryTitleBlockEditComesBackWHOLE) {
    DrawingDocument document{"Sheet"};
    ASSERT_TRUE(document.setTitleBlockField(kTitleBlockTitleLabel, "Bearing Housing"));
    ASSERT_TRUE(document.addTitleBlockField("Weight", TitleBlockSource::Free));
    ASSERT_TRUE(document.setTitleBlockField("Weight", "2.4 kg"));
    ASSERT_TRUE(document.removeTitleBlockField("Material"));
    ASSERT_TRUE(document.setTitleBlockSize(200.0, 10.0));

    ASSERT_TRUE(document.undo()); // the size
    EXPECT_NEAR(document.titleBlock().widthMm(), 180.0, 1e-9);
    EXPECT_NEAR(document.titleBlock().rowHeightMm(), 8.0, 1e-9)
        << "only half of the size came back";
    ASSERT_TRUE(document.undo()); // the removal
    EXPECT_NE(document.titleBlock().findField("Material"), nullptr)
        << "undoing a removal did not put the row back";
    ASSERT_TRUE(document.undo()); // the weight
    EXPECT_TRUE(document.titleBlock().findField("Weight")->value.empty());
    ASSERT_TRUE(document.undo()); // adding Weight
    EXPECT_EQ(document.titleBlock().findField("Weight"), nullptr);
    ASSERT_TRUE(document.undo()); // the title
    EXPECT_TRUE(document.titleBlock().findField(kTitleBlockTitleLabel)->value.empty());

    while (document.canRedo()) ASSERT_TRUE(document.redo());
    EXPECT_EQ(document.titleBlock().findField(kTitleBlockTitleLabel)->value, "Bearing Housing");
    EXPECT_EQ(document.titleBlock().findField("Weight")->value, "2.4 kg");
    EXPECT_EQ(document.titleBlock().findField("Material"), nullptr);
    EXPECT_NEAR(document.titleBlock().widthMm(), 200.0, 1e-9);
}

TEST(SheetFrameTest, M35_DOC_004_MarginsWiderThanThePaperAreREFUSEDByTheDocument) {
    // Refused HERE rather than at draw time. Finding out when the frame
    // silently stops drawing is finding out too late.
    DrawingDocument document{"Sheet"};
    ASSERT_TRUE(document.setSheetCustomSize(60.0, 60.0));
    FrameMargins wide;
    wide.bindingMm = 40.0;
    wide.otherMm = 30.0;
    EXPECT_FALSE(document.setFrameMargins(wide));
    EXPECT_NEAR(document.frameMargins().bindingMm, 20.0, 1e-9) << "a refused margin was kept";
    EXPECT_TRUE(document.frame().ok);
}

TEST(SheetFrameTest, M35_DOC_005_TheFrameAndTheBlockSurviveASaveAndAReopen) {
    DrawingDocument document{"Sheet"};
    ASSERT_TRUE(document.setSheetSize(SheetSize::A2));
    ASSERT_TRUE(document.setTitleBlockField(kTitleBlockTitleLabel, "Bearing Housing"));
    ASSERT_TRUE(document.setTitleBlockField(kTitleBlockNumberLabel, "EP3D-1042-A"));
    ASSERT_TRUE(document.addTitleBlockField("Weight", TitleBlockSource::Free));
    ASSERT_TRUE(document.setTitleBlockField("Weight", "2.4 kg"));
    ASSERT_TRUE(document.setTitleBlockSize(200.0, 9.0));
    FrameMargins margins;
    margins.bindingMm = 25.0;
    margins.otherMm = 12.0;
    ASSERT_TRUE(document.setFrameMargins(margins));

    const std::string saved = SaveToString(document);
    std::istringstream in(saved);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const DrawingDocument& back = *loaded.document;

    EXPECT_EQ(SaveToString(back), saved);
    EXPECT_EQ(back.undoDepth(), 0u);
    EXPECT_EQ(back.titleBlock().findField(kTitleBlockTitleLabel)->value, "Bearing Housing");
    EXPECT_EQ(back.titleBlock().findField("Weight")->value, "2.4 kg");
    EXPECT_NEAR(back.titleBlock().widthMm(), 200.0, 1e-9);
    EXPECT_NEAR(back.frameMargins().bindingMm, 25.0, 1e-9);
    EXPECT_NEAR(back.frame().innerMinMm.x, 25.0, 1e-9);
}

TEST(SheetFrameTest, M35_DOC_006_TheDERIVEDROWSAreNotInTheFile) {
    // A file carrying them would come back holding an A3 border and an A3 size
    // printed in the corner on a sheet somebody has since made A2 -- and it
    // would look right.
    DrawingDocument document{"Sheet"};
    ASSERT_TRUE(document.setSheetSize(SheetSize::A3));
    ASSERT_TRUE(document.setSheetScale(DrawingScale{1, 7}));
    const std::string saved = SaveToString(document);
    // THE SHEET SAYS IT ONCE. That copy is the source of truth and belongs in
    // the file; what must not exist is a SECOND one, in the title block, able
    // to come back disagreeing with it.
    std::size_t copies = 0;
    for (std::size_t at = saved.find("1:7"); at != std::string::npos;
         at = saved.find("1:7", at + 1))
        ++copies;
    EXPECT_EQ(copies, 1u)
        << "the scale is in the file " << copies
        << " times; the title block wrote its own copy";
    // The frame's rectangle and its zone labels are derived too, and there are
    // dozens of the latter.
    EXPECT_EQ(saved.find("\"zones\""), std::string::npos);
    EXPECT_EQ(saved.find("innerMin"), std::string::npos);
}

TEST(SheetFrameTest, M35_DOC_007_ADerivedRowCarryingATypedValueIsREFUSEDByTheLoader) {
    DrawingDocument document{"Sheet"};
    std::string text = SaveToString(document);
    const std::string source = "\"source\": \"SheetScale\"";
    const std::size_t at = text.find(source);
    ASSERT_NE(at, std::string::npos) << text;
    text.insert(at + source.size(), ", \"value\": \"1:99\"");

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded) << "a title block carrying its own stale scale was accepted";
}

TEST(SheetFrameTest, M35_DOC_008_ASourceThisBuildDoesNotKnowIsREFUSED) {
    // Not defaulted to Free. A derived row quietly turned into a typed one
    // holding whatever string was beside it is how a title block starts
    // stating a scale nothing was plotted at.
    DrawingDocument document{"Sheet"};
    std::string text = SaveToString(document);
    const std::string was = "\"source\": \"SheetScale\"";
    const std::size_t at = text.find(was);
    ASSERT_NE(at, std::string::npos);
    text.replace(at, was.size(), "\"source\": \"SheetWeather\"");

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::InvalidEnumValue);
}

TEST(SheetFrameTest, M35_DOC_009_ATitleBlockThatCannotIdentifyTheDrawingIsREFUSEDAtSave) {
    // ADR-M3-008: the save checks exactly what the load checks. The raw
    // restore path is what a bad reader or a future migration would leave
    // behind, and it is the state the check exists for.
    DrawingDocument document{"Sheet"};
    TitleBlock stripped;
    stripped.restoreFields({TitleBlockField{"Drawn by", "", TitleBlockSource::Free}});
    document.restoreTitleBlock(std::move(stripped));

    std::ostringstream out;
    const SaveResult saved = saveDrawingDocument(document, out);
    EXPECT_FALSE(saved) << "a drawing that cannot be identified saved cleanly";
    EXPECT_EQ(saved.error, SerializationError::MissingField);
}

TEST(SheetFrameTest, M35_VIEWPOINT_001_ThereIsONEPlaceAViewsScaleIsApplied) {
    // It was written out three times -- the canvas, resolveAnchor, and the DXF
    // writer -- each correct on its own, with nothing making them agree. This
    // pins the one function they all now ask.
    DrawingDocument document{"Sheet"};
    ASSERT_TRUE(document.setSheetScale(DrawingScale{1, 2}));
    // A path, because a view must name a model file -- it is never recomputed
    // here, and does not need to be: where a point LANDS depends on the view's
    // position and scale, not on what it draws.
    DrawingView& view = document.addView("Front", "unbuilt.ep3d", "Block",
                                         ViewDirection::Front, Vec2{100.0, 100.0});

    EXPECT_NEAR(document.viewScaleFactor(view.id()), 0.5, 1e-12);
    // 80 mm of model at 1:2 is 40 mm of paper, from the view's own position.
    const Vec2 onPaper = document.viewPointToSheetMm(view.id(), Vec2{80.0, 20.0});
    EXPECT_NEAR(onPaper.x, 140.0, 1e-9);
    EXPECT_NEAR(onPaper.y, 110.0, 1e-9);
    // The view's own origin is where the view sits.
    const Vec2 origin = document.viewPointToSheetMm(view.id(), Vec2{0.0, 0.0});
    EXPECT_NEAR(origin.x, 100.0, 1e-9);
    EXPECT_NEAR(origin.y, 100.0, 1e-9);

    // NO VIEW MEANS THE POINT IS ALREADY ON THE SHEET, rather than being
    // silently scaled by whatever the last view used.
    const Vec2 plain = document.viewPointToSheetMm(kInvalidObjectId, Vec2{7.0, 9.0});
    EXPECT_NEAR(plain.x, 7.0, 1e-9);
    EXPECT_NEAR(plain.y, 9.0, 1e-9);
    EXPECT_NEAR(document.viewScaleFactor(kInvalidObjectId), 1.0, 1e-12);
}
