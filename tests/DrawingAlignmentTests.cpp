// M32.3 -- projected views that line up, and views that know they are behind
// their models.
//
// An orthographic drawing is not a set of independent pictures: the top view
// sits directly above or below the front and SHARES its horizontal position.
// That sharing is what lets a reader carry a measurement from one view to
// another with a ruler, and it is the thing these tests protect.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <gtest/gtest.h>

#include <cmath>
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

DrawingLoadResult LoadFromString(const std::string& text) {
    std::istringstream in(text);
    return loadDrawingDocument(in);
}

// A sheet with a front view on it, ready to project from.
struct Sheet3 {
    DrawingDocument document{"Plate"};
    ObjectId front = kInvalidObjectId;
    Sheet3() {
        front = document
                    .addView("Front", "parts/block.ep3d", "Block", ViewDirection::Front,
                             Vec2{150.0, 150.0})
                    .id();
    }
};

} // namespace

// =============================================================================
// Which side a projected view falls on
// =============================================================================

TEST(DrawingAlignmentTest, M32_ALIGN_001_TheRuleIsDerivedFromTheCameraTable) {
    // "The top view goes above the front" is not an independent fact -- it
    // follows from the top view looking ALONG the front view's up vector. This
    // asks the derivation, so a second hand-written table cannot appear
    // without contradicting it.
    EXPECT_EQ(AlignmentOf(ViewDirection::Front, ViewDirection::Top).alignment,
              ViewAlignment::Vertical);
    EXPECT_EQ(AlignmentOf(ViewDirection::Front, ViewDirection::Bottom).alignment,
              ViewAlignment::Vertical);
    EXPECT_EQ(AlignmentOf(ViewDirection::Front, ViewDirection::Left).alignment,
              ViewAlignment::Horizontal);
    EXPECT_EQ(AlignmentOf(ViewDirection::Front, ViewDirection::Right).alignment,
              ViewAlignment::Horizontal);

    // IN THIRD ANGLE the view from above goes ABOVE and the view from the
    // right goes RIGHT. Opposite signs for the opposite views, or one of the
    // four would sit on top of another.
    EXPECT_EQ(AlignmentOf(ViewDirection::Front, ViewDirection::Top).sign, 1);
    EXPECT_EQ(AlignmentOf(ViewDirection::Front, ViewDirection::Bottom).sign, -1);
    EXPECT_EQ(AlignmentOf(ViewDirection::Front, ViewDirection::Right).sign, 1);
    EXPECT_EQ(AlignmentOf(ViewDirection::Front, ViewDirection::Left).sign, -1);
}

TEST(DrawingAlignmentTest, M32_ALIGN_002_AnIsometricIsNotSquareToAnythingAndSaysSo) {
    // Inventing a side for it to sit on would put it somewhere the user cannot
    // predict, and the drawing would look almost right.
    EXPECT_EQ(AlignmentOf(ViewDirection::Front, ViewDirection::Isometric).alignment,
              ViewAlignment::None);
    EXPECT_EQ(AlignmentOf(ViewDirection::Front, ViewDirection::Isometric).sign, 0);

    Sheet3 rig;
    EXPECT_THROW(rig.document.addProjectedView("Iso", rig.front, ViewDirection::Isometric, 80.0),
                 std::invalid_argument);
    const std::string why =
        rig.document.whyViewCannotBeProjectedFrom(rig.front, ViewDirection::Isometric);
    EXPECT_NE(why.find("square"), std::string::npos) << why;
}

TEST(DrawingAlignmentTest, M32_ALIGN_003_ACHILDSHARESONEOfItsParentsCoordinates) {
    // THE WHOLE POINT of an orthographic layout: a top view sits directly
    // above the front and shares its horizontal position, so a width measured
    // in one is the width in the other.
    Sheet3 rig;
    rig.document.setSheetProjectionAngle(ProjectionAngle::Third);
    const ObjectId top =
        rig.document.addProjectedView("Top", rig.front, ViewDirection::Top, 80.0).id();
    const ObjectId right =
        rig.document.addProjectedView("Right", rig.front, ViewDirection::Right, 100.0).id();

    const Vec2 frontAt = rig.document.viewPositionMm(rig.front);
    const Vec2 topAt = rig.document.viewPositionMm(top);
    const Vec2 rightAt = rig.document.viewPositionMm(right);

    EXPECT_NEAR(topAt.x, frontAt.x, 1e-9) << "the top view is not above the front one";
    EXPECT_NEAR(topAt.y, frontAt.y + 80.0, 1e-9) << "in third angle the top view goes ABOVE";
    EXPECT_NEAR(rightAt.y, frontAt.y, 1e-9) << "the right view is not level with the front one";
    EXPECT_NEAR(rightAt.x, frontAt.x + 100.0, 1e-9)
        << "in third angle the right view goes to the RIGHT";
}

