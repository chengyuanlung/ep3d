// M36 -- connectivity.
//
// THE RULE THAT MUST NOT BE WRONG is the crossing one.
//
// Two wires that cross without either having an end there are NOT connected;
// a wire whose END lands on another IS. Get it the other way round and every
// circuit on the sheet joins into one net -- and the drawing looks completely
// unchanged, so nothing but the netlist can tell anybody.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Electrical/Netlist.h"
#include "Core/Electrical/SymbolLibrary.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

WireRun Wire(ObjectId id, std::vector<Vec2> points) {
    WireRun run;
    run.id = id;
    run.pointsMm = std::move(points);
    return run;
}

PlacedSymbol Place(ObjectId id, const char* tag, const char* symbol, Vec2 at,
                   double rotationRad = 0.0, bool mirrored = false) {
    PlacedSymbol placed;
    placed.id = id;
    placed.tag = tag;
    placed.symbolName = symbol;
    placed.positionMm = at;
    placed.rotationRad = rotationRad;
    placed.mirrored = mirrored;
    return placed;
}

// How many nets have at least one wire or pin -- the ones a reader would call
// a net.
std::size_t RealNets(const Netlist& netlist) {
    std::size_t count = 0;
    for (const Net& net : netlist.nets)
        if (!net.wires.empty() || !net.pins.empty()) ++count;
    return count;
}

} // namespace

// =============================================================================
// The crossing rule
// =============================================================================

TEST(NetlistTest, M36_NET_001_TwoWiresThatMerelyCROSSAreNOTConnected) {
    // The commonest thing on a schematic. Joining them merges every circuit on
    // the sheet, and the picture does not change by one pixel.
    const std::vector<WireRun> wires{
        Wire(1, {Vec2{0.0, 50.0}, Vec2{100.0, 50.0}}),  // horizontal
        Wire(2, {Vec2{50.0, 0.0}, Vec2{50.0, 100.0}}),  // vertical, through it
    };
    const Netlist netlist = BuildNetlist(wires, {}, BuiltInSymbols());
    EXPECT_EQ(RealNets(netlist), 2u) << "two crossing wires were joined into one net";
}

TEST(NetlistTest, M36_NET_002_AWireWhoseENDLandsOnAnotherIsConnected) {
    // A T junction, which on a schematic is drawn exactly like this.
    const std::vector<WireRun> wires{
        Wire(1, {Vec2{0.0, 50.0}, Vec2{100.0, 50.0}}),
        Wire(2, {Vec2{50.0, 50.0}, Vec2{50.0, 100.0}}), // starts ON the first
    };
    const Netlist netlist = BuildNetlist(wires, {}, BuiltInSymbols());
    EXPECT_EQ(RealNets(netlist), 1u) << "a T junction did not connect";
}

TEST(NetlistTest, M36_NET_003_AWireBendingPASTAnotherDoesNotConnectToIt) {
    // A polyline's interior vertices are CORNERS, not ends. A wire that turns
    // a corner on top of another wire is crossing it, not joining it -- and
    // treating a corner as an end is the same defect as the crossing one,
    // wearing different clothes.
    const std::vector<WireRun> wires{
        Wire(1, {Vec2{0.0, 50.0}, Vec2{100.0, 50.0}}),
        // Down, corner exactly on the first wire, then off to the right.
        Wire(2, {Vec2{50.0, 100.0}, Vec2{50.0, 50.0}, Vec2{90.0, 20.0}}),
    };
    const Netlist netlist = BuildNetlist(wires, {}, BuiltInSymbols());
    EXPECT_EQ(RealNets(netlist), 2u)
        << "a wire that turned a corner on another wire was joined to it";
}

TEST(NetlistTest, M36_NET_004_ThreeWiresMeetingAtAPointAreONENet) {
    // Union-find has no rule to get wrong about the third one: joining is
    // joining, whatever order they arrive in.
    const std::vector<WireRun> wires{
        Wire(1, {Vec2{0.0, 50.0}, Vec2{50.0, 50.0}}),
        Wire(2, {Vec2{50.0, 50.0}, Vec2{100.0, 50.0}}),
        Wire(3, {Vec2{50.0, 50.0}, Vec2{50.0, 0.0}}),
    };
    const Netlist netlist = BuildNetlist(wires, {}, BuiltInSymbols());
    ASSERT_EQ(RealNets(netlist), 1u);
    EXPECT_EQ(netlist.nets.front().wires.size(), 3u);
}

