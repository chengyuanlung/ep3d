// M51.2 -- a PART that is made of sheet, and the three facts that only mean
// anything together.
//
// SheetMetalStandardsTests pins the table and the arithmetic. This pins the
// part-level decision: a thickness with no material has no K factor, and a
// material with no thickness has no minimum bend radius. Half of it set is a
// part whose flat pattern is computed from a number nobody chose -- and the
// flat pattern is self-consistent either way.

#include "Core/Document/PartDocument.h"
#include "Core/Serialization/PartDocumentSerializer.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace {

using namespace paramcad;

SheetMetalSettings MildSteel2mm() {
    SheetMetalSettings settings;
    settings.isSheetMetal = true;
    settings.thicknessMm = 2.0;
    settings.material = SheetMaterial::MildSteelAluminium;
    settings.defaultBendRadiusMm = 2.0;
    return settings;
}

TEST(SheetMetalPartTest, M51_PART_001_APartIsNotSheetMetalUntilItSaysSo) {
    PartDocument part{"Bracket"};
    EXPECT_FALSE(part.sheetMetal().isSheetMetal);
    // AND ZERO IS NOT A THICKNESS. An ordinary solid has no stated thickness
    // rather than a stated thickness of nothing, which is what a later reader
    // would have to tell apart.
    EXPECT_EQ(part.sheetMetal().thicknessMm, 0.0);

    ASSERT_TRUE(part.setSheetMetal(MildSteel2mm()));
    EXPECT_TRUE(part.sheetMetal().isSheetMetal);
    EXPECT_NEAR(part.sheetMetal().thicknessMm, 2.0, 1e-9);
}

TEST(SheetMetalPartTest, M51_PART_002_HalfASettingIsRefused) {
    PartDocument part{"Bracket"};
    SheetMetalSettings noThickness = MildSteel2mm();
    noThickness.thicknessMm = 0.0;
    EXPECT_FALSE(part.setSheetMetal(noThickness))
        << "sheet metal with no thickness was accepted, so its bends have no K factor";

    SheetMetalSettings noRadius = MildSteel2mm();
    noRadius.defaultBendRadiusMm = 0.0;
    EXPECT_FALSE(part.setSheetMetal(noRadius))
        << "sheet metal with no default radius was accepted, so every bend invents its own";

    // AND THE REASON NAMES THE FIELD THAT IS MISSING.
    //
    // The mutation gate found that deleting either of these two checks changes
    // nothing about WHETHER the setting is refused: the bend probe at the end
    // catches both, because a thickness of zero has no bend and a radius of
    // zero is tighter than any material takes. What it changes is what the
    // user is told -- an answer about a BEND, when they never mentioned one,
    // for a box they left blank.
    //
    // So the message is what these two are for, and the message is what is
    // pinned. Anything else would be leaving them in on the strength of a
    // comment.
    EXPECT_NE(part.whySheetMetalRefused(noThickness).find("K factor"), std::string::npos)
        << part.whySheetMetalRefused(noThickness);
    EXPECT_NE(part.whySheetMetalRefused(noRadius).find("invents its own"), std::string::npos)
        << part.whySheetMetalRefused(noRadius);

    // AND A DEFAULT RADIUS THE MATERIAL CRACKS AT IS REFUSED HERE, once --
    // rather than refusing every bend that used it, one at a time, after the
    // part is drawn.
    SheetMetalSettings tooTight = MildSteel2mm();
    tooTight.defaultBendRadiusMm = 0.5;
    EXPECT_FALSE(part.setSheetMetal(tooTight));
    EXPECT_FALSE(part.whySheetMetalRefused(tooTight).empty());

    // ...and none of the refusals left anything behind.
    EXPECT_FALSE(part.sheetMetal().isSheetMetal);
}

TEST(SheetMetalPartTest, M51_PART_003_TurningItOffIsAlwaysAllowed) {
    // A part that stops being sheet metal is an ordinary solid, and nothing
    // about it needs a thickness. Requiring one to switch off would leave a
    // part that cannot stop being what it was made.
    PartDocument part{"Bracket"};
    ASSERT_TRUE(part.setSheetMetal(MildSteel2mm()));
    ASSERT_TRUE(part.clearSheetMetal());
    EXPECT_FALSE(part.sheetMetal().isSheetMetal);
    // ...and again is a no-op rather than a second undo step for nothing.
    EXPECT_FALSE(part.clearSheetMetal());

    SheetMetalSettings off;
    EXPECT_TRUE(part.whySheetMetalRefused(off).empty());
}

