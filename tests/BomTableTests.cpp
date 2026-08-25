// M35.6 -- the parts list.
//
// ONE PROPERTY MATTERS MORE THAN THE REST: the numbers are COUNTED, never
// stored. A list that kept its own quantities is a drawing stating a bill of
// materials the assembly no longer has -- somebody adds two bolts, the drawing
// still says four, and the wrong number is the one that gets ordered.
//
// It is the same failure the title block's Scale row exists to rule out, on
// the field that costs money.

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Drawing/BomTable.h"
#include "Core/Drawing/DrawingDocument.h"
#include "Core/Document/PartDocument.h"
#include "Core/Serialization/AssemblyDocumentSerializer.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"
#include "Core/Serialization/PartDocumentSerializer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

struct Scratch {
    std::string path;
    explicit Scratch(const char* name) {
        path = (std::filesystem::temp_directory_path() / (std::string("ep3d-bom-") + name))
                   .string();
        std::remove(path.c_str());
    }
    ~Scratch() { std::remove(path.c_str()); }
};

// A part file with one body, so an instance has something to name.
void WritePart(const std::string& path, const std::string& bodyName) {
    PartDocument part{"Source"};
    part.addBody(bodyName);
    ASSERT_TRUE(savePartDocumentToFile(part, path));
}

void WriteAssembly(const std::string& path, const AssemblyDocument& assembly) {
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good());
    ASSERT_TRUE(saveAssemblyDocument(assembly, out));
}

std::string SaveDrawing(const DrawingDocument& document) {
    std::ostringstream out;
    EXPECT_TRUE(saveDrawingDocument(document, out));
    return out.str();
}

} // namespace

// =============================================================================
// Counting
// =============================================================================

TEST(BomTableTest, M35_BOM_001_IdenticalPartsAreONERowWithAQuantity) {
    // "Identical" is the same FILE and BODY, not the same instance name. A
    // user who called them Bolt1..Bolt4 still ordered four of one thing, and a
    // list that gave each its own line is one somebody orders four lines from.
    Scratch bolt{"bolt.ep3d"};
    WritePart(bolt.path, "M6x20");

    AssemblyDocument assembly{"Rig"};
    for (int i = 1; i <= 4; ++i)
        assembly.addInstance("Bolt" + std::to_string(i), bolt.path, "M6x20");

    const BomContents counted = CountAssembly(assembly, BomDepth::TopLevel);
    ASSERT_TRUE(counted.ok) << counted.why;
    ASSERT_EQ(counted.rows.size(), 1u) << "four copies of one bolt came out as four rows";
    EXPECT_EQ(counted.rows[0].quantity, 4);
    EXPECT_EQ(counted.rows[0].item, 1);
    EXPECT_EQ(counted.rows[0].partName, "M6x20");
    EXPECT_EQ(counted.totalQuantity(), 4);
}

TEST(BomTableTest, M35_BOM_002_DifferentPartsAreDifferentRowsNumberedInOrder) {
    Scratch bolt{"b2.ep3d"};
    Scratch plate{"p2.ep3d"};
    WritePart(bolt.path, "M6x20");
    WritePart(plate.path, "Plate");

    AssemblyDocument assembly{"Rig"};
    assembly.addInstance("P", plate.path, "Plate");
    assembly.addInstance("B1", bolt.path, "M6x20");
    assembly.addInstance("B2", bolt.path, "M6x20");

    const BomContents counted = CountAssembly(assembly, BomDepth::TopLevel);
    ASSERT_TRUE(counted.ok) << counted.why;
    ASSERT_EQ(counted.rows.size(), 2u);
    // Item numbers run 1, 2, ... in the order the parts first appear -- which
    // is what a balloon on the drawing points at.
    EXPECT_EQ(counted.rows[0].item, 1);
    EXPECT_EQ(counted.rows[0].partName, "Plate");
    EXPECT_EQ(counted.rows[0].quantity, 1);
    EXPECT_EQ(counted.rows[1].item, 2);
    EXPECT_EQ(counted.rows[1].quantity, 2);
}