TEST(NetlistTest, M36_NET_005_AWireThatJUSTMISSESIsNotConnected) {
    // The tolerance is a tenth of a millimetre: far tighter than anything a
    // user places by hand on a 2.5 mm grid, far looser than floating point
    // noise. A wire a whole millimetre short LOOKS connected at plot scale and
    // is not, which is precisely what the netlist is for.
    const std::vector<WireRun> close{
        Wire(1, {Vec2{0.0, 50.0}, Vec2{100.0, 50.0}}),
        Wire(2, {Vec2{50.0, 50.05}, Vec2{50.0, 100.0}}), // 0.05 mm away
    };
    EXPECT_EQ(RealNets(BuildNetlist(close, {}, BuiltInSymbols())), 1u)
        << "half a tolerance away was treated as a gap";

    const std::vector<WireRun> apart{
        Wire(1, {Vec2{0.0, 50.0}, Vec2{100.0, 50.0}}),
        Wire(2, {Vec2{50.0, 51.0}, Vec2{50.0, 100.0}}), // 1 mm away
    };
    EXPECT_EQ(RealNets(BuildNetlist(apart, {}, BuiltInSymbols())), 2u)
        << "a wire a millimetre short was wired up anyway";
}

// =============================================================================
// Symbols
// =============================================================================

TEST(NetlistTest, M36_SYM_001_APinConnectsToTheWireItSitsOn) {
    // A resistor lying flat: pin 1 at -10, pin 2 at +10 from its position.
    const std::vector<PlacedSymbol> symbols{Place(10, "-R1", "Resistor", Vec2{50.0, 50.0})};
    const std::vector<WireRun> wires{
        Wire(1, {Vec2{0.0, 50.0}, Vec2{40.0, 50.0}}),  // up to pin 1
        Wire(2, {Vec2{60.0, 50.0}, Vec2{100.0, 50.0}}), // away from pin 2
    };
    const Netlist netlist = BuildNetlist(wires, symbols, BuiltInSymbols());
    ASSERT_EQ(RealNets(netlist), 2u) << "a resistor shorted its own two nets";

    const Net* left = netlist.netOfPin(10, "1");
    const Net* right = netlist.netOfPin(10, "2");
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_NE(left, right) << "both ends of the resistor came out on one net";
    EXPECT_EQ(left->wires.size(), 1u);
    EXPECT_EQ(right->wires.size(), 1u);
}

TEST(NetlistTest, M36_SYM_002_TurningASymbolMOVESItsPins) {
    // A quarter turn puts a horizontal resistor's pins above and below it. A
    // netlist built from unrotated pins would wire the circuit that was drawn
    // before somebody turned the part.
    const SymbolLibrary& library = BuiltInSymbols();
    const std::vector<NetPin> flat = PinsOf(Place(10, "-R1", "Resistor", Vec2{50.0, 50.0}),
                                            library);
    ASSERT_EQ(flat.size(), 2u);
    EXPECT_NEAR(flat[0].atMm.x, 40.0, 1e-9);
    EXPECT_NEAR(flat[0].atMm.y, 50.0, 1e-9);

    constexpr double kQuarterTurn = 1.5707963267948966;
    const std::vector<NetPin> turned =
        PinsOf(Place(10, "-R1", "Resistor", Vec2{50.0, 50.0}, kQuarterTurn), library);
    ASSERT_EQ(turned.size(), 2u);
    EXPECT_NEAR(turned[0].atMm.x, 50.0, 1e-6);
    EXPECT_NEAR(turned[0].atMm.y, 40.0, 1e-6) << "a turned symbol kept its old pin positions";
    // ...and the pin NAMES do not move with the geometry: pin 1 is pin 1
    // whichever way round the part is fitted.
    EXPECT_EQ(turned[0].pinName, "1");
    EXPECT_EQ(turned[1].pinName, "2");
}

TEST(NetlistTest, M36_SYM_003_ATerminalsTwoPinsAreONENode) {
    // A terminal is a screw, not a component: the wire arriving and the wire
    // leaving are the same electrical point. Without this, every wire through
    // a terminal strip becomes two nets and the wiring list claims twice the
    // connections there are.
    const std::vector<PlacedSymbol> symbols{Place(10, "-X1", "Terminal", Vec2{50.0, 50.0})};
    const std::vector<WireRun> wires{
        Wire(1, {Vec2{50.0, 54.0}, Vec2{50.0, 90.0}}),
        Wire(2, {Vec2{50.0, 46.0}, Vec2{50.0, 10.0}}),
    };
    const Netlist netlist = BuildNetlist(wires, symbols, BuiltInSymbols());
    EXPECT_EQ(RealNets(netlist), 1u)
        << "a terminal split one net into two";
    EXPECT_EQ(netlist.netOfPin(10, "1"), netlist.netOfPin(10, "2"));

    // ...and a RESISTOR in the same place does NOT, because it is a component.
    const std::vector<PlacedSymbol> resistor{Place(11, "-R1", "Resistor", Vec2{50.0, 50.0},
                                                   1.5707963267948966)};
    EXPECT_EQ(RealNets(BuildNetlist(wires, resistor, BuiltInSymbols())), 2u)
        << "a resistor was treated as a piece of wire";
}

