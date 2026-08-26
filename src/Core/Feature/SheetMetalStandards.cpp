#include "Core/Feature/SheetMetalStandards.h"

#include "Core/Text/NumberText.h"

#include <cmath>

namespace paramcad {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTiny = 1e-9;

// ONE ROW PER MATERIAL, so a class cannot be in the K table and missing from
// the minimum-radius one. The same shape M39's thread table has, and for the
// same reason.
//
// The three K columns are the published bands: a bend tighter than the
// thickness, one between one and three thicknesses, and one looser than three.
// NOT samples of a curve -- these are what was measured, and interpolating
// between them would be inventing a number and calling it a standard.
struct Row {
    SheetMaterial material;
    double kTight;    // R < T
    double kMedium;   // T <= R <= 3T
    double kLoose;    // R > 3T
    // The tightest bend the material takes, as a multiple of thickness.
    double minRadiusPerThickness;
};

constexpr Row kRows[] = {
    {SheetMaterial::SoftBrassCopper, 0.33, 0.40, 0.45, 0.5},
    {SheetMaterial::MildSteelAluminium, 0.38, 0.43, 0.46, 1.0},
    {SheetMaterial::HardBronzeSpringSteel, 0.40, 0.45, 0.50, 2.0},
};

const Row* RowFor(SheetMaterial material) noexcept {
    for (const Row& row : kRows)
        if (row.material == material) return &row;
    return nullptr;
}

double Radians(double degrees) noexcept { return degrees * kPi / 180.0; }

} // namespace

std::string_view toString(SheetMaterial material) noexcept {
    switch (material) {
        case SheetMaterial::SoftBrassCopper: return "soft-brass-copper";
        case SheetMaterial::MildSteelAluminium: return "mild-steel-aluminium";
        case SheetMaterial::HardBronzeSpringSteel: return "hard-bronze-spring-steel";
    }
    return "mild-steel-aluminium";
}

bool ParseSheetMaterial(std::string_view text, SheetMaterial& into) noexcept {
    // READ FROM THE SAME LIST IT IS WRITTEN FROM. A name that fell back would
    // become mild steel, whose K is a tenth away from spring steel's -- and a
    // tenth of a K is about a third of a millimetre per bend.
    for (const Row& row : kRows)
        if (text == toString(row.material)) {
            into = row.material;
            return true;
        }
    return false;
}

double KFactorFor(SheetMaterial material, double bendRadiusMm, double thicknessMm) noexcept {
    const Row* row = RowFor(material);
    if (row == nullptr || thicknessMm <= kTiny || bendRadiusMm < 0.0) return 0.0;
    const double ratio = bendRadiusMm / thicknessMm;
    if (ratio < 1.0) return row->kTight;
    if (ratio <= 3.0) return row->kMedium;
    return row->kLoose;
}

double MinimumBendRadiusMm(SheetMaterial material, double thicknessMm) noexcept {
    const Row* row = RowFor(material);
    if (row == nullptr || thicknessMm <= kTiny) return 0.0;
    return row->minRadiusPerThickness * thicknessMm;
}

std::string WhyBendRefused(const SheetBend& bend, SheetMaterial material,
                           double thicknessMm) {
    if (thicknessMm <= kTiny)
        return "sheet with no thickness does not bend -- it has no outside to stretch";
    // A BEND OF NOTHING IS NOT A BEND, and one of half a turn is a HEM, which
    // is folded flat against itself and has arithmetic of its own. Both are
    // refused rather than run through a formula that quietly returns a number.
    if (bend.angleDeg <= kTiny)
        return "a bend has to turn through something";
    if (bend.angleDeg >= 180.0 - kTiny)
        return "a bend of half a turn is a hem, which is folded flat against itself and is "
               "not this";
    if (bend.innerRadiusMm < 0.0) return "a bend radius cannot be negative";
    // TIGHTER THAN THE MATERIAL TAKES AND THE OUTSIDE CRACKS. Refused here, on
    // the drawing, rather than found at the press brake with the blanks
    // already cut.
    const double least = MinimumBendRadiusMm(material, thicknessMm);
    if (bend.innerRadiusMm < least - kTiny)
        return "a radius of " + ShortNumber(bend.innerRadiusMm) + " cracks " +
               std::string(toString(material)) + " at " + ShortNumber(thicknessMm) +
               " thick -- the tightest it takes is " + ShortNumber(least);
    return {};
}

