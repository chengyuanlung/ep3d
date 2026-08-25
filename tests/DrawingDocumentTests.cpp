// M32.1 -- the third document type.
//
// These are Core tests: no kernel, no OCCT, nothing projected. What they check
// is that a DRAWING is a document -- ids, names, undo, tables, a file that
// round-trips -- which is the half of M32 that has nothing to do with
// geometry. The projection half arrives in M32.2 and is checked where a solid
// can actually be built.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using namespace paramcad;

std::string SaveToString(const DrawingDocument& document) {
    std::ostringstream out;
    EXPECT_TRUE(saveDrawingDocument(document, out));
    return out.str();
}

SaveResult TrySave(const DrawingDocument& document) {
    std::ostringstream out;
    return saveDrawingDocument(document, out);
}

DrawingLoadResult LoadFromString(const std::string& text) {
    std::istringstream in(text);
    return loadDrawingDocument(in);
}

} // namespace

// =============================================================================
// The paper
// =============================================================================

TEST(DrawingDocumentTest, M32_SHEET_001_ASheetHasARealSizeInMillimetres) {
    // A drawing is the first document whose contents have a PHYSICAL size, and
    // an A3 that is not 420 x 297 is a drawing that prints wrong.
    DrawingDocument document{"Plate"};
    EXPECT_EQ(document.sheet().size(), SheetSize::A3);
    EXPECT_EQ(document.sheet().orientation(), SheetOrientation::Landscape);
    EXPECT_NEAR(document.sheet().widthMm(), 420.0, 1e-9);
    EXPECT_NEAR(document.sheet().heightMm(), 297.0, 1e-9);

    ASSERT_TRUE(document.setSheetOrientation(SheetOrientation::Portrait));
    EXPECT_NEAR(document.sheet().widthMm(), 297.0, 1e-9)
        << "turning the paper did not swap its sides";
    EXPECT_NEAR(document.sheet().heightMm(), 420.0, 1e-9);
}

TEST(DrawingDocumentTest, M32_SHEET_002_AScaleIsARatioNotANumber) {
    // "1:3" is what a title block prints and what a reader measures against,
    // and 0.333333 is not that. Kept as two integers so the sentence survives
    // the file, the arithmetic and the print.
    DrawingScale scale;
    ASSERT_TRUE(ParseDrawingScale("1:2", scale));
    EXPECT_EQ(scale.numerator, 1);
    EXPECT_EQ(scale.denominator, 2);
    EXPECT_EQ(scale.toString(), "1:2");
    EXPECT_NEAR(scale.factor(), 0.5, 1e-12);

    // ...and 1:3 and 2:6 stay TELLABLE APART, because they print differently.
    DrawingScale third;
    DrawingScale sixth;
    ASSERT_TRUE(ParseDrawingScale("1:3", third));
    ASSERT_TRUE(ParseDrawingScale("2:6", sixth));
    EXPECT_NE(third, sixth) << "two scales that print differently compared equal";
    EXPECT_NEAR(third.factor(), sixth.factor(), 1e-12);

    // A bare number is 1:1's shorthand and is accepted; nonsense is refused
    // rather than guessed at.
    DrawingScale whole;
    EXPECT_TRUE(ParseDrawingScale("2", whole));
    EXPECT_EQ(whole.denominator, 1);
    DrawingScale bad;
    EXPECT_FALSE(ParseDrawingScale("1:0", bad)) << "a zero denominator is not a scale";
    EXPECT_FALSE(ParseDrawingScale("half", bad));
    EXPECT_FALSE(ParseDrawingScale("1:-2", bad));
}

TEST(DrawingDocumentTest, M32_SHEET_003_ASheetWithNoAreaIsREFUSED) {
    DrawingDocument document{"Plate"};
    EXPECT_FALSE(document.setSheetCustomSize(0.0, 100.0));
    EXPECT_FALSE(document.setSheetCustomSize(100.0, -1.0));
    EXPECT_EQ(document.sheet().size(), SheetSize::A3) << "a refused size was applied anyway";
    EXPECT_TRUE(document.setSheetCustomSize(500.0, 250.0));
    EXPECT_EQ(document.sheet().size(), SheetSize::Custom);
}

