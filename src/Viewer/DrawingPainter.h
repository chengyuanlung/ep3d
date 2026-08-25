#pragma once

#include "Core/Drawing/DrawingDocument.h"

#include <QColor>
#include <QPointF>
#include <QRectF>

#include <cstddef>

class QPainter;

namespace paramcad {

// HOW A DRAWING IS PAINTED -- ONCE (M35).
//
// This was inside DrawingCanvasWidget until the PDF plot needed it. Writing a
// second renderer for the page is the shape of defect this project keeps
// closing: two things that must agree, written by hand, each tested alone --
// and the way anybody finds out they disagree is that a plot does not look
// like what was on the screen, on a drawing that has already been sent out.
//
// So there is one painter. The screen calls it with a pan and a zoom; the page
// calls it with the transform that makes a millimetre a millimetre. What is
// NOT in here is everything that belongs to the screen alone -- the desk, the
// rubber band, the snap marker -- because none of those are part of the
// drawing.

// Sheet millimetres to device units. THE one conversion, so the paper, the
// frame, the views and the annotation cannot end up on four different grids.
struct DrawingTransform {
    QPointF originPx{0.0, 0.0}; // where sheet (0, 0) lands
    double pixelsPerMm = 1.0;

    QPointF toScreen(Vec2 sheetMm) const {
        // Y IS UP ON PAPER AND DOWN ON SCREEN. Flipped here, once -- a drawing
        // placed in one convention and drawn in the other is upside down in a
        // way that looks almost plausible.
        return QPointF(originPx.x() + sheetMm.x * pixelsPerMm,
                       originPx.y() - sheetMm.y * pixelsPerMm);
    }
    QRectF paperRect(const Sheet& sheet) const {
        return QRectF(toScreen(Vec2{0.0, 0.0}),
                      toScreen(Vec2{sheet.widthMm(), sheet.heightMm()}))
            .normalized();
    }
};

struct DrawingPaintOptions {
    QColor paper{255, 255, 255};
    QColor ink{24, 28, 34};
    // A SCREEN FILLS THE PAPER; A PAGE IS ALREADY PAPER. Filling it on a plot
    // wastes toner and, on a printer that cannot do white, comes out grey.
    bool fillPaper = true;
    // The thin outline round the sheet. It marks where the paper ENDS, which
    // on a page the reader can already see by holding it.
    bool drawPaperEdge = true;
    // Mid-drag, the screen paints a dimension where the pointer has it rather
    // than where the document still has it -- the document is not told until
    // the button comes up, so one gesture stays one undo step. A page is never
    // mid-gesture and leaves these alone.
    ObjectId draggingDimension = kInvalidObjectId;
    Vec2 draggedToMm{0.0, 0.0};
};

// WHAT REACHED THE PAINT. Returned rather than kept, so a caller that wants to
// assert "the curves are on screen" has something to assert -- the gap between
// "the document holds them" and "they were drawn" is where a shell defect
// lives (M6.14).
struct DrawnTally {
    std::size_t curves = 0;
    std::size_t hidden = 0;
    std::size_t dimensions = 0;
    std::size_t dangling = 0;
    std::size_t frameLines = 0;
    std::size_t titleBlockRows = 0;
    std::size_t bomRows = 0;
    // Lists that could not be counted -- the file has gone, or a sub-assembly
    // inside it could not be read. Counted separately so a self test can
    // assert the ALARM reached the screen, not just that the document knows.
    std::size_t bomUncounted = 0;
};

DrawnTally PaintDrawing(QPainter& painter, const DrawingDocument& document,
                        const DrawingTransform& page, const DrawingPaintOptions& options);

// The ACI colour table, on the presentation side where it belongs. `fallback`
// is what colour 7 means, which is black on paper and near-white on a dark
// desk -- a decision about the screen, not about the drawing.
QColor ScreenColorOf(int aci, const QColor& fallback);

} // namespace paramcad