TEST(SheetMetalPartTest, M51_PART_004_TheWholeSettingComesBackOnUndo) {
    // The three fields are one decision. Half of it restored is a part whose
    // flat pattern is computed from a number nobody chose -- and it is
    // self-consistent, so nothing says so.
    PartDocument part{"Bracket"};
    ASSERT_TRUE(part.setSheetMetal(MildSteel2mm()));

    SheetMetalSettings thicker = MildSteel2mm();
    thicker.thicknessMm = 3.0;
    thicker.material = SheetMaterial::SoftBrassCopper;
    thicker.defaultBendRadiusMm = 4.0;
    ASSERT_TRUE(part.setSheetMetal(thicker));
    EXPECT_EQ(part.sheetMetal().material, SheetMaterial::SoftBrassCopper);

    ASSERT_TRUE(part.undo());
    EXPECT_TRUE(part.sheetMetal().isSheetMetal);
    EXPECT_NEAR(part.sheetMetal().thicknessMm, 2.0, 1e-9);
    EXPECT_EQ(part.sheetMetal().material, SheetMaterial::MildSteelAluminium)
        << "undo put the thickness back and left the material where it was";
    EXPECT_NEAR(part.sheetMetal().defaultBendRadiusMm, 2.0, 1e-9);

    ASSERT_TRUE(part.undo());
    EXPECT_FALSE(part.sheetMetal().isSheetMetal);
    ASSERT_TRUE(part.redo());
    EXPECT_TRUE(part.sheetMetal().isSheetMetal);
}

TEST(SheetMetalPartTest, M51_PART_005_TheSettingSurvivesTheFile) {
    PartDocument part{"Bracket"};
    SheetMetalSettings brass = MildSteel2mm();
    brass.material = SheetMaterial::SoftBrassCopper;
    brass.thicknessMm = 1.5;
    brass.defaultBendRadiusMm = 1.0;
    ASSERT_TRUE(part.setSheetMetal(brass));

    std::ostringstream out;
    const SaveResult saved = savePartDocument(part, out);
    ASSERT_EQ(saved.error, SerializationError::None) << saved.message;
    EXPECT_NE(out.str().find("sheetMetal"), std::string::npos);

    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_TRUE(loaded.document->sheetMetal().isSheetMetal);
    EXPECT_NEAR(loaded.document->sheetMetal().thicknessMm, 1.5, 1e-9);
    EXPECT_EQ(loaded.document->sheetMetal().material, SheetMaterial::SoftBrassCopper)
        << "a brass part came back as mild steel, whose K is a tenth away";
    EXPECT_NEAR(loaded.document->sheetMetal().defaultBendRadiusMm, 1.0, 1e-9);
    // ...AND WITH NO HISTORY. Loading is not an edit, and a freshly opened
    // part with a step of history is one somebody can undo into a state the
    // file never held.
    EXPECT_FALSE(loaded.document->canUndo());
}

TEST(SheetMetalPartTest, M51_PART_006_AnOrdinaryPartWritesNoThickness) {
    // An ordinary solid's file must not carry a thickness of nothing that a
    // later reader could mistake for a stated one.
    PartDocument part{"Bracket"};
    std::ostringstream out;
    ASSERT_EQ(savePartDocument(part, out).error, SerializationError::None);
    EXPECT_EQ(out.str().find("sheetMetal"), std::string::npos);

    std::istringstream in(out.str());
    const LoadResult loaded = loadPartDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_FALSE(loaded.document->sheetMetal().isSheetMetal);
}

TEST(SheetMetalPartTest, M51_PART_007_WhatTheSaverRefusesTheLoaderRefuses) {
    // ADR-M3-008. A hand-edited file whose default radius the material cracks
    // at would otherwise open as a part every one of whose bends is refused,
    // one at a time, after it is drawn.
    PartDocument part{"Bracket"};
    ASSERT_TRUE(part.setSheetMetal(MildSteel2mm()));
    std::ostringstream out;
    ASSERT_EQ(savePartDocument(part, out).error, SerializationError::None);
    std::string text = out.str();

    const std::string::size_type at = text.find("\"defaultBendRadiusMm\": 2");
    ASSERT_NE(at, std::string::npos) << text.substr(0, 600);
    text.replace(at, std::string("\"defaultBendRadiusMm\": 2").size(),
                 "\"defaultBendRadiusMm\": 0.5");
    std::istringstream tight(text);
    EXPECT_FALSE(loadPartDocument(tight)) << "a radius the material cracks at loaded anyway";

    // ...and a material this build does not know is refused rather than
    // becoming mild steel.
    std::string other = out.str();
    const std::string::size_type mat = other.find("\"material\": \"mild-steel-aluminium\"");
    ASSERT_NE(mat, std::string::npos);
    other.replace(mat, std::string("\"material\": \"mild-steel-aluminium\"").size(),
                  "\"material\": \"titanium\"");
    std::istringstream unknown(other);
    EXPECT_FALSE(loadPartDocument(unknown)) << "an unknown sheet material loaded as mild steel";
}

} // namespace
