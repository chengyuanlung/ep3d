#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

// M51 -- WHAT A BEND DOES TO A LENGTH.
//
// A sheet metal part is cut flat and then folded. The flat blank is NOT the
// sum of the finished part's sides: metal on the outside of a bend stretches
// and metal on the inside compresses, and somewhere between them is a line
// that does neither. Where that line sits is the K FACTOR, and everything in
// this file exists to get it from a published table rather than from a guess.
//
// THE FAILURE THIS IS FOR, AND IT IS THE WHOLE POINT:
//
// A flat pattern computed with the wrong K is entirely self-consistent. Every
// dimension on it agrees with every other dimension. It cuts cleanly, it folds
// cleanly, and the finished part is the wrong size -- by a millimetre or two
// per bend, which on an eight-bend enclosure is a lid that does not fit. There
// is nothing on the drawing to look at, because the drawing is right about the
// blank it describes.
//
// It is the same shape as M37's fits and M39's tap drills: a number somebody
// could interpolate, that has to come out of a table instead.

// THE THREE CLASSES THE TABLE IS PUBLISHED FOR.
//
// Not the document's Material -- that carries a density and an elastic
// modulus, and forming depends on the ALLOY AND ITS TEMPER, which no density
// implies. Joining them would mean inventing a temper for every material or
// defaulting one, and a defaulted temper is a defaulted K, which is the
// failure at the top of this file arriving by a side door.
enum class SheetMaterial {
    SoftBrassCopper,        // soft brass, soft copper
    MildSteelAluminium,     // semi-hard copper and brass, mild steel, aluminium
    HardBronzeSpringSteel,  // bronze, cold-rolled steel, spring steel
};
std::string_view toString(SheetMaterial material) noexcept;
bool ParseSheetMaterial(std::string_view text, SheetMaterial& into) noexcept;

// THE K FACTOR, from the table.
//
// It is banded by the ratio of bend radius to thickness, because that is what
// decides how much of the section is in tension: a tight bend moves the
// neutral line further in than a generous one. Three materials, three bands,
// nine published numbers -- and no arithmetic between them, because the bands
// are what the standard measured rather than samples of a curve.
double KFactorFor(SheetMaterial material, double bendRadiusMm, double thicknessMm) noexcept;

// HOW TIGHT A BEND THE MATERIAL WILL TAKE, as a radius in millimetres.
//
// Tighter than this and the outside of the bend cracks. It is refused rather
// than computed: a flat pattern for a bend that cannot be made is a correct
// answer to a question nobody can act on, and the cracking is found at the
// press brake with the blanks already cut.
double MinimumBendRadiusMm(SheetMaterial material, double thicknessMm) noexcept;

// ONE BEND.
struct SheetBend {
    // The angle the metal turns THROUGH, not the angle between the faces. A
    // right-angled box corner is 90 degrees here. The two are supplementary
    // and both are called "the bend angle" in different shops, so this says
    // which -- getting it backwards on anything but 90 gives a flat pattern
    // that is wrong and looks ordinary.
    double angleDeg = 90.0;
    double innerRadiusMm = 1.0;
};

// --- THE ARITHMETIC ---------------------------------------------------------
//
// BEND ALLOWANCE is how much material the bend itself consumes, measured
// along the neutral line: the arc length at radius R + K*T.
double BendAllowanceMm(const SheetBend& bend, double thicknessMm, double kFactor) noexcept;

// OUTSIDE SETBACK is how far the outside corner of the bend sits from the
// tangent line -- the amount an outside dimension includes that the flat does
// not.
double OutsideSetbackMm(const SheetBend& bend, double thicknessMm) noexcept;

// BEND DEDUCTION is what comes OFF the sum of the outside dimensions:
// two setbacks less the allowance.
//
// ALLOWANCE AND DEDUCTION ARE NOT THE SAME NUMBER and using one where the
// other belongs is the classic shop error -- it is out by two setbacks per
// bend, which is a blank a few millimetres wrong in a direction nobody
// notices until the last flange will not close.
double BendDeductionMm(const SheetBend& bend, double thicknessMm, double kFactor) noexcept;

// --- THE FLAT LENGTH --------------------------------------------------------

struct FlatPatternResult {
    bool ok = false;
    double lengthMm = 0.0;
    std::string why;
};

// FROM THE OUTSIDE DIMENSIONS, the way a drawing states them: the sum of the
// flange lengths measured to the outside corners, less a deduction per bend.
//
// `outsideMm` has one entry per flange and `bends` one per bend, so there is
// always exactly one more flange than bend -- a list that did not would be
// describing a part with a bend at nothing, and it is refused rather than
// truncated.
FlatPatternResult FlatLengthFromOutside(const std::vector<double>& outsideMm,
                                        const std::vector<SheetBend>& bends,
                                        SheetMaterial material, double thicknessMm);

// FROM THE TANGENT LENGTHS -- the flat parts between the bends -- plus an
// allowance per bend.
//
// THE SAME NUMBER, REACHED THE OTHER WAY. Kept because both are how shops
// state the job, and because two routes to one answer is a thing a test can
// hold against itself: a sign error in either shows up as the two disagreeing,
// which no single-method check could see.
FlatPatternResult FlatLengthFromTangents(const std::vector<double>& tangentMm,
                                         const std::vector<SheetBend>& bends,
                                         SheetMaterial material, double thicknessMm);

// Why this bend cannot be made in this material at this thickness, or empty
// when it can.
std::string WhyBendRefused(const SheetBend& bend, SheetMaterial material,
                           double thicknessMm);

} // namespace paramcad