TEST(NetlistTest, M36_SYM_004_APlacementNamingAnUnknownSymbolConnectsToNOTHING) {
    // Inventing pins for it would wire a circuit to a component nobody can
    // identify -- which is worse than the part being visibly missing.
    const std::vector<PlacedSymbol> symbols{Place(10, "-Q1", "Thyristor", Vec2{50.0, 50.0})};
    EXPECT_TRUE(PinsOf(symbols.front(), BuiltInSymbols()).empty());
    const std::vector<WireRun> wires{Wire(1, {Vec2{0.0, 50.0}, Vec2{100.0, 50.0}})};
    const Netlist netlist = BuildNetlist(wires, symbols, BuiltInSymbols());
    EXPECT_EQ(RealNets(netlist), 1u);
    EXPECT_TRUE(netlist.nets.front().pins.empty());
}

// =============================================================================
// What the netlist says about the drawing
// =============================================================================

TEST(NetlistTest, M36_NET_010_AWireToNOTHINGIsReportedAsDangling) {
    // The commonest schematic mistake and the hardest to see, because a wire
    // to nothing looks exactly like a wire.
    const std::vector<PlacedSymbol> symbols{Place(10, "-R1", "Resistor", Vec2{50.0, 50.0})};
    const std::vector<WireRun> wires{
        Wire(1, {Vec2{0.0, 50.0}, Vec2{40.0, 50.0}}),   // reaches pin 1
        Wire(2, {Vec2{60.0, 50.0}, Vec2{100.0, 50.0}}), // reaches pin 2
    };
    const Netlist netlist = BuildNetlist(wires, symbols, BuiltInSymbols());
    // Each net touches ONE pin, so both are dangling: the resistor is wired at
    // both ends to wires that go nowhere else.
    EXPECT_EQ(netlist.danglingNets().size(), 2u);

    // ...and joining the two ends to a second component clears them.
    const std::vector<PlacedSymbol> two{
        Place(10, "-R1", "Resistor", Vec2{50.0, 50.0}),
        Place(11, "-R2", "Resistor", Vec2{50.0, 90.0}),
    };
    const std::vector<WireRun> looped{
        Wire(1, {Vec2{40.0, 50.0}, Vec2{20.0, 50.0}, Vec2{20.0, 90.0}, Vec2{40.0, 90.0}}),
        Wire(2, {Vec2{60.0, 50.0}, Vec2{80.0, 50.0}, Vec2{80.0, 90.0}, Vec2{60.0, 90.0}}),
    };
    const Netlist joined = BuildNetlist(looped, two, BuiltInSymbols());
    EXPECT_TRUE(joined.danglingNets().empty())
        << "a closed loop of two resistors still reports a dangling wire";
    EXPECT_EQ(RealNets(joined), 2u);
}

TEST(NetlistTest, M36_NET_011_NetsAreNumberedInTheOrderTheSheetIsREAD) {
    // Left to right, then DOWN the sheet -- sheet Y runs upward, so reading
    // order is descending y. A first draft sorted the other way and numbered
    // the bottom rung W1, which is the drawing counting backwards.
    const std::vector<PlacedSymbol> symbols{
        Place(10, "-R1", "Resistor", Vec2{200.0, 50.0}),
        Place(11, "-R2", "Resistor", Vec2{50.0, 50.0}),
    };
    const std::vector<WireRun> wires{
        Wire(1, {Vec2{190.0, 50.0}, Vec2{150.0, 50.0}}),
        Wire(2, {Vec2{40.0, 50.0}, Vec2{10.0, 50.0}}),
    };
    Netlist netlist = BuildNetlist(wires, symbols, BuiltInSymbols());
    NumberNets(netlist);

    const Net* leftmost = netlist.netOfPin(11, "1"); // -R2, at x = 40
    const Net* rightmost = netlist.netOfPin(10, "2"); // -R1, at x = 210
    ASSERT_NE(leftmost, nullptr);
    ASSERT_NE(rightmost, nullptr);
    EXPECT_EQ(leftmost->name, "W1") << "the leftmost net is not W1";
    EXPECT_NE(rightmost->name, "W1");
    // Every net WITH A WIRE got a name. A pin-only net gets none, because the
    // name lives on the wires and it has none -- see M36_NET_011c.
    for (const Net& net : netlist.nets)
        if (!net.wires.empty()) EXPECT_FALSE(net.name.empty());
}

