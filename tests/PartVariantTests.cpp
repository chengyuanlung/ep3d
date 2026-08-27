// M54.1 -- one part, several sizes, and the answer nobody writes down.
//
// THE FAILURE THIS IS FOR: a drawing of variant B carrying variant A's
// dimensions. Both are real sizes of a real part, every number is one the part
// has had, nothing is dangling or red. The wrong bracket gets made, in the
// right quantity, to a drawing that checks out.
//
// It cannot happen here because NOTHING REMEMBERS which variant is active. A
// stored answer is a second copy of what the parameters already hold, and it
// goes stale the first time a dimension is touched. The question is answered
// by comparing, so there is nowhere to keep a stale answer.

#include "Core/Document/PartDocument.h"
#include "Core/Parameter/PartVariant.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

struct Bracket {
    PartDocument part{"Bracket"};
    ObjectId length = kInvalidObjectId;
    ObjectId width = kInvalidObjectId;
    ObjectId unrelated = kInvalidObjectId;

    Bracket() {
        length = part.addParameter("L", 100.0, UnitType::Millimeter).id();
        width = part.addParameter("W", 40.0, UnitType::Millimeter).id();
        // A parameter the table does NOT vary by, so "which variant is this"
        // has something to correctly ignore.
        unrelated = part.addParameter("Fillet", 3.0, UnitType::Millimeter).id();
    }

    PartVariant Size(const std::string& name, double lengthMm, double widthMm) const {
        PartVariant variant;
        variant.name = name;
        variant.values[length] = lengthMm;
        variant.values[width] = widthMm;
        return variant;
    }
};

TEST(PartVariantTest, M54_VAR_001_WhichVariantThisIsCOMESFromComparing) {
    Bracket bracket;
    ASSERT_TRUE(bracket.part.addVariant(bracket.Size("Short", 100.0, 40.0)));
    ASSERT_TRUE(bracket.part.addVariant(bracket.Size("Long", 250.0, 40.0)));

    // The parameters start at Short's numbers, so that is what it is -- with
    // nothing having been applied.
    EXPECT_EQ(bracket.part.activeVariantName(), "Short");

    ASSERT_TRUE(bracket.part.applyVariant("Long"));
    EXPECT_EQ(bracket.part.activeVariantName(), "Long");
    EXPECT_NEAR(bracket.part.parameters().findById(bracket.length)->value(), 250.0, 1e-9);

    // THE ONE THAT MATTERS. Nudge a dimension and the part is not Long any
    // more -- and it does not go on saying it is.
    ASSERT_TRUE(bracket.part.setParameterValue(bracket.length, 251.0));
    EXPECT_EQ(bracket.part.activeVariantName(), "")
        << "the part was changed and still claims to be a stated size";
}

TEST(PartVariantTest, M54_VAR_002_OnlyTheColumnsTheTableVariesByAreCompared) {
    // A variant is a set of stated dimensions, not the whole part. Editing
    // something the table does not vary by leaves the size what it was --
    // otherwise adding a fillet would take the part off its own size list.
    Bracket bracket;
    ASSERT_TRUE(bracket.part.addVariant(bracket.Size("Short", 100.0, 40.0)));
    ASSERT_EQ(bracket.part.activeVariantName(), "Short");

    ASSERT_TRUE(bracket.part.setParameterValue(bracket.unrelated, 5.0));
    EXPECT_EQ(bracket.part.activeVariantName(), "Short")
        << "editing a dimension the table does not vary by took the part off its size";
}

TEST(PartVariantTest, M54_VAR_003_EveryRowFillsEveryColumn) {
    // A row naming fewer columns is a size with a dimension nobody stated,
    // and "whichever value happened to be in the parameter when you switched"
    // is a leftover rather than a specification.
    Bracket bracket;
    ASSERT_TRUE(bracket.part.addVariant(bracket.Size("Short", 100.0, 40.0)));

    PartVariant partial;
    partial.name = "Medium";
    partial.values[bracket.length] = 160.0;   // says nothing about W
    EXPECT_FALSE(bracket.part.addVariant(partial))
        << "a variant that leaves a dimension unstated was accepted";
    EXPECT_FALSE(bracket.part.whyVariantRefused(partial).empty());

    PartVariant extra = bracket.Size("Wide", 100.0, 60.0);
    extra.values[bracket.unrelated] = 4.0;    // an extra column
    EXPECT_FALSE(bracket.part.addVariant(extra))
        << "a variant naming a column the table does not have was accepted";

    // THE SAME NUMBER OF COLUMNS AND DIFFERENT ONES -- which the count check
    // above cannot see. Found by the mutation gate: both rows above were the
    // wrong SIZE, so the check on WHICH parameters was never asked anything.
    //
    // A row like this states two dimensions, so it looks complete, and one of
    // them is a dimension the other rows do not vary -- so switching to it
    // leaves the length at whatever the last size left behind.
    PartVariant sideways;
    sideways.name = "Sideways";
    sideways.values[bracket.width] = 60.0;
    sideways.values[bracket.unrelated] = 4.0;
    EXPECT_EQ(sideways.values.size(), 2u);
    EXPECT_FALSE(bracket.part.addVariant(sideways))
        << "a variant with the right number of columns and the wrong ones was accepted";
    EXPECT_NE(bracket.part.whyVariantRefused(sideways).find("does not say what"),
              std::string::npos)
        << bracket.part.whyVariantRefused(sideways);
}

