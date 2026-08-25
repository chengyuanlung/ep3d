#pragma once

#include "Core/Drawing/Geometry2D.h"
#include "Core/Drawing/DrawingView.h"

#include "Core/Document/ObjectId.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace paramcad {

// M39.4 -- THE HOLE TABLE: every hole in a part, tagged, placed and called out.
//
// What it replaces is a drawing covered in leaders. Twenty holes each with its
// own callout is a view nobody can read; a table beside it, with a tag in each
// hole, is what a shop actually works from.
//
// EVERY COLUMN IS DERIVED (ADR-M10-002). The positions come from the sketch
// points the holes are drilled at, the sizes from the hole features, and the
// callouts from the same SizeAHole the cut is made with -- so a table cannot
// describe a part that is no longer there. What the DRAWING stores is one
// sentence: which view the table is for and where its datum is.
//
// THE DATUM IS THE DRAWING'S, NOT THE MODEL'S. A machinist works from a corner
// or an edge of the part, and "37.5 from the origin the modeller happened to
// sketch on" is a number that means nothing at a machine. So the datum is
// stated on the sheet and every position is measured from it.

struct HoleTableRow {
    std::string tag;      // "A1", "A2", "B1" -- letter by size, number by order
    Vec2 atMm{0.0, 0.0};  // in MODEL millimetres, measured from the datum
    std::string callout;  // the same sentence the hole is cut from
    double diameterMm = 0.0;
};

struct HoleTableContents {
    bool ok = false;
    std::string why; // set on refusal, and only then
    std::vector<HoleTableRow> rows;

    explicit operator bool() const noexcept { return ok; }
};

// Reads `partPath`, finds every hole in it, and places each one on the page of
// a view looking in `direction`.
//
// A part with no holes gives an EMPTY TABLE AND ok -- that is a true answer,
// not a failure. A file that cannot be read is a refusal: a hole table that
// silently came back empty would say "this part has no holes", which is the
// one wrong answer that looks like a right one.
HoleTableContents HolesOfPart(const std::string& partPath, ViewDirection direction, Vec2 datumMm);

// WHICH COLUMN SAYS WHAT.
//
// A hole table is read across, one row per hole, and the columns are the four
// things a machinist needs: which hole this tag is, where it is, and what to
// make it. Offered as a list so a shop's own template can drop or reorder
// them, refused empty for the reason a parts list is: a table with no columns
// is a rectangle.
enum class HoleColumn { Tag, X, Y, Description };
std::string_view toString(HoleColumn column) noexcept;
bool ParseHoleColumn(std::string_view text, HoleColumn& into) noexcept;
std::string_view HeadingOf(HoleColumn column) noexcept;
std::string CellOf(const HoleTableRow& row, HoleColumn column);

// THE TABLE ON THE SHEET.
//
// It BELONGS TO A VIEW rather than naming a file of its own, and that is the
// whole point of the design: a table that carried its own source path could
// name a different part from the view its tags are drawn on, and every row in
// it would still be a correct row about some part. Asked of the view, the two
// cannot disagree.
//
// The DATUM is in the view's own millimetres -- the corner a machinist works
// from -- and every position in the table is measured from it. Stored, because
// it is a decision; the positions themselves are derived.
class HoleTable {
public:
    HoleTable(std::string name, ObjectId viewId, Vec2 positionMm);
    HoleTable(ObjectId id, std::string name, ObjectId viewId, Vec2 positionMm);

    ObjectId id() const noexcept { return id_; }
    static std::string_view typeName() noexcept { return "HoleTable"; }

    const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    ObjectId viewId() const noexcept { return viewId_; }
    void setViewId(ObjectId viewId) noexcept { viewId_ = viewId; }

    Vec2 positionMm() const noexcept { return positionMm_; }
    void setPositionMm(Vec2 at) noexcept { positionMm_ = at; }

    Vec2 datumMm() const noexcept { return datumMm_; }
    void setDatumMm(Vec2 at) noexcept { datumMm_ = at; }

    // WHICH PAGE THIS SITS ON (M44). kInvalidObjectId means the drawing's
    // first page, which is what every object made before there was more than
    // one page belongs to.
    ObjectId sheetId() const noexcept { return sheetId_; }
    void setSheetId(ObjectId sheetId) noexcept { sheetId_ = sheetId; }


    const std::vector<HoleColumn>& columns() const noexcept { return columns_; }
    bool setColumns(std::vector<HoleColumn> columns);

    double rowHeightMm() const noexcept { return rowHeightMm_; }
    bool setRowHeightMm(double heightMm) noexcept;
    double columnWidthMm(HoleColumn column) const noexcept;
    bool setColumnWidthMm(HoleColumn column, double widthMm);
    double widthMm() const noexcept;

    // WHERE ROW `index` SITS, given how many rows there are. Row 0 is the
    // HEADING. Here rather than in the painter for the reason TitleBlock's and
    // BomTable's are: worked out inline in the renderer, the rule between the
    // heading and the first row came out on the wrong edge.
    double rowBottomMm(std::size_t index, std::size_t rowCount) const noexcept;

private:
    ObjectId id_;
    std::string name_;
    ObjectId viewId_ = kInvalidObjectId;
    Vec2 positionMm_{0.0, 0.0};
    // M44. Set when the object is added, from whichever page was current.
    ObjectId sheetId_ = kInvalidObjectId;
    Vec2 datumMm_{0.0, 0.0};
    std::vector<HoleColumn> columns_{HoleColumn::Tag, HoleColumn::X, HoleColumn::Y,
                                     HoleColumn::Description};
    double rowHeightMm_ = 7.0;
    std::vector<std::pair<HoleColumn, double>> columnWidths_;
};

} // namespace paramcad