TEST(BomTableTest, M35_BOM_003_ASubAssemblyIsONELineAtTopLevelAndItsPARTSWhenExploded) {
    // Two different lists, and a reader has to be able to tell which they are
    // holding -- which is why the depth is two named answers and not a number.
    Scratch bolt{"b3.ep3d"};
    Scratch plate{"p3.ep3d"};
    Scratch sub{"sub3.ep3da"};
    WritePart(bolt.path, "M6x20");
    WritePart(plate.path, "Plate");

    AssemblyDocument inner{"Sub"};
    inner.addInstance("P", plate.path, "Plate");
    inner.addInstance("B1", bolt.path, "M6x20");
    inner.addInstance("B2", bolt.path, "M6x20");
    WriteAssembly(sub.path, inner);

    AssemblyDocument outer{"Rig"};
    outer.addInstance("Sub", sub.path, "");
    outer.addInstance("B3", bolt.path, "M6x20");

    const BomContents top = CountAssembly(outer, BomDepth::TopLevel);
    ASSERT_TRUE(top.ok) << top.why;
    EXPECT_EQ(top.rows.size(), 2u) << "a sub-assembly was opened at top level";
    EXPECT_EQ(top.totalQuantity(), 2);

    const BomContents exploded = CountAssembly(outer, BomDepth::Exploded);
    ASSERT_TRUE(exploded.ok) << exploded.why;
    // The plate, and THREE bolts -- two inside the sub-assembly and one
    // outside, added together because they are the same part.
    ASSERT_EQ(exploded.rows.size(), 2u);
    int bolts = 0;
    for (const BomRow& row : exploded.rows)
        if (row.partName == "M6x20") bolts = row.quantity;
    EXPECT_EQ(bolts, 3) << "the bolts inside and outside the sub-assembly were not added up";
    EXPECT_EQ(exploded.totalQuantity(), 4);
}

TEST(BomTableTest, M35_BOM_004_AFileThatCannotBeOPENEDStopsAnEXPLODEDCount) {
    // A COUNT NOBODY CAN MAKE, said out loud.
    //
    // IsAssemblySourceFile answers false both for a real part and for a file
    // it could not read, and the first draft treated the second as the first:
    // a sub-assembly whose file had moved was counted as a single line and
    // everything inside it vanished from the list. A list with parts missing
    // is one somebody orders from.
    //
    // At TOP LEVEL the same file is fine -- one line for one thing the
    // assembly contains -- which is checked at the end.
    Scratch bolt{"b4.ep3d"};
    WritePart(bolt.path, "M6x20");
    AssemblyDocument assembly{"Rig"};
    assembly.addInstance("Missing", "no-such-assembly.ep3da", "");
    assembly.addInstance("B", bolt.path, "M6x20");

    const BomContents counted = CountAssembly(assembly, BomDepth::Exploded);
    EXPECT_FALSE(counted.ok);
    EXPECT_FALSE(counted.why.empty());
    EXPECT_TRUE(counted.rows.empty())
        << "a half-counted list was handed back as if it were complete";

    // ...and a TOP LEVEL list of the same assembly still counts, because what
    // it says is what the assembly says: two things, one of each.
    const BomContents top = CountAssembly(assembly, BomDepth::TopLevel);
    ASSERT_TRUE(top.ok) << top.why;
    EXPECT_EQ(top.rows.size(), 2u);
}

// =============================================================================
// On a drawing
// =============================================================================

