#pragma once

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Drawing/ObjectSnap.h"
#include "Viewer/DrawingPainter.h"

#include <QWidget>

#include <string>
#include <vector>

class QPaintEvent;
class QMouseEvent;
class QWheelEvent;
class QKeyEvent;

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
// WHAT THE LEFT BUTTON IS CURRENTLY FOR (M33/M34).
//
// A drawing canvas with no tool is a picture viewer. These are the modes the
// pointer can be in, and `None` -- select and drag -- is the one it returns to
// after every completed pick, because a tool that stayed armed is how a user
// ends up with nine lines they did not want.
enum class DrawingTool { None, Line, Circle, Rectangle, Wire };

// How many clicks each tool needs before it has something to make. Written
// once, HERE, so the canvas and the window cannot disagree about whether a
// circle is done -- the disagreement would show up as a tool that never
// finishes, which looks exactly like a dead button.
int ClicksNeededBy(DrawingTool tool) noexcept;

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
    std::size_t drawnDimensionCountForTesting() const noexcept { return drawnDimensions_; }
    // Dimensions that painted "<?>" instead of a number. Counted separately so
    // a self test can assert the alarm actually reached the screen, rather
    // than trusting that the document knowing is the same as the reader
    // seeing.
    std::size_t danglingDrawnForTesting() const noexcept { return danglingDrawn_; }
    std::size_t drawnFrameLinesForTesting() const noexcept { return drawnFrameLines_; }
    std::size_t drawnTitleBlockRowsForTesting() const noexcept { return drawnTitleRows_; }
    std::size_t drawnBomRowsForTesting() const noexcept { return drawnBomRows_; }
    std::size_t drawnUncountedBomsForTesting() const noexcept { return drawnUncounted_; }
    std::size_t drawnWiresForTesting() const noexcept { return drawnWires_; }
    std::size_t drawnSymbolsForTesting() const noexcept { return drawnSymbols_; }
    std::size_t drawnUnknownSymbolsForTesting() const noexcept { return drawnUnknown_; }
    std::size_t drawnJunctionsForTesting() const noexcept { return drawnJunctions_; }
    Box2D drawnSymbolExtentForTesting() const noexcept { return drawnSymbolExtent_; }
    std::size_t drawnSymbolCountForTesting() const noexcept { return drawnSymbols2_; }
    std::size_t drawnRevisionTablesForTesting() const noexcept { return drawnRevisionTables_; }
    std::size_t drawnDetailCirclesForTesting() const noexcept { return drawnDetailCircles_; }
    std::size_t drawnBreakLinesForTesting() const noexcept { return drawnBreakLines_; }
    std::size_t drawnRevisionRowsForTesting() const noexcept { return drawnRevisionRows_; }
    std::size_t danglingSymbolsForTesting() const noexcept { return danglingSymbols_; }
    std::size_t unreadableSymbolsForTesting() const noexcept { return unreadableSymbols_; }
    std::size_t drawnHoleRowsForTesting() const noexcept { return drawnHoleRows_; }
    std::size_t drawnHoleTagsForTesting() const noexcept { return drawnHoleTags_; }
    std::size_t uncountedHoleTablesForTesting() const noexcept { return uncountedHoleTables_; }
    std::size_t drawnHatchLinesForTesting() const noexcept { return drawnHatch_; }
    std::size_t drawnSectionArrowsForTesting() const noexcept { return drawnArrows_; }
    std::size_t unhatchedSectionsForTesting() const noexcept { return unhatched_; }
    // The sheet's rectangle in WIDGET pixels, so a test can check the paper is
    // actually inside the window after a fit.
    QRectF sheetRectForTesting() const;

    // --- Tools (M33/M34) ------------------------------------------------------
    void setTool(DrawingTool tool);
    DrawingTool tool() const noexcept { return tool_; }
    // The clicks banked so far towards the current tool. Non-empty means a
    // half-drawn thing is on screen, which is why Esc has to be able to reach
    // it.
    std::size_t pendingClickCountForTesting() const noexcept { return picked_.size(); }
    // Drives a pick from a test without a mouse. Sheet millimetres, so a test
    // says where in the drawing rather than where on somebody's monitor.
    void pickForTesting(Vec2 sheetMm);
    Vec2 toSheetForTesting(QPointF widgetPx) const { return toSheet(widgetPx); }
    // WHERE THE POINTER IS, in sheet millimetres (M40).
    //
    // Every editing tool has two equally valid answers -- which piece of a
    // trimmed line goes, which side an offset lands on -- and the pick point
    // is the only thing that distinguishes them. It is why those tools live on
    // shortcuts: the pointer has to still be over the piece you mean.
    Vec2 pointerSheetMm() const noexcept { return cursorMm_; }