// =============================================================================
// The tables
// =============================================================================

TEST(DrawingDocumentTest, M32_TABLE_001_EveryDrawingStartsWithLayerZeroAndCONTINUOUS) {
    // AutoCAD's rule, kept because a DXF without them is a DXF other programs
    // refuse -- and a drawing's whole purpose is to leave the program.
    DrawingDocument document{"Plate"};
    ASSERT_NE(document.findLayerNamed(kDefaultLayerName), nullptr);
    ASSERT_NE(document.findLinetypeNamed(kContinuousLinetypeName), nullptr);
    EXPECT_EQ(document.currentLayerId(), document.findLayerNamed(kDefaultLayerName)->id());

    // ...AND THEY ARRIVED AS CONSTRUCTION, not as an edit. "Undo" on a fresh
    // drawing must not delete the layer everything is about to be drawn on.
    EXPECT_EQ(document.undoDepth(), 0u);
    EXPECT_FALSE(document.removeObject(document.findLayerNamed(kDefaultLayerName)->id()))
        << "layer 0 was deleted";
    EXPECT_FALSE(document.removeObject(document.findLinetypeNamed(kContinuousLinetypeName)->id()))
        << "CONTINUOUS was deleted while a layer was using it";
}

TEST(DrawingDocumentTest, M32_TABLE_002_ALayerNamingALinetypeThatIsNotThereIsREFUSED) {
    // A layer that names a linetype the table has not got is a file other
    // programs refuse to open. Caught where the name is still in the caller's
    // hand rather than at save time.
    DrawingDocument document{"Plate"};
    EXPECT_THROW(document.addLayer("Hidden", 1, "HIDDEN"), std::invalid_argument);
    document.addLinetype("HIDDEN", "Hidden line", std::vector<double>{5.0, -2.5});
    EXPECT_NO_THROW(document.addLayer("Hidden", 1, "HIDDEN"));
}

TEST(DrawingDocumentTest, M32_TABLE_003_ThreeFlagsBecauseTheyDoThreeDifferentThings) {
    // Off, frozen and locked are not one visibility. A drawing that merged
    // them would come back from a round trip with layers behaving differently
    // than they were left.
    DrawingDocument document{"Plate"};
    Layer& hidden = document.addLayer("Hidden");
    ASSERT_TRUE(document.setLayerOn(hidden.id(), false));
    EXPECT_FALSE(document.findLayer(hidden.id())->isVisible());
    ASSERT_TRUE(document.setLayerOn(hidden.id(), true));
    ASSERT_TRUE(document.setLayerFrozen(hidden.id(), true));
    EXPECT_FALSE(document.findLayer(hidden.id())->isVisible())
        << "a frozen layer is still being drawn";
    ASSERT_TRUE(document.setLayerFrozen(hidden.id(), false));
    ASSERT_TRUE(document.setLayerLocked(hidden.id(), true));
    EXPECT_TRUE(document.findLayer(hidden.id())->isVisible())
        << "a locked layer is visible -- it is unselectable, not hidden";
}

TEST(DrawingDocumentTest, M32_TABLE_004_TheCurrentLayerCannotBeOneNothingCanBeDrawnOn) {
    // New geometry would land somewhere invisible or unselectable, and the
    // user would draw a line that appears not to have been drawn.
    DrawingDocument document{"Plate"};
    Layer& hidden = document.addLayer("Hidden");
    ASSERT_TRUE(document.setLayerFrozen(hidden.id(), true));
    EXPECT_FALSE(document.setCurrentLayer(hidden.id())) << "a frozen layer became current";
    ASSERT_TRUE(document.setLayerFrozen(hidden.id(), false));
    ASSERT_TRUE(document.setLayerLocked(hidden.id(), true));
    EXPECT_FALSE(document.setCurrentLayer(hidden.id())) << "a locked layer became current";
    ASSERT_TRUE(document.setLayerLocked(hidden.id(), false));
    EXPECT_TRUE(document.setCurrentLayer(hidden.id()));

    // ...and the CURRENT layer cannot be deleted out from under new geometry.
    EXPECT_FALSE(document.removeObject(hidden.id()));
}