TEST(PartVariantTest, M54_VAR_004_TwoRowsWithTheSameNumbersAreRefused) {
    // Then "which variant is this" would have two right answers, which is the
    // one thing the comparison must never face.
    Bracket bracket;
    ASSERT_TRUE(bracket.part.addVariant(bracket.Size("Short", 100.0, 40.0)));
    EXPECT_FALSE(bracket.part.addVariant(bracket.Size("Stubby", 100.0, 40.0)))
        << "two sizes with identical numbers were both accepted";
    // ...and so is the same NAME twice, which a drawing would resolve to
    // whichever came first.
    EXPECT_FALSE(bracket.part.addVariant(bracket.Size("Short", 300.0, 40.0)));
    EXPECT_EQ(bracket.part.variants().size(), 1u);
}

TEST(PartVariantTest, M54_VAR_005_AVariantOfAParameterThatIsGoneIsRefused) {
    Bracket bracket;
    PartVariant ghost;
    ghost.name = "Ghost";
    ghost.values[bracket.length] = 100.0;
    ghost.values[9999] = 40.0;   // not a parameter of this part
    EXPECT_FALSE(bracket.part.addVariant(ghost))
        << "a variant naming a parameter this part does not have was accepted";
    EXPECT_NE(bracket.part.whyVariantRefused(ghost).find("not in this part"),
              std::string::npos);
}

TEST(PartVariantTest, M54_VAR_006_ApplyingASizeThisPartDoesNotHaveIsRefused) {
    // A quiet no-op would leave the caller drawing, dimensioning and ordering
    // the size it was already at.
    Bracket bracket;
    ASSERT_TRUE(bracket.part.addVariant(bracket.Size("Short", 100.0, 40.0)));
    EXPECT_FALSE(bracket.part.applyVariant("Enormous"));
    EXPECT_NEAR(bracket.part.parameters().findById(bracket.length)->value(), 100.0, 1e-9);
}

TEST(PartVariantTest, M54_VAR_007_ApplyingASizeIsONEUndoStep) {
    // Switching size is one thing the user did, and a half-applied variant is
    // a part no size ever was.
    Bracket bracket;
    ASSERT_TRUE(bracket.part.addVariant(bracket.Size("Short", 100.0, 40.0)));
    ASSERT_TRUE(bracket.part.addVariant(bracket.Size("Big", 250.0, 80.0)));
    ASSERT_TRUE(bracket.part.applyVariant("Big"));
    ASSERT_EQ(bracket.part.activeVariantName(), "Big");

    ASSERT_TRUE(bracket.part.undo());
    EXPECT_EQ(bracket.part.activeVariantName(), "Short")
        << "one undo left the part between two sizes";
    EXPECT_NEAR(bracket.part.parameters().findById(bracket.length)->value(), 100.0, 1e-9);
    EXPECT_NEAR(bracket.part.parameters().findById(bracket.width)->value(), 40.0, 1e-9);

    ASSERT_TRUE(bracket.part.redo());
    EXPECT_EQ(bracket.part.activeVariantName(), "Big");
}

TEST(PartVariantTest, M54_VAR_008_TheROWComesBackWholeOnUndo) {
    // A variant is a set of values that only mean anything together, and half
    // of one restored is a size nobody specified.
    Bracket bracket;
    ASSERT_TRUE(bracket.part.addVariant(bracket.Size("Short", 100.0, 40.0)));
    ASSERT_TRUE(bracket.part.addVariant(bracket.Size("Long", 250.0, 40.0)));
    ASSERT_TRUE(bracket.part.removeVariant("Short"));
    ASSERT_EQ(bracket.part.variants().size(), 1u);

    ASSERT_TRUE(bracket.part.undo());
    ASSERT_EQ(bracket.part.variants().size(), 2u);
    // BACK WHERE IT WAS, which is what makes the FIRST row the first row --
    // and the first row is what fixes the table's columns.
    EXPECT_EQ(bracket.part.variants()[0].name, "Short");
    EXPECT_EQ(bracket.part.variants()[0].values.size(), 2u);
    EXPECT_NEAR(bracket.part.variants()[0].values.at(bracket.length), 100.0, 1e-9);
}

TEST(PartVariantTest, M54_VAR_009_APartWithNoVariantsIsNotOnOne) {
    // "No variants" and "not on a variant" are the same empty answer here, and
    // both are correct: an ordinary part is not a size out of a list.
    Bracket bracket;
    EXPECT_EQ(bracket.part.activeVariantName(), "");
    EXPECT_TRUE(bracket.part.variants().empty());
    EXPECT_TRUE(VariantColumns(bracket.part.variants()).empty());
    EXPECT_EQ(FindVariant(bracket.part.variants(), "Short"), nullptr);
}

TEST(PartVariantTest, M54_VAR_010_AVariantThatSetsNothingIsThePartItAlreadyWas) {
    Bracket bracket;
    PartVariant empty;
    empty.name = "Empty";
    EXPECT_FALSE(bracket.part.addVariant(empty));

    PartVariant unnamed;
    unnamed.values[bracket.length] = 100.0;
    EXPECT_FALSE(bracket.part.addVariant(unnamed))
        << "a variant with no name was accepted, and a drawing has nothing to ask for";
}

} // namespace
