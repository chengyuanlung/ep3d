#include "Core/Parameter/PartVariant.h"

namespace paramcad {

std::vector<ObjectId> VariantColumns(const std::vector<PartVariant>& variants) {
    std::vector<ObjectId> columns;
    if (variants.empty()) return columns;
    // FROM THE FIRST ROW, because the first row is what fixed them.
    //
    // AND THE LAST ROW WOULD DO AS WELL, which the mutation gate proved by
    // swapping them: every row of a table names the same columns, because
    // WhyVariantRefused will not let a row in that does not. Recorded rather
    // than defended -- claiming the choice matters when it does not is how a
    // comment stops being worth reading.
    //
    // What it must NOT be is the union of every row. That would let the table
    // grow a column quietly and leave every earlier row missing it, and there
    // is no rule stopping that one.
    columns.reserve(variants.front().values.size());
    for (const auto& entry : variants.front().values) columns.push_back(entry.first);
    return columns;
}

const PartVariant* FindVariant(const std::vector<PartVariant>& variants,
                               const std::string& name) {
    for (const PartVariant& variant : variants)
        if (variant.name == name) return &variant;
    return nullptr;
}

std::string WhyVariantRefused(const PartVariant& variant,
                              const std::vector<PartVariant>& existing) {
    if (variant.name.empty())
        return "a variant has to have a name -- it is what a drawing will ask for";
    if (variant.values.empty())
        return "a variant that sets nothing is the part it already was";
    // THE SAME NAME TWICE IS TWO SIZES CALLED THE SAME THING, and a drawing
    // asking for that name would get whichever was found first.
    for (const PartVariant& one : existing)
        if (one.name == variant.name)
            return "this part already has a variant called '" + variant.name + "'";

    if (!existing.empty()) {
        // EVERY ROW FILLS EVERY COLUMN. See the header: a row naming fewer is a
        // size with a dimension nobody stated.
        const std::vector<ObjectId> columns = VariantColumns(existing);
        if (variant.values.size() != columns.size())
            return "this part's variants set " + std::to_string(columns.size()) +
                   " parameter(s) and this one sets " + std::to_string(variant.values.size()) +
                   " -- every variant of a part states the same dimensions";
        for (const ObjectId column : columns)
            if (variant.values.count(column) == 0)
                return "this variant does not say what parameter " +
                       std::to_string(static_cast<unsigned long long>(column)) +
                       " is, and the other variants of this part do";
    }

    // TWO ROWS WITH THE SAME NUMBERS ARE ONE SIZE WITH TWO NAMES -- and then
    // "which variant is this" has two right answers, which is the one thing
    // the comparison below must never face.
    for (const PartVariant& one : existing)
        if (one.values == variant.values)
            return "this variant has exactly the same values as '" + one.name +
                   "', so nothing could tell them apart";
    return {};
}

std::string WhichVariantMatches(const std::vector<PartVariant>& variants,
                                const std::map<ObjectId, double>& current) {
    for (const PartVariant& variant : variants) {
        bool all = true;
        for (const auto& entry : variant.values) {
            const auto found = current.find(entry.first);
            // EXACTLY. A variant is a stated size; "close enough to B" is how a
            // part gets made to a number nobody chose. And the values being
            // compared came from the same table they were written into, so
            // there is no arithmetic between them to lose a bit to.
            if (found == current.end() || found->second != entry.second) {
                all = false;
                break;
            }
        }
        if (all) return variant.name;
    }
    // NOBODY'S IS A REAL ANSWER. A part that has been nudged off a variant is
    // not still that variant, and saying so is the whole point of not
    // remembering one.
    return {};
}

} // namespace paramcad
