#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Drawing/Geometry2D.h"
#include "Core/Electrical/SymbolLibrary.h"

#include <string>
#include <vector>

namespace paramcad {

// CONNECTIVITY (M36.3).
//
// THE NETS ARE DERIVED. Always, from the geometry, every time they are asked
// for -- the same rule projected curves, BOM rows and dimension measurements
// follow, and for the strongest version of the same reason: a stored netlist
// is a schematic stating a circuit the drawing no longer shows. Somebody moves
// a wire, the picture changes, the stored list does not, and what gets built
// is the list.
//
// WHAT COUNTS AS A CONNECTION, and what does not:
//
//   * A wire's END touching another wire -- at its end, or anywhere along it --
//     IS a connection. That is a T junction, and on a schematic it is drawn
//     exactly like that.
//   * Two wires CROSSING, with neither having an end there, IS NOT. Crossing
//     wires that are not joined is the commonest thing on a schematic, and a
//     reader tells them apart by exactly this rule.
//   * A PIN at a point on a wire IS a connection.
//
// Getting the crossing rule the other way round joins every circuit on the
// sheet into one net, and the drawing looks completely unchanged.

// A PLACED SYMBOL, as the netlist needs to see it. The document's own object
// carries more; this is the part connectivity depends on, so that the rule can
// be tested without a document.
struct PlacedSymbol {
    ObjectId id{kInvalidObjectId};
    std::string tag;        // "-K1", "-R7" -- what the schematic calls this part
    std::string symbolName; // which definition, in the library
    Vec2 positionMm{};
    double rotationRad = 0.0;
    bool mirrored = false;
};

// A DRAWN CONNECTION. Its own kind, not a line on a layer called WIRE.
//
// Layer-based wires mean moving a line to another layer silently changes the
// circuit while the drawing looks identical -- which is this project's
// recurring defect in its most expensive form. A wire is a wire because it IS
// one.
struct WireRun {
    ObjectId id{kInvalidObjectId};
    std::vector<Vec2> pointsMm;
};

// WHERE A NET TOUCHES A COMPONENT.
struct NetPin {
    ObjectId symbolId{kInvalidObjectId};
    std::string tag;
    std::string pinName;
    Vec2 atMm{};
};

struct Net {
    std::string name; // assigned by numbering, empty until then
    std::vector<NetPin> pins;
    std::vector<ObjectId> wires;

    // A net touching fewer than two pins goes nowhere. Reported rather than
    // hidden: a wire to nothing is the commonest schematic mistake and the
    // hardest to see, because it looks exactly like a wire.
    bool isDangling() const noexcept { return pins.size() < 2; }
};

struct Netlist {
    std::vector<Net> nets;

    const Net* netOfPin(ObjectId symbolId, const std::string& pinName) const noexcept;
    std::vector<const Net*> danglingNets() const;
};

// HOW CLOSE COUNTS AS TOUCHING.
//
// Schematics are drawn on a 2.5 mm grid, so a tenth of a millimetre is far
// tighter than anything a user can place by hand and far looser than floating
// point noise. It is a named constant because the number IS the rule: too big
// and separate circuits merge, too small and a wire that looks connected is
// not.
constexpr double kNetToleranceMm = 0.1;

// BUILDS THE NETS. The one place connectivity is decided.
Netlist BuildNetlist(const std::vector<WireRun>& wires,
                     const std::vector<PlacedSymbol>& symbols, const SymbolLibrary& library);

// Where a placed symbol's pins actually are, on the sheet.
std::vector<NetPin> PinsOf(const PlacedSymbol& placed, const SymbolLibrary& library);

// NUMBERS THE NETS, in place: W1, W2, ... in the order they first appear
// left-to-right then bottom-to-top, which is the order an electrician reads a
// schematic. A net that already carries a LABEL keeps its name -- a label is
// the user saying what the net is called, and renumbering over it would rename
// a wire somebody has already crimped a ferrule for.
void NumberNets(Netlist& netlist, const std::string& prefix = "W");

} // namespace paramcad