TEST(DrawingAlignmentTest, M32_ALIGN_004_FIRSTAnglePutsThemOnTheOtherSide) {
    // The convention a reader has to be told, and the reason the angle lives
    // on the SHEET: a drawing is in one or the other and never both.
    Sheet3 rig;
    rig.document.setSheetProjectionAngle(ProjectionAngle::First);
    const ObjectId top =
        rig.document.addProjectedView("Top", rig.front, ViewDirection::Top, 80.0).id();
    const ObjectId right =
        rig.document.addProjectedView("Right", rig.front, ViewDirection::Right, 100.0).id();

    const Vec2 frontAt = rig.document.viewPositionMm(rig.front);
    EXPECT_NEAR(rig.document.viewPositionMm(top).y, frontAt.y - 80.0, 1e-9)
        << "in first angle the view from ABOVE is drawn BELOW";
    EXPECT_NEAR(rig.document.viewPositionMm(right).x, frontAt.x - 100.0, 1e-9)
        << "in first angle the view from the RIGHT is drawn on the LEFT";
}

TEST(DrawingAlignmentTest, M32_ALIGN_005_MovingTheParentMovesTheChildren) {
    // Composed, never stored (ADR-M10-002). Nothing is told; the child's
    // position is derived when it is asked for.
    Sheet3 rig;
    const ObjectId top =
        rig.document.addProjectedView("Top", rig.front, ViewDirection::Top, 80.0).id();
    const Vec2 before = rig.document.viewPositionMm(top);

    ASSERT_TRUE(rig.document.setViewPosition(rig.front, Vec2{200.0, 120.0}));
    const Vec2 after = rig.document.viewPositionMm(top);
    EXPECT_NEAR(after.x - before.x, 50.0, 1e-9) << "the child did not follow its parent";
    EXPECT_NEAR(after.y - before.y, -30.0, 1e-9);
}

TEST(DrawingAlignmentTest, M32_ALIGN_006_AChildCannotBeDraggedOffItsAxis) {
    // A child that could be put anywhere would silently break the
    // ruler-across-views property a reader relies on. It slides along one axis
    // and shares the other, and `setViewAlignmentOffsetMm` is the only way.
    Sheet3 rig;
    const ObjectId top =
        rig.document.addProjectedView("Top", rig.front, ViewDirection::Top, 80.0).id();
    EXPECT_FALSE(rig.document.setViewPosition(top, Vec2{10.0, 10.0}))
        << "a projected view was dragged off its alignment";

    ASSERT_TRUE(rig.document.setViewAlignmentOffsetMm(top, 120.0));
    const Vec2 frontAt = rig.document.viewPositionMm(rig.front);
    EXPECT_NEAR(rig.document.viewPositionMm(top).x, frontAt.x, 1e-9)
        << "sliding a child moved it sideways too";
    // ...and a BASE view has no offset to set.
    EXPECT_FALSE(rig.document.setViewAlignmentOffsetMm(rig.front, 50.0));
}

TEST(DrawingAlignmentTest, M32_ALIGN_007_AChildLooksAtTheSameModelAsItsParent) {
    // A "projected view" of a different file is not a projected view -- it is
    // a second base view that happens to sit beside this one, and calling it a
    // child would make the alignment a lie.
    Sheet3 rig;
    const DrawingView& top =
        rig.document.addProjectedView("Top", rig.front, ViewDirection::Top, 80.0);
    EXPECT_EQ(top.sourcePath(), rig.document.findView(rig.front)->sourcePath());
    EXPECT_EQ(top.bodyName(), rig.document.findView(rig.front)->bodyName());
}

