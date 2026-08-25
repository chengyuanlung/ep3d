#pragma once

#include "Core/Drawing/Geometry2D.h"

#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

// HATCHING (M38.2).
//
// The parallel lines that say "this is where the knife went". Without them a
// section view is a drawing of the inside of a part with no way to tell the
// cut faces from the ones behind them -- and a reader who cannot see where the
// material is cannot read the section at all.
//
// Nothing in this project hatched anything before. The projected curves and
// the drawn geometry can both be flattened to polylines (FlattenShape,
// ProjectedPolyline), so what a hatch needs is one operation over closed
// loops, and that is all this is.

// A REGION TO FILL: an outer loop and the holes in it.
//
// Every loop is CLOSED and given as its points -- the first is not repeated at
// the end. Holes are not marked as holes: which loops are holes falls out of
// the even-odd rule below, which is what makes a hole inside a hole inside a
// hole work without anybody having to say so.
struct HatchRegion {
    std::vector<std::vector<Vec2>> loops;

    void add(std::vector<Vec2> loop) { loops.push_back(std::move(loop)); }
    bool empty() const noexcept;
    Box2D bounds() const;
};

// HOW THE LINES RUN.
//
// The 45-degree spacing ISO 128 asks for on a general section. Both are in
// SHEET millimetres, because a hatch is annotation: it is drawn at the same
// pitch whatever the view's scale, so a 1:10 view is not filled solid.
struct HatchStyle {
    double angleRad = 0.7853981633974483; // 45 degrees
    double spacingMm = 3.0;
    // Where the pattern starts, so two adjacent parts can be hatched out of
    // step with each other -- which is how a reader tells one part from the
    // next in an assembly section.
    double offsetMm = 0.0;

    bool usable() const noexcept { return spacingMm > 0.0; }
};

// WHAT CAME OUT: the line segments, in the same millimetres the region was in.
struct HatchLines {
    bool ok = false;
    std::string why;
    std::vector<std::pair<Vec2, Vec2>> segments;
};

// FILLS `region`.
//
// THE EVEN-ODD RULE, and the half-open edge test that makes it work.
//
// A scanline crossing the boundary an odd number of times is inside. The trap
// is a vertex that lands exactly ON a scanline: counted from both of its edges
// it flips the parity twice and the fill comes out inverted from there on --
// which looks like the hatch leaking out of the part. Treating each edge as
// spanning [ymin, ymax) counts such a vertex once, and that one decision is
// most of what makes this correct.
HatchLines HatchTheRegion(const HatchRegion& region, const HatchStyle& style);

} // namespace paramcad