TEST(BomTableTest, M35_BOM_010_TheListFOLLOWSTheAssembly) {
    // THE WHOLE POINT. A list that stored its quantities would still say four
    // after two more bolts went in, and it would look completely right.
    Scratch bolt{"b10.ep3d"};
    Scratch rig{"r10.ep3da"};
    WritePart(bolt.path, "M6x20");

    AssemblyDocument assembly{"Rig"};
    for (int i = 0; i < 4; ++i)
        assembly.addInstance("B" + std::to_string(i), bolt.path, "M6x20");
    WriteAssembly(rig.path, assembly);

    DrawingDocument drawing{"Sheet"};
    const BomTable& table = drawing.addBomTable("Parts", rig.path, Vec2{200.0, 40.0});
    ASSERT_EQ(drawing.countBom(table).totalQuantity(), 4);

    // Two more bolts go in, and the file is saved over.
    assembly.addInstance("B4", bolt.path, "M6x20");
    assembly.addInstance("B5", bolt.path, "M6x20");
    WriteAssembly(rig.path, assembly);

    EXPECT_EQ(drawing.countBom(table).totalQuantity(), 6)
        << "the parts list is still stating the quantity the assembly used to have";
}

TEST(BomTableTest, M35_BOM_011_AListWhoseFileHasCHANGEDSaysSo) {
    // The same question a view answers, through the same CONTENT hash: two
    // saves inside one filesystem tick are indistinguishable by modification
    // time, and a list that quietly stopped noticing changes is the failure
    // this design exists to prevent.
    Scratch bolt{"b11.ep3d"};
    Scratch rig{"r11.ep3da"};
    WritePart(bolt.path, "M6x20");
    AssemblyDocument assembly{"Rig"};
    assembly.addInstance("B", bolt.path, "M6x20");
    WriteAssembly(rig.path, assembly);

    DrawingDocument drawing{"Sheet"};
    const BomTable& table = drawing.addBomTable("Parts", rig.path, Vec2{200.0, 40.0});
    EXPECT_TRUE(drawing.staleBomTables().empty()) << "a freshly made list is already stale";

    assembly.addInstance("B2", bolt.path, "M6x20");
    WriteAssembly(rig.path, assembly);
    ASSERT_EQ(drawing.staleBomTables().size(), 1u)
        << "the assembly changed and the parts list did not notice";
    EXPECT_EQ(drawing.staleBomTables().front(), table.id());

    ASSERT_TRUE(drawing.markBomCounted(table.id()));
    EXPECT_TRUE(drawing.staleBomTables().empty());
}

TEST(BomTableTest, M35_BOM_012_AListNamingNoAssemblyIsREFUSED) {
    DrawingDocument drawing{"Sheet"};
    EXPECT_THROW(drawing.addBomTable("Parts", "", Vec2{0.0, 0.0}), std::invalid_argument);
    EXPECT_THROW(drawing.addBomTable("", "rig.ep3da", Vec2{0.0, 0.0}), std::invalid_argument);
}

TEST(BomTableTest, M35_BOM_013_TheWidthFOLLOWSTheColumnsAndNoColumnAppearsTwice) {
    BomTable table{"Parts", "rig.ep3da", Vec2{0.0, 0.0}};
    const double before = table.widthMm();
    ASSERT_TRUE(table.setColumns({BomColumn::Item, BomColumn::Quantity}));
    EXPECT_LT(table.widthMm(), before) << "dropping two columns did not narrow the table";
    EXPECT_NEAR(table.widthMm(),
                table.columnWidthMm(BomColumn::Item) + table.columnWidthMm(BomColumn::Quantity),
                1e-9);

    // A list with no columns is a rectangle; one with a column twice has two
    // headings a reader cannot tell apart.
    EXPECT_FALSE(table.setColumns({}));
    EXPECT_FALSE(table.setColumns({BomColumn::Item, BomColumn::Item}));
    EXPECT_EQ(table.columns().size(), 2u) << "a refused column set was applied anyway";
}

