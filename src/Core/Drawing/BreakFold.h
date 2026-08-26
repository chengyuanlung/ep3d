#pragma once

#include "Core/Drawing/ProjectedGeometry.h"
#include "Core/Geometry/MathTypes.h"

#include <string>
#include <vector>

namespace paramcad {

// M50 -- THE BROKEN VIEW: a long part with its middle taken out.
//
// AND THE WHOLE MILESTONE IS ONE DECISION: DO NOT CUT THE MODEL. FOLD THE
// PAPER.
//
// The tempting implementation removes a span of the part from the projection
// and then remembers to add it back whenever a dimension crosses the gap. That
// is TWO FACTS ABOUT ONE LENGTH kept in step by hand -- this project's named
// defect -- and the way it fails is the worst kind: the drawing is entirely
// self-consistent, every number agrees with every other number, and the bar
// comes out of the shop 400 mm short.
//
// So nothing is removed. A break is a MAPPING FROM MODEL MILLIMETRES TO PAPER,
// applied in the one place that mapping already happens
// (DrawingDocument::viewPointToSheetMm), and it has an inverse. Everything
// that puts a thing on the paper goes through the fold; everything that
// MEASURES goes through the unfold and gets model millimetres back.
//
// A dimension across a break therefore reads the true length BY CONSTRUCTION.
// There is no rule to remember, because there is nowhere to write the second
// number down.

// WHERE THE MIDDLE WENT.
//
// `fromMm` and `toMm` are in the view's own MODEL millimetres, along the axis
// the break runs on -- the same units the projection is in, and the same units
// a dimension reads (see ProjectedGeometry.h).
struct BreakSpan {
    bool active = false;
    double fromMm = 0.0;
    double toMm = 0.0;
    // ALONG X for a long part lying down, along Y for one standing up.
    bool horizontal = true;
    // How much paper is left between the two halves, in model millimetres so
    // it scales with everything else in the view. A drafting choice, exposed
    // rather than buried: butted together the two halves read as one
    // continuous part, and the break symbols alone are a thin thing to hang
    // that on.
    double gapMm = 3.0;

    bool usable() const noexcept { return active && toMm - fromMm > 1e-9 && gapMm >= 0.0; }
};

// WHY THIS BREAK CANNOT BE DRAWN, or empty when it can. `extentFromMm` and
// `extentToMm` are the part's own reach along the break axis; pass them equal
// to skip that check (a view with nothing projected yet).
std::string WhyBreakRefused(const BreakSpan& span, double extentFromMm, double extentToMm);

// MODEL MILLIMETRES TO FOLDED MODEL MILLIMETRES, along the break axis.
//
// Below the break: unchanged. Above it: pulled back by what was removed, less
// the gap left showing. Inside it: the seam -- and nothing is drawn there,
// because that is the material the break took out.
double FoldAlongMm(double alongMm, const BreakSpan& span) noexcept;
// The other way. A point at the seam cannot be told apart from either lip, and
// the seam is exactly where nothing is anchored.
double UnfoldAlongMm(double foldedMm, const BreakSpan& span) noexcept;

// The same, as points -- the axis chosen by the span.
Vec2 FoldPointMm(Vec2 modelMm, const BreakSpan& span) noexcept;
Vec2 UnfoldPointMm(Vec2 foldedMm, const BreakSpan& span) noexcept;

// How much shorter the view draws than the part is. Offered so a caller that
// wants to lay out paper does not work it out from the ends.
double RemovedMm(const BreakSpan& span) noexcept;

// THE CURVES, CUT AT THE LIPS.
//
// Folding a curve's ENDPOINTS is not enough and the way it fails is invisible:
// a line running the whole length of the bar has both its ends outside the
// break, so both fold cleanly and it draws as one straight line STRAIGHT
// ACROSS THE GAP. The break symbols sit on top of a continuous edge, and the
// picture says the material is still there.
//
// So anything crossing the removed span is cut at its lips first. A piece of a
// line is a line and a piece of an arc is an arc -- the same rule the detail
// crop follows (DetailClip.h), and for the same reason: a hole cut by a break
// is still a hole to whatever wants to dimension it.
//
// Returns the curves unchanged when the span is not usable, so a caller has
// one path rather than two.
std::vector<ProjectedCurve> SplitAtBreak(const std::vector<ProjectedCurve>& curves,
                                         const BreakSpan& span);

} // namespace paramcad
