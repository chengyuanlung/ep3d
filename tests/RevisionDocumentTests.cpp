// M48.2 -- the revision as a thing ON A DRAWING, and in a file.
//
// RevisionTests pins the letters. This pins the failure the milestone exists
// for: THE BLOCK IN THE CORNER SAYS Rev B AND THE TABLE SAYS Rev C.
//
// Both are neat. Both are complete. Nothing is dangling, nothing is red, and
// the shop builds to whichever one they read first. The only defence is that
// there is no second place to type it -- the block's field is derived, and
// setField refuses it the way it refuses the scale.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace {

using namespace paramcad;

Revision Issue(std::string letter, std::string what) {
    Revision one;
    one.letter = std::move(letter);
    one.description = std::move(what);
    one.date = "2026-08-26";
    one.by = "EP";
    return one;
}

const TitleBlockField* RevisionField(const DrawingDocument& document) {
    return document.titleBlock().findField("Rev");
}

TEST(RevisionDocumentTest, M48_DOC_001_TheBlockCannotSayADifferentIssueFromTheTable) {
    DrawingDocument document{"Bracket"};
    // SEEDED, not added here. A drawing that has a history and nowhere in the
    // corner to print it is the half-answer: the block is what a reader checks
    // first, and often all they check.
    const TitleBlockField* field = RevisionField(document);
    ASSERT_NE(field, nullptr) << "a new drawing has no revision row in its title block";
    EXPECT_TRUE(field->isDerived());

    // AN UNISSUED DRAWING IS NOT AT REV A. It is at no revision, and printing
    // A would be a claim nobody made about a drawing nobody has released.
    EXPECT_EQ(document.titleBlockValue(*field), "");
    EXPECT_EQ(document.currentRevision(), "");

    ASSERT_TRUE(document.addRevision(Issue("A", "first issue")));
    EXPECT_EQ(document.titleBlockValue(*field), "A");
    ASSERT_TRUE(document.addRevision(Issue("B", "clearance holes opened to 8.4")));
    EXPECT_EQ(document.titleBlockValue(*field), "B");

    // AND THERE IS NO WAY TO TYPE THE SECOND COPY. This is the whole defence:
    // the field is derived, so the drift cannot be expressed rather than
    // being tested for after the fact.
    EXPECT_FALSE(document.setTitleBlockField("Rev", "C"))
        << "an issue letter was typed into the title block";
    EXPECT_EQ(document.titleBlockValue(*field), "B");
}

TEST(RevisionDocumentTest, M48_DOC_002_TheOfferedLetterFollowsTheHistory) {
    DrawingDocument document{"Bracket"};
    EXPECT_EQ(document.nextRevisionLetter(), "A");
    ASSERT_TRUE(document.addRevision(Issue("A", "first issue")));
    EXPECT_EQ(document.nextRevisionLetter(), "B");
    ASSERT_TRUE(document.addRevision(Issue("B", "holes moved")));
    EXPECT_EQ(document.nextRevisionLetter(), "C");

    // A ROW THE STANDARD REFUSES DOES NOT GET IN, and the history is unchanged.
    EXPECT_FALSE(document.addRevision(Issue("B", "again")));
    EXPECT_FALSE(document.addRevision(Issue("O", "misreadable")));
    EXPECT_FALSE(document.addRevision(Issue("C", "")));
    EXPECT_EQ(document.revisions().size(), 2u);
    EXPECT_EQ(document.currentRevision(), "B");
}

TEST(RevisionDocumentTest, M48_DOC_003_UndoPutsTheLetterBackRatherThanRecomputingIt) {
    // THE POINT OF STORING IT. If undo re-derived the letter from position, a
    // withdrawn Rev B coming back after a Rev C had been issued would return
    // as C -- rewriting a history that other people's paperwork cites, with
    // nothing on screen looking wrong.
    DrawingDocument document{"Bracket"};
    ASSERT_TRUE(document.addRevision(Issue("A", "first issue")));
    ASSERT_TRUE(document.addRevision(Issue("B", "holes moved")));
    ASSERT_TRUE(document.addRevision(Issue("C", "material changed")));
    ASSERT_TRUE(document.removeRevision("B"));
    ASSERT_EQ(document.revisions().size(), 2u);

    ASSERT_TRUE(document.undo());
    ASSERT_EQ(document.revisions().size(), 3u);
    // BACK IN THE MIDDLE, still called B. Appended it would be the latest
    // issue, and the title block would say B on a drawing that is at C.
    EXPECT_EQ(document.revisions()[1].letter, "B");
    EXPECT_EQ(document.currentRevision(), "C");
}