TEST(BomTableTest, M35_BOM_014_EveryListEditComesBack) {
    Scratch rig{"r14.ep3da"};
    AssemblyDocument assembly{"Rig"};
    WriteAssembly(rig.path, assembly);

    DrawingDocument drawing{"Sheet"};
    const ObjectId id = drawing.addBomTable("Parts", rig.path, Vec2{200.0, 40.0}).id();
    ASSERT_TRUE(drawing.setBomPosition(id, Vec2{150.0, 60.0}));
    ASSERT_TRUE(drawing.setBomDepth(id, BomDepth::Exploded));
    ASSERT_TRUE(drawing.setBomColumns(id, {BomColumn::Item, BomColumn::Quantity}));

    ASSERT_TRUE(drawing.undo()); // the columns
    EXPECT_EQ(drawing.findBomTable(id)->columns().size(), 4u);
    ASSERT_TRUE(drawing.undo()); // the depth
    EXPECT_EQ(drawing.findBomTable(id)->depth(), BomDepth::TopLevel);
    ASSERT_TRUE(drawing.undo()); // the move
    EXPECT_NEAR(drawing.findBomTable(id)->positionMm().x, 200.0, 1e-9);
    ASSERT_TRUE(drawing.undo()); // the list itself
    EXPECT_EQ(drawing.findBomTable(id), nullptr);

    while (drawing.canRedo()) ASSERT_TRUE(drawing.redo());
    ASSERT_NE(drawing.findBomTable(id), nullptr);
    EXPECT_EQ(drawing.findBomTable(id)->depth(), BomDepth::Exploded);
    EXPECT_EQ(drawing.findBomTable(id)->columns().size(), 2u);
}

TEST(BomTableTest, M35_BOM_015_AListSurvivesASaveAndAReopenAndTheROWSAreNotInTheFile) {
    Scratch bolt{"b15.ep3d"};
    Scratch rig{"r15.ep3da"};
    WritePart(bolt.path, "M6x20");
    AssemblyDocument assembly{"Rig"};
    for (int i = 0; i < 7; ++i)
        assembly.addInstance("B" + std::to_string(i), bolt.path, "M6x20");
    WriteAssembly(rig.path, assembly);

    DrawingDocument drawing{"Sheet"};
    const ObjectId id = drawing.addBomTable("Parts", rig.path, Vec2{200.0, 40.0}).id();
    ASSERT_TRUE(drawing.setBomDepth(id, BomDepth::Exploded));
    ASSERT_EQ(drawing.countBom(*drawing.findBomTable(id)).totalQuantity(), 7);

    const std::string saved = SaveDrawing(drawing);
    // THE COUNT IS NOT IN THE FILE. One that carried it would come back
    // holding a bill of materials the assembly no longer has.
    EXPECT_EQ(saved.find("\"quantity\""), std::string::npos);
    EXPECT_EQ(saved.find("M6x20"), std::string::npos)
        << "the counted rows were written into the drawing";

    std::istringstream in(saved);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const BomTable* back = loaded.document->findBomTable(id);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->depth(), BomDepth::Exploded);
    EXPECT_EQ(back->sourcePath(), rig.path);
    // ...and it still COUNTS, which a restored-but-unhooked list would fail.
    EXPECT_EQ(loaded.document->countBom(*back).totalQuantity(), 7);
    EXPECT_EQ(SaveDrawing(*loaded.document), saved);
    EXPECT_EQ(loaded.document->undoDepth(), 0u);
}

TEST(BomTableTest, M35_BOM_016_AColumnThisBuildDoesNotKnowIsREFUSED) {
    // Defaulting it to the item number would turn a quantity column into a row
    // number, and that is a list somebody orders from.
    Scratch rig{"r16.ep3da"};
    AssemblyDocument assembly{"Rig"};
    WriteAssembly(rig.path, assembly);
    DrawingDocument drawing{"Sheet"};
    drawing.addBomTable("Parts", rig.path, Vec2{200.0, 40.0});

    std::string text = SaveDrawing(drawing);
    const std::size_t at = text.find("\"Quantity\"");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"Quantity\"").size(), "\"Weight\"");

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::InvalidEnumValue);
}