double BendAllowanceMm(const SheetBend& bend, double thicknessMm, double kFactor) noexcept {
    // The arc the NEUTRAL LINE travels: it sits K thicknesses in from the
    // inside face, and the metal there neither stretches nor compresses.
    return Radians(bend.angleDeg) * (bend.innerRadiusMm + kFactor * thicknessMm);
}

double OutsideSetbackMm(const SheetBend& bend, double thicknessMm) noexcept {
    return (bend.innerRadiusMm + thicknessMm) * std::tan(Radians(bend.angleDeg) / 2.0);
}

double BendDeductionMm(const SheetBend& bend, double thicknessMm, double kFactor) noexcept {
    // TWO SETBACKS LESS THE ALLOWANCE. Each bend pushes an outside corner away
    // from both of its flanges, so the outside dimensions count that corner
    // twice; what the metal actually spends going round is the allowance.
    return 2.0 * OutsideSetbackMm(bend, thicknessMm) -
           BendAllowanceMm(bend, thicknessMm, kFactor);
}

namespace {

// THE CHECKS BOTH ROUTES MAKE, asked once. Two copies would be two chances for
// one route to accept a job the other refuses -- and then the two flat lengths
// a shop can ask for would exist for different sets of parts.
std::string WhyRunRefused(std::size_t flanges, const std::vector<SheetBend>& bends,
                          SheetMaterial material, double thicknessMm) {
    if (thicknessMm <= kTiny) return "sheet with no thickness has no flat pattern";
    if (flanges == 0) return "a flat pattern needs at least one flange";
    // ONE MORE FLANGE THAN BEND, always. Anything else is a part with a bend
    // at nothing, and truncating to the shorter list would hand back a length
    // for a different part.
    if (bends.size() + 1 != flanges)
        return "this part has " + std::to_string(flanges) + " flanges and " +
               std::to_string(bends.size()) + " bends -- there is always one more flange "
                                              "than bend";
    for (const SheetBend& bend : bends) {
        const std::string why = WhyBendRefused(bend, material, thicknessMm);
        if (!why.empty()) return why;
    }
    return {};
}

} // namespace

FlatPatternResult FlatLengthFromOutside(const std::vector<double>& outsideMm,
                                        const std::vector<SheetBend>& bends,
                                        SheetMaterial material, double thicknessMm) {
    FlatPatternResult out;
    out.why = WhyRunRefused(outsideMm.size(), bends, material, thicknessMm);
    if (!out.why.empty()) return out;
    double total = 0.0;
    for (const double flange : outsideMm) {
        if (flange <= kTiny) {
            out.why = "a flange of no length is not a flange";
            return out;
        }
        total += flange;
    }
    for (const SheetBend& bend : bends)
        total -= BendDeductionMm(bend, thicknessMm,
                                 KFactorFor(material, bend.innerRadiusMm, thicknessMm));
    if (total <= kTiny) {
        // THE DEDUCTIONS ATE THE PART. On a small flange with a generous
        // radius the outside dimensions can be less than the bends consume,
        // and a negative blank is not a small one -- it is a part that cannot
        // be made this way.
        out.why = "the bends take more material than the flanges have -- these flanges are "
                  "too short for these radii";
        return out;
    }
    out.ok = true;
    out.lengthMm = total;
    return out;
}

FlatPatternResult FlatLengthFromTangents(const std::vector<double>& tangentMm,
                                         const std::vector<SheetBend>& bends,
                                         SheetMaterial material, double thicknessMm) {
    FlatPatternResult out;
    out.why = WhyRunRefused(tangentMm.size(), bends, material, thicknessMm);
    if (!out.why.empty()) return out;
    double total = 0.0;
    for (const double flat : tangentMm) {
        if (flat < 0.0) {
            out.why = "a flat between bends cannot be shorter than nothing";
            return out;
        }
        total += flat;
    }
    for (const SheetBend& bend : bends)
        total += BendAllowanceMm(bend, thicknessMm,
                                 KFactorFor(material, bend.innerRadiusMm, thicknessMm));
    out.ok = true;
    out.lengthMm = total;
    return out;
}

} // namespace paramcad
