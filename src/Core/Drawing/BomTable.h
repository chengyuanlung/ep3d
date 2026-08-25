#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Geometry/MathTypes.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

class AssemblyDocument;

// THE PARTS LIST (M35.6).
//
// A BOM is a VIEW OF AN ASSEMBLY, in exactly the way a projected view is a view
// of a body: it names a file and reads it (ADR-M22-003, store a sentence not
// geometry), it goes stale when that file changes, and its ROWS ARE DERIVED --
// asked for, never stored.
//
// That last part is the whole design. A parts list that kept its own copy of
// the quantities is a drawing stating a bill of materials the assembly no
// longer has: somebody adds two bolts, the drawing still says four, and the
// wrong number is the one that gets ordered. It is the same failure the title
// block's Scale row exists to rule out, on the field that costs money.
//
// So what is stored is: which file, which columns, and how deep to go. The
// numbers are counted on demand.

enum class BomColumn {
    Item,        // 1, 2, 3 -- the balloon number
    Quantity,
    PartName,    // the body, or the sub-assembly
    SourceFile,  // where it came from
    Description, // free text a user types per part number
};

std::string_view toString(BomColumn column) noexcept;
bool ParseBomColumn(std::string_view text, BomColumn& into) noexcept;
// What the column's heading says on the paper.
std::string_view HeadingOf(BomColumn column) noexcept;

// HOW DEEP.
//
// Two real answers, not a number: a parts list is either what this assembly is
// made of, or every part in it however deep. "Three levels down" is a question
// nobody asks about a drawing, and offering it would mean every reader has to
// check which one they are holding.
enum class BomDepth {
    TopLevel, // sub-assemblies counted as one line each
    Exploded, // every part, wherever it sits
};

std::string_view toString(BomDepth depth) noexcept;
bool ParseBomDepth(std::string_view text, BomDepth& into) noexcept;

// ONE LINE OF THE LIST, as counted right now. Never stored.
struct BomRow {
    int item = 0;
    int quantity = 0;
    std::string partName;
    // THE FULL PATH, which is what the row was GROUPED BY -- and the filename
    // is derived from it for the column.
    //
    // It kept only the filename, and that threw away the identity it had just
    // been grouped on: two parts of the same name in different folders are
    // correctly two rows, and nothing downstream could tell which was which.
    // A balloon needs exactly that (M42), and so would anything else that has
    // to point back at a row.
    std::string sourcePath;
    std::string description;

    // The cell this row shows under `column`.
    std::string cell(BomColumn column) const;
};

struct BomContents {
    bool ok = false;
    std::string why;
    std::vector<BomRow> rows;

    int totalQuantity() const noexcept;
};

// COUNTING AN ASSEMBLY, in one place.
//
// Identical parts are ONE ROW with a quantity, which is what a parts list is
// for -- and "identical" means the same source file and body, not the same
// instance name, because a user who named them Bolt1..Bolt8 still ordered
// eight of one thing.
BomContents CountAssembly(const AssemblyDocument& assembly, BomDepth depth);

// The table on the sheet: where it sits, what it shows, and which file it
// reads. It has an ObjectId because a user selects it, moves it and deletes
// it -- unlike a frame, which is the paper's own edge.
class BomTable {
public:
    BomTable(std::string name, std::string sourcePath, Vec2 positionMm);
    BomTable(ObjectId id, std::string name, std::string sourcePath, Vec2 positionMm);

    ObjectId id() const noexcept { return id_; }
    static std::string_view typeName() noexcept { return "BomTable"; }

    const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }
    const std::string& sourcePath() const noexcept { return sourcePath_; }
    void setSourcePath(std::string path) { sourcePath_ = std::move(path); }

    Vec2 positionMm() const noexcept { return positionMm_; }
    void setPositionMm(Vec2 at) noexcept { positionMm_ = at; }

    BomDepth depth() const noexcept { return depth_; }
    void setDepth(BomDepth depth) noexcept { depth_ = depth; }

    const std::vector<BomColumn>& columns() const noexcept { return columns_; }
    // A LIST, so a user can drop the ones their template does not use and put
    // the rest in their own order. Refused when it would leave no columns at
    // all -- a parts list with no columns is a rectangle.
    bool setColumns(std::vector<BomColumn> columns);

    // PAPER MILLIMETRES, like a dimension style's lengths.
    double rowHeightMm() const noexcept { return rowHeightMm_; }
    bool setRowHeightMm(double rowHeightMm) noexcept;
    double columnWidthMm(BomColumn column) const noexcept;
    bool setColumnWidthMm(BomColumn column, double widthMm);
    double widthMm() const noexcept;

    // WHETHER THE ROWS COUNT UP OR DOWN from the table's position. A list that
    // grows upward keeps its heading in one place as parts are added, which is
    // what a title-block-anchored list needs; one that grows down is what a
    // list at the top of a sheet wants.
    bool growsUpward() const noexcept { return growsUpward_; }
    void setGrowsUpward(bool upward) noexcept { growsUpward_ = upward; }

    // WHERE ROW `index` SITS, given how many rows there are in total.
    //
    // Row 0 IS THE HEADING. Which end of the table that is depends on the
    // direction: growing upward it is the bottom row, hard against whatever
    // the list sits on, and parts push up the sheet; growing downward it is
    // the top.
    //
    // Returns the row's bottom edge in sheet millimetres, measured from this
    // table's position. It lives here rather than in the painter for the
    // reason TitleBlock::rowBottomMm does: the painter worked it out inline
    // and got the border edge wrong, so an upward table -- the default, and
    // the one in every screenshot -- lost the rule between its heading and its
    // first part.
    double rowBottomMm(std::size_t index) const noexcept;
    // Whether row `index`'s bottom edge IS the table's own border, and so must
    // not be ruled a second time.
    bool rowBottomIsBorder(std::size_t index, std::size_t rowCount) const noexcept;

    // The stamp of the file when this table was last counted, so staleness is
    // the same question a view answers (M32.4 -- the CONTENT is hashed, not
    // the modification time).
    long long sourceStamp() const noexcept { return sourceStamp_; }
    void setSourceStamp(long long stamp) noexcept { sourceStamp_ = stamp; }

private:
    ObjectId id_;
    std::string name_;
    std::string sourcePath_;
    Vec2 positionMm_{};
    BomDepth depth_ = BomDepth::TopLevel;
    std::vector<BomColumn> columns_;
    std::vector<std::pair<BomColumn, double>> columnWidths_;
    double rowHeightMm_ = 8.0;
    bool growsUpward_ = true;
    long long sourceStamp_ = 0;
};

} // namespace paramcad
