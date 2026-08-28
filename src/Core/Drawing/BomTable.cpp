#include "Core/Drawing/BomTable.h"

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Document/SourceShapeResolver.h"
#include "Core/Library/LibraryPart.h"
#include "Core/Serialization/AssemblyDocumentSerializer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <utility>

namespace paramcad {

std::string_view toString(BomColumn column) noexcept {
    switch (column) {
        case BomColumn::Item: return "Item";
        case BomColumn::Quantity: return "Quantity";
        case BomColumn::PartName: return "PartName";
        case BomColumn::SourceFile: return "SourceFile";
        case BomColumn::Description: return "Description";
    }
    return "Item";
}

bool ParseBomColumn(std::string_view text, BomColumn& into) noexcept {
    // REFUSED, not defaulted. A column this build does not know, read from a
    // newer file, would silently become the Item number -- and a parts list
    // whose quantity column turned into a row number is one somebody orders
    // from.
    if (text == "Item") { into = BomColumn::Item; return true; }
    if (text == "Quantity") { into = BomColumn::Quantity; return true; }
    if (text == "PartName") { into = BomColumn::PartName; return true; }
    if (text == "SourceFile") { into = BomColumn::SourceFile; return true; }
    if (text == "Description") { into = BomColumn::Description; return true; }
    return false;
}

std::string_view HeadingOf(BomColumn column) noexcept {
    // WHAT THE PAPER SAYS, which is not what the enum is called. "ITEM" and
    // "QTY" are what a reader expects on a parts list; "PartName" is a
    // programmer's word.
    switch (column) {
        case BomColumn::Item: return "ITEM";
        case BomColumn::Quantity: return "QTY";
        case BomColumn::PartName: return "PART";
        case BomColumn::SourceFile: return "FILE";
        case BomColumn::Description: return "DESCRIPTION";
    }
    return "ITEM";
}

std::string_view toString(BomDepth depth) noexcept {
    switch (depth) {
        case BomDepth::TopLevel: return "TopLevel";
        case BomDepth::Exploded: return "Exploded";
    }
    return "TopLevel";
}

bool ParseBomDepth(std::string_view text, BomDepth& into) noexcept {
    if (text == "TopLevel") { into = BomDepth::TopLevel; return true; }
    if (text == "Exploded") { into = BomDepth::Exploded; return true; }
    return false;
}

std::string BomRow::cell(BomColumn column) const {
    switch (column) {
        case BomColumn::Item: return std::to_string(item);
        case BomColumn::Quantity: return std::to_string(quantity);
        case BomColumn::PartName: return partName;
        // THE FILENAME, derived. A drawing has no room for D:/work/2026/... and
        // a reader does not want it; what the row is ABOUT is the whole path.
        case BomColumn::SourceFile:
            return std::filesystem::path{sourcePath}.filename().string();
        case BomColumn::Description: return description;
    }
    return {};
}

int BomContents::totalQuantity() const noexcept {
    int total = 0;
    for (const BomRow& row : rows) total += row.quantity;
    return total;
}

namespace {

// WHAT MAKES TWO INSTANCES THE SAME PART: the file and the body, not the name.
//
// A user who named them Bolt1..Bolt8 still ordered eight of one thing, and a
// list that gave each its own line is one somebody orders eight lines from.
std::string PartKeyOf(const Instance& instance) {
    return instance.sourcePath() + "\n" + instance.bodyName();
}

std::string PartNameOf(const Instance& instance) {
    // The BODY's name when there is one, because that is what the supplier is
    // being asked for. An instance name is what this assembly calls its copy.
    if (!instance.bodyName().empty()) return instance.bodyName();
    const std::filesystem::path path{instance.sourcePath()};
    return path.stem().string();
}

void CountInto(const AssemblyDocument& assembly, BomDepth depth,
               std::vector<BomRow>& rows, std::vector<std::string>& keys,
               std::vector<std::string>& visiting, int quantityEach, BomContents& out);

// Adds `instance` to the list, or bumps the row it already has.
void Tally(const Instance& instance, int quantityEach, std::vector<BomRow>& rows,
           std::vector<std::string>& keys) {
    const std::string key = PartKeyOf(instance);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] != key) continue;
        rows[i].quantity += quantityEach;
        return;
    }
    BomRow row;
    row.item = static_cast<int>(rows.size()) + 1;
    row.quantity = quantityEach;
    row.partName = PartNameOf(instance);
    row.sourcePath = instance.sourcePath();
    rows.push_back(std::move(row));
    keys.push_back(key);
}

