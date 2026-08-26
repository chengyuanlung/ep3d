#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace paramcad {

// M47 -- THE WELD SYMBOL (ISO 2553).
//
// The fifth body an Annotation can carry, and it reuses everything M41 built:
// a symbol, a leader that is a DimensionAnchor, and a set of fields. What is
// new is that this symbol's MEANING IS ITS POSITION. A fillet triangle on the
// reference line and the same triangle under it are the same drawing twice and
// two different joints -- one weld you can reach, one you cannot.
//
// So the side is not a field that could be defaulted, copied wrong, or left at
// whatever the last symbol used. IT IS THE STRUCTURE: a bead exists in
// arrowSide or in otherSide, and there is nowhere else to put one. A weld with
// no side is not a weld with a wrong side; it is a spec that does not compile
// into a symbol, and WhyWeldRefused says so.
//
// This is the same move as M42's balloon (no stored number) and M38's section
// letters (no stored letter): not "test that the side is right", but "leave no
// way to write it down twice".

// --- ELEMENTARY SYMBOLS (ISO 2553 table 1) ---------------------------------
//
// The joint preparation, which is what the shop actually cuts before it welds.
enum class WeldType {
    SquareButt,    // no preparation -- plates butted square
    SingleV,       // both edges bevelled, a V
    SingleBevel,   // one edge bevelled, the other square
    SingleU,       // both edges radiused, a U
    SingleJ,       // one edge radiused
    Fillet,        // the triangle: the corner weld of a T or lap joint
    Plug,          // a hole in the near plate, filled
    Spot,          // resistance spot
    Seam,          // a rolled continuous resistance weld
    Backing,       // the sealing run behind a butt weld
    Surfacing,     // material laid on a face, not joining anything
    Edge           // two edges welded along their tops
};
std::string_view toString(WeldType type) noexcept;
bool ParseWeldType(std::string_view text, WeldType& into) noexcept;
// The ISO 2553 glyph, as UTF-8.
std::string SymbolOfWeldType(WeldType type);

// --- HOW A FILLET IS SIZED --------------------------------------------------
//
// THE RULE THAT COSTS THIRTY PER CENT OF THE METAL.
//
//   a   the design THROAT: the height of the largest triangle inside the weld.
//   z   the LEG: how far the weld runs up each plate.
//
// For the ordinary equal-leg fillet, z is a times the square root of two. So
// a5 and z5 are different welds -- z5 has a throat of 3.54, and the two are
// drawn as one number next to one triangle. A drawing that dropped the letter
// does not look incomplete; it looks like a size.
//
// Which is why Unspecified is refused rather than assumed. Assuming a is the
// ISO habit and assuming z is the workshop habit, and a program that picks one
// is choosing which half of its users to be silently wrong for.
enum class FilletSizeKind { Unspecified, Throat, Leg };
std::string_view toString(FilletSizeKind kind) noexcept;
bool ParseFilletSizeKind(std::string_view text, FilletSizeKind& into) noexcept;

// The throat, whichever way the size was written. ONE conversion, in one
// place, so no caller does it by hand with the wrong constant.
double ThroatOfMm(double sizeMm, FilletSizeKind kind) noexcept;

// --- CONTOUR ----------------------------------------------------------------
//
// The shape of the finished bead. Concave and convex are not decoration: a
// convex fillet toe is a stress raiser, and a concave one is what a fatigue
// joint asks for.
enum class WeldContour { AsWelded, Flat, Convex, Concave, Blended };
std::string_view toString(WeldContour contour) noexcept;
bool ParseWeldContour(std::string_view text, WeldContour& into) noexcept;
std::string SymbolOfContour(WeldContour contour);

// --- AN INTERMITTENT RUN ----------------------------------------------------
//
// Written n x l (e). THE NUMBER IN BRACKETS IS NOT THE PITCH.
//
// ISO 2553 puts the SPACE BETWEEN welds in the brackets; AWS A2.4 puts the
// centre-to-centre PITCH there. Same three numbers, same layout, welds in
// different places: read 3 x 50 (30) the American way and the run is 100 long
// instead of 160, and the last weld ends 60 short of where the designer put
// it. Nothing on the drawing looks wrong either way.
//
// The field is named after what it is, and the two lengths a reader might want
// are DERIVED below rather than left to whoever needs them next.
struct WeldRun {
    int count = 0;
    double lengthMm = 0.0;
    // The GAP between welds, which is ISO's number. Zero would mean the welds
    // touch, which is a continuous weld written the hard way -- refused.
    double gapMm = 0.0;
};

