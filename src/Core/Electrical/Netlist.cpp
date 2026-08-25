#include "Core/Electrical/Netlist.h"

#include <algorithm>
#include <cmath>

namespace paramcad {

namespace {

bool Same(Vec2 a, Vec2 b) noexcept {
    return std::hypot(a.x - b.x, a.y - b.y) <= kNetToleranceMm;
}

// Is `point` ON the segment a-b, between its ends? Ends included: a wire whose
// end lands exactly on another wire's end is the ordinary case.
bool OnSegment(Vec2 point, Vec2 a, Vec2 b) noexcept {
    return DistancePointToSegment(point, a, b) <= kNetToleranceMm;
}

// --- UNION-FIND over "things that are electrically the same" -----------------
//
// Straight loops would be quadratic in the number of wires and, worse, would
// need a rule for what to do when a third wire joins two that were already
// separate. Union-find has no such rule to get wrong: joining is joining.
class Groups {
public:
    std::size_t add() {
        parent_.push_back(parent_.size());
        return parent_.size() - 1;
    }
    std::size_t rootOf(std::size_t i) {
        while (parent_[i] != i) {
            parent_[i] = parent_[parent_[i]]; // halve the path as we go
            i = parent_[i];
        }
        return i;
    }
    void join(std::size_t a, std::size_t b) {
        const std::size_t ra = rootOf(a);
        const std::size_t rb = rootOf(b);
        if (ra != rb) parent_[ra] = rb;
    }
    std::size_t count() const noexcept { return parent_.size(); }

private:
    std::vector<std::size_t> parent_;
};

} // namespace

const Net* Netlist::netOfPin(ObjectId symbolId, const std::string& pinName) const noexcept {
    for (const Net& net : nets)
        for (const NetPin& pin : net.pins)
            if (pin.symbolId == symbolId && pin.pinName == pinName) return &net;
    return nullptr;
}

std::vector<const Net*> Netlist::danglingNets() const {
    std::vector<const Net*> found;
    for (const Net& net : nets)
        if (net.isDangling()) found.push_back(&net);
    return found;
}

std::vector<NetPin> PinsOf(const PlacedSymbol& placed, const SymbolLibrary& library) {
    std::vector<NetPin> pins;
    const ElectricalSymbol* symbol = library.find(placed.symbolName);
    // A PLACEMENT NAMING A SYMBOL THE LIBRARY DOES NOT HAVE CONTRIBUTES NO
    // PINS. It is drawn as a question mark elsewhere; here it simply connects
    // to nothing, which is the truth -- inventing pins for it would wire a
    // circuit to a component nobody can identify.
    if (symbol == nullptr) return pins;
    const Matrix2D transform =
        SymbolPlacementTransform(placed.positionMm, placed.rotationRad, placed.mirrored);
    for (const SymbolPin& pin : symbol->pins())
        pins.push_back(NetPin{placed.id, placed.tag, pin.name, transform.apply(pin.at)});
    return pins;
}

Netlist BuildNetlist(const std::vector<WireRun>& wires,
                     const std::vector<PlacedSymbol>& symbols, const SymbolLibrary& library) {
    Netlist out;
    Groups groups;
    // One group per wire to start with; every wire is its own net until
    // something joins it to another.
    for (std::size_t i = 0; i < wires.size(); ++i) (void)groups.add();

    // --- WIRE TO WIRE ---------------------------------------------------------
    for (std::size_t i = 0; i < wires.size(); ++i) {
        for (std::size_t j = 0; j < wires.size(); ++j) {
            if (i == j) continue;
            const std::vector<Vec2>& mine = wires[i].pointsMm;
            const std::vector<Vec2>& theirs = wires[j].pointsMm;
            if (mine.size() < 2 || theirs.size() < 2) continue;

            // ONLY MY ENDS COUNT. Testing every vertex against every segment
            // would join two wires that merely cross, and crossing wires that
            // are not joined are the commonest thing on a schematic.
            //
            // A polyline's INTERIOR vertices are corners, not ends -- a wire
            // that bends past another does not connect to it any more than a
            // straight one that crosses it does.
            const Vec2 ends[2] = {mine.front(), mine.back()};
            bool joined = false;
            for (const Vec2 end : ends) {
                for (std::size_t k = 0; k + 1 < theirs.size() && !joined; ++k)
                    if (OnSegment(end, theirs[k], theirs[k + 1])) joined = true;
                if (joined) break;
            }
            if (joined) groups.join(i, j);
        }
    }

    // --- PINS -----------------------------------------------------------------
    std::vector<NetPin> allPins;
    std::vector<std::size_t> pinGroup;
    for (const PlacedSymbol& placed : symbols) {
        const ElectricalSymbol* symbol = library.find(placed.symbolName);
        std::vector<NetPin> pins = PinsOf(placed, library);
        std::vector<std::size_t> groupOfPin;
        for (const NetPin& pin : pins) {
            std::size_t group = groups.add();
            // A pin joins every wire it touches -- at an end or along a run.
            for (std::size_t w = 0; w < wires.size(); ++w) {
                const std::vector<Vec2>& points = wires[w].pointsMm;
                bool touches = false;
                for (std::size_t k = 0; k + 1 < points.size() && !touches; ++k)
                    if (OnSegment(pin.atMm, points[k], points[k + 1])) touches = true;
                if (!points.empty() && points.size() == 1 && Same(pin.atMm, points.front()))
                    touches = true;
                if (touches) groups.join(group, w);
            }
            groupOfPin.push_back(group);
            allPins.push_back(pin);
            pinGroup.push_back(group);
        }

        // A TERMINAL'S TWO PINS ARE ONE NODE.
        //
        // A terminal is a screw, not a component: the wire arriving and the
        // wire leaving are the same electrical point. Without this, every wire
        // through a terminal strip becomes two nets and the wiring list says a
        // hundred connections where there are fifty.
        if (symbol != nullptr && symbol->name() == "Terminal" && groupOfPin.size() == 2)
            groups.join(groupOfPin[0], groupOfPin[1]);
    }

    // --- COLLECT --------------------------------------------------------------
    std::vector<std::size_t> rootOfNet;
    const auto netFor = [&](std::size_t group) -> Net& {
        const std::size_t root = groups.rootOf(group);
        for (std::size_t i = 0; i < rootOfNet.size(); ++i)
            if (rootOfNet[i] == root) return out.nets[i];
        rootOfNet.push_back(root);
        out.nets.emplace_back();
        return out.nets.back();
    };
    for (std::size_t i = 0; i < wires.size(); ++i) netFor(i).wires.push_back(wires[i].id);
    for (std::size_t i = 0; i < allPins.size(); ++i) netFor(pinGroup[i]).pins.push_back(allPins[i]);

    // A PIN CONNECTED TO NOTHING AT ALL IS STILL A NET -- of one pin, which
    // `isDangling` reports. Dropping it would hide exactly the mistake a
    // reader most needs to find.
    return out;
}

void NumberNets(Netlist& netlist, const std::string& prefix) {
    // THE ORDER AN ELECTRICIAN READS THE SHEET: left to right, then DOWN it.
    //
    // Down, not up. Sheet Y runs upward -- it is paper, not screen -- so
    // reading order is DESCENDING y, and a first draft sorted ascending and
    // numbered the bottom rung W1. Found by looking at the screenshot: a
    // two-wire circuit came out labelled W2 above W1, which is the drawing
    // counting backwards.
    //
    // Numbering in whatever order the wires happen to sit in memory would be
    // worse still: a list whose numbers jump about the drawing is one nobody
    // can follow along a terminal strip.
    struct Ordered {
        std::size_t index;
        Vec2 at;
    };
    std::vector<Ordered> order;
    for (std::size_t i = 0; i < netlist.nets.size(); ++i) {
        // THE NET'S READING POSITION: leftmost, and among those the HIGHEST
        // up the sheet -- the first place a reader's eye would land on it.
        Vec2 at{0.0, 0.0};
        bool have = false;
        for (const NetPin& pin : netlist.nets[i].pins) {
            if (!have || pin.atMm.x < at.x || (pin.atMm.x == at.x && pin.atMm.y > at.y))
                at = pin.atMm;
            have = true;
        }
        order.push_back(Ordered{i, at});
    }
    std::stable_sort(order.begin(), order.end(), [](const Ordered& a, const Ordered& b) {
        if (a.at.x != b.at.x) return a.at.x < b.at.x;
        return a.at.y > b.at.y; // down the sheet, because that is how it is read
    });

    int next = 1;
    for (const Ordered& one : order) {
        Net& net = netlist.nets[one.index];
        // A NET WITH NO WIRES CANNOT BE NUMBERED, because the name has nowhere
        // to live: it is kept on the wires (see WireEntity), and a pin with
        // nothing attached has none.
        //
        // Numbering one anyway BURNS the number. The drawing then jumps from
        // nothing to W2, and an electrician following a terminal strip has to
        // wonder what they missed -- which is what happened: an unconnected
        // fuse pin sorted first, took W1, and the first real wire came out W2.
        // Found by looking at the schematic.
        if (net.wires.empty()) continue;
        // A NET THAT ALREADY HAS A NAME KEEPS IT. A label is the user saying
        // what this wire is called; renumbering over it would rename a wire
        // somebody has already crimped a ferrule for.
        if (!net.name.empty()) continue;
        net.name = prefix + std::to_string(next++);
    }
}

} // namespace paramcad