void CountInto(const AssemblyDocument& assembly, BomDepth depth,
               std::vector<BomRow>& rows, std::vector<std::string>& keys,
               std::vector<std::string>& visiting, int quantityEach, BomContents& out) {
    for (const Instance* instance : assembly.instances()) {
        if (depth == BomDepth::TopLevel) {
            // AT TOP LEVEL, WHAT THE ASSEMBLY SAYS IS THE ANSWER. One line per
            // thing it contains, sub-assembly or part, whether or not that
            // file can be opened right now -- the count is right either way,
            // and a missing file is the ASSEMBLY's problem to report, where
            // the message a reader can act on already lives.
            Tally(*instance, quantityEach, rows, keys);
            continue;
        }

        // EXPLODED IS DIFFERENT: a sub-assembly's contents have to be opened
        // and added in, so a file that cannot be examined is not "one part" --
        // it is A COUNT NOBODY CAN MAKE.
        //
        // IsAssemblySourceFile answers false both for a real part and for a
        // file it could not read, and the first draft treated the second as
        // the first: a sub-assembly whose file had moved was silently counted
        // as a single line, and the parts inside it vanished from the list. A
        // list with parts missing is one somebody orders from.
        // A LIBRARY PART IS NOT A FILE and never was one -- it is built from
        // its own path (M45's catalogue, M56's frame members). Counted as the
        // single part it is, without being opened.
        //
        // FOUND BY M56, and it had been wrong since M45: this walk opened
        // every source path to see what was inside it, and `std:ISO 4762 M8x30`
        // is not something ifstream can open. An exploded parts list of an
        // assembly containing one catalogue screw did not come out short -- it
        // REFUSED, whole, with a message about a file that had never existed.
        // Frame members would have made it the common case.
        if (IsLibraryPath(instance->sourcePath())) {
            Tally(*instance, quantityEach, rows, keys);
            continue;
        }

        std::ifstream probe(instance->sourcePath(), std::ios::binary);
        if (!probe) {
            out.why = "could not open " +
                      std::filesystem::path{instance->sourcePath()}.filename().string() +
                      ", so what is inside it cannot be counted";
            return;
        }
        probe.close();
        if (!IsAssemblySourceFile(instance->sourcePath())) {
            Tally(*instance, quantityEach, rows, keys);
            continue;
        }

        // ...and now it is known to be a readable sub-assembly.
        //
        // A CYCLE WOULD RECURSE FOR EVER. An assembly that contains itself is
        // refused when it is built, but a file edited outside this program can
        // still describe one -- and a parts list that hangs is worse than one
        // that says it cannot be counted.
        for (const std::string& already : visiting) {
            if (already != instance->sourcePath()) continue;
            out.why = "this assembly contains itself, through " +
                      std::filesystem::path{instance->sourcePath()}.filename().string();
            return;
        }
        std::ifstream file(instance->sourcePath(), std::ios::binary);
        const AssemblyLoadResult loaded = loadAssemblyDocument(file);
        if (!loaded) {
            out.why = std::filesystem::path{instance->sourcePath()}.filename().string() +
                      " could not be read: " + loaded.message;
            return;
        }
        visiting.push_back(instance->sourcePath());
        CountInto(*loaded.document, depth, rows, keys, visiting, quantityEach, out);
        visiting.pop_back();
        if (!out.why.empty()) return;
    }
}

} // namespace

BomContents CountAssembly(const AssemblyDocument& assembly, BomDepth depth) {
    BomContents out;
    std::vector<std::string> keys;
    std::vector<std::string> visiting;
    CountInto(assembly, depth, out.rows, keys, visiting, 1, out);
    if (!out.why.empty()) {
        out.rows.clear();
        return out;
    }
    out.ok = true;
    return out;
}

BomTable::BomTable(std::string name, std::string sourcePath, Vec2 positionMm)
    : id_(ObjectIdGenerator::Next()),
      name_(std::move(name)),
      sourcePath_(std::move(sourcePath)),
      positionMm_(positionMm),
      columns_{BomColumn::Item, BomColumn::Quantity, BomColumn::PartName,
               BomColumn::Description} {}

BomTable::BomTable(ObjectId id, std::string name, std::string sourcePath, Vec2 positionMm)
    : id_(RestoreObjectId(id)),
      name_(std::move(name)),
      sourcePath_(std::move(sourcePath)),
      positionMm_(positionMm),
      columns_{BomColumn::Item, BomColumn::Quantity, BomColumn::PartName,
               BomColumn::Description} {}

bool BomTable::setColumns(std::vector<BomColumn> columns) {
    // A PARTS LIST WITH NO COLUMNS IS A RECTANGLE, and one with the same
    // column twice has two headings a reader cannot tell apart.
    if (columns.empty()) return false;
    for (std::size_t i = 0; i < columns.size(); ++i)
        for (std::size_t j = i + 1; j < columns.size(); ++j)
            if (columns[i] == columns[j]) return false;
    columns_ = std::move(columns);
    return true;
}

bool BomTable::setRowHeightMm(double rowHeightMm) noexcept {
    if (!(rowHeightMm > 0.0)) return false;
    rowHeightMm_ = rowHeightMm;
    return true;
}

double BomTable::columnWidthMm(BomColumn column) const noexcept {
    for (const auto& set : columnWidths_)
        if (set.first == column) return set.second;
    // DEFAULTS THAT SUIT WHAT THE COLUMN HOLDS. An item number needs a
    // finger's width and a description needs a sentence, and one width for
    // both wastes the paper on one and truncates the other.
    switch (column) {
        case BomColumn::Item: return 12.0;
        case BomColumn::Quantity: return 14.0;
        case BomColumn::PartName: return 45.0;
        case BomColumn::SourceFile: return 45.0;
        case BomColumn::Description: return 70.0;
    }
    return 30.0;
}

bool BomTable::setColumnWidthMm(BomColumn column, double widthMm) {
    if (!(widthMm > 0.0)) return false;
    for (auto& set : columnWidths_) {
        if (set.first != column) continue;
        set.second = widthMm;
        return true;
    }
    columnWidths_.emplace_back(column, widthMm);
    return true;
}

double BomTable::rowBottomMm(std::size_t index) const noexcept {
    const double step = static_cast<double>(index);
    return growsUpward_ ? positionMm_.y + step * rowHeightMm_
                        : positionMm_.y - (step + 1.0) * rowHeightMm_;
}

bool BomTable::rowBottomIsBorder(std::size_t index, std::size_t rowCount) const noexcept {
    if (rowCount == 0) return true;
    return growsUpward_ ? index == 0 : index + 1 == rowCount;
}

double BomTable::widthMm() const noexcept {
    // DERIVED from the columns it shows. Stored, it would disagree the moment
    // somebody dropped a column.
    double total = 0.0;
    for (const BomColumn column : columns_) total += columnWidthMm(column);
    return total;
}

} // namespace paramcad
