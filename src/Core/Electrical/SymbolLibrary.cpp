#include "Core/Electrical/SymbolLibrary.h"

#include <cmath>
#include <utility>

namespace paramcad {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kPi = kTwoPi / 2.0;

// THE GRID EVERY SYMBOL IS BUILT ON.
//
// 2.5 mm, which is what schematic wire spacing has been since drawing boards.
// Pins land on it exactly, so a symbol dropped on the grid has its pins on the
// grid too -- and two symbols placed a whole number of squares apart wire up
// with a straight line rather than a dog-leg nobody meant to draw.
constexpr double kGrid = 2.5;

ElectricalSymbol MakeSymbol(const char* name, const char* description, const char* prefix,
                            std::vector<DrawShape> shapes, std::vector<SymbolPin> pins) {
    ElectricalSymbol symbol{name, description, std::move(shapes), std::move(pins)};
    symbol.setTagPrefix(prefix);
    return symbol;
}

// A pin plus the stub of wire that reaches it, which is what makes a symbol
// look connectable rather than floating.
void AddPin(std::vector<DrawShape>& shapes, std::vector<SymbolPin>& pins, const char* name,
            Vec2 at, Vec2 direction, Vec2 bodyEdge) {
    pins.push_back(SymbolPin{name, at, direction});
    shapes.push_back(DrawLine{bodyEdge, at});
}

} // namespace

ElectricalSymbol::ElectricalSymbol(std::string name, std::string description,
                                   std::vector<DrawShape> shapes, std::vector<SymbolPin> pins)
    : name_(std::move(name)),
      description_(std::move(description)),
      shapes_(std::move(shapes)),
      pins_(std::move(pins)) {}

const SymbolPin* ElectricalSymbol::findPin(const std::string& name) const noexcept {
    for (const SymbolPin& pin : pins_)
        if (pin.name == name) return &pin;
    return nullptr;
}

Box2D ElectricalSymbol::bounds() const {
    Box2D box;
    for (const DrawShape& shape : shapes_) box.grow(BoundsOf(shape));
    // THE PINS ARE PART OF THE SYMBOL'S EXTENT, even when a stub was not drawn
    // for one: a pin outside the box would be a connection point a window
    // selection could not reach.
    for (const SymbolPin& pin : pins_) box.grow(pin.at);
    return box;
}

SymbolLibrary::SymbolLibrary(std::string name) : name_(std::move(name)) {}

const ElectricalSymbol* SymbolLibrary::find(const std::string& name) const noexcept {
    for (const ElectricalSymbol& symbol : symbols_)
        if (symbol.name() == name) return &symbol;
    return nullptr;
}

bool SymbolLibrary::add(ElectricalSymbol symbol) {
    if (symbol.name().empty()) return false;
    if (find(symbol.name()) != nullptr) return false;
    // A SYMBOL WITH NO PINS CONNECTS TO NOTHING. It can be placed and plotted
    // and it carries no more meaning than a rectangle -- and a schematic with
    // a component nobody can wire is one somebody has to debug on the machine.
    if (symbol.pins().empty()) return false;
    symbols_.push_back(std::move(symbol));
    return true;
}

Matrix2D SymbolPlacementTransform(Vec2 positionMm, double rotationRad, bool mirrored) noexcept {
    // ROTATE, THEN MIRROR, THEN MOVE -- in that order, and only here.
    //
    // The order is not a preference: mirroring after rotating is what a user
    // means by "turn it round and flip it", and doing it the other way puts a
    // rotated-and-flipped symbol somewhere neither operation asked for. Since
    // a pin's world position and the geometry it belongs to both come through
    // this one function, they cannot end up in different places.
    Matrix2D transform = Matrix2D::rotation(rotationRad);
    if (mirrored)
        transform = transform.then(Matrix2D::mirror(Vec2{0.0, 0.0}, Vec2{0.0, 1.0}));
    return transform.then(Matrix2D::translation(positionMm));
}