TEST(NetlistTest, M36_NET_011b_TwoNetsInAColumnAreNumberedDOWNTheSheet) {
    // The half of the reading order the test above cannot see, because its
    // two nets are at different x. Here they are stacked, and the TOP one has
    // to be W1: a schematic is read down its rungs, and sheet Y runs upward,
    // so "first" is the LARGER y.
    const std::vector<PlacedSymbol> symbols{
        Place(10, "-R1", "Resistor", Vec2{100.0, 200.0}),
        Place(11, "-R2", "Resistor", Vec2{100.0, 100.0}),
    };
    const std::vector<WireRun> wires{
        Wire(1, {Vec2{90.0, 200.0}, Vec2{60.0, 200.0}}),  // the upper rung
        Wire(2, {Vec2{90.0, 100.0}, Vec2{60.0, 100.0}}),  // the lower one
    };
    Netlist netlist = BuildNetlist(wires, symbols, BuiltInSymbols());
    NumberNets(netlist);

    const Net* upper = netlist.netOfPin(10, "1");
    const Net* lower = netlist.netOfPin(11, "1");
    ASSERT_NE(upper, nullptr);
    ASSERT_NE(lower, nullptr);
    EXPECT_EQ(upper->name, "W1") << "the sheet was numbered from the bottom up";
    EXPECT_NE(lower->name, "W1");
}

TEST(NetlistTest, M36_NET_011c_ANetWithNoWiresDoesNotEATANumber) {
    // The name lives on the WIRES, so a net made only of an unconnected pin
    // has nowhere to keep one. Numbering it anyway BURNS the number: the
    // drawing jumps from nothing to W2, and an electrician following a
    // terminal strip has to wonder what they missed.
    //
    // This is what the real schematic did -- an unconnected fuse pin sorted
    // first, took W1, and the first actual wire came out W2 -- and the earlier
    // numbering tests could not see it, because in those every net had a wire.
    const std::vector<PlacedSymbol> symbols{
        // A fuse whose TOP pin is higher than anything else on the sheet and
        // reaches nothing.
        Place(10, "-F1", "Fuse", Vec2{100.0, 200.0}),
    };
    const std::vector<WireRun> wires{
        Wire(1, {Vec2{100.0, 192.0}, Vec2{100.0, 150.0}}), // off the fuse's bottom pin
    };
    Netlist netlist = BuildNetlist(wires, symbols, BuiltInSymbols());
    NumberNets(netlist);

    const Net* wired = netlist.netOfPin(10, "2");
    ASSERT_NE(wired, nullptr);
    EXPECT_EQ(wired->name, "W1")
        << "an unconnected pin ate W1, so the first real wire is W2";
    // ...and the pin-only net is left unnamed, which is honest: there is
    // nowhere to write a name on it.
    const Net* loose = netlist.netOfPin(10, "1");
    ASSERT_NE(loose, nullptr);
    EXPECT_TRUE(loose->name.empty());
    EXPECT_TRUE(loose->isDangling());
}

TEST(NetlistTest, M36_NET_012_ANetThatALREADYHasANameKeepsIt) {
    // A label is the user saying what this wire is called. Renumbering over it
    // would rename a wire somebody has already crimped a ferrule for.
    const std::vector<WireRun> wires{
        Wire(1, {Vec2{0.0, 50.0}, Vec2{40.0, 50.0}}),
        Wire(2, {Vec2{0.0, 90.0}, Vec2{40.0, 90.0}}),
    };
    Netlist netlist = BuildNetlist(wires, {}, BuiltInSymbols());
    ASSERT_EQ(netlist.nets.size(), 2u);
    netlist.nets[0].name = "L1";
    NumberNets(netlist);
    EXPECT_EQ(netlist.nets[0].name, "L1") << "a labelled net was renumbered";
    EXPECT_FALSE(netlist.nets[1].name.empty());
    EXPECT_NE(netlist.nets[1].name, "L1");
}

// =============================================================================
// The library
// =============================================================================

TEST(NetlistTest, M36_LIB_001_EverySymbolShippedHasPinsAndATagPrefix) {
    // A symbol with no pins connects to nothing: it can be placed and plotted
    // and carries no more meaning than a rectangle. A schematic where every
    // part is called "SYMBOL1" is one nobody can cross-reference.
    const SymbolLibrary& library = BuiltInSymbols();
    EXPECT_GE(library.symbols().size(), 8u);
    for (const ElectricalSymbol& symbol : library.symbols()) {
        EXPECT_FALSE(symbol.pins().empty()) << symbol.name() << " has no pins";
        EXPECT_FALSE(symbol.tagPrefix().empty()) << symbol.name() << " has no tag prefix";
        EXPECT_FALSE(symbol.shapes().empty()) << symbol.name() << " draws nothing";
        EXPECT_FALSE(symbol.description().empty()) << symbol.name() << " says nothing about itself";
        // No two pins in one symbol may share a name, or a wiring list points
        // at two screws with one number.
        for (std::size_t i = 0; i < symbol.pins().size(); ++i)
            for (std::size_t j = i + 1; j < symbol.pins().size(); ++j)
                EXPECT_NE(symbol.pins()[i].name, symbol.pins()[j].name)
                    << symbol.name() << " has two pins called " << symbol.pins()[i].name;
    }
}

