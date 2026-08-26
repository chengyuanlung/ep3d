#include "Core/Drawing/TitleBlock.h"

#include <algorithm>
#include <utility>

namespace paramcad {

std::string_view toString(TitleBlockSource source) noexcept {
    switch (source) {
        case TitleBlockSource::Free: return "Free";
        case TitleBlockSource::SheetScale: return "SheetScale";
        case TitleBlockSource::SheetSize: return "SheetSize";
        case TitleBlockSource::ProjectionSymbol: return "ProjectionSymbol";
        case TitleBlockSource::SheetCount: return "SheetCount";
        case TitleBlockSource::LatestRevision: return "LatestRevision";
    }
    return "Free";
}

bool ParseTitleBlockSource(std::string_view text, TitleBlockSource& into) noexcept {
    // NO DEFAULT CASE and no silent fallback to Free: a source this build does
    // not know, read from a newer file, would turn a derived field into a
    // typed one holding whatever string happened to be beside it -- which is
    // how a title block starts stating a scale nothing was plotted at.
    if (text == "Free") { into = TitleBlockSource::Free; return true; }
    if (text == "SheetScale") { into = TitleBlockSource::SheetScale; return true; }
    if (text == "SheetSize") { into = TitleBlockSource::SheetSize; return true; }
    if (text == "ProjectionSymbol") { into = TitleBlockSource::ProjectionSymbol; return true; }
    if (text == "SheetCount") { into = TitleBlockSource::SheetCount; return true; }
    if (text == "LatestRevision") { into = TitleBlockSource::LatestRevision; return true; }
    return false;
}

TitleBlock::TitleBlock() {
    // ISO 7200's mandatory set, in its order. Seeded, so a new drawing already
    // has somewhere to put its number -- a user who has to build a title block
    // before they can name the thing they are drawing builds it once and then
    // copies that file forever.
    fields_.push_back(TitleBlockField{kTitleBlockTitleLabel, {}, TitleBlockSource::Free});
    fields_.push_back(TitleBlockField{kTitleBlockNumberLabel, {}, TitleBlockSource::Free});
    fields_.push_back(TitleBlockField{"Drawn by", {}, TitleBlockSource::Free});
    fields_.push_back(TitleBlockField{"Approved by", {}, TitleBlockSource::Free});
    fields_.push_back(TitleBlockField{"Date", {}, TitleBlockSource::Free});
    fields_.push_back(TitleBlockField{"Material", {}, TitleBlockSource::Free});
    // ...and the four the SHEET answers for itself.
    fields_.push_back(TitleBlockField{"Scale", {}, TitleBlockSource::SheetScale});
    fields_.push_back(TitleBlockField{"Size", {}, TitleBlockSource::SheetSize});
    fields_.push_back(TitleBlockField{"Projection", {}, TitleBlockSource::ProjectionSymbol});
    fields_.push_back(TitleBlockField{"Sheet", {}, TitleBlockSource::SheetCount});
    // ...and the fifth, from the drawing's own history (M48). Seeded rather
    // than left to be added, because a drawing that HAS a revision history and
    // nowhere in the corner to print it is the half-answer: the table is on
    // the sheet somewhere and the block -- which is what a reader checks
    // first, and often all they check -- says nothing about the issue.
    //
    // It prints BLANK until the drawing has been issued, which is what every
    // drawing office leaves and is not the same as Rev A.
    fields_.push_back(TitleBlockField{kTitleBlockRevisionLabel, {},
                                      TitleBlockSource::LatestRevision});
}

bool TitleBlock::setWidthMm(double widthMm) noexcept {
    // A BLOCK WITH NO WIDTH IS A BLOCK NOBODY CAN READ, and it would be found
    // at plot time. The old value stays, which is a block that still works --
    // the same rule DimensionStyle follows for text height.
    if (!(widthMm > 0.0)) return false;
    widthMm_ = widthMm;
    return true;
}

bool TitleBlock::setRowHeightMm(double rowHeightMm) noexcept {
    if (!(rowHeightMm > 0.0)) return false;
    rowHeightMm_ = rowHeightMm;
    return true;
}

double TitleBlock::heightMm() const noexcept {
    // DERIVED FROM HOW MANY ROWS THERE ARE. Stored, it would be a second
    // answer that disagrees the moment somebody adds a field.
    return rowHeightMm_ * static_cast<double>(fields_.size());
}

const TitleBlockField* TitleBlock::findField(const std::string& label) const noexcept {
    for (const TitleBlockField& field : fields_)
        if (field.label == label) return &field;
    return nullptr;
}

bool TitleBlock::setField(const std::string& label, std::string value) {
    for (TitleBlockField& field : fields_) {
        if (field.label != label) continue;
        // TYPING INTO A DERIVED FIELD IS REFUSED, not ignored and not
        // accepted-then-overwritten. This is the whole point of the file: the
        // scale in the block and the scale the views are drawn at are ONE
        // fact, and the only way to keep them one is for there to be no way to
        // type the second.
        if (field.isDerived()) return false;
        field.value = std::move(value);
        return true;
    }
    return false;
}

bool TitleBlock::addField(std::string label, TitleBlockSource source) {
    if (label.empty()) return false;
    // A SECOND FIELD WITH THE SAME LABEL is two rows saying different things
    // under one name, and a reader has no way to tell which is meant.
    if (findField(label) != nullptr) return false;
    fields_.push_back(TitleBlockField{std::move(label), {}, source});
    return true;
}

bool TitleBlock::removeField(const std::string& label) {
    // THE TITLE AND THE DRAWING NUMBER CANNOT GO. ISO 7200 calls them
    // mandatory, and a drawing that cannot be identified is not a drawing --
    // it is a picture somebody will have to guess about.
    if (label == kTitleBlockTitleLabel || label == kTitleBlockNumberLabel) return false;
    const auto at = std::find_if(fields_.begin(), fields_.end(),
                                 [&label](const TitleBlockField& field) {
                                     return field.label == label;
                                 });
    if (at == fields_.end()) return false;
    fields_.erase(at);
    return true;
}

double TitleBlock::rowBottomMm(std::size_t index, double blockBottomMm) const noexcept {
    if (fields_.empty()) return blockBottomMm;
    // Clamped rather than wrapped: an index past the end is a caller's mistake,
    // and wrapping would put a row somewhere plausible-looking.
    const std::size_t at = index < fields_.size() ? index : fields_.size() - 1;
    return blockBottomMm + static_cast<double>(fields_.size() - 1 - at) * rowHeightMm_;
}

std::string TitleBlock::valueOf(const TitleBlockField& field, const Sheet& sheet,
                                int sheetNumber, int sheetTotal,
                                const std::string& latestRevision) const {
    switch (field.source) {
        case TitleBlockSource::Free: return field.value;
        case TitleBlockSource::SheetScale: return sheet.scale().toString();
        case TitleBlockSource::SheetSize:
            // A CUSTOM SHEET SAYS ITS SIZE, not the word "Custom". "Custom" in
            // a title block tells a reader holding the paper nothing they did
            // not already know, and loses the one number they might want.
            if (sheet.size() != SheetSize::Custom) return std::string(toString(sheet.size()));
            return std::to_string(static_cast<long long>(sheet.widthMm())) + " x " +
                   std::to_string(static_cast<long long>(sheet.heightMm()));
        case TitleBlockSource::ProjectionSymbol:
            return std::string(toString(sheet.projectionAngle())) + " angle";
        case TitleBlockSource::SheetCount:
            return std::to_string(sheetNumber) + " / " + std::to_string(sheetTotal);
        case TitleBlockSource::LatestRevision:
            // A DRAWING WITH NO HISTORY PRINTS NOTHING, not "A". An issue
            // letter on a drawing that has never been issued is a claim, and
            // the blank is what every drawing office actually leaves.
            return latestRevision;
    }
    return field.value;
}

} // namespace paramcad
