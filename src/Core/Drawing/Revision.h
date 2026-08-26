#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Geometry/MathTypes.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

// M48 -- THE REVISION TABLE.
//
// WHY THIS ONE IS STORED, WHEN EVERYTHING ELSE HERE IS DERIVED.
//
// This project derives: a balloon's number (M42), a datum's letter (M41), a
// section's letter (M38), a sheet's count (M44). The rule has been that two
// copies of one fact drift apart, so keep one. A revision letter looks like
// exactly that shape and IS NOT, and getting it backwards would be the defect
// this milestone introduced rather than closed.
//
// A balloon's number NAMES A ROW THAT EXISTS NOW. Renumber the list and the
// balloon has to follow, because it is a pointer at a live thing.
//
// A revision letter is a HISTORICAL FACT THAT HAS ALREADY LEFT THE BUILDING.
// Purchase orders cite Rev C. Inspection reports cite Rev C. There are parts
// in a stores bin with Rev C on the label. Nothing anybody does to this file
// can change what Rev C meant, and a letter derived from row position would
// rewrite all of that the first time somebody deleted a row -- silently, and
// in a direction no test would think to look.
//
// So the letter is STORED. What is DERIVED is the two things that actually
// drift: the NEXT letter to offer, and the revision the title block prints.
//
// THE FAILURE THIS FILE IS FOR: the block in the corner says Rev B while the
// last row of the table says Rev C. Both are complete, both are neat, and the
// shop builds to whichever one they read first.

// ONE ENTRY IN THE DRAWING'S HISTORY.
struct Revision {
    // Stored, for the reason above. Uppercase, one or more letters.
    std::string letter;
    // WHAT CHANGED. Refused when empty: a row that says nothing changed is a
    // row that makes the reader look for a change that is not described, and
    // the usual conclusion is that they missed it.
    std::string description;
    // Free text on purpose. A date format is a company's, not a standard's,
    // and a drawing that reformatted the dates its QA department wrote is a
    // drawing that lost an audit trail.
    std::string date;
    std::string by;
};

// THE LETTERS A DRAWING MAY USE, and the three it may not.
//
// I, O and Q are SKIPPED (ASME Y14.35, and every shop that has ever misread
// one): on a photocopy of a photocopy they are 1, 0 and O. A formula that
// walked the alphabet would produce all three; a list cannot, which is why
// this is a list.
//
// After Z it doubles: AA, AB, ... -- and the doubles skip the same three.
std::string_view RevisionLetters() noexcept;
// The letter that follows `previous`, or the first one when it is empty.
std::string NextRevisionLetter(std::string_view previous);
// Whether this is a letter a drawing may carry at all.
bool IsRevisionLetter(std::string_view text) noexcept;

// WHY THIS REVISION CANNOT BE ADDED, or empty when it can. `existing` is the
// drawing's history as it stands.
std::string WhyRevisionRefused(const Revision& revision,
                               const std::vector<Revision>& existing);

// --- THE TABLE ON THE PAPER -------------------------------------------------
//
// The same shape the parts list and the hole table have: an object with a
// place, a width and a page, whose ROWS ARE NOT IN IT. They are the document's
// history, asked for at every repaint -- so a table cannot show an issue the
// drawing does not have, and cannot miss one it does.
class RevisionTable {
public:
    RevisionTable(std::string name, Vec2 positionMm, ObjectId layerId);
    RevisionTable(ObjectId id, std::string name, Vec2 positionMm, double widthMm,
                  double rowHeightMm, ObjectId layerId);

    ObjectId id() const noexcept { return id_; }
    static std::string_view typeName() noexcept { return "RevisionTable"; }

    const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    Vec2 positionMm() const noexcept { return positionMm_; }
    void setPositionMm(Vec2 at) noexcept { positionMm_ = at; }

    double widthMm() const noexcept { return widthMm_; }
    bool setWidthMm(double widthMm) noexcept;
    double rowHeightMm() const noexcept { return rowHeightMm_; }
    bool setRowHeightMm(double rowHeightMm) noexcept;

    // WHERE ROW `index` SITS, counted from the table's bottom-left corner --
    // the same arithmetic the title block needed a home for, and for the same
    // reason: done inline by a painter it came out upside down.
    //
    // A REVISION TABLE GROWS UPWARDS. The heading is at the BOTTOM and the
    // newest issue is at the top, which is the opposite of every other table
    // on the sheet and is what a drawing office expects. A table that grew
    // downwards would put the newest revision where a reader looks for the
    // oldest.
    //
    // No row COUNT is taken, unlike the parts list and the hole table. They
    // need it to count down from the top; this one counts up from the corner,
    // and a parameter kept for symmetry with them would be a number nothing
    // reads -- which is how a caller comes to pass the wrong one.
    double rowBottomMm(std::size_t index) const noexcept;

    ObjectId layerId() const noexcept { return layerId_; }
    void setLayerId(ObjectId layerId) noexcept { layerId_ = layerId; }

    ObjectId sheetId() const noexcept { return sheetId_; }
    void setSheetId(ObjectId sheetId) noexcept { sheetId_ = sheetId; }

private:
    ObjectId id_;
    std::string name_;
    Vec2 positionMm_{};
    double widthMm_ = 120.0;
    double rowHeightMm_ = 6.0;
    ObjectId sheetId_ = kInvalidObjectId;
    ObjectId layerId_ = kInvalidObjectId;
};

// The columns, in the order ISO 7200 lists them. ONE list, so the heading and
// the cells cannot disagree about which column is which -- the defect that
// would put a date under "By".
enum class RevisionColumn { Letter, Description, Date, By };
std::string_view toString(RevisionColumn column) noexcept;
const std::vector<RevisionColumn>& RevisionColumns();
// What this row prints in that column.
std::string CellOf(const Revision& revision, RevisionColumn column);

} // namespace paramcad