TEST(RevisionDocumentTest, M48_DOC_004_TheTableHoldsNoneOfTheHistory) {
    DrawingDocument document{"Bracket"};
    RevisionTable& table = document.addRevisionTable("Revisions", Vec2{150.0, 30.0});
    ASSERT_TRUE(document.addRevision(Issue("A", "first issue")));
    ASSERT_TRUE(document.addRevision(Issue("B", "holes moved")));

    // The table is a place and a size. What it SHOWS is asked of the drawing
    // at every repaint, so it cannot show an issue this drawing does not have
    // and cannot miss one it does.
    EXPECT_EQ(document.revisions().size(), 2u);
    EXPECT_NE(document.findRevisionTable(table.id()), nullptr);
    ASSERT_TRUE(document.removeRevision("B"));
    EXPECT_EQ(document.revisions().size(), 1u);
    EXPECT_NE(document.findRevisionTable(table.id()), nullptr)
        << "withdrawing an issue took the table with it";
}

TEST(RevisionDocumentTest, M48_DOC_005_MovingTheTableIsUndoable) {
    // The first cut of M48 carried moving on the existence record, which had
    // no before to go back to -- so dragging the table was the one change on
    // this drawing undo could not take back.
    DrawingDocument document{"Bracket"};
    RevisionTable& table = document.addRevisionTable("Revisions", Vec2{150.0, 30.0});
    const ObjectId id = table.id();
    ASSERT_TRUE(document.setRevisionTablePosition(id, Vec2{40.0, 90.0}));
    ASSERT_NE(document.findRevisionTable(id), nullptr);
    EXPECT_NEAR(document.findRevisionTable(id)->positionMm().x, 40.0, 1e-9);

    ASSERT_TRUE(document.undo());
    EXPECT_NEAR(document.findRevisionTable(id)->positionMm().x, 150.0, 1e-9);
    EXPECT_NEAR(document.findRevisionTable(id)->positionMm().y, 30.0, 1e-9);
}

TEST(RevisionDocumentTest, M48_DOC_006_TheHistorySurvivesTheFileInOrder) {
    DrawingDocument document{"Bracket"};
    ASSERT_TRUE(document.addRevision(Issue("A", "first issue")));
    ASSERT_TRUE(document.addRevision(Issue("B", "clearance holes opened to 8.4")));
    document.addRevisionTable("Revisions", Vec2{150.0, 30.0});

    std::ostringstream out;
    const SaveResult saved = saveDrawingDocument(document, out);
    ASSERT_EQ(saved.error, SerializationError::None) << saved.message;

    std::istringstream in(out.str());
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->revisions().size(), 2u);
    // THE ORDER IS THE MEANING: the last row is what the drawing is issued at.
    EXPECT_EQ(loaded.document->revisions()[0].letter, "A");
    EXPECT_EQ(loaded.document->revisions()[1].letter, "B");
    EXPECT_EQ(loaded.document->revisions()[1].description, "clearance holes opened to 8.4");
    EXPECT_EQ(loaded.document->currentRevision(), "B");
    EXPECT_EQ(loaded.document->revisionTables().size(), 1u);

    const TitleBlockField* field = loaded.document->titleBlock().findField("Rev");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(loaded.document->titleBlockValue(*field), "B");
}

TEST(RevisionDocumentTest, M48_DOC_007_WhatTheSaverRefusesTheLoaderRefuses) {
    // ADR-M3-008, by calling the same function. A file hand-edited to carry
    // two Rev Cs is a history nobody can cite, and it opens looking ordinary.
    DrawingDocument document{"Bracket"};
    ASSERT_TRUE(document.addRevision(Issue("A", "first issue")));
    ASSERT_TRUE(document.addRevision(Issue("B", "holes moved")));
    std::ostringstream out;
    ASSERT_EQ(saveDrawingDocument(document, out).error, SerializationError::None);

    std::string text = out.str();
    const std::string::size_type at = text.find("\"letter\": \"B\"");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, std::string("\"letter\": \"B\"").size(), "\"letter\": \"A\"");

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded) << "a drawing with two Rev As loaded anyway";
}

} // namespace
