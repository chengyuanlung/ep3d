#pragma once

#include "Core/Drawing/BomTable.h"
#include "Core/Frame/FrameProfile.h"

#include <string>
#include <vector>

namespace paramcad {

class AssemblyDocument;

// M56.4 -- THE CUT LIST, WHICH IS THE PARTS LIST.
//
// This is the whole point of putting a member behind a source path.
//
// A cut list and a parts list count the same assembly and answer the same
// question -- what is in this, and how many. The only difference is which
// columns get printed: a fabricator wants the section, the length and the two
// saw settings; a buyer wants a part number and a quantity. Written as two
// counters they would be two answers, and the day somebody adds a member the
// two lists would disagree about a length of steel that has to be cut.
//
// So CutList calls CountAssembly. There is one counting function in this
// program, and this file is a way of READING its rows -- it groups nothing,
// counts nothing and opens nothing.
//
// WHAT MAKES THAT POSSIBLE is that a member's path already carries its
// identity: `frm:SHS 40x40x3 L=600 A=45 B=45` says the section, the length and
// both cuts. Two members that group into one BOM row are, by construction, two
// sticks a saw would set up for once.

// ONE LINE OF THE CUT LIST: this many of this section, this long, cut like
// this. Every field is read back out of the path the row was grouped on, so
// there is nothing here that could disagree with the model.
struct CutListRow {
    int item = 0;
    int quantity = 0;
    FrameMemberSpec spec;

    // What the saw reads. Both derived from the axis length and the angles --
    // see FrameMemberSpec, where the ambiguity a shop asks about ("is that the
    // long point?") is settled once.
    double axisLengthMm() const noexcept { return spec.lengthMm; }
    double longPointMm() const noexcept { return spec.longPointMm(); }
    double shortPointMm() const noexcept { return spec.shortPointMm(); }
    // For this row: all of them together.
    double massKg() const noexcept { return spec.massKg() * static_cast<double>(quantity); }
};

struct CutListContents {
    bool ok = false;
    std::string why;
    std::vector<CutListRow> rows;
    // WHAT WAS IN THE ASSEMBLY AND IS NOT STEEL. A frame is bolted together
    // and sits on castors, and those are real parts that belong on the PARTS
    // list and not on the saw's list. Counted here rather than dropped
    // silently, because "the cut list has eleven lines" and "the assembly has
    // fourteen parts" is a difference somebody will notice and have to explain.
    int otherParts = 0;

    double totalMassKg() const noexcept;
    // How much of each section to buy, in metres -- the number an order is
    // written from. Summed over rows, so a section used in four lengths is one
    // line here.
    struct StockLine {
        FrameProfile profile;
        double totalLengthMm = 0.0;
        double massKg = 0.0;
    };
    std::vector<StockLine> stock() const;

    explicit operator bool() const noexcept { return ok; }
};

// THE CUT LIST OF AN ASSEMBLY, counted through CountAssembly.
//
// `depth` is passed straight through, and both answers are meaningful: a frame
// built as one assembly is TopLevel, and a machine whose frame is a
// sub-assembly needs Exploded to see the steel inside it.
CutListContents CutListOf(const AssemblyDocument& assembly,
                          BomDepth depth = BomDepth::Exploded);

// The same reading applied to rows already counted, so a caller that has a BOM
// in hand does not count twice. This is the function; the one above is the
// convenience.
CutListContents CutListFromRows(const BomContents& counted);

} // namespace paramcad