TEST(BomTableTest, M35_BOM_017_DeletingAListTakesOnlyItself) {
    Scratch rig{"r17.ep3da"};
    AssemblyDocument assembly{"Rig"};
    WriteAssembly(rig.path, assembly);

    DrawingDocument drawing{"Sheet"};
    const ObjectId id = drawing.addBomTable("Parts", rig.path, Vec2{200.0, 40.0}).id();
    drawing.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{10.0, 0.0}});
    ASSERT_TRUE(drawing.removeObject(id));
    EXPECT_EQ(drawing.findBomTable(id), nullptr);
    EXPECT_EQ(drawing.entities().size(), 1u) << "deleting a parts list took geometry with it";

    ASSERT_TRUE(drawing.undo());
    EXPECT_NE(drawing.findBomTable(id), nullptr) << "undoing the delete did not bring it back";
}

TEST(BomTableTest, M35_BOM_018_TheHEADINGIsAtTheEndTheListGrowsFROM) {
    // Row 0 is the heading, and which END of the table that is depends on the
    // direction. Growing upward it is the BOTTOM row, hard against the title
    // block it sits on, so it stays in one place as parts are added; growing
    // downward it is the top.
    //
    // The painter worked this out inline and got the BORDER edge wrong, so an
    // upward table -- the default, and the one in every screenshot -- lost the
    // rule between its heading and its first part. Found by looking at it,
    // which does not scale; hence this.
    BomTable table{"Parts", "rig.ep3da", Vec2{100.0, 50.0}};
    ASSERT_TRUE(table.setRowHeightMm(10.0));
    ASSERT_TRUE(table.growsUpward());

    // Upward: the heading's bottom is the table's own position, and rows
    // stack above it.
    EXPECT_NEAR(table.rowBottomMm(0), 50.0, 1e-9);
    EXPECT_NEAR(table.rowBottomMm(1), 60.0, 1e-9);
    EXPECT_NEAR(table.rowBottomMm(2), 70.0, 1e-9);
    // ...and the heading's lower edge IS the border, so it is not ruled twice.
    EXPECT_TRUE(table.rowBottomIsBorder(0, 3));
    EXPECT_FALSE(table.rowBottomIsBorder(1, 3))
        << "the rule between the heading and the first part was skipped";
    EXPECT_FALSE(table.rowBottomIsBorder(2, 3));

    // Downward: the heading is the top row, and the LAST row's lower edge is
    // the border instead.
    table.setGrowsUpward(false);
    EXPECT_NEAR(table.rowBottomMm(0), 40.0, 1e-9);
    EXPECT_NEAR(table.rowBottomMm(2), 20.0, 1e-9);
    EXPECT_FALSE(table.rowBottomIsBorder(0, 3));
    EXPECT_TRUE(table.rowBottomIsBorder(2, 3));

    // Every row is exactly one row height from the next, either way round --
    // no gaps, no overlaps.
    for (std::size_t i = 1; i < 4; ++i)
        EXPECT_NEAR(std::fabs(table.rowBottomMm(i) - table.rowBottomMm(i - 1)), 10.0, 1e-9);
}

TEST(BomTableTest, M35_BOM_019_AnAssemblyThatCONTAINSITSELFIsRefusedRatherThanHanging) {
    // A parts list that HANGS is worse than one that says it cannot be
    // counted: the program stops responding and nobody can tell why.
    //
    // Building a cycle is refused while an assembly is open, but a pair of
    // FILES can still describe one -- which is what this arranges, and what a
    // file edited outside this program, or a folder moved on top of another,
    // would produce.
    Scratch bolt{"b19.ep3d"};
    Scratch outer{"a19.ep3da"};
    Scratch inner{"b19.ep3da"};
    WritePart(bolt.path, "M6x20");

    // A holds a bolt, and nothing else yet -- so B can be built naming it.
    {
        AssemblyDocument first{"A"};
        first.addInstance("B", bolt.path, "M6x20");
        WriteAssembly(outer.path, first);
    }
    {
        AssemblyDocument second{"B"};
        second.addInstance("A", outer.path, "");
        WriteAssembly(inner.path, second);
    }
    // ...and now A is written again, this time holding B. The cycle exists
    // only on disk, which is exactly the case the in-memory guard cannot see.
    {
        AssemblyDocument first{"A"};
        first.addInstance("Bolt", bolt.path, "M6x20");
        first.addInstance("Sub", inner.path, "");
        WriteAssembly(outer.path, first);
    }

    std::ifstream file(outer.path, std::ios::binary);
    ASSERT_TRUE(file.good());
    const AssemblyLoadResult loaded = loadAssemblyDocument(file);
    ASSERT_TRUE(loaded) << loaded.message;

    // If this recurses, the test HANGS rather than fails -- which is itself
    // the report, because a hang is what the guard exists to prevent.
    const BomContents counted = CountAssembly(*loaded.document, BomDepth::Exploded);
    EXPECT_FALSE(counted.ok) << "a cycle was counted to a finite answer";
    EXPECT_NE(counted.why.find("contains itself"), std::string::npos) << counted.why;
    EXPECT_TRUE(counted.rows.empty());

    // ...and a TOP LEVEL list of the same file is fine, because it never opens
    // the sub-assembly at all.
    EXPECT_TRUE(CountAssembly(*loaded.document, BomDepth::TopLevel).ok);
}