TEST(NetlistTest, M36_LIB_002_TheContactorPinsAreTheNumbersOnTheTERMINAL) {
    // A1/A2 for the coil, 13/14 for a normally open contact, 11/12 for a
    // normally closed one -- IEC 81346, and what the part is stamped with. A
    // wiring list calling them 1 and 2 sends an electrician to the wrong screw.
    const SymbolLibrary& library = BuiltInSymbols();
    ASSERT_NE(library.find("Coil"), nullptr);
    EXPECT_NE(library.find("Coil")->findPin("A1"), nullptr);
    EXPECT_NE(library.find("Coil")->findPin("A2"), nullptr);
    ASSERT_NE(library.find("ContactNO"), nullptr);
    EXPECT_NE(library.find("ContactNO")->findPin("13"), nullptr);
    EXPECT_NE(library.find("ContactNO")->findPin("14"), nullptr);
    ASSERT_NE(library.find("ContactNC"), nullptr);
    EXPECT_NE(library.find("ContactNC")->findPin("11"), nullptr);
    EXPECT_NE(library.find("ContactNC")->findPin("12"), nullptr);
}

TEST(NetlistTest, M36_LIB_003_ASymbolWithNoPinsIsREFUSED) {
    SymbolLibrary library{"Test"};
    EXPECT_FALSE(library.add(ElectricalSymbol{"Blob", "A picture", {DrawCircle{Vec2{}, 5.0}},
                                              {}}));
    EXPECT_FALSE(library.add(ElectricalSymbol{"", "No name", {DrawCircle{Vec2{}, 5.0}},
                                              {SymbolPin{"1", Vec2{}, Vec2{1.0, 0.0}}}}));
    EXPECT_TRUE(library.add(ElectricalSymbol{"Thing", "Has a pin", {DrawCircle{Vec2{}, 5.0}},
                                             {SymbolPin{"1", Vec2{}, Vec2{1.0, 0.0}}}}));
    // ...and not twice under one name, or a placement naming it has two
    // answers.
    EXPECT_FALSE(library.add(ElectricalSymbol{"Thing", "Again", {DrawCircle{Vec2{}, 5.0}},
                                              {SymbolPin{"1", Vec2{}, Vec2{1.0, 0.0}}}}));
    EXPECT_EQ(library.symbols().size(), 1u);
}

TEST(NetlistTest, M36_LIB_004_MirroringHappensAFTERRotating) {
    // "Turn it round and flip it" is what a user means, and doing it the other
    // way puts the symbol somewhere neither operation asked for. Since a pin's
    // world position and the geometry it belongs to both come through this one
    // function, they cannot end up in different places.
    constexpr double kQuarterTurn = 1.5707963267948966;
    const Matrix2D transform =
        SymbolPlacementTransform(Vec2{100.0, 100.0}, kQuarterTurn, true);
    // A point one to the right, turned a quarter, is one up; mirrored about
    // the Y axis it is still one up; then moved to (100, 100).
    const Vec2 at = transform.apply(Vec2{1.0, 0.0});
    EXPECT_NEAR(at.x, 100.0, 1e-6);
    EXPECT_NEAR(at.y, 101.0, 1e-6);
    // ...and a point one UP, turned a quarter, is one to the LEFT; mirrored it
    // is one to the right. Doing the two the other way round would give the
    // opposite, which is how a flipped-and-turned symbol ends up wrong.
    const Vec2 up = transform.apply(Vec2{0.0, 1.0});
    EXPECT_NEAR(up.x, 101.0, 1e-6);
    EXPECT_NEAR(up.y, 100.0, 1e-6);
}

// =============================================================================
// On a drawing
// =============================================================================