TEST(DrawingAlignmentTest, M32_ALIGN_008_DeletingTheParentTakesItsChildrenAndONEUndoBringsThemBack) {
    // A child whose parent is gone has nowhere to be. One deletion, one undo
    // step -- the same cascade a mate's relations get.
    Sheet3 rig;
    rig.document.addProjectedView("Top", rig.front, ViewDirection::Top, 80.0);
    rig.document.addProjectedView("Right", rig.front, ViewDirection::Right, 100.0);
    ASSERT_EQ(rig.document.views().size(), 3u);

    const std::size_t before = rig.document.undoDepth();
    ASSERT_TRUE(rig.document.removeObject(rig.front));
    EXPECT_EQ(rig.document.views().size(), 0u) << "a projected view outlived its parent";
    EXPECT_EQ(rig.document.undoDepth(), before + 1) << "one deletion, one undo step";

    ASSERT_TRUE(rig.document.undo());
    EXPECT_EQ(rig.document.views().size(), 3u);
    EXPECT_NE(rig.document.findViewNamed("Top"), nullptr);
    // ...and the alignment still works, which a restored-but-unwired child
    // would fail.
    const Vec2 frontAt = rig.document.viewPositionMm(rig.document.findViewNamed("Front")->id());
    const Vec2 topAt = rig.document.viewPositionMm(rig.document.findViewNamed("Top")->id());
    EXPECT_NEAR(topAt.x, frontAt.x, 1e-9);
    EXPECT_NEAR(std::abs(topAt.y - frontAt.y), 80.0, 1e-9);
}

// =============================================================================
// The file
// =============================================================================

TEST(DrawingAlignmentTest, M32_ALIGN_009_AProjectedViewSurvivesASaveAndStillLinesUp) {
    Sheet3 rig;
    rig.document.setSheetProjectionAngle(ProjectionAngle::Third);
    rig.document.addProjectedView("Top", rig.front, ViewDirection::Top, 80.0);

    const std::string text = SaveToString(rig.document);
    const DrawingLoadResult loaded = LoadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    const DrawingDocument& back = *loaded.document;

    EXPECT_EQ(back.sheet().projectionAngle(), ProjectionAngle::Third);
    const DrawingView* top = back.findViewNamed("Top");
    ASSERT_NE(top, nullptr);
    EXPECT_NE(top->parentViewId(), kInvalidObjectId) << "the view came back with no parent";
    const Vec2 frontAt = back.viewPositionMm(back.findViewNamed("Front")->id());
    EXPECT_NEAR(back.viewPositionMm(top->id()).y, frontAt.y + 80.0, 1e-9)
        << "the reopened child is not where it was";
    EXPECT_EQ(SaveToString(back), text);
}

TEST(DrawingAlignmentTest, M32_ALIGN_010_AFileWhoseViewsAreProjectedFromEachOtherIsREFUSED) {
    // A loop would make viewPositionMm's walk return a position nobody can
    // explain. The walk is bounded so it cannot spin for ever, but bounded is
    // not the same as correct -- so the file is refused instead.
    Sheet3 rig;
    rig.document.addProjectedView("Top", rig.front, ViewDirection::Top, 80.0);
    std::string text = SaveToString(rig.document);

    // Point the FRONT view at the Top view, closing the ring.
    const ObjectId topId = rig.document.findViewNamed("Top")->id();
    const std::size_t at = text.find("\"parentViewId\": \"0\"");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"parentViewId\": \"0\"").size(),
                 "\"parentViewId\": \"" + std::to_string(topId) + "\"");

    const DrawingLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded) << "a ring of projected views was accepted";
    EXPECT_EQ(loaded.error, SerializationError::InvalidDependency);
}

TEST(DrawingAlignmentTest, M32_ALIGN_011_AFileNamingAParentThatIsNotThereIsREFUSED) {
    Sheet3 rig;
    rig.document.addProjectedView("Top", rig.front, ViewDirection::Top, 80.0);
    std::string text = SaveToString(rig.document);
    const std::string real = "\"parentViewId\": \"" + std::to_string(rig.front) + "\"";
    const std::size_t at = text.find(real);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, real.size(), "\"parentViewId\": \"777333\"");

    const DrawingLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::UnknownDependencyId);
    EXPECT_NE(loaded.message.find("777333"), std::string::npos) << loaded.message;
}