// Metal actually laid down: count times length.
double DepositedLengthMm(const WeldRun& run) noexcept;
// From the start of the first weld to the end of the last: the metal plus the
// gaps between. This is the number that differs when the brackets are read as
// AWS, and it is the one that decides whether the run reaches the end of the
// joint.
double RunExtentMm(const WeldRun& run) noexcept;
// Centre to centre -- what AWS would have put in the brackets. Offered so a
// user interface can show both without anyone recomputing it from the wrong
// end.
double PitchMm(const WeldRun& run) noexcept;

// --- ONE SIDE'S WELD --------------------------------------------------------
//
// There is no side field here on purpose: see the top of this file.
struct WeldBead {
    WeldType type = WeldType::Fillet;
    // The size: a fillet's throat or leg, a butt's depth of preparation, a
    // spot's diameter. Zero means the size is not stated, which is legal for a
    // full-penetration butt weld and refused for a fillet.
    double sizeMm = 0.0;
    // Only meaningful for a fillet, and REQUIRED for one.
    FilletSizeKind sizeKind = FilletSizeKind::Unspecified;
    WeldContour contour = WeldContour::AsWelded;
    // Absent for a continuous weld along the whole joint.
    std::optional<WeldRun> run;
};

// --- THE SYMBOL -------------------------------------------------------------
//
// ALL-ROUND AND FIELD WELD LIVE HERE, NOT ON A BEAD, because they are drawn on
// the elbow of the leader and describe the whole instruction. A flag on a bead
// would let a drawing say "weld all round, but only on the arrow side", which
// is not a thing a welder can be told.
struct WeldSymbolSpec {
    std::optional<WeldBead> arrowSide;
    std::optional<WeldBead> otherSide;
    // The circle at the elbow: weld right round the joint, not just where the
    // arrow lands.
    bool allAround = false;
    // The flag: made on site, not in the shop. It changes who welds it, in
    // what position, and to which procedure.
    bool fieldWeld = false;
    // STAGGERED, not chained: the welds on one side sit in the gaps of the
    // other. It needs a run on BOTH sides -- staggered against nothing is a
    // word with no second half, and it draws as an ordinary double fillet.
    bool staggered = false;
    // The tail. Free text: a process reference, a procedure number, an
    // acceptance level. Deliberately NOT an enum -- unlike a fit or a tap
    // drill, what goes in a tail is the shop's own reference, and a table here
    // would refuse correct drawings. The same call M41 made for Ra, for the
    // same reason.
    std::string tail;
};

// A PROCESS NUMBER'S NAME, for a user interface to show beside the tail.
// Empty for anything not in ISO 4063's common list -- and that emptiness is
// NOT a refusal: the tail is free text and stays free text.
std::string_view NameOfWeldProcess(std::string_view reference) noexcept;

// WHY THIS SYMBOL CANNOT BE DRAWN, or empty when it can. Every rule here fails
// the same way: a symbol that draws perfectly and specifies a weld nobody can
// make, or a different weld from the one intended.
std::string WhyWeldRefused(const WeldSymbolSpec& spec);

// WHAT ONE SIDE SAYS: the size with its letter, the elementary symbol, the
// contour, and the run. NOT which side it is -- that is the caller's, because
// the caller is the only thing that knows where it is putting it.
//
// Exposed so the painter can put each side on its own line of an ISO 2553
// reference line without a second idea of how a bead is written. One
// derivation, two places that show it.
std::string WeldBeadText(const WeldBead& bead);

// WHAT THE SYMBOL SAYS. The words "arrow" and "other" are written out rather
// than left to the reader's idea of which line is which, because in text there
// is no line to be above or below. ON PAPER there is a line, and the drawing
// says it the way ISO 2553 does -- see DrawingPainter.
std::string WeldSymbolText(const WeldSymbolSpec& spec);

} // namespace paramcad