TEST(NetlistTest, M36_DOC_001_ASchematicIsADrawingAndTheNetlistFOLLOWSIt) {
    // A schematic is a DrawingDocument: same paper, frame, title block and
    // layers. What it adds is components and wires -- and the netlist, which
    // is derived, so moving a wire changes the circuit with nothing told.
    DrawingDocument document{"Schematic"};
    const ObjectId lamp = document.addSymbol("-H1", "Lamp", Vec2{100.0, 100.0}).id();
    const ObjectId supply = document.addWire({Vec2{100.0, 107.5}, Vec2{100.0, 150.0}}).id();
    (void)supply;

    // One wire to the lamp's top pin: the net has one pin, so it dangles.
    EXPECT_EQ(document.netlist().danglingNets().size(), 2u);

    // Move the wire away and the lamp's pin is on its own.
    ASSERT_TRUE(document.setWirePoints(supply, {Vec2{200.0, 107.5}, Vec2{200.0, 150.0}}));
    const Netlist moved = document.netlist();
    const Net* top = moved.netOfPin(lamp, "X1");
    ASSERT_NE(top, nullptr);
    EXPECT_TRUE(top->wires.empty()) << "the wire moved away and the pin is still wired to it";
}

TEST(NetlistTest, M36_DOC_002_TwoComponentsCannotShareATag) {
    // The tag is what every cross-reference and wiring list points at. Two
    // -K1s is a list that sends an electrician to whichever they find first.
    DrawingDocument document{"Schematic"};
    document.addSymbol("-K1", "Coil", Vec2{100.0, 100.0});
    EXPECT_THROW(document.addSymbol("-K1", "Coil", Vec2{200.0, 100.0}), std::invalid_argument);
    EXPECT_THROW(document.addSymbol("", "Coil", Vec2{200.0, 100.0}), std::invalid_argument);
    EXPECT_THROW(document.addSymbol("-K2", "", Vec2{200.0, 100.0}), std::invalid_argument);
    EXPECT_EQ(document.symbols().size(), 1u);
}

TEST(NetlistTest, M36_DOC_003_AWireOfONEPointIsREFUSED) {
    // A click somebody did not finish. It would sit on the sheet connecting
    // nothing while looking like nothing.
    DrawingDocument document{"Schematic"};
    EXPECT_THROW(document.addWire({Vec2{10.0, 10.0}}), std::invalid_argument);
    EXPECT_THROW(document.addWire({}), std::invalid_argument);
    EXPECT_TRUE(document.wires().empty());
}

TEST(NetlistTest, M36_DOC_004_NumberingWritesTheNameOntoTheWIRES) {
    // A net is derived and has nowhere to keep a name, so the name lives on
    // the wires that make it up -- and the netlist reads it back off them.
    DrawingDocument document{"Schematic"};
    document.addSymbol("-R1", "Resistor", Vec2{50.0, 50.0});
    document.addSymbol("-R2", "Resistor", Vec2{150.0, 50.0});
    const ObjectId left = document.addWire({Vec2{40.0, 50.0}, Vec2{20.0, 50.0}}).id();
    const ObjectId middle = document.addWire({Vec2{60.0, 50.0}, Vec2{140.0, 50.0}}).id();

    EXPECT_EQ(document.numberNets(), 2u);
    EXPECT_FALSE(document.findWire(left)->label().empty());
    EXPECT_FALSE(document.findWire(middle)->label().empty());
    // ...and the netlist now reports those names.
    const Netlist named = document.netlist();
    bool sawW1 = false;
    for (const Net& net : named.nets)
        if (net.name == "W1") sawW1 = true;
    EXPECT_TRUE(sawW1) << "numbering wrote labels the netlist cannot read back";

    // NUMBERING AGAIN CHANGES NOTHING, because every net already has a name.
    EXPECT_EQ(document.numberNets(), 0u) << "numbering twice renamed the wires";
}

TEST(NetlistTest, M36_DOC_005_NumberingIsONEUndoStepForTheWholeSheet) {
    // It is one thing the user did. Undoing it a wire at a time would stop
    // somewhere no schematic was ever in.
    DrawingDocument document{"Schematic"};
    document.addSymbol("-R1", "Resistor", Vec2{50.0, 50.0});
    document.addSymbol("-R2", "Resistor", Vec2{150.0, 50.0});
    const ObjectId left = document.addWire({Vec2{40.0, 50.0}, Vec2{20.0, 50.0}}).id();
    const ObjectId middle = document.addWire({Vec2{60.0, 50.0}, Vec2{140.0, 50.0}}).id();

    const std::size_t before = document.undoDepth();
    ASSERT_GE(document.numberNets(), 2u);
    EXPECT_EQ(document.undoDepth(), before + 1) << "numbering was more than one undo step";

    ASSERT_TRUE(document.undo());
    EXPECT_TRUE(document.findWire(left)->label().empty());
    EXPECT_TRUE(document.findWire(middle)->label().empty())
        << "undoing the numbering left one wire named";
}