TEST(DrawingAlignmentTest, M38_ALIGN_012_ASectionSitsOFFTheViewItWasCutFrom) {
    // THE FIRST SCREENSHOT HAD THEM ON TOP OF EACH OTHER, two captions written
    // over one another.
    //
    // A section records its PARENT'S direction -- its real camera is worked
    // out from the cut line at every recompute -- so the six-direction
    // alignment table compares Front with Front, finds no relationship, and
    // leaves the offset unapplied. Where a section belongs is off to the side
    // the reader looks FROM, along the arrows.
    Sheet3 sheet;
    const Vec2 parent = sheet.document.viewPositionMm(sheet.front);

    // A VERTICAL cut line: the arrows are horizontal, so the section goes to
    // one side, not above or below.
    DrawingView& section = sheet.document.addSectionView(
        "A-A", sheet.front, Vec2{20.0, -30.0}, Vec2{20.0, 30.0}, 1, 70.0);
    const Vec2 placed = sheet.document.viewPositionMm(section.id());

    EXPECT_NEAR(std::hypot(placed.x - parent.x, placed.y - parent.y), 70.0, 1e-9)
        << "the section was not moved off its parent by the offset it was given";
    EXPECT_NEAR(placed.y, parent.y, 1e-9) << "a vertical cut should not move it up or down";
    EXPECT_NEAR(placed.x, parent.x + 70.0, 1e-9);

    // TURNING THE ARROWS PUTS IT ON THE OTHER SIDE, because the side it sits
    // on IS the side the reader is looking from -- the same direction the
    // arrows are drawn in, computed the same way, so the two cannot disagree.
    ASSERT_TRUE(sheet.document.setSectionCut(section.id(), Vec2{20.0, -30.0},
                                             Vec2{20.0, 30.0}, -1));
    const Vec2 flipped = sheet.document.viewPositionMm(section.id());
    EXPECT_NEAR(flipped.x, parent.x - 70.0, 1e-9)
        << "turning the arrows did not move the section to the other side";
}

TEST(DrawingAlignmentTest, M38_ALIGN_013_WhatIsWrittenUnderAViewIsTheDocumentsAnswer) {
    // The caption used to be composed in the painter, where the only way to
    // read it was off a screenshot -- so "titled by its name instead of its
    // letter" was a change nothing could catch. A section's caption has to
    // match the cut line on its parent, and both now come from here.
    Sheet3 sheet;
    EXPECT_EQ(sheet.document.viewLabelText(sheet.front), "Front");

    DrawingView& first = sheet.document.addSectionView("Through the boss", sheet.front,
                                                       Vec2{20.0, -30.0}, Vec2{20.0, 30.0},
                                                       1, 70.0);
    EXPECT_EQ(sheet.document.viewLabelText(first.id()), "A-A")
        << "a section was titled by its name rather than its letter";
    EXPECT_EQ(sheet.document.sectionLetterOf(first.id()), "A")
        << "the caption and the cut line would carry different letters";

    DrawingView& second = sheet.document.addSectionView("B-B", sheet.front, Vec2{60.0, -30.0},
                                                        Vec2{60.0, 30.0}, 1, 70.0);
    EXPECT_EQ(sheet.document.viewLabelText(second.id()), "B-B");

    // A SCALE THAT IS NOT THE SHEET'S IS SAID. One that is, is not: written
    // always it is noise, written never a detail view at 2:1 reads full size.
    ASSERT_TRUE(sheet.document.setViewScale(second.id(), DrawingScale{2, 1}));
    EXPECT_EQ(sheet.document.viewLabelText(second.id()), "B-B  (2:1)");
    EXPECT_EQ(sheet.document.viewLabelText(sheet.front), "Front");

    // ...and the two sections hatch at DIFFERENT angles, which is the whole
    // reason the angle exists -- two cut parts meeting on one sheet.
    EXPECT_NE(sheet.document.sectionHatchStyle(first.id()).angleRad,
              sheet.document.sectionHatchStyle(second.id()).angleRad);
}

TEST(DrawingAlignmentTest, M38_ALIGN_014_AFileWhoseCutLineHasNOLENGTHIsREFUSED) {
    // ADR-M3-008: what the saver refuses, the loader has to refuse too, and by
    // the SAME rule -- a document that saves cleanly and will not reopen is
    // the named worst case. The document refuses a cut of no length; so must
    // a file, because a file can be written by an older build or by hand.
    Sheet3 rig;
    rig.document.addSectionView("A-A", rig.front, Vec2{20.0, -30.0}, Vec2{20.0, 30.0}, 1,
                                70.0);
    std::string text = SaveToString(rig.document);

    // Put the far end of the cut on top of the near end: a knife with no
    // length, which cuts nothing and whose arrows have no direction. Written
    // by finding the key and replacing whatever number follows it, rather than
    // matching a number the writer formats.
    const std::string key = "\"toYMm\":";
    const std::size_t at = text.find(key);
    ASSERT_NE(at, std::string::npos) << text;
    const std::size_t ends = text.find_first_of(",}", at + key.size());
    ASSERT_NE(ends, std::string::npos) << text;
    text.replace(at, ends - at, key + " -30");

    const DrawingLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded) << "a section whose cut line has no length was accepted";
    EXPECT_FALSE(loaded.message.empty());
}
