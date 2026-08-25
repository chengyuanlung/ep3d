#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace paramcad {

// M39.1 -- THE NUMBERS A HOLE IS DRILLED TO, AS PUBLISHED VALUES.
//
// A tapped hole is not drilled to its thread size. An M8 hole is drilled 6.8
// and then cut to 8; drill it 8 and the tap has nothing to bite and the part
// is scrap. Nothing on the screen says which of those happened -- a hole is a
// round hole either way, at a diameter nobody looks at twice.
//
// So the drilled diameter of a tapped hole is DERIVED FROM ITS DESIGNATION and
// never typed. "M8x1.25" is the sentence the hole stores (ADR-M22-003); 6.8 is
// what this table says that sentence means.
//
// WHY TABLES AND NOT THE FORMULA. The formula everybody quotes -- tap drill =
// nominal minus pitch -- is right for seventeen of the nineteen sizes here and
// WRONG for M8 and M12, which are the two most common threads in the world:
// it gives 6.75 and 10.25 where the standard says 6.8 and 10.2. This is the
// M37 lesson arriving a second time (see Tolerance.cpp, where an ISO 286
// formula was one micrometre out on four steps). A formula that is nearly
// right is worse than no formula, because the cases it gets wrong are the ones
// that get used.
//
// AND A SIZE THAT IS NOT IN THE TABLE IS REFUSED, never approximated. A hole
// callout is a manufacturing instruction; an interpolated tap drill is a
// number this program made up and printed as though a standard said it.

struct MetricThread {
    double nominalMm = 0.0;
    double pitchMm = 0.0;
    // What the hole is drilled to BEFORE tapping.
    double tapDrillMm = 0.0;

    // "M8x1.25" -- the designation as a drawing writes it.
    std::string designation() const;
};

// The coarse thread a designation names. "M8", "M8x1.25", "m8 " all give the
// same row; "M8x1" is REFUSED, because a fine pitch is a different thread with
// a different drill and this table does not hold it.
std::optional<MetricThread> MetricCoarseThread(std::string_view designation);
std::optional<MetricThread> MetricCoarseThreadOfSize(double nominalMm);

// HOW LOOSE THE HOLE A SCREW PASSES THROUGH IS (ISO 273).
//
// Three of them, and the difference matters: a plate of M6 holes at the close
// fit will not go on if the mating holes were drilled with any error at all,
// and one at the loose fit will rattle. The choice belongs to whoever is
// designing the joint, so it is asked for rather than defaulted silently.
enum class ClearanceFit { Close, Normal, Loose };
const char* NameOf(ClearanceFit fit) noexcept;
std::optional<double> ClearanceHoleMm(double nominalMm, ClearanceFit fit);

// The recess a socket head cap screw (ISO 4762) sits in, from DIN 974-1.
//
// The DEPTH is the screw's head height and no more: the point of a counterbore
// is that the head finishes flush, and a deeper one is a head below the
// surface that a spanner cannot reach.
struct Counterbore {
    double diameterMm = 0.0;
    double depthMm = 0.0;
};
std::optional<Counterbore> CounterboreForSocketHead(double nominalMm);

// The cone a countersunk head (ISO 7046) sits in.
//
// The ANGLE is part of the answer and not an assumption: 90 degrees is the
// metric standard, and an 82 degree screw -- the imperial one -- in a 90
// degree hole stands proud of the surface by a hair, which is enough to stop
// two faces meeting.
struct Countersink {
    double diameterMm = 0.0;
    double includedAngleDeg = 0.0;
};
std::optional<Countersink> CountersinkForFlatHead(double nominalMm);


// WHAT KIND OF RECESS THE HOLE HAS AT ITS MOUTH.
enum class HoleKind { Simple, Counterbore, Countersink };

// WHAT SCREW A HOLE IS FOR -- A SENTENCE, NOT A SET OF NUMBERS (ADR-M22-003).
//
// "M8x1.25, tapped" is what the designer decided. 6.8 is what that means
// today, and it is looked up every time rather than stored: a hole carrying
// both a designation and a diameter is two answers to one question, and the
// way anybody finds out they disagree is a part that comes back untappable.
//
// An EMPTY designation is the escape hatch -- a hole that is just a hole, at
// whatever diameter its parameter says. That is a real thing to want and it is
// said explicitly rather than by leaving a standard half-filled-in.
struct HoleScrew {
    std::string designation; // "M8x1.25", or empty for a plain hole
    bool tapped = false;     // tapped -> the tap drill; otherwise a clearance hole
    ClearanceFit fit = ClearanceFit::Normal;

    bool named() const noexcept { return !designation.empty(); }
};

// EVERY NUMBER A HOLE IS MADE FROM, AND THE SENTENCE THE DRAWING WRITES,
// WORKED OUT TOGETHER.
//
// One function, because the cut and the callout MUST agree. Computed apart --
// the feature sizing its tools, the drawing composing its text -- they are two
// hand-copied readings of one standard, which is the defect this project keeps
// closing. A drawing that says M8 over a hole drilled 8.4 is worse than either
// mistake alone: it is a correct-looking instruction to make the wrong part.
struct HoleSizes {
    bool ok = false;
    std::string why;

    double drillDiameterMm = 0.0;
    // Zero unless the kind asks for them.
    double counterboreDiameterMm = 0.0;
    double counterboreDepthMm = 0.0;
    double countersinkDiameterMm = 0.0;
    double countersinkAngleDeg = 0.0;

    // What goes on the drawing, in the order a reader expects: the hole, then
    // how deep, then the recess.
    std::string callout;
};

// A depthMm of zero means THROUGH, exactly as the feature's depth parameter
// does -- the callout says THRU rather than a depth.
HoleSizes SizeAHole(const HoleScrew& screw, HoleKind kind, double typedDiameterMm,
                    double depthMm);

// THE NAMES A FILE USES, and the only place they are spelled.
//
// A writer and a reader that each spell "counterbore" in their own function
// are two hand-copied lists that must agree; the way anybody finds out they do
// not is a saved document that reopens as a different shape.
const char* NameOfHoleKind(HoleKind kind) noexcept;
std::optional<HoleKind> HoleKindNamed(std::string_view name) noexcept;
std::optional<ClearanceFit> ClearanceFitNamed(std::string_view name) noexcept;

} // namespace paramcad
