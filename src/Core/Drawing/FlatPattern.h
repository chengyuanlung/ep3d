#pragma once

#include "Core/Drawing/ProjectedGeometry.h"
#include "Core/Feature/SheetContour.h"
#include "Core/Geometry/MathTypes.h"

#include <string>
#include <vector>

namespace paramcad {

// M53 -- THE FLAT PATTERN: the shape to cut, before anything is folded.
//
// It is the last piece of the sheet metal block and the place that block meets
// the drawing side: a flat pattern is a KIND OF VIEW, the same way a section
// (M38) and a detail (M49) are, and it goes on a sheet with dimensions and a
// title like any other.
//
// AND IT IS NOT RECOVERED FROM THE SOLID. It is derived from the DESCRIPTION
// the part was folded from -- the chain of flanges and bends M52 already
// holds. This program can flatten what it folded, because it kept the sentence
// that folded it.
//
// WHICH MEANS IT CANNOT FLATTEN EVERYTHING, and that is said rather than
// guessed. A solid that arrived through STEP has no chain: there is no record
// of which faces were flanges, which cylinders were bends, or which way the
// metal was meant to go. Handing back a rectangle anyway would be a blank
// somebody would cut.
//
// THE FAILURE THIS IS FOR: a blank of exactly the right size with its bend
// lines in the wrong places. Everything measures correctly, the outline fits
// the material, and the operator folds three millimetres from where the
// designer meant. So the lines and the length come from ONE walk -- the same
// cumulative sum, not a second one that agrees today.

// WHERE A FOLD GOES ON THE BLANK, and what to do there.
struct BendLine {
    // Distance from the near end of the blank to where the bend STARTS, and
    // where it ends. A bend is not a line on the metal: it is a band as wide
    // as the allowance, and drawing it as one line leaves the operator to
    // decide which edge of it the press meets.
    double fromMm = 0.0;
    double toMm = 0.0;
    // The angle the metal turns through, and which way.
    double angleDeg = 0.0;
    bool turnsLeft = true;
    double innerRadiusMm = 0.0;

    double middleMm() const noexcept { return 0.5 * (fromMm + toMm); }
};

struct FlatPatternResultGeometry {
    bool ok = false;
    std::string why;
    // The blank: length along the chain, width across it.
    double lengthMm = 0.0;
    double widthMm = 0.0;
    std::vector<BendLine> bendLines;
};

// THE BLANK AND ITS FOLDS, from the contour that made the part.
//
// One walk: the running total that places each bend line IS the running total
// that ends as the blank's length. There is no second sum to agree with.
FlatPatternResultGeometry FlatPatternOf(const SheetContour& contour, SheetMaterial material,
                                        double thicknessMm, double widthMm);

// WHAT A DRAWING DRAWS OF IT: the outline, and a line at each edge of every
// bend band, as ordinary projected curves in model millimetres -- so the
// dimensions, the anchors and the break views all work on it exactly as they
// do on any other view.
//
// THE BEND LINES ARE CURVES, not an annotation layer of their own. A flat
// pattern whose folds were a different kind of object would be a view whose
// most important content nothing else on the drawing could measure to.
ProjectedDrawing FlatPatternDrawing(const FlatPatternResultGeometry& pattern);

} // namespace paramcad