// =============================================================================
// Views
// =============================================================================

TEST(DrawingDocumentTest, M32_VIEW_001_AViewStoresASENTENCENotGeometry) {
    // The same decision Instance made (ADR-M22-003): the model file is the
    // source of truth, so a model that was edited shows up here on the next
    // rebuild rather than as a stale picture nobody can trace back.
    DrawingDocument document{"Plate"};
    DrawingView& front =
        document.addView("Front", "parts/bracket.ep3d", "Body", ViewDirection::Front,
                         Vec2{100.0, 150.0});
    EXPECT_EQ(front.sourcePath(), "parts/bracket.ep3d");
    EXPECT_EQ(front.bodyName(), "Body");
    EXPECT_EQ(front.direction(), ViewDirection::Front);
}

TEST(DrawingDocumentTest, M32_VIEW_002_AViewWithNoFileOrOffThePaperIsREFUSED) {
    DrawingDocument document{"Plate"};
    EXPECT_THROW(document.addView("Front", "", "", ViewDirection::Front, Vec2{10.0, 10.0}),
                 std::invalid_argument);
    // The sheet is a finite piece of paper, and a view outside it is a view
    // nobody prints. Said here rather than discovered at plot time.
    EXPECT_THROW(document.addView("Front", "parts/a.ep3d", "", ViewDirection::Front,
                                  Vec2{9999.0, 10.0}),
                 std::invalid_argument);
    EXPECT_THROW(document.addView("Front", "parts/a.ep3d", "", ViewDirection::Front,
                                  Vec2{10.0, -1.0}),
                 std::invalid_argument);
    EXPECT_EQ(document.views().size(), 0u) << "a refused view was created anyway";
}

TEST(DrawingDocumentTest, M32_VIEW_003_AViewMayOverrideTheSheetScaleAndGiveItBack) {
    // A drawing that shows one detail at 2:1 beside a general view at 1:5 is
    // ordinary. What matters is that "same as the sheet" SURVIVES the sheet
    // later being changed -- storing the resolved number would silently pin
    // the view the day somebody rescales the paper.
    DrawingDocument document{"Plate"};
    DrawingView& front = document.addView("Front", "parts/a.ep3d", "", ViewDirection::Front,
                                          Vec2{100.0, 150.0});
    EXPECT_FALSE(front.hasOwnScale());
    EXPECT_EQ(front.effectiveScale(DrawingScale{1, 2}), (DrawingScale{1, 2}));

    ASSERT_TRUE(document.setViewScale(front.id(), DrawingScale{2, 1}));
    EXPECT_TRUE(front.hasOwnScale());
    EXPECT_EQ(front.effectiveScale(DrawingScale{1, 2}), (DrawingScale{2, 1}));

    ASSERT_TRUE(document.clearViewScale(front.id()));
    ASSERT_TRUE(document.setSheetScale(DrawingScale{1, 5}));
    EXPECT_EQ(front.effectiveScale(document.sheet().scale()), (DrawingScale{1, 5}))
        << "a view with no opinion did not follow the sheet";
}

TEST(DrawingDocumentTest, M32_VIEW_004_TheSixDirectionsHaveAnUpThatAgreesWithThem) {
    // A caller that handed in a direction alone would leave "up" to be
    // guessed, and a guessed up is a view that is right but rotated. The one
    // place this table is not mechanical is TOP and BOTTOM -- you are looking
    // down +Z, so +Z cannot also be up.
    for (const ViewDirection direction :
         {ViewDirection::Front, ViewDirection::Back, ViewDirection::Left, ViewDirection::Right,
          ViewDirection::Top, ViewDirection::Bottom, ViewDirection::Isometric}) {
        const ViewCamera camera = CameraFor(direction);
        const double dot = camera.towards.x * camera.up.x + camera.towards.y * camera.up.y +
                           camera.towards.z * camera.up.z;
        EXPECT_NEAR(dot, 0.0, 1e-9)
            << toString(direction) << " looks along its own up vector, which projects to nothing";
        const double upLength = camera.up.x * camera.up.x + camera.up.y * camera.up.y +
                                camera.up.z * camera.up.z;
        EXPECT_GT(upLength, 0.0) << toString(direction) << " has no up at all";
    }
}

