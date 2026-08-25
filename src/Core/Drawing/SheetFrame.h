#pragma once

#include "Core/Drawing/Sheet.h"
#include "Core/Geometry/MathTypes.h"

#include <string>
#include <vector>

namespace paramcad {

// THE FRAME AND ITS ZONES (M35, ISO 5457).
//
// DERIVED, NEVER STORED. A frame is the sheet's own edge said out loud: two
// rectangles and a ring of zone labels, all of them a function of the paper's
// width, height and binding side. Nothing here is a decision a user makes
// twice.
//
// This is ADR-M10-002 again -- composed, never stored -- and it matters for a
// concrete reason: a sheet resized from A3 to A2 must not leave an A3 frame
// drawn on it. If the frame were entities on the paper, the resize would have
// to find and rewrite them, and the day it missed one the drawing would carry
// a border that measures something the paper is not.
//
// It is also why there is no `Frame` object with an id: there is nothing to
// select, nothing to delete, and nothing that can disagree with the sheet.

// The margins ISO 5457 asks for. The binding edge is wider because that is
// the edge a drawing is punched and filed on, and a narrow one loses the
// border to the holes.
struct FrameMargins {
    double bindingMm = 20.0; // the left edge, portrait or landscape
    double otherMm = 10.0;

    static FrameMargins standard() noexcept { return FrameMargins{}; }
    // A frame is only a frame while it fits: on a sheet narrower than twice
    // the margin there is no inside left, and drawing one would put the border
    // through itself.
    bool fitsOn(double widthMm, double heightMm) const noexcept;
};

// A zone is a labelled band along an edge, so a reader can say "the slot in
// B4" over a telephone. Rows are LETTERS from the bottom up and columns are
// NUMBERS from the left -- ISO 5457's order, which is not the order a
// spreadsheet uses, and getting it the other way round makes every reference
// on the drawing point somewhere else.
struct SheetZone {
    std::string label;  // "A", "B", ... or "1", "2", ...
    double fromMm = 0.0; // along the edge, from the frame's corner
    double toMm = 0.0;
    bool isRow = false;  // a letter up the side, rather than a number along
};

// WHAT THE FRAME IS, on this paper. One call, so the canvas, a PDF plot and a
// DXF write cannot draw three different borders.
struct SheetFrameGeometry {
    bool ok = false;
    std::string why;          // why there is no frame, when there is not
    Vec2 innerMinMm{};        // the border rectangle -- what the drawing lives in
    Vec2 innerMaxMm{};
    // The zone strip runs BETWEEN the paper edge and the border. Its width is
    // the smaller margin, so the strip is the same thickness all the way
    // round even though the binding edge is wider.
    double zoneStripMm = 0.0;
    std::vector<SheetZone> zones;

    double innerWidthMm() const noexcept { return innerMaxMm.x - innerMinMm.x; }
    double innerHeightMm() const noexcept { return innerMaxMm.y - innerMinMm.y; }
};

// `zoneTargetMm` is the size a zone would like to be; the real ones divide the
// edge evenly and land near it. ISO 5457 uses about 148 mm on A0 down to
// leaving A4 unzoned, but the rule that matters is that zones are WHOLE
// divisions of the edge -- a half zone in the corner is a reference nobody can
// use.
SheetFrameGeometry FrameOf(const Sheet& sheet, const FrameMargins& margins,
                           double zoneTargetMm = 100.0) noexcept;

// Which zone a point on the sheet is in ("B4"), or empty when it is outside
// the frame. What a balloon or a revision note prints.
std::string ZoneAt(const SheetFrameGeometry& frame, Vec2 sheetMm);

} // namespace paramcad
