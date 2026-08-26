// M48.1 -- the revision table, and the one place this project does NOT derive.
//
// Everything else on this drawing is composed at draw time: a balloon's
// number, a datum's letter, a section's letter, a sheet count. This file pins
// the opposite decision and the reason for it -- a revision letter is a fact
// that has already left the building, and rewriting it from row position would
// rewrite the history that purchase orders and inspection reports cite.
//
// What IS derived is the pair that drifts: the next letter to offer, and the
// revision the title block prints.

#include "Core/Drawing/Revision.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace paramcad;

Revision Made(std::string letter, std::string what = "hole pattern moved 3 mm") {
    Revision one;
    one.letter = std::move(letter);
    one.description = std::move(what);
    one.date = "2026-08-26";
    one.by = "EP";
    return one;
}

TEST(RevisionTest, M48_REV_001_TheLettersSkipITheOAndTheQ) {
    // ON A PHOTOCOPY THEY ARE 1, 0 AND O. A formula walking the alphabet would
    // produce all three, and the drawing that says Rev O sends somebody
    // looking for a revision zero.
    const std::string_view letters = RevisionLetters();
    EXPECT_EQ(letters.find('I'), std::string_view::npos);
    EXPECT_EQ(letters.find('O'), std::string_view::npos);
    EXPECT_EQ(letters.find('Q'), std::string_view::npos);
    EXPECT_NE(letters.find('H'), std::string_view::npos);
    EXPECT_NE(letters.find('J'), std::string_view::npos);

    EXPECT_FALSE(IsRevisionLetter("I"));
    EXPECT_FALSE(IsRevisionLetter("O"));
    EXPECT_FALSE(IsRevisionLetter("Q"));
    EXPECT_FALSE(IsRevisionLetter(""));
    EXPECT_TRUE(IsRevisionLetter("A"));
    EXPECT_TRUE(IsRevisionLetter("AA"));

    // THE THREE ARE SKIPPED WHERE THEY FALL, not shifted off the end.
    EXPECT_EQ(NextRevisionLetter("H"), "J");
    EXPECT_EQ(NextRevisionLetter("N"), "P");
    EXPECT_EQ(NextRevisionLetter("P"), "R");
}

TEST(RevisionTest, M48_REV_002_TheOfferedLetterIsAlwaysOneTheRuleAccepts) {
    // ONE LIST, WALKED AND CHECKED. Two lists here would be the classic pair:
    // one that offers Q and one that refuses it, each correct on its own.
    std::string letter = NextRevisionLetter("");
    EXPECT_EQ(letter, "A");
    for (int step = 0; step < 60; ++step) {
        ASSERT_TRUE(IsRevisionLetter(letter))
            << "the next letter offered was one the rule refuses: " << letter;
        letter = NextRevisionLetter(letter);
    }
    // AND IT DOUBLES RATHER THAN RUNNING OUT.
    EXPECT_EQ(NextRevisionLetter("Z"), "AA");
    EXPECT_EQ(NextRevisionLetter("AH"), "AJ");
    EXPECT_EQ(NextRevisionLetter("ZZ"), "AAA");
}

TEST(RevisionTest, M48_REV_003_ARevisionHasToSayWhatChanged) {
    const std::vector<Revision> none;
    Revision blank = Made("A");
    blank.description.clear();
    EXPECT_FALSE(WhyRevisionRefused(blank, none).empty())
        << "a revision that says nothing changed was accepted";

    Revision unlettered = Made("");
    EXPECT_FALSE(WhyRevisionRefused(unlettered, none).empty());

    Revision misread = Made("O");
    EXPECT_FALSE(WhyRevisionRefused(misread, none).empty()) << "Rev O was accepted";

    EXPECT_TRUE(WhyRevisionRefused(Made("A"), none).empty());
}

TEST(RevisionTest, M48_REV_004_TheSameLetterTwiceIsTwoDrawingsWithOneName) {
    // Parts already made to one Rev C cannot be told from parts made to the
    // other, and nothing about the table looks wrong.
    const std::vector<Revision> history{Made("A"), Made("B"), Made("C")};
    EXPECT_FALSE(WhyRevisionRefused(Made("C"), history).empty())
        << "a second Rev C was accepted";
    EXPECT_TRUE(WhyRevisionRefused(Made("D"), history).empty());
    // ...and the letter the drawing would OFFER next is the one after the last.
    EXPECT_EQ(NextRevisionLetter(history.back().letter), "D");
}

TEST(RevisionTest, M48_REV_005_TheTableGrowsUpwardsFromItsCorner) {
    // THE NEWEST ISSUE IS AT THE TOP and the heading at the bottom, which is
    // the opposite of the parts list. A table that grew downwards would put
    // the newest revision where the reader looks for the oldest -- and both
    // read as a perfectly ordinary table.
    RevisionTable table{"Revisions", Vec2{100.0, 20.0}, kInvalidObjectId};
    ASSERT_TRUE(table.setRowHeightMm(6.0));
    EXPECT_NEAR(table.rowBottomMm(0), 0.0, 1e-9);
    EXPECT_NEAR(table.rowBottomMm(1), 6.0, 1e-9);
    EXPECT_NEAR(table.rowBottomMm(3), 18.0, 1e-9);
    EXPECT_GT(table.rowBottomMm(3), table.rowBottomMm(1));

    EXPECT_FALSE(table.setRowHeightMm(0.0));
    EXPECT_FALSE(table.setWidthMm(-1.0));
}

TEST(RevisionTest, M48_REV_006_TheHeadingAndTheCellsWalkOneList) {
    // A column headed DATE and filled with a name is the defect a second list
    // produces, and it reads as a table somebody filled in carelessly rather
    // than as a program that is wrong.
    const Revision one = Made("B", "clearance holes opened to 8.4");
    const std::vector<RevisionColumn>& columns = RevisionColumns();
    ASSERT_EQ(columns.size(), 4u);
    EXPECT_EQ(CellOf(one, columns[0]), "B");
    EXPECT_EQ(CellOf(one, columns[1]), "clearance holes opened to 8.4");
    EXPECT_EQ(CellOf(one, columns[2]), "2026-08-26");
    EXPECT_EQ(CellOf(one, columns[3]), "EP");
    for (const RevisionColumn column : columns) EXPECT_FALSE(toString(column).empty());
}

} // namespace