// =============================================================================
// Undo
// =============================================================================

TEST(DrawingDocumentTest, M32_UNDO_001_EverySheetChangeIsOneUndoStepAndComesBack) {
    DrawingDocument document{"Plate"};
    ASSERT_TRUE(document.setSheetSize(SheetSize::A1));
    ASSERT_TRUE(document.setSheetScale(DrawingScale{1, 5}));
    EXPECT_EQ(document.undoDepth(), 2u);

    ASSERT_TRUE(document.undo());
    EXPECT_EQ(document.sheet().scale(), (DrawingScale{1, 1}));
    EXPECT_EQ(document.sheet().size(), SheetSize::A1) << "undoing the scale also undid the size";
    ASSERT_TRUE(document.undo());
    EXPECT_EQ(document.sheet().size(), SheetSize::A3);
    ASSERT_TRUE(document.redo());
    EXPECT_EQ(document.sheet().size(), SheetSize::A1);
}

TEST(DrawingDocumentTest, M32_UNDO_002_ACustomSheetComesBackTheSizeItWas) {
    // The portrait table has no entry for Custom, so setSize alone cannot put
    // one back -- the delta has to carry the millimetres. Pinned because the
    // failure is silent: the sheet becomes 0 x 0 and every view is then off
    // the paper.
    DrawingDocument document{"Plate"};
    ASSERT_TRUE(document.setSheetCustomSize(500.0, 250.0));
    ASSERT_TRUE(document.setSheetSize(SheetSize::A4));
    ASSERT_TRUE(document.undo());
    EXPECT_EQ(document.sheet().size(), SheetSize::Custom);
    EXPECT_NEAR(document.sheet().widthMm(), 500.0, 1e-9) << "a custom sheet lost its width";
    EXPECT_NEAR(document.sheet().heightMm(), 250.0, 1e-9);
}

TEST(DrawingDocumentTest, M32_UNDO_003_LayerViewAndCurrentLayerEditsAllComeBack) {
    DrawingDocument document{"Plate"};
    const ObjectId zero = document.findLayerNamed(kDefaultLayerName)->id();
    Layer& hidden = document.addLayer("Hidden", 1);
    const ObjectId hiddenId = hidden.id();
    ASSERT_TRUE(document.setLayerColor(hiddenId, 5));
    ASSERT_TRUE(document.setCurrentLayer(hiddenId));
    DrawingView& front = document.addView("Front", "parts/a.ep3d", "", ViewDirection::Front,
                                          Vec2{100.0, 150.0});
    const ObjectId viewId = front.id();
    ASSERT_TRUE(document.setViewPosition(viewId, Vec2{200.0, 100.0}));

    ASSERT_TRUE(document.undo()); // the move
    EXPECT_NEAR(document.findView(viewId)->positionMm().x, 100.0, 1e-9);
    ASSERT_TRUE(document.undo()); // the view
    EXPECT_EQ(document.findView(viewId), nullptr);
    ASSERT_TRUE(document.undo()); // the current layer
    EXPECT_EQ(document.currentLayerId(), zero);
    ASSERT_TRUE(document.undo()); // the colour
    EXPECT_EQ(document.findLayer(hiddenId)->color(), 1);
    ASSERT_TRUE(document.undo()); // the layer
    EXPECT_EQ(document.findLayer(hiddenId), nullptr);

    // ...and all the way forward again.
    while (document.canRedo()) ASSERT_TRUE(document.redo());
    EXPECT_NE(document.findLayerNamed("Hidden"), nullptr);
    EXPECT_EQ(document.findLayerNamed("Hidden")->color(), 5);
    EXPECT_NE(document.findViewNamed("Front"), nullptr);
    EXPECT_NEAR(document.findViewNamed("Front")->positionMm().x, 200.0, 1e-9);
}

