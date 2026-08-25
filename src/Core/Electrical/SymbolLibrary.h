#pragma once

#include "Core/Drawing/DrawingEntity.h"
#include "Core/Drawing/Geometry2D.h"

#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

// THE SYMBOL LIBRARY (M36.1).
//
// A schematic is a DRAWING -- the same paper, frame, title block, layers and
// plot path as a mechanical sheet. What makes it electrical is what is ON it:
// symbols with PINS, and wires that connect them.
//
// A FOURTH DOCUMENT TYPE WAS CONSIDERED AND REJECTED. It would have duplicated
// or forced a refactor of some three thousand lines of sheet machinery to gain
// nothing a reader or a plotter can see; AutoCAD Electrical keeps schematics
// in DWG for the same reason. What a schematic needs that a drawing does not
// is symbols and connectivity, and those are objects, not a document class.
//
// THE THING THAT MAKES A SYMBOL A SYMBOL IS ITS PINS.
//
// Without them it is a picture of a resistor: it can be placed and plotted and
// it carries no more meaning than a rectangle. The pins are where the geometry
// stops being decoration and starts being a circuit.

// WHERE A WIRE MEETS A SYMBOL, in the symbol's OWN millimetres.
//
// `name` is what the component's datasheet calls it -- "1", "2", "A1", "13",
// "14". It is not a position and not an index: a contactor's auxiliary
// contacts are 13/14 and 21/22, and renumbering them 1..4 because that is the
// order they were drawn in would make every wiring list wrong.
struct SymbolPin {
    std::string name;
    Vec2 at{};
    // Which way a wire leaves the pin, as a unit vector in symbol space. Used
    // to draw the stub and to decide which side a wire should approach from --
    // NOT for connectivity, which is decided by position alone.
    Vec2 direction{1.0, 0.0};
};

// A SYMBOL DEFINITION: what it looks like, and where it connects.
//
// Its geometry is the same DrawShape variant the sheet uses, so a symbol is
// drawn, exported to DXF and plotted by exactly the code that already does
// those things. A separate "symbol geometry" type would have been a second
// thing that must agree with the first.
class ElectricalSymbol {
public:
    ElectricalSymbol() = default;
    ElectricalSymbol(std::string name, std::string description,
                     std::vector<DrawShape> shapes, std::vector<SymbolPin> pins);

    const std::string& name() const noexcept { return name_; }
    const std::string& description() const noexcept { return description_; }
    const std::vector<DrawShape>& shapes() const noexcept { return shapes_; }
    const std::vector<SymbolPin>& pins() const noexcept { return pins_; }

    // WHAT THE TAG LOOKS LIKE for this kind of part: "R" for a resistor, "K"
    // for a contactor coil, "X" for a terminal (IEC 81346). A schematic where
    // every part was called "SYMBOL1" is one nobody can cross-reference.
    const std::string& tagPrefix() const noexcept { return tagPrefix_; }
    void setTagPrefix(std::string prefix) { tagPrefix_ = std::move(prefix); }

    const SymbolPin* findPin(const std::string& name) const noexcept;
    Box2D bounds() const;

private:
    std::string name_;
    std::string description_;
    std::string tagPrefix_{"U"};
    std::vector<DrawShape> shapes_;
    std::vector<SymbolPin> pins_;
};

// A NAMED SET OF SYMBOLS.
//
// A library is a FILE a company keeps and shares, and a placement on a sheet
// stores the library's name and the symbol's -- a sentence, not a copy of the
// geometry (ADR-M22-003). The alternative, copying the shapes into every
// drawing, means a corrected symbol never reaches the drawings already made.
class SymbolLibrary {
public:
    explicit SymbolLibrary(std::string name);

    const std::string& name() const noexcept { return name_; }
    const std::vector<ElectricalSymbol>& symbols() const noexcept { return symbols_; }
    const ElectricalSymbol* find(const std::string& name) const noexcept;
    // Refused for an empty name, a duplicate, or a symbol with NO PINS -- one
    // that connects to nothing is a picture, and putting it in an electrical
    // library is how a schematic ends up with a component nobody can wire.
    bool add(ElectricalSymbol symbol);

private:
    std::string name_;
    std::vector<ElectricalSymbol> symbols_;
};

// THE SYMBOLS EVERY SCHEMATIC NEEDS, to IEC 60617.
//
// Shipped rather than left to the user, for the reason the title block seeds
// its rows: somebody who has to draw a resistor before they can draw a circuit
// draws it once and copies that file for ever. Sized on a 2.5 mm grid, which
// is what the wire spacing on a schematic is built from.
const SymbolLibrary& BuiltInSymbols();

// The world transform of a placement: rotate, then mirror, then move. ONE
// composition, so a pin's position and the geometry it belongs to cannot end
// up in different places.
Matrix2D SymbolPlacementTransform(Vec2 positionMm, double rotationRad, bool mirrored) noexcept;

} // namespace paramcad
