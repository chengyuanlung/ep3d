#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

class PartDocument;
class IGeometryKernel;

// M60 -- THE SECOND HALF OF THE DESIGN ACCELERATOR.
//
// A spring reaches the program as a source path, the fourth use of the scheme
// M45 introduced:
//
//   spr:d2 D16 n8 L50
//
// and nothing downstream had to learn what a spring is. That much is now
// routine. What is NOT routine is the identity question a spring asks and the
// four kinds before it did not.
//
// A FRAME MEMBER'S LENGTH IS ITS IDENTITY: a 600 stick and a 400 stick are two
// different things to cut, and M56 put the length in the path for exactly that
// reason. A SPRING'S LENGTH IS NOT. The same spring is 50 long on the shelf
// and 38 long in the machine, and it is one part with one part number. Putting
// the fitted length in the path would order two springs where one is needed,
// and a parts list that does that is worse than no parts list.
//
// So the path names the SPRING -- wire, mean diameter, active coils, free
// length, ends -- and the geometry is always built at the free length. What
// the spring is doing at some other length is a CALCULATION (forceAtLengthMm,
// and the checks beside it) and not a second part.
//
// WHICH MEANS EP3D CANNOT SHOW A SPRING COMPRESSED, and that is worth saying
// plainly rather than leaving to be discovered. A part's geometry comes from
// its path and its path is its identity; a compressed one would need a size
// per instance, which is M54's variants applied to a library part rather than
// to a file. The number is available; the picture is not.

// --- The wire ---------------------------------------------------------------

// THE WIRE SIZES SPRING STEEL IS DRAWN TO (EN 10270-1).
//
// A gate, like M58's modules and M45's screw lengths: a wire diameter is not a
// preference, it is what comes off the reel. A spring at 1.9 mm is a spring
// nobody can wind because nobody stocks the wire.
const std::vector<double>& StandardWireDiameters();

enum class SpringEnds {
    Plain,       // the coil simply stops; the spring rocks on its own helix
    Closed,      // the end coils are wound flat against their neighbours
    ClosedGround // ...and then ground flat, which is what a spring seat wants
};
std::string_view toString(SpringEnds ends) noexcept;
bool ParseSpringEnds(std::string_view text, SpringEnds& into) noexcept;

// --- The spring -------------------------------------------------------------

struct CompressionSpring {
    double wireMm = 2.0;           // d
    double meanDiameterMm = 16.0;  // D, axis to axis of the wire
    double activeCoils = 8.0;      // n, the coils that actually deflect
    double freeLengthMm = 50.0;    // L0
    SpringEnds ends = SpringEnds::ClosedGround;

    // ALL DERIVED. A spring has five numbers in it and everything a catalogue
    // prints follows from them, so nothing else is stored.
    double outerDiameterMm() const noexcept;  // D + d, what has to fit in a bore
    double innerDiameterMm() const noexcept;  // D - d, what has to clear a rod
    // C = D/d. THE NUMBER THAT DECIDES WHETHER IT CAN BE WOUND AT ALL.
    double springIndex() const noexcept;
    double totalCoilsCount() const noexcept;  // nt: the dead ends count here
    double solidLengthMm() const noexcept;    // Ls, coil against coil
    double pitchMm() const noexcept;          // of the ACTIVE coils
    double maxDeflectionMm() const noexcept;  // L0 - Ls, and no further

    // THE RATE, in newtons per millimetre: R = G d^4 / (8 D^3 n).
    //
    // The one number a spring is specified by, and the one nothing else in
    // this program can check -- so it is computed here and nowhere else, for
    // the reason M58's GearRatio is.
    double rateNPerMm() const noexcept;
    double forceAtLengthN(double lengthMm) const noexcept;
    // Wahl-corrected shear stress, which is the one that predicts failure: the
    // inside of the coil sees more than the plain torsion formula says, and
    // the correction is bigger the tighter the index.
    double shearStressAtLengthMPa(double lengthMm) const noexcept;

    // L0/D. Above about 2.6 an unguided spring folds sideways instead of
    // shortening.
    double slendernessRatio() const noexcept;

    // "d2 D16 n8 L50", with the end style appended only when it is not the
    // ordinary closed-and-ground -- so the common spring's path is the short
    // one, and two of the same spring are one line on a parts list.
    std::string designation() const;
};

std::optional<CompressionSpring> LookUpSpring(std::string_view designation);
// The same text read but not judged, so a resolver can hand back the real
// reason instead of a complaint about syntax (M58's split, for M58's reason).
std::optional<CompressionSpring> ParseSpringDesignation(std::string_view designation);

// WHY THIS SPRING CANNOT BE MADE, or empty when it can.
//
// THE ONE THAT EARNS ITS PLACE IS THE INDEX. Below about 4 the wire cracks on
// the mandrel as it is wound -- the coil is tighter than the wire will take --
// and above about 12 the spring tangles with its neighbours in a bin and
// wanders under load. A generator that produced an index of 2 would produce
// something that looks exactly like a spring in every picture and cannot be
// wound at all.
std::string WhySpringRefused(const CompressionSpring& spring);

// WHAT TO WATCH FOR, or empty. NOT a refusal: a slender spring is perfectly
// makeable and simply has to be guided, in a bore or on a rod, or it folds
// sideways instead of shortening. Said rather than refused, because refusing
// it would be refusing half the springs in a machine.
std::string WhySpringMayBuckle(const CompressionSpring& spring);

// --- The path scheme --------------------------------------------------------
constexpr std::string_view kCompressionSpringScheme = "spr:";
bool IsCompressionSpringPath(std::string_view path) noexcept;
std::string CompressionSpringPath(const CompressionSpring& spring);
std::optional<CompressionSpring> CompressionSpringOfPath(std::string_view path);

// THE SPRING AS A PART, at its FREE length, built from its numbers on every
// rebuild.
//
// Needs the kernel because a spring is a helix and a helix is not something a
// sketch can hold -- unlike every other library part in this program, whose
// geometry is a feature tree. That is stated rather than hidden: a spring has
// no feature tree, so it has no editable history, and the way to change one is
// to change its path.
std::unique_ptr<PartDocument> BuildCompressionSpring(const CompressionSpring& spring,
                                                     IGeometryKernel& kernel);

} // namespace paramcad