// =============================================================================
// The file
// =============================================================================

TEST(DrawingDocumentTest, M32_SER_001_ADrawingSurvivesASaveAndAReopen) {
    DrawingDocument document{"Plate"};
    ASSERT_TRUE(document.setSheetSize(SheetSize::A2));
    ASSERT_TRUE(document.setSheetOrientation(SheetOrientation::Portrait));
    ASSERT_TRUE(document.setSheetScale(DrawingScale{1, 2}));
    document.addLinetype("HIDDEN", "Hidden line", std::vector<double>{5.0, -2.5});
    Layer& hidden = document.addLayer("Hidden", 1, "HIDDEN");
    ASSERT_TRUE(document.setLayerLineweight(hidden.id(), 25));
    ASSERT_TRUE(document.setLayerLocked(hidden.id(), true));
    ASSERT_TRUE(document.setCurrentLayer(document.findLayerNamed(kDefaultLayerName)->id()));
    DrawingView& front = document.addView("Front", "parts/bracket.ep3d", "Body",
                                          ViewDirection::Front, Vec2{100.0, 150.0});
    ASSERT_TRUE(document.setViewScale(front.id(), DrawingScale{2, 1}));

    const std::string text = SaveToString(document);
    const DrawingLoadResult loaded = LoadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    const DrawingDocument& back = *loaded.document;

    EXPECT_EQ(back.sheet().size(), SheetSize::A2);
    EXPECT_EQ(back.sheet().orientation(), SheetOrientation::Portrait);
    EXPECT_EQ(back.sheet().scale(), (DrawingScale{1, 2}))
        << "the scale came back as a rounded number rather than the ratio typed";

    const Layer* hiddenBack = back.findLayerNamed("Hidden");
    ASSERT_NE(hiddenBack, nullptr);
    EXPECT_EQ(hiddenBack->color(), 1);
    EXPECT_EQ(hiddenBack->linetype(), "HIDDEN");
    EXPECT_EQ(hiddenBack->lineweight(), 25);
    EXPECT_TRUE(hiddenBack->isLocked());

    const Linetype* dashed = back.findLinetypeNamed("HIDDEN");
    ASSERT_NE(dashed, nullptr);
    ASSERT_EQ(dashed->pattern().size(), 2u);
    EXPECT_NEAR(dashed->pattern()[1], -2.5, 1e-12) << "a gap came back as a dash";

    const DrawingView* frontBack = back.findViewNamed("Front");
    ASSERT_NE(frontBack, nullptr);
    EXPECT_EQ(frontBack->sourcePath(), "parts/bracket.ep3d");
    EXPECT_EQ(frontBack->direction(), ViewDirection::Front);
    EXPECT_TRUE(frontBack->hasOwnScale());
    EXPECT_EQ(frontBack->scale(), (DrawingScale{2, 1}));
    EXPECT_NEAR(frontBack->positionMm().y, 150.0, 1e-9);

    // The two properties every kind in this format has.
    EXPECT_EQ(back.undoDepth(), 0u);
    EXPECT_EQ(SaveToString(back), text);
}

TEST(DrawingDocumentTest, M32_SER_002_ACustomSheetComesBackTheSizeItWasSaved) {
    // Written as PORTRAIT millimetres, because writing the oriented pair would
    // make a landscape custom sheet come back rotated -- the reader orients
    // them again.
    DrawingDocument document{"Plate"};
    ASSERT_TRUE(document.setSheetCustomSize(500.0, 250.0));
    ASSERT_TRUE(document.setSheetOrientation(SheetOrientation::Landscape));
    const double width = document.sheet().widthMm();
    const double height = document.sheet().heightMm();

    const DrawingLoadResult loaded = LoadFromString(SaveToString(document));
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_NEAR(loaded.document->sheet().widthMm(), width, 1e-9)
        << "a custom sheet came back rotated";
    EXPECT_NEAR(loaded.document->sheet().heightMm(), height, 1e-9);
}