TEST(NetlistTest, M36_DOC_006_TwoWiresInOneNetDisagreeingAboutTheNameIsREPORTED) {
    // Picking one silently would hide that the schematic says two things about
    // one wire; refusing to name it at all would lose what the user typed. So
    // the first is taken AND the disagreement is reported.
    DrawingDocument document{"Schematic"};
    const ObjectId a = document.addWire({Vec2{0.0, 50.0}, Vec2{50.0, 50.0}}).id();
    const ObjectId b = document.addWire({Vec2{50.0, 50.0}, Vec2{100.0, 50.0}}).id();
    ASSERT_TRUE(document.setWireLabel(a, "L1"));
    EXPECT_TRUE(document.conflictingNetNames().empty());

    ASSERT_TRUE(document.setWireLabel(b, "L2"));
    const std::vector<std::string> clashes = document.conflictingNetNames();
    ASSERT_EQ(clashes.size(), 1u) << "two names on one net went unreported";
    // Said as the PAIR, because "there is a conflict" is not something a user
    // can act on and "L1 and L2 are the same wire" is.
    EXPECT_NE(clashes.front().find("L1"), std::string::npos) << clashes.front();
    EXPECT_NE(clashes.front().find("L2"), std::string::npos) << clashes.front();

    // ...and giving them the same name clears it.
    ASSERT_TRUE(document.setWireLabel(b, "L1"));
    EXPECT_TRUE(document.conflictingNetNames().empty());
}

TEST(NetlistTest, M36_DOC_007_EverySchematicEditComesBack) {
    DrawingDocument document{"Schematic"};
    const ObjectId coil = document.addSymbol("-K1", "Coil", Vec2{100.0, 100.0}).id();
    const ObjectId wire = document.addWire({Vec2{100.0, 107.5}, Vec2{100.0, 150.0}}).id();
    ASSERT_TRUE(document.setSymbolPosition(coil, Vec2{120.0, 100.0}));
    ASSERT_TRUE(document.setSymbolRotation(coil, 1.5707963267948966));
    ASSERT_TRUE(document.setSymbolMirrored(coil, true));
    ASSERT_TRUE(document.setWireLabel(wire, "L1"));

    ASSERT_TRUE(document.undo()); // the label
    EXPECT_TRUE(document.findWire(wire)->label().empty());
    ASSERT_TRUE(document.undo()); // the flip
    EXPECT_FALSE(document.findSymbol(coil)->isMirrored());
    ASSERT_TRUE(document.undo()); // the turn
    EXPECT_NEAR(document.findSymbol(coil)->rotationRad(), 0.0, 1e-12);
    ASSERT_TRUE(document.undo()); // the move
    EXPECT_NEAR(document.findSymbol(coil)->positionMm().x, 100.0, 1e-9);
    ASSERT_TRUE(document.undo()); // the wire
    EXPECT_EQ(document.findWire(wire), nullptr);
    ASSERT_TRUE(document.undo()); // the coil
    EXPECT_EQ(document.findSymbol(coil), nullptr);

    while (document.canRedo()) ASSERT_TRUE(document.redo());
    ASSERT_NE(document.findSymbol(coil), nullptr);
    EXPECT_TRUE(document.findSymbol(coil)->isMirrored());
    EXPECT_EQ(document.findWire(wire)->label(), "L1");
}