TEST(BomTableTest, M35_BOM_020_SavingIsREFUSEDWhenAListNamesNoAssembly) {
    // ADR-M3-008: the save checks exactly what the load checks. addBomTable
    // refuses an empty path, but restoreBomTable is the RAW path the loader
    // uses and checks nothing -- which is the state a bad reader or a future
    // migration would leave behind, and the one the check exists for.
    DrawingDocument drawing{"Sheet"};
    drawing.restoreBomTable(920001u, "Parts", "", Vec2{100.0, 50.0}, BomDepth::TopLevel,
                            {BomColumn::Item, BomColumn::Quantity}, 8.0, true, 0);

    std::ostringstream out;
    const SaveResult saved = saveDrawingDocument(drawing, out);
    EXPECT_FALSE(saved) << "a parts list with nothing to count saved cleanly";
    EXPECT_EQ(saved.error, SerializationError::MissingField);
}

TEST(BomTableTest, M35_BOM_021_ABrokenColumnSetIsREFUSEDWhereItCanActuallyARRIVE) {
    // A first draft checked this at SAVE, and the check was dead: the
    // constructor seeds four columns, setColumns refuses an empty or repeating
    // set, and restoreBomTable routes through setColumns -- so no in-memory
    // table can be in either state.
    //
    // A HAND-EDITED FILE can, which is where the rule belongs and where a
    // reader actually meets it.
    Scratch rig{"r21.ep3da"};
    AssemblyDocument assembly{"Rig"};
    WriteAssembly(rig.path, assembly);
    DrawingDocument drawing{"Sheet"};
    drawing.addBomTable("Parts", rig.path, Vec2{200.0, 40.0});
    const std::string good = SaveDrawing(drawing);

    // ...with no columns at all.
    {
        const std::size_t at = good.find("\"columns\": [");
        ASSERT_NE(at, std::string::npos) << good;
        const std::size_t close = good.find(']', at);
        ASSERT_NE(close, std::string::npos);
        std::string text = good;
        text.replace(at, close - at + 1, "\"columns\": []");
        std::istringstream in(text);
        const DrawingLoadResult loaded = loadDrawingDocument(in);
        EXPECT_FALSE(loaded) << "a parts list with no columns loaded cleanly";
        EXPECT_EQ(loaded.error, SerializationError::MissingField);
    }

    // ...and with the same column twice, which would put two headings a reader
    // cannot tell apart on the paper.
    {
        const std::size_t at = good.find("\"columns\": [");
        const std::size_t close = good.find(']', at);
        std::string text = good;
        text.replace(at, close - at + 1, "\"columns\": [\"Item\", \"Item\"]");
        std::istringstream in(text);
        const DrawingLoadResult loaded = loadDrawingDocument(in);
        EXPECT_FALSE(loaded) << "a parts list showing one column twice loaded cleanly";
        EXPECT_EQ(loaded.error, SerializationError::DuplicateId);
    }
}