const SymbolLibrary& BuiltInSymbols() {
    static const SymbolLibrary library = [] {
        SymbolLibrary made{"IEC"};

        // --- RESISTOR: the IEC box, 10 x 4, pins either side ------------------
        {
            std::vector<DrawShape> shapes;
            std::vector<SymbolPin> pins;
            shapes.push_back(DrawLine{Vec2{-2.0 * kGrid, -0.8 * kGrid},
                                      Vec2{2.0 * kGrid, -0.8 * kGrid}});
            shapes.push_back(DrawLine{Vec2{2.0 * kGrid, -0.8 * kGrid},
                                      Vec2{2.0 * kGrid, 0.8 * kGrid}});
            shapes.push_back(DrawLine{Vec2{2.0 * kGrid, 0.8 * kGrid},
                                      Vec2{-2.0 * kGrid, 0.8 * kGrid}});
            shapes.push_back(DrawLine{Vec2{-2.0 * kGrid, 0.8 * kGrid},
                                      Vec2{-2.0 * kGrid, -0.8 * kGrid}});
            AddPin(shapes, pins, "1", Vec2{-4.0 * kGrid, 0.0}, Vec2{-1.0, 0.0},
                   Vec2{-2.0 * kGrid, 0.0});
            AddPin(shapes, pins, "2", Vec2{4.0 * kGrid, 0.0}, Vec2{1.0, 0.0},
                   Vec2{2.0 * kGrid, 0.0});
            made.add(MakeSymbol("Resistor", "Resistor, IEC 60617-4", "R", std::move(shapes),
                                std::move(pins)));
        }

        // --- CAPACITOR: two plates, pins above and below ----------------------
        {
            std::vector<DrawShape> shapes;
            std::vector<SymbolPin> pins;
            shapes.push_back(DrawLine{Vec2{-1.6 * kGrid, 0.4 * kGrid},
                                      Vec2{1.6 * kGrid, 0.4 * kGrid}});
            shapes.push_back(DrawLine{Vec2{-1.6 * kGrid, -0.4 * kGrid},
                                      Vec2{1.6 * kGrid, -0.4 * kGrid}});
            AddPin(shapes, pins, "1", Vec2{0.0, 2.0 * kGrid}, Vec2{0.0, 1.0},
                   Vec2{0.0, 0.4 * kGrid});
            AddPin(shapes, pins, "2", Vec2{0.0, -2.0 * kGrid}, Vec2{0.0, -1.0},
                   Vec2{0.0, -0.4 * kGrid});
            made.add(MakeSymbol("Capacitor", "Capacitor, IEC 60617-4", "C", std::move(shapes),
                                std::move(pins)));
        }

        // --- COIL: a contactor's operating coil, A1 and A2 --------------------
        //
        // The pin names matter more than the box: A1/A2 is what the terminal
        // is stamped with, and a wiring list calling them 1 and 2 sends an
        // electrician to the wrong screw.
        {
            std::vector<DrawShape> shapes;
            std::vector<SymbolPin> pins;
            shapes.push_back(DrawLine{Vec2{-1.6 * kGrid, -1.0 * kGrid},
                                      Vec2{1.6 * kGrid, -1.0 * kGrid}});
            shapes.push_back(DrawLine{Vec2{1.6 * kGrid, -1.0 * kGrid},
                                      Vec2{1.6 * kGrid, 1.0 * kGrid}});
            shapes.push_back(DrawLine{Vec2{1.6 * kGrid, 1.0 * kGrid},
                                      Vec2{-1.6 * kGrid, 1.0 * kGrid}});
            shapes.push_back(DrawLine{Vec2{-1.6 * kGrid, 1.0 * kGrid},
                                      Vec2{-1.6 * kGrid, -1.0 * kGrid}});
            AddPin(shapes, pins, "A1", Vec2{0.0, 3.0 * kGrid}, Vec2{0.0, 1.0},
                   Vec2{0.0, 1.0 * kGrid});
            AddPin(shapes, pins, "A2", Vec2{0.0, -3.0 * kGrid}, Vec2{0.0, -1.0},
                   Vec2{0.0, -1.0 * kGrid});
            made.add(MakeSymbol("Coil", "Contactor coil, IEC 60617-7", "K", std::move(shapes),
                                std::move(pins)));
        }

        // --- CONTACT, normally open: 13/14 ------------------------------------
        {
            std::vector<DrawShape> shapes;
            std::vector<SymbolPin> pins;
            // The blade, drawn open -- which is what "normally open" means and
            // is the whole difference from the next symbol.
            shapes.push_back(DrawLine{Vec2{0.0, -1.2 * kGrid}, Vec2{1.2 * kGrid, 1.0 * kGrid}});
            AddPin(shapes, pins, "13", Vec2{0.0, 3.0 * kGrid}, Vec2{0.0, 1.0},
                   Vec2{0.0, 1.2 * kGrid});
            AddPin(shapes, pins, "14", Vec2{0.0, -3.0 * kGrid}, Vec2{0.0, -1.0},
                   Vec2{0.0, -1.2 * kGrid});
            made.add(MakeSymbol("ContactNO", "Contact, normally open, IEC 60617-7", "K",
                                std::move(shapes), std::move(pins)));
        }

        // --- CONTACT, normally closed: 11/12 ----------------------------------
        {
            std::vector<DrawShape> shapes;
            std::vector<SymbolPin> pins;
            shapes.push_back(DrawLine{Vec2{0.0, -1.2 * kGrid}, Vec2{1.2 * kGrid, 1.0 * kGrid}});
            // The bar across the top is what says CLOSED.
            shapes.push_back(DrawLine{Vec2{1.2 * kGrid, 1.0 * kGrid},
                                      Vec2{1.2 * kGrid, 1.6 * kGrid}});
            shapes.push_back(DrawLine{Vec2{0.6 * kGrid, 1.2 * kGrid},
                                      Vec2{1.8 * kGrid, 1.2 * kGrid}});
            AddPin(shapes, pins, "11", Vec2{0.0, 3.0 * kGrid}, Vec2{0.0, 1.0},
                   Vec2{0.0, 1.2 * kGrid});
            AddPin(shapes, pins, "12", Vec2{0.0, -3.0 * kGrid}, Vec2{0.0, -1.0},
                   Vec2{0.0, -1.2 * kGrid});
            made.add(MakeSymbol("ContactNC", "Contact, normally closed, IEC 60617-7", "K",
                                std::move(shapes), std::move(pins)));
        }

        // --- TERMINAL: one pin each side of a circle --------------------------
        //
        // A terminal is the one symbol whose two pins are the SAME electrical
        // node -- it is a screw, not a component -- and the netlist has to know
        // that or every wire through a terminal strip becomes two nets.
        {
            std::vector<DrawShape> shapes;
            std::vector<SymbolPin> pins;
            shapes.push_back(DrawCircle{Vec2{0.0, 0.0}, 0.5 * kGrid});
            pins.push_back(SymbolPin{"1", Vec2{0.0, 1.6 * kGrid}, Vec2{0.0, 1.0}});
            pins.push_back(SymbolPin{"2", Vec2{0.0, -1.6 * kGrid}, Vec2{0.0, -1.0}});
            shapes.push_back(DrawLine{Vec2{0.0, 0.5 * kGrid}, Vec2{0.0, 1.6 * kGrid}});
            shapes.push_back(DrawLine{Vec2{0.0, -0.5 * kGrid}, Vec2{0.0, -1.6 * kGrid}});
            made.add(MakeSymbol("Terminal", "Terminal, IEC 60617-3", "X", std::move(shapes),
                                std::move(pins)));
        }

        // --- FUSE -------------------------------------------------------------
        {
            std::vector<DrawShape> shapes;
            std::vector<SymbolPin> pins;
            shapes.push_back(DrawLine{Vec2{-0.8 * kGrid, -1.6 * kGrid},
                                      Vec2{0.8 * kGrid, -1.6 * kGrid}});
            shapes.push_back(DrawLine{Vec2{0.8 * kGrid, -1.6 * kGrid},
                                      Vec2{0.8 * kGrid, 1.6 * kGrid}});
            shapes.push_back(DrawLine{Vec2{0.8 * kGrid, 1.6 * kGrid},
                                      Vec2{-0.8 * kGrid, 1.6 * kGrid}});
            shapes.push_back(DrawLine{Vec2{-0.8 * kGrid, 1.6 * kGrid},
                                      Vec2{-0.8 * kGrid, -1.6 * kGrid}});
            shapes.push_back(DrawLine{Vec2{0.0, -1.6 * kGrid}, Vec2{0.0, 1.6 * kGrid}});
            AddPin(shapes, pins, "1", Vec2{0.0, 3.2 * kGrid}, Vec2{0.0, 1.0},
                   Vec2{0.0, 1.6 * kGrid});
            AddPin(shapes, pins, "2", Vec2{0.0, -3.2 * kGrid}, Vec2{0.0, -1.0},
                   Vec2{0.0, -1.6 * kGrid});
            made.add(MakeSymbol("Fuse", "Fuse, IEC 60617-7", "F", std::move(shapes),
                                std::move(pins)));
        }

        // --- MOTOR, three phase -----------------------------------------------
        {
            std::vector<DrawShape> shapes;
            std::vector<SymbolPin> pins;
            shapes.push_back(DrawCircle{Vec2{0.0, 0.0}, 2.0 * kGrid});
            for (int i = 0; i < 3; ++i) {
                const double x = (static_cast<double>(i) - 1.0) * kGrid;
                const char* names[] = {"U", "V", "W"};
                AddPin(shapes, pins, names[i], Vec2{x, 4.0 * kGrid}, Vec2{0.0, 1.0},
                       Vec2{x, std::sqrt(std::max(0.0, 4.0 * kGrid * kGrid - x * x))});
            }
            made.add(MakeSymbol("Motor3", "Motor, three phase, IEC 60617-6", "M",
                                std::move(shapes), std::move(pins)));
        }

        // --- LAMP -------------------------------------------------------------
        {
            std::vector<DrawShape> shapes;
            std::vector<SymbolPin> pins;
            shapes.push_back(DrawCircle{Vec2{0.0, 0.0}, 1.2 * kGrid});
            const double d = 1.2 * kGrid * std::cos(kPi / 4.0);
            shapes.push_back(DrawLine{Vec2{-d, -d}, Vec2{d, d}});
            shapes.push_back(DrawLine{Vec2{-d, d}, Vec2{d, -d}});
            AddPin(shapes, pins, "X1", Vec2{0.0, 3.0 * kGrid}, Vec2{0.0, 1.0},
                   Vec2{0.0, 1.2 * kGrid});
            AddPin(shapes, pins, "X2", Vec2{0.0, -3.0 * kGrid}, Vec2{0.0, -1.0},
                   Vec2{0.0, -1.2 * kGrid});
            made.add(MakeSymbol("Lamp", "Indicator lamp, IEC 60617-8", "H", std::move(shapes),
                                std::move(pins)));
        }

        return made;
    }();
    return library;
}

} // namespace paramcad
