#include "Core/Frame/CutList.h"

#include "Core/Assembly/AssemblyDocument.h"

#include <cmath>
#include <optional>

namespace paramcad {

double CutListContents::totalMassKg() const noexcept {
    double total = 0.0;
    for (const CutListRow& row : rows) total += row.massKg();
    return total;
}

std::vector<CutListContents::StockLine> CutListContents::stock() const {
    std::vector<StockLine> lines;
    for (const CutListRow& row : rows) {
        StockLine* found = nullptr;
        for (StockLine& line : lines)
            if (line.profile.designation() == row.spec.profile.designation()) found = &line;
        if (found == nullptr) {
            lines.push_back(StockLine{row.spec.profile, 0.0, 0.0});
            found = &lines.back();
        }
        // THE AXIS LENGTH, NOT THE LONG POINT. What is bought is what is used
        // up, and a mitred member consumes exactly its axis length of stock --
        // the wedge that sticks out past one joint is the wedge missing from
        // the other. It is the same fact that makes a mitre cost no mass, and
        // it holds for the same reason.
        found->totalLengthMm += row.axisLengthMm() * static_cast<double>(row.quantity);
        found->massKg += row.massKg();
    }
    return lines;
}

CutListContents CutListFromRows(const BomContents& counted) {
    CutListContents out;
    if (!counted.ok) {
        out.why = counted.why;
        return out;
    }
    for (const BomRow& row : counted.rows) {
        const std::optional<FrameMemberSpec> spec = FrameMemberOfPath(row.sourcePath);
        if (!spec) {
            out.otherParts += row.quantity;
            continue;
        }
        CutListRow made;
        // NUMBERED WITHIN THE CUT LIST, not carried over from the parts list.
        // The two are read side by side on a shop floor and a cut list whose
        // items ran 2, 5, 6, 9 -- the frame's rows out of the assembly's -- is
        // one somebody has to check against the other list to use at all.
        made.item = static_cast<int>(out.rows.size()) + 1;
        made.quantity = row.quantity;
        made.spec = *spec;
        out.rows.push_back(std::move(made));
    }
    out.ok = true;
    return out;
}

CutListContents CutListOf(const AssemblyDocument& assembly, BomDepth depth) {
    return CutListFromRows(CountAssembly(assembly, depth));
}

} // namespace paramcad
