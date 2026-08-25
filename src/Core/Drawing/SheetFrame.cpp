#include "Core/Drawing/SheetFrame.h"

#include <algorithm>
#include <cmath>

namespace paramcad {

namespace {

// A zone letter. Past Z it doubles up -- AA, AB -- rather than wrapping back
// to A, because two zones called "A" on one sheet is a reference that points
// at two places.
std::string RowLabel(int index) {
    std::string label;
    ++index;
    while (index > 0) {
        const int remainder = (index - 1) % 26;
        label.insert(label.begin(), static_cast<char>('A' + remainder));
        index = (index - 1) / 26;
    }
    return label;
}

// How many whole divisions to cut an edge into. AT LEAST TWO, because a single
// zone spanning the whole edge tells a reader nothing they did not already
// know.
int DivisionsFor(double edgeMm, double targetMm) noexcept {
    if (!(edgeMm > 0.0) || !(targetMm > 0.0)) return 2;
    const int wanted = static_cast<int>(std::lround(edgeMm / targetMm));
    return std::max(2, wanted);
}

} // namespace

bool FrameMargins::fitsOn(double widthMm, double heightMm) const noexcept {
    if (!(bindingMm >= 0.0) || !(otherMm >= 0.0)) return false;
    // The binding margin eats one side; the other margin eats the remaining
    // three. What must be left over is a positive inside.
    return widthMm - bindingMm - otherMm > 0.0 && heightMm - 2.0 * otherMm > 0.0;
}

SheetFrameGeometry FrameOf(const Sheet& sheet, const FrameMargins& margins,
                           double zoneTargetMm) noexcept {
    SheetFrameGeometry frame;
    const double widthMm = sheet.widthMm();
    const double heightMm = sheet.heightMm();
    if (!(widthMm > 0.0) || !(heightMm > 0.0)) {
        frame.why = "this sheet has no area to frame";
        return frame;
    }
    if (!margins.fitsOn(widthMm, heightMm)) {
        // SAID, not silently shrunk. A frame quietly narrowed to fit would
        // print a border that measures something other than what was asked
        // for, and nobody would know which.
        frame.why = "the margins are wider than the paper, so there is no inside left";
        return frame;
    }

    // THE BINDING EDGE IS THE LEFT ONE, portrait or landscape. A sheet that
    // moved its binding edge when it was turned would file the wrong way up
    // half the time.
    frame.innerMinMm = Vec2{margins.bindingMm, margins.otherMm};
    frame.innerMaxMm = Vec2{widthMm - margins.otherMm, heightMm - margins.otherMm};
    frame.zoneStripMm = margins.otherMm;

    // ZONES DIVIDE THE BORDER EVENLY. Not "a zone every 100 mm with a stub at
    // the end" -- a half zone in the corner is a reference nobody can use, so
    // the target only decides HOW MANY, and they then share the edge exactly.
    const int columns = DivisionsFor(frame.innerWidthMm(), zoneTargetMm);
    const int rows = DivisionsFor(frame.innerHeightMm(), zoneTargetMm);
    const double columnMm = frame.innerWidthMm() / columns;
    const double rowMm = frame.innerHeightMm() / rows;

    frame.zones.reserve(static_cast<std::size_t>(columns + rows));
    // NUMBERS ALONG THE BOTTOM, LEFT TO RIGHT.
    for (int i = 0; i < columns; ++i) {
        SheetZone zone;
        zone.label = std::to_string(i + 1);
        zone.fromMm = i * columnMm;
        zone.toMm = (i + 1) * columnMm;
        zone.isRow = false;
        frame.zones.push_back(std::move(zone));
    }
    // LETTERS UP THE SIDE, BOTTOM TO TOP. ISO 5457's direction, which is the
    // opposite of a spreadsheet's -- and getting it upside down makes every
    // zone reference already on the drawing point somewhere else.
    for (int i = 0; i < rows; ++i) {
        SheetZone zone;
        zone.label = RowLabel(i);
        zone.fromMm = i * rowMm;
        zone.toMm = (i + 1) * rowMm;
        zone.isRow = true;
        frame.zones.push_back(std::move(zone));
    }

    frame.ok = true;
    return frame;
}

std::string ZoneAt(const SheetFrameGeometry& frame, Vec2 sheetMm) {
    if (!frame.ok) return {};
    const double alongX = sheetMm.x - frame.innerMinMm.x;
    const double alongY = sheetMm.y - frame.innerMinMm.y;
    // OUTSIDE THE FRAME IS NOT A ZONE. Clamping to the nearest one would hand
    // back a reference to a place the thing is not.
    if (alongX < 0.0 || alongY < 0.0 || alongX > frame.innerWidthMm() ||
        alongY > frame.innerHeightMm())
        return {};

    std::string row;
    std::string column;
    for (const SheetZone& zone : frame.zones) {
        const double along = zone.isRow ? alongY : alongX;
        // The upper edge is inclusive only on the LAST zone, so a point
        // exactly on a division belongs to one zone and not to two.
        const bool inside = along >= zone.fromMm && along < zone.toMm;
        const bool atTheVeryEnd =
            along >= zone.toMm &&
            std::fabs(zone.toMm - (zone.isRow ? frame.innerHeightMm() : frame.innerWidthMm())) <
                1e-9;
        if (!inside && !atTheVeryEnd) continue;
        (zone.isRow ? row : column) = zone.label;
    }
    if (row.empty() || column.empty()) return {};
    // LETTER THEN NUMBER -- "B4", the way it is said out loud.
    return row + column;
}

} // namespace paramcad