TEST(DrawingDocumentTest, M32_SER_003_ALayerNamingAMissingLinetypeIsREFUSEDAtBothDoors) {
    // ADR-M3-008: the named worst case is a document that saves cleanly and
    // then refuses to load, because the good file on disk is already gone by
    // the time anybody finds out.
    DrawingDocument document{"Plate"};
    document.addLinetype("HIDDEN", "Hidden line", std::vector<double>{5.0, -2.5});
    document.addLayer("Hidden", 1, "HIDDEN");

    std::string text = SaveToString(document);
    const std::size_t at = text.find("\"HIDDEN\"");
    ASSERT_NE(at, std::string::npos) << text;
    // Rename the LINETYPE only, leaving the layer pointing at a name that is
    // no longer in the table.
    text.replace(at, std::string("\"HIDDEN\"").size(), "\"PHANTOM\"");

    const DrawingLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::UnknownDependencyId);
    EXPECT_NE(loaded.message.find("HIDDEN"), std::string::npos) << loaded.message;
}

TEST(DrawingDocumentTest, M32_SER_004_ADrawingWithNoLayerZeroIsREFUSEDBeforeItIsWritten) {
    // Not reachable through the facade -- layer 0 cannot be deleted -- so this
    // goes through the restore path, which is exactly how a hand-edited or
    // future file would arrive.
    DrawingDocument document{"Plate"};
    document.restoreRemoveObject(document.findLayerNamed(kDefaultLayerName)->id());
    const SaveResult refused = TrySave(document);
    EXPECT_FALSE(refused);
    EXPECT_NE(refused.message.find("layer '0'"), std::string::npos) << refused.message;
}

TEST(DrawingDocumentTest, M32_SER_005_AViewOffThePaperIsREFUSEDByTheLoader) {
    DrawingDocument document{"Plate"};
    document.addView("Front", "parts/a.ep3d", "", ViewDirection::Front, Vec2{100.0, 150.0});
    std::string text = SaveToString(document);
    const std::size_t at = text.find("\"xMm\": 100");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"xMm\": 100").size(), "\"xMm\": 9999");

    const DrawingLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded) << "a view off the edge of the paper was accepted";
}

// =============================================================================
// It is a document like the other two
// =============================================================================

TEST(DrawingDocumentTest, M32_DOC_001_EveryKindADrawingNamesIsAnswerableThreeWays) {
    // The same property NamedObjectTests pins for a part and an assembly. A
    // third document type is exactly when three hand-kept walks would have
    // become three more.
    DrawingDocument document{"Plate"};
    document.addLinetype("HIDDEN", "Hidden line", std::vector<double>{5.0, -2.5});
    Layer& hidden = document.addLayer("Hidden", 1, "HIDDEN");
    DrawingView& front = document.addView("Front", "parts/a.ep3d", "", ViewDirection::Front,
                                          Vec2{100.0, 150.0});

    for (const ObjectId id : {hidden.id(), front.id(),
                              document.findLinetypeNamed("HIDDEN")->id()}) {
        const std::string name = document.objectName(id);
        EXPECT_FALSE(name.empty()) << "the drawing cannot say what " << id << " is called";
        EXPECT_NE(document.unusedNameLike(name), name)
            << name << " is not seen as taken, so another object could take it";
        const auto renamed = document.renameObject(id, name + " renamed");
        EXPECT_TRUE(renamed.ok) << renamed.message;
        EXPECT_EQ(document.objectName(id), name + " renamed");
    }
}

TEST(DrawingDocumentTest, M32_DOC_002_ADrawingKnowsWhatKindOfDocumentItIs) {
    DrawingDocument document{"Plate"};
    EXPECT_EQ(document.type(), DocumentType::Drawing);
    // ...and the file says so too, which is what File > Open dispatches on.
    EXPECT_NE(SaveToString(document).find("\"documentType\": \"Drawing\""), std::string::npos);
}
