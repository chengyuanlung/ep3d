#pragma once

#include "Core/Drawing/DrawingDocument.h"

#include <QWidget>

#include <string>
#include <vector>

class QPaintEvent;
class QMouseEvent;
class QWheelEvent;

namespace paramcad {

// THE SHEET, ON SCREEN (M32.4).
//
// A drawing is looked at as a piece of paper, not as a scene: there is no
// perspective, no orbit, and the only camera is a pan and a zoom. So this is a
// QWidget with a QPainter, not the OCCT view -- and it is a THIRD page in the
// central stack rather than a mode of either existing one, for the reason the
// sketch canvas is its own page: the 3D navigation gestures and the 2D ones
// end up fighting over the same mouse buttons.
//
// IT DRAWS, IT DOES NOT DECIDE. Where a view sits, what it contains and which
// layer a curve is on are all answered by the document; this turns them into
// pixels. The one thing it owns is the pan and the zoom, which are
// presentation and never reach Core (A02).
class DrawingCanvasWidget : public QWidget {
    Q_OBJECT

public:
    explicit DrawingCanvasWidget(QWidget* parent = nullptr);

    // Non-owning, and null when the window is not looking at a drawing.
    void setDocument(DrawingDocument* document);
    const DrawingDocument* document() const noexcept { return document_; }

    // Fits the whole sheet in the window. The state a drawing is opened in,
    // because a drawing that opens zoomed into one corner looks broken.
    void fitSheet();

    // --- Readbacks, so a self test can ask what is on the screen -------------
    //
    // The M6.14 lesson, applied ahead of time: "the document holds the curves"
    // and "the curves are on screen" are two claims, and the gap between them
    // is where a shell defect lives.
    std::size_t drawnCurveCountForTesting() const noexcept { return drawnCurves_; }
    std::size_t drawnHiddenCurveCountForTesting() const noexcept { return drawnHidden_; }
    // The sheet's rectangle in WIDGET pixels, so a test can check the paper is
    // actually inside the window after a fit.
    QRectF sheetRectForTesting() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    // Sheet millimetres to widget pixels. ONE conversion, so the paper, the
    // views and the frame cannot end up on three different grids.
    QPointF toScreen(Vec2 sheetMm) const;
    double pixelsPerMm() const noexcept { return zoom_; }

    DrawingDocument* document_ = nullptr;
    // The pan, in widget pixels, of the sheet's bottom-left corner.
    QPointF originPx_{0.0, 0.0};
    double zoom_ = 2.0;
    bool panning_ = false;
    QPointF panFrom_{0.0, 0.0};

    std::size_t drawnCurves_ = 0;
    std::size_t drawnHidden_ = 0;
};

} // namespace paramcad