signals:
    // A tool collected everything it needs. The WINDOW makes the geometry --
    // the canvas never touches the document, which is the same split the
    // sketch canvas keeps (A02).
    void toolFinished(DrawingTool tool, std::vector<Vec2> pointsMm);
    // Something on the paper was clicked, or nothing was.
    void objectPicked(ObjectId id);
    // A dimension was dragged to a new place for its line, and LET GO.
    //
    // ON RELEASE, not on every mouse move: one gesture is one edit, and a drag
    // that recorded a step per pixel would leave a user pressing Ctrl+Z two
    // hundred times to undo one nudge. While the button is down the canvas
    // paints the dimension at `dragToMm_` and the document does not know.
    void dimensionDropped(ObjectId id, Vec2 toMm);

public:

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    // The pan and the zoom, as the shared painter wants them. THE widget's
    // whole contribution to how a drawing looks: everything else about the
    // picture is the same on screen as it is on a page, which is why it lives
    // in DrawingPainter and not here.
    DrawingTransform transform() const noexcept { return DrawingTransform{originPx_, zoom_}; }
    QPointF toScreen(Vec2 sheetMm) const { return transform().toScreen(sheetMm); }
    // ...and back. The INVERSE of toScreen and nothing else -- a second
    // hand-written conversion is exactly the defect this project keeps
    // finding, and here it would put a click a few millimetres from where the
    // user aimed.
    Vec2 toSheet(QPointF widgetPx) const;
    // Where a pick actually lands: the snap point when one is near, otherwise
    // the raw position.
    Vec2 snapped(Vec2 rawMm) const;
    double pixelsPerMm() const noexcept { return zoom_; }

    DrawingDocument* document_ = nullptr;
    // The pan, in widget pixels, of the sheet's bottom-left corner.
    QPointF originPx_{0.0, 0.0};
    double zoom_ = 2.0;
    bool panning_ = false;
    QPointF panFrom_{0.0, 0.0};

    std::size_t drawnCurves_ = 0;
    std::size_t drawnHidden_ = 0;
    std::size_t drawnDimensions_ = 0;
    std::size_t danglingDrawn_ = 0;
    std::size_t drawnFrameLines_ = 0;
    std::size_t drawnTitleRows_ = 0;
    std::size_t drawnBomRows_ = 0;
    std::size_t drawnUncounted_ = 0;
    std::size_t drawnWires_ = 0;
    std::size_t drawnSymbols_ = 0;
    std::size_t drawnUnknown_ = 0;
    std::size_t drawnJunctions_ = 0;
    Box2D drawnSymbolExtent_;
    std::size_t drawnHatch_ = 0;
    std::size_t drawnHoleRows_ = 0;
    // `drawnSymbols_` is already the schematic's component count (M36), so
    // this one carries the suffix rather than the other losing its name.
    std::size_t drawnSymbols2_ = 0;
    std::size_t drawnRevisionTables_ = 0;
    std::size_t drawnDetailCircles_ = 0;
    std::size_t drawnBreakLines_ = 0;
    std::size_t drawnRevisionRows_ = 0;
    std::size_t danglingSymbols_ = 0;
    std::size_t unreadableSymbols_ = 0;
    std::size_t drawnHoleTags_ = 0;
    std::size_t uncountedHoleTables_ = 0;
    std::size_t drawnArrows_ = 0;
    std::size_t unhatched_ = 0;

    DrawingTool tool_ = DrawingTool::None;
    std::vector<Vec2> picked_;
    Vec2 cursorMm_{0.0, 0.0};
    bool snapHit_ = false;
    // The dimension being dragged, and where the pointer took hold of it --
    // so it moves WITH the pointer instead of jumping its line position to
    // wherever the click landed.
    ObjectId draggingDimension_ = kInvalidObjectId;
    Vec2 dragGrabbedAtMm_{0.0, 0.0};
    Vec2 dragStartedFromMm_{0.0, 0.0};
    Vec2 dragToMm_{0.0, 0.0};
};

} // namespace paramcad
