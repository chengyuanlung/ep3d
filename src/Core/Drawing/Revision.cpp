#include "Core/Drawing/Revision.h"

#include "Core/Document/ObjectId.h"

namespace paramcad {

namespace {

// THE LIST, not the alphabet. I, O and Q are absent because on paper they are
// 1, 0 and O -- and a drawing that says Rev O is a drawing somebody will read
// as Rev 0 and then look for a revision zero that does not exist.
constexpr std::string_view kLetters = "ABCDEFGHJKLMNPRSTUVWXYZ";

bool IsUsableLetter(char c) noexcept {
    return kLetters.find(c) != std::string_view::npos;
}

} // namespace

std::string_view RevisionLetters() noexcept { return kLetters; }

bool IsRevisionLetter(std::string_view text) noexcept {
    if (text.empty()) return false;
    for (const char c : text)
        if (!IsUsableLetter(c)) return false;
    return true;
}

std::string NextRevisionLetter(std::string_view previous) {
    if (previous.empty()) return std::string(1, kLetters.front());
    // WALKED FROM THE SAME LIST IT IS CHECKED AGAINST, so the letter this
    // offers is always one IsRevisionLetter accepts. Two lists here would be
    // the classic pair: one that produces Q and one that refuses it.
    std::string out(previous);
    for (std::size_t i = out.size(); i-- > 0;) {
        const std::size_t at = kLetters.find(out[i]);
        if (at == std::string_view::npos) return {};
        if (at + 1 < kLetters.size()) {
            out[i] = kLetters[at + 1];
            return out;
        }
        // Rolled off the end of the list: this place goes back to the first
        // letter and the one before it carries.
        out[i] = kLetters.front();
    }
    // Every place rolled over, so the history is one letter longer than it
    // was: Z becomes AA, ZZ becomes AAA.
    return std::string(1, kLetters.front()) + out;
}

std::string WhyRevisionRefused(const Revision& revision,
                               const std::vector<Revision>& existing) {
    if (revision.letter.empty())
        return "a revision has to have a letter -- it is what everything outside this file "
               "will cite";
    if (!IsRevisionLetter(revision.letter))
        return "'" + revision.letter +
               "' is not a revision letter: I, O and Q are left out because on paper they "
               "read as 1, 0 and O";
    // A DESCRIPTION IS NOT OPTIONAL. A row that says nothing changed sends the
    // reader hunting for a change nobody wrote down, and the usual conclusion
    // is that they have missed it.
    if (revision.description.empty())
        return "a revision has to say what changed";
    // THE SAME LETTER TWICE IS TWO DIFFERENT DRAWINGS CALLED Rev C, and the
    // parts already made to one of them cannot be told from the other.
    for (const Revision& one : existing)
        if (one.letter == revision.letter)
            return "this drawing already has a Rev " + revision.letter;
    return {};
}

// --- the table --------------------------------------------------------------

RevisionTable::RevisionTable(std::string name, Vec2 positionMm, ObjectId layerId)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), positionMm_(positionMm),
      layerId_(layerId) {}

RevisionTable::RevisionTable(ObjectId id, std::string name, Vec2 positionMm, double widthMm,
                             double rowHeightMm, ObjectId layerId)
    : id_(RestoreObjectId(id)), name_(std::move(name)), positionMm_(positionMm),
      widthMm_(widthMm),
      rowHeightMm_(rowHeightMm), layerId_(layerId) {}

bool RevisionTable::setWidthMm(double widthMm) noexcept {
    if (!(widthMm > 0.0)) return false;
    widthMm_ = widthMm;
    return true;
}

bool RevisionTable::setRowHeightMm(double rowHeightMm) noexcept {
    if (!(rowHeightMm > 0.0)) return false;
    rowHeightMm_ = rowHeightMm;
    return true;
}

double RevisionTable::rowBottomMm(std::size_t index) const noexcept {
    // UPWARDS, heading at the bottom. Index 0 is the heading and sits on the
    // table's own corner; the rows stack above it, newest at the top. Written
    // here rather than in the painter because the title block taught this
    // exact lesson the hard way (see TitleBlock::rowBottomMm).
    return static_cast<double>(index) * rowHeightMm_;
}

std::string_view toString(RevisionColumn column) noexcept {
    switch (column) {
        case RevisionColumn::Letter: return "REV";
        case RevisionColumn::Description: return "DESCRIPTION";
        case RevisionColumn::Date: return "DATE";
        case RevisionColumn::By: return "BY";
    }
    return "REV";
}

const std::vector<RevisionColumn>& RevisionColumns() {
    // ONE list. The heading row and the cells both walk it, so a column cannot
    // be headed DATE and filled with a name.
    static const std::vector<RevisionColumn> kColumns{
        RevisionColumn::Letter, RevisionColumn::Description, RevisionColumn::Date,
        RevisionColumn::By};
    return kColumns;
}

std::string CellOf(const Revision& revision, RevisionColumn column) {
    switch (column) {
        case RevisionColumn::Letter: return revision.letter;
        case RevisionColumn::Description: return revision.description;
        case RevisionColumn::Date: return revision.date;
        case RevisionColumn::By: return revision.by;
    }
    return {};
}

} // namespace paramcad
