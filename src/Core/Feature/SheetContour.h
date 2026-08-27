#pragma once

#include "Core/Feature/SheetMetalStandards.h"
#include "Core/Kernel/ProfileDefinition.h"

#include <string>
#include <vector>

namespace paramcad {

// M52 -- A SHEET METAL PART AS ITS OWN CROSS-SECTION.
//
// Draw the line the metal follows -- flange, bend, flange, bend, flange -- give
// it a thickness and a width, and that is a channel, an angle, a Z, a hat
// section, and most of the brackets in any machine. It is how a press brake
// operator reads the job, and it is the shape M51's arithmetic was already
// written in.
//
// TWO ANSWERS COME OUT OF ONE DESCRIPTION: the folded solid, and the flat
// blank. That is the whole reason to build it this way. A program where the
// model and the flat pattern are separate descriptions is a program where they
// can disagree -- and they disagree quietly, because each is self-consistent.

// ONE MORE FLANGE THAN BEND, MADE STRUCTURAL.
//
// M51 has to CHECK that, because it takes two loose lists and a caller can
// hand it any pair of lengths. Here a step IS a flange and the bend that
// follows it, and the run after the last bend is its own field -- so the count
// cannot be got wrong, and there is no rule to enforce.
//
// This is the same move as M47's weld side: not "test that the lists match",
// but "leave no way to write down a pair that does not".
struct ContourStep {
    // The flat run BEFORE this bend, measured between tangent lines.
    double flangeMm = 0.0;
    SheetBend bend;
    // WHICH WAY IT FOLDS, and there is no default that is safe.
    //
    // A Z and a channel are the same three lengths and the same two bends; the
    // only difference is that one turns the same way twice and the other turns
    // back. Same numbers, different part, and both look ordinary on a table of
    // dimensions.
    bool turnsLeft = true;
};

struct SheetContour {
    std::vector<ContourStep> steps;
    // The run after the last bend. A contour with no steps at all is a flat
    // strip of this length, which is a real part.
    double lastFlangeMm = 0.0;
};

// Why this contour cannot be built, or empty when it can.
//
// WHAT IT DOES NOT CHECK, said out loud: whether the chain crosses itself. A
// contour that folds back far enough to run into its own earlier flange is a
// self-intersecting profile, and the kernel refuses it with its own message
// rather than this returning a friendlier one it cannot actually justify.
// Claiming to check for something and missing cases is worse than not claiming.
std::string WhyContourRefused(const SheetContour& contour, SheetMaterial material,
                              double thicknessMm);

// THE FLAT BLANK'S LENGTH, through M51 -- not a second implementation of the
// same arithmetic. The contour is unpacked into the lists M51 already reads,
// so there is one bend allowance in this program and one place it is wrong if
// it is wrong.
FlatPatternResult ContourFlatLength(const SheetContour& contour, SheetMaterial material,
                                    double thicknessMm);

// THE FOLDED CROSS-SECTION, as a closed profile in the sketch's own u,v.
//
// The chain is walked once, producing the face the walk traces and the face a
// thickness away from it; the two are closed with a cap at each end. Both
// faces come from the SAME walk, so the solid cannot end up with one side
// bent to a different radius from the other.
struct ContourProfileResult {
    bool ok = false;
    std::vector<ProfileSegment> segments;
    std::string why;
};
ContourProfileResult ContourProfile(const SheetContour& contour, SheetMaterial material,
                                    double thicknessMm);

// HOW MUCH METAL THE FOLDED SOLID ACTUALLY CONTAINS, per millimetre of width.
//
// AND IT IS NOT THE FLAT BLANK'S AREA. This is the fact this file exists to
// state, because it looks like a bug and is not:
//
//   a bend's cross-section is an annulus segment, whose area is
//       angle/2 * ((R+T)^2 - R^2)
//   the flat blank spends, for the same bend,
//       T * angle * (R + K*T)
//
// Those are equal only when K is exactly one half. For every real material
// they differ -- by angle * T^2 * (1/2 - K) per bend -- because K is NOT a
// geometric property of the folded shape. It is a statement about how the
// metal STRETCHES, and metal on the outside of a bend gets thinner.
//
// Anyone who "fixes" the two to agree has silently set K to 0.5 and thrown the
// material table away. So both are offered, and the difference is a test.
double FoldedSectionAreaMm2(const SheetContour& contour, double thicknessMm);

} // namespace paramcad