TEST(NetlistTest, M36_DOC_008_ASchematicSurvivesASaveAndTheNETLISTIsNotInTheFile) {
    DrawingDocument document{"Schematic"};
    document.addSymbol("-K1", "Coil", Vec2{100.0, 100.0});
    document.addSymbol("-X1", "Terminal", Vec2{100.0, 150.0});
    const ObjectId wire =
        document.addWire({Vec2{100.0, 107.5}, Vec2{100.0, 146.0}}).id();
    // A THIRD part carries the rotation, well clear of the wire: turning the
    // coil would move its A1 pin off the wire and break the connection this
    // test is about. (A first draft did exactly that, and the failure read as
    // "the reopened schematic is not connected" -- which was true, and about
    // the rig rather than the code.)
    document.addSymbol("-R1", "Resistor", Vec2{250.0, 100.0});
    ASSERT_TRUE(document.setSymbolRotation(document.findSymbolTagged("-R1")->id(), 0.5));
    ASSERT_TRUE(document.setWireLabel(wire, "L1"));

    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(document, out));
    const std::string saved = out.str();
    // THE NETLIST IS NOT WRITTEN. A file carrying it would hold a circuit the
    // drawing no longer shows, and what gets built is the netlist.
    EXPECT_EQ(saved.find("\"nets\""), std::string::npos);
    EXPECT_EQ(saved.find("\"netlist\""), std::string::npos);
    // ...and neither is the symbol's geometry: a placement is a sentence.
    EXPECT_EQ(saved.find("\"shapes\""), std::string::npos);

    std::istringstream in(saved);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    ASSERT_TRUE(loaded) << loaded.message;
    const DrawingDocument& back = *loaded.document;
    ASSERT_EQ(back.symbols().size(), 3u);
    ASSERT_EQ(back.wires().size(), 1u);
    EXPECT_NEAR(back.findSymbolTagged("-R1")->rotationRad(), 0.5, 1e-9);
    EXPECT_NEAR(back.findSymbolTagged("-K1")->rotationRad(), 0.0, 1e-12);
    EXPECT_EQ(back.wires().front()->label(), "L1");
    EXPECT_EQ(back.undoDepth(), 0u);

    // ...and it still CONNECTS, which a restored-but-unhooked schematic would
    // fail: the coil's A1 and the terminal are one net.
    const Netlist netlist = back.netlist();
    const Net* net = netlist.netOfPin(back.findSymbolTagged("-K1")->id(), "A1");
    ASSERT_NE(net, nullptr);
    // THREE pins, not two: the coil's A1, and BOTH of the terminal's, because
    // a terminal is one node (M36_SYM_003). Expecting two here would have been
    // asserting that the terminal rule does not apply after a reload.
    EXPECT_EQ(net->pins.size(), 3u) << "the reopened schematic is not connected";
    EXPECT_EQ(net->name, "L1");

    std::ostringstream again;
    ASSERT_TRUE(saveDrawingDocument(back, again));
    EXPECT_EQ(again.str(), saved);
}

TEST(NetlistTest, M36_DOC_009_TwoComponentsSharingATagIsREFUSEDByTheLoader) {
    DrawingDocument document{"Schematic"};
    document.addSymbol("-K1", "Coil", Vec2{100.0, 100.0});
    document.addSymbol("-K2", "Coil", Vec2{200.0, 100.0});
    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(document, out));
    std::string text = out.str();
    const std::size_t at = text.find("\"-K2\"");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"-K2\"").size(), "\"-K1\"");

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded) << "two components with one tag loaded cleanly";
    EXPECT_EQ(loaded.error, SerializationError::DuplicateId);
}

TEST(NetlistTest, M36_DOC_011_AWireOfONEPointIsREFUSEDByTheLoaderToo) {
    // addWire refuses it, so a file this program wrote never has one -- which
    // is exactly why the LOADER's check needs its own test: the state arrives
    // from a hand-edited file, or a reader that got it wrong, and a wire of
    // one point would sit on the sheet connecting nothing while looking like
    // nothing.
    DrawingDocument document{"Schematic"};
    document.addWire({Vec2{100.0, 100.0}, Vec2{100.0, 150.0}});
    std::ostringstream out;
    ASSERT_TRUE(saveDrawingDocument(document, out));
    std::string text = out.str();

    // Take the second point away.
    const std::size_t at = text.find("\"points\": [");
    ASSERT_NE(at, std::string::npos) << text;
    const std::size_t close = text.find(']', at);
    ASSERT_NE(close, std::string::npos);
    text.replace(at, close - at + 1,
                 "\"points\": [{\"x\": 100.0, \"y\": 100.0}]");

    std::istringstream in(text);
    const DrawingLoadResult loaded = loadDrawingDocument(in);
    EXPECT_FALSE(loaded) << "a wire of one point loaded cleanly";
    EXPECT_EQ(loaded.error, SerializationError::InvalidFieldType);
}

TEST(NetlistTest, M36_DOC_010_DeletingAComponentOrAWireTakesOnlyItself) {
    DrawingDocument document{"Schematic"};
    const ObjectId coil = document.addSymbol("-K1", "Coil", Vec2{100.0, 100.0}).id();
    const ObjectId wire = document.addWire({Vec2{100.0, 107.5}, Vec2{100.0, 150.0}}).id();
    document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{10.0, 0.0}});

    ASSERT_TRUE(document.removeObject(coil));
    EXPECT_EQ(document.findSymbol(coil), nullptr);
    EXPECT_NE(document.findWire(wire), nullptr) << "deleting a component took its wire";
    EXPECT_EQ(document.entities().size(), 1u);

    ASSERT_TRUE(document.removeObject(wire));
    EXPECT_EQ(document.findWire(wire), nullptr);
    EXPECT_EQ(document.entities().size(), 1u) << "deleting a wire took drawn geometry";

    ASSERT_TRUE(document.undo());
    EXPECT_NE(document.findWire(wire), nullptr);
    ASSERT_TRUE(document.undo());
    EXPECT_NE(document.findSymbol(coil), nullptr);
}
