#pragma once

#include "Core/Geometry/MathTypes.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

class PartDocument;

// M58 -- THE DESIGN ACCELERATOR, starting where this program already had half
// of one.
//
// EP3D has had a GEAR RELATION since M26: two rotations coupled by a ratio, so
// turning one instance turns the other. The ratio is a number somebody types.
//
// That is the shape this codebase keeps closing. A pair of modelled gears has
// a ratio -- it is the tooth counts, and nothing else -- and a typed one is a
// second copy of that fact, kept in step by hand. Change a gear from 20 teeth
// to 24 and the model meshes differently while the mechanism goes on turning
// at the old rate: every number still plausible, the animation still smooth,
// and the machine wrong.
//
// So a gear reaches the program as a SOURCE PATH, as M45's fasteners and M56's
// frame members do:
//
//   gear:m2 z20 b10
//
// and the ratio of a pair is READ OFF THE TWO PATHS. GearRatio is the only
// place a gear ratio is computed, and the relation gets its number from there.
//
// WHAT IS NOT MODELLED is the fillet at the tooth root, profile shift, helix
// angle and crowning. Each is a real thing and each would change the tooth
// FORM rather than its size, so leaving them out is visible in the shape
// rather than hidden in a number -- which is the right way round for an
// omission.

// --- The gear ---------------------------------------------------------------

// THE MODULE SERIES (ISO 54, series I).
//
// A gate, like M45's screw lengths and unlike M41's roughness numbers: a
// module is not a preference, it is which cutter the shop owns. A gear at
// module 1.7 is a gear nobody can cut.
const std::vector<double>& StandardModules();

// The pressure angles a cutter exists for. 20 degrees is the modern standard;
// 14.5 is what older machines and replacement parts are on, and a shop that
// has to make one part for a 1950s machine needs it.
const std::vector<double>& StandardPressureAngles();

struct SpurGear {
    double moduleMm = 2.0;
    int teeth = 20;
    double faceWidthMm = 10.0;
    double pressureAngleDeg = 20.0;

    // ALL DERIVED. A gear has exactly four numbers in it and everything else
    // follows from them, so nothing else is stored -- a pitch diameter kept
    // beside a module and a tooth count is a third thing that has to agree
    // with two others.
    double pitchDiameterMm() const noexcept;  // d  = m z
    double baseDiameterMm() const noexcept;   // db = d cos(alpha)
    double tipDiameterMm() const noexcept;    // da = m (z + 2)
    double rootDiameterMm() const noexcept;   // df = m (z - 2.5)
    // The thickness of one tooth measured ROUND the pitch circle. Half the
    // pitch, because a tooth and the space beside it share it -- and that is
    // the property that makes two gears of the same module mesh at all.
    double toothThicknessMm() const noexcept; // s = pi m / 2

    // "m2 z20 b10", with the pressure angle appended only when it is not 20 --
    // so an ordinary gear's path is the short one, and two ordinary gears of
    // the same size are the same path and one line on a parts list.
    std::string designation() const;
};

// The gear a designation names, or nothing.
std::optional<SpurGear> LookUpGear(std::string_view designation);

// THE SAME TEXT, READ BUT NOT JUDGED.
//
// "This is not written like a gear" and "this is a gear nobody can cut" are
// different failures with different next moves -- fix your typing, or change a
// tooth count -- and LookUpGear collapses them into one nothing. The library
// resolver needs them apart so it can hand back the undercut sentence instead
// of a note about syntax, so parsing is available on its own.
std::optional<SpurGear> ParseGearDesignation(std::string_view designation);

// WHY THIS GEAR CANNOT BE CUT, or empty when it can.
//
// The one that earns its place is UNDERCUT. Below a certain tooth count the
// generating cutter sweeps into the flank near the root and eats away part of
// the involute -- the tooth is still there, it still looks like a tooth, and
// it is weaker and runs rougher than the drawing says. The limit is
// 2/sin^2(alpha), which is 17.1 at 20 degrees and 31.9 at 14.5 -- so the fewest
// teeth that are CLEAR of it are 18 and 32, and the textbook's "17 teeth" is
// the limit itself rather than a count that avoids it. A generator that
// quietly produced a 12-tooth pinion would be producing exactly the part that
// fails in service.
std::string WhyGearRefused(const SpurGear& gear);
// The smallest tooth count that does not undercut at this pressure angle.
int MinimumTeethWithoutUndercut(double pressureAngleDeg) noexcept;

// --- A pair -----------------------------------------------------------------

// WHY THESE TWO WILL NOT MESH, or empty when they will.
//
// Different modules is the answer nearly every time, and it is worth its own
// sentence: two gears of different module have teeth of different SIZE, and no
// centre distance makes them run. The pressure angles have to match for the
// same reason -- the flanks are different curves.
std::string WhyPairRefused(const SpurGear& driver, const SpurGear& driven);

// THE CENTRE DISTANCE for a pair, which is the number that goes on the
// drawing and into the housing: a = m (z1 + z2) / 2.
//
// Zero when the pair is refused, so a caller that ignores WhyPairRefused gets
// a number it cannot use rather than one it can.
double CentreDistanceMm(const SpurGear& driver, const SpurGear& driven);

// THE RATIO, and THE ONLY PLACE IT IS COMPUTED.
//
// driven turns z1/z2 as fast as driver, and BACKWARDS -- two external gears in
// mesh turn opposite ways, which is a fact about the machine and not a
// preference, so the sign is here rather than in a `reversed` flag somebody
// sets separately.
//
// This is what Relation::ratio is set from for a meshing pair. A typed ratio
// and a modelled pair are two copies of one fact.
double GearRatio(const SpurGear& driver, const SpurGear& driven);

// HOW MANY TEETH ARE IN CONTACT AT ONCE, on average.
//
// Below 1.0 the drive breaks contact between teeth and stops turning; below
// about 1.2 it runs, badly, and is noisy under load. It is the one number
// that says whether a pair that meshes is a pair that works, and it cannot be
// seen in the model at all -- so it is reported rather than left to be
// discovered on the bench.
double ContactRatio(const SpurGear& driver, const SpurGear& driven);

// --- The path scheme --------------------------------------------------------
//
//   gear:m2 z20 b10
//
// Same scheme as `std:` and `frm:`, resolved in the same place
// (Library/LibraryPart.h).
constexpr std::string_view kSpurGearScheme = "gear:";
bool IsSpurGearPath(std::string_view path) noexcept;
std::string SpurGearPath(const SpurGear& gear);
std::optional<SpurGear> SpurGearOfPath(std::string_view path);

// --- The shape --------------------------------------------------------------

// THE TOOTH OUTLINE, as a closed loop of points, counter-clockwise, centred on
// the origin with a tooth space centred on +X.
//
// THE FLANK IS AN INVOLUTE APPROXIMATED BY CHORDS, and the count is chosen so
// the deviation stays under a micron on any gear this catalogue will build --
// far below what a hobbed gear holds. Said out loud because it is a real
// approximation: what this draws is a gear to look at, to weigh and to check
// clearances against, not a master gear to inspect one against.
std::vector<Vec2> SpurGearOutline(const SpurGear& gear);

// The gear as a part, built from its numbers on every rebuild.
std::unique_ptr<PartDocument> BuildSpurGear(const SpurGear& gear);

} // namespace paramcad
