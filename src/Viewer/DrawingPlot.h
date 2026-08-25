#pragma once

#include "Core/Drawing/DrawingDocument.h"

#include "Viewer/DrawingPainter.h"

#include <QString>

namespace paramcad {

// PLOTTING A DRAWING (M35.4).
//
// THE POINT OF THIS FILE IS WHAT IS NOT IN IT.
//
// There is no drawing code here. The page is painted by the same
// `PaintDrawing` the screen uses, through a transform that makes a millimetre
// a millimetre -- because a second renderer for the page would be two things
// that must agree, written by hand, each tested alone, and the way anybody
// finds out they disagree is that a plot does not look like what was on the
// screen, on a drawing that has already been sent to a supplier.
//
// So what is here is only the two things a page has and a window does not: the
// paper it is printed on, and the scale that makes it true size.

struct PlotResult {
    bool ok = false;
    std::string why;
    // What the page came out as, in millimetres. Written down so a caller can
    // say "this really is A3" rather than trusting that asking for it worked.
    double widthMm = 0.0;
    double heightMm = 0.0;
    explicit operator bool() const noexcept { return ok; }
};

// Writes `document` to `path` as a PDF, one page, AT TRUE SIZE.
//
// 1:1 in millimetres, always: the drawing's own scale is already in the views
// (a 1:2 view is drawn half size on the sheet), so scaling the page as well
// would apply it twice. A printed A3 has to measure A3 under a rule, or every
// dimension on it is a lie a reader can check.
PlotResult PlotDrawingToPdf(const DrawingDocument& document, const QString& path,
                            int resolutionDpi = 600);

// THE TRANSFORM THAT MAKES A PAGE TRUE SIZE.
//
// Split out from the plot so it can be ASSERTED. A page that came out the
// right size with the drawing on it at half scale passes every check that only
// looks at the paper -- and it is the failure a reader finds with a rule,
// after the drawing has gone out.
//
// `sheetHeightMm` is needed because sheet Y runs up and a page's runs down, so
// sheet (0, 0) is the page's BOTTOM-left.
DrawingTransform PageTransformFor(double sheetHeightMm, int resolutionDpi) noexcept;

} // namespace paramcad
