#pragma once

#include "Core/Drawing/Sheet.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

// THE TITLE BLOCK (M35, ISO 7200).
//
// The box in the bottom-right corner that says what the drawing IS: its title,
// its number, who drew it, at what scale, in which projection convention, on
// what size paper.
//
// THE DECISION THIS FILE EXISTS TO MAKE: a title block is not drawn geometry.
//
// If it were text entities on the paper, then changing the drawing number
// would mean finding and editing an entity; resizing the sheet would leave the
// block in the old corner; and -- the one that actually hurts -- the scale
// printed in the block and the scale the views are drawn at would be two
// separate facts, kept by hand, each true on its own. That is this project's
// recurring defect exactly, and on a title block it means the drawing states a
// scale nothing on it was plotted at.
//
// So: the block is DOCUMENT STATE with a DERIVED APPEARANCE, and the fields
// that describe the sheet are not typed at all. They are read from the sheet
// every time the block is drawn.

// WHICH FIELDS THE SHEET ANSWERS FOR ITSELF.
//
// A user cannot type these, and there is no code path that lets them: the
// value is fetched at draw time. `Free` is everything else -- title, number,
// drawn by, approved by, material, and whatever rows a company adds.
enum class TitleBlockSource {
    Free,             // typed, and the document remembers it
    SheetScale,       // "1:2"
    SheetSize,        // "A3"
    ProjectionSymbol, // "First angle" / "Third angle"
    SheetCount,       // "1 / 3" -- how many sheets this drawing is
    // v48 (M48). THE LATEST ISSUE, read from the drawing's revision history.
    //
    // Typed in, this is the field that goes stale: somebody adds Rev C to the
    // table and the block in the corner still says B. Both are neat, both are
    // complete, and the shop builds to whichever they read first. So it is not
    // typable -- setField refuses it like every other derived source.
    LatestRevision,
};

std::string_view toString(TitleBlockSource source) noexcept;
bool ParseTitleBlockSource(std::string_view text, TitleBlockSource& into) noexcept;

struct TitleBlockField {
    std::string label;  // "Title", "Drawn by", "Scale"
    std::string value;  // meaningful only when source == Free
    TitleBlockSource source = TitleBlockSource::Free;

    bool isDerived() const noexcept { return source != TitleBlockSource::Free; }
};

// The fields ISO 7200 calls mandatory, in the order it lists them. Seeded into
// every drawing, so a new sheet already has somewhere to put its number rather
// than requiring the user to build a title block before they can name the
// thing they are drawing.
const char* const kTitleBlockTitleLabel = "Title";
const char* const kTitleBlockNumberLabel = "Drawing No.";
// M48. The row that says what the drawing is issued at -- derived, so it can
// only ever say what the revision table says.
const char* const kTitleBlockRevisionLabel = "Rev";

class TitleBlock {
public:
    TitleBlock();

    // THE SIZE OF THE BOX, in paper millimetres. ISO 7200 fixes the width at
    // 180 mm so that a drawing folded to A4 shows its title block; the height
    // follows from how many rows there are.
    double widthMm() const noexcept { return widthMm_; }
    bool setWidthMm(double widthMm) noexcept;
    double rowHeightMm() const noexcept { return rowHeightMm_; }
    bool setRowHeightMm(double rowHeightMm) noexcept;
    double heightMm() const noexcept;

    bool isVisible() const noexcept { return visible_; }
    void setVisible(bool visible) noexcept { visible_ = visible; }

    const std::vector<TitleBlockField>& fields() const noexcept { return fields_; }
    // Adds, or replaces the value of an existing label. Returns false for an
    // empty label, and for any attempt to type into a DERIVED field -- which
    // is where the "the block says 1:2 and the views are 1:5" failure would
    // otherwise get in.
    bool setField(const std::string& label, std::string value);
    bool addField(std::string label, TitleBlockSource source);
    bool removeField(const std::string& label);
    const TitleBlockField* findField(const std::string& label) const noexcept;

    // THE RAW PATH, for the loader and for undo.
    //
    // It replaces the field list wholesale and checks nothing -- the same
    // shape every other restore in this project takes, and for the same
    // reason: rebuilding through the guarded API cannot express "these exact
    // rows", because the guards protect Title and Drawing No. from being
    // removed and would leave them behind when the recorded state has them
    // somewhere else. Undoing a removal has to be able to put the list back
    // exactly, not approximately.
    void restoreFields(std::vector<TitleBlockField> fields) { fields_ = std::move(fields); }
    void restoreSize(double widthMm, double rowHeightMm) noexcept {
        widthMm_ = widthMm;
        rowHeightMm_ = rowHeightMm;
    }

    // WHERE ROW `index` SITS, given the block's bottom-left corner.
    //
    // THE FIRST FIELD IS THE TOP ROW. The block is measured from its bottom
    // and paper Y runs up, so the index has to be counted down -- and the
    // painter did it inline and got it backwards, which put Title at the
    // bottom of the block and Sheet at the top, exactly reversing the order
    // ISO 7200 lists them in. Found by looking at a screenshot, which is not a
    // thing that scales; so the arithmetic lives here now, where a test can
    // reach it.
    //
    // Returns the row's BOTTOM edge in sheet millimetres.
    double rowBottomMm(std::size_t index, double blockBottomMm) const noexcept;

    // WHAT A FIELD ACTUALLY PRINTS, on this sheet. THE one reader: the canvas,
    // a PDF plot and a DXF write all ask here, so none of them can print a
    // different scale from the one the drawing is at.
    // NO DEFAULTS (M48). They used to be sheetNumber = 1, sheetTotal = 1, and
    // a caller that forgot printed "1 / 1" on a three-page drawing -- which is
    // the half-truth M44 went and fixed everywhere else. A revision default
    // would be worse: an empty string reads as "no issue yet" on a drawing
    // that is at Rev C.
    //
    // So every fact the block derives has to be handed in, and the compiler
    // finds the callers rather than a reader finding the wrong number. In a
    // running program there is exactly one caller: DrawingDocument::
    // titleBlockValue, which is the only thing that knows all three.
    std::string valueOf(const TitleBlockField& field, const Sheet& sheet, int sheetNumber,
                        int sheetTotal, const std::string& latestRevision) const;

private:
    std::vector<TitleBlockField> fields_;
    double widthMm_ = 180.0;
    double rowHeightMm_ = 8.0;
    bool visible_ = true;
};

} // namespace paramcad
