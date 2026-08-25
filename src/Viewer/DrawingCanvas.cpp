#include "Viewer/DrawingCanvas.h"

#include "Viewer/DesignTokens.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <variant>

namespace paramcad {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kPi = kTwoPi / 2.0;

// AutoCAD Color Index, for the handful a drawing actually uses.
//
// ACI is what the FILE carries (see DrawingTables.h) because pen tables are
// keyed on it. Turning it into something a screen can show is a PRESENTATION
// job, so the table lives here rather than in Core -- and a drawing that went
// to a plotter would never come through this function at all.
QColor ScreenColorOf(int aci, const QColor& fallback) {
    switch (aci) {
        case 1: return QColor(255, 60, 60);    // red
        case 2: return QColor(255, 210, 60);   // yellow
        case 3: return QColor(80, 200, 90);    // green
        case 4: return QColor(70, 200, 210);   // cyan
        case 5: return QColor(80, 140, 240);   // blue
        case 6: return QColor(220, 110, 220);  // magenta
        case 7: return fallback;               // "white/black" -- whatever reads on this ground
        case 8: return QColor(128, 128, 128);
        case 9: return QColor(192, 192, 192);
        default: return fallback;
    }
}

} // namespace

DrawingCanvasWidget::DrawingCanvasWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(true);
    // KEYBOARD FOCUS, or Esc never reaches keyPressEvent and the only way out
    // of a tool is to pick another one.
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

void DrawingCanvasWidget::setDocument(DrawingDocument* document) {
    document_ = document;
    fitSheet();
    update();
}

Vec2 DrawingCanvasWidget::toSheet(QPointF widgetPx) const {
    // THE INVERSE OF toScreen, derived from it rather than written again --
    // two hand-kept conversions is the defect shape this project keeps
    // finding, and here it would put every click a few millimetres from where
    // the user aimed.
    if (!(zoom_ > 0.0)) return Vec2{0.0, 0.0};
    return Vec2{(widgetPx.x() - originPx_.x()) / zoom_,
                (originPx_.y() - widgetPx.y()) / zoom_};
}

Vec2 DrawingCanvasWidget::snapped(Vec2 rawMm) const {
    if (document_ == nullptr) return rawMm;
    // THE APERTURE IS IN PIXELS, converted to millimetres here. A tolerance
    // fixed in millimetres would swallow the whole drawing when zoomed out and
    // be unhittable when zoomed in -- snapping is a POINTING tolerance, and
    // pointing happens on the screen.
    constexpr double kAperturePx = 12.0;
    SnapSettings settings;
    settings.apertureMm = kAperturePx / std::max(0.01, zoom_);
    const SnapHit hit = SnapTo(document_->entities(), rawMm, settings, std::nullopt);
    return hit.found ? hit.at : rawMm;
}

int ClicksNeededBy(DrawingTool tool) noexcept {
    switch (tool) {
        case DrawingTool::None: return 0;
        case DrawingTool::Line: return 2;
        case DrawingTool::Circle: return 2;   // centre, then a point on the rim
        case DrawingTool::Rectangle: return 2;  // two opposite corners
        // A WIRE IS TWO CLICKS for now: from, to. Multi-segment runs are drawn
        // as several wires, which the netlist joins end to end -- so a corner
        // costs a click and nothing else.
        case DrawingTool::Wire: return 2;
    }
    return 0;
}

void DrawingCanvasWidget::setTool(DrawingTool tool) {
    tool_ = tool;
    // THE HALF-FINISHED PICK GOES WITH THE OLD TOOL. Keeping it would start
    // the new tool one click in, from a point the user chose for something
    // else -- the "why is my circle starting over there" defect.
    picked_.clear();
    setCursor(tool == DrawingTool::None ? Qt::ArrowCursor : Qt::CrossCursor);
    update();
}

void DrawingCanvasWidget::pickForTesting(Vec2 sheetMm) {
    cursorMm_ = sheetMm;
    if (tool_ == DrawingTool::None) {
        if (document_ != nullptr) {
            const std::vector<ObjectId> under =
                document_->entitiesNear(sheetMm, 12.0 / std::max(0.01, zoom_));
            emit objectPicked(under.empty() ? kInvalidObjectId : under.front());
        }
        return;
    }
    picked_.push_back(sheetMm);
    if (static_cast<int>(picked_.size()) >= ClicksNeededBy(tool_)) {
        const DrawingTool finished = tool_;
        std::vector<Vec2> points;
        points.swap(picked_);
        // BACK TO SELECT once a thing is made. A tool that stayed armed is how
        // a user ends up with nine lines they did not ask for.
        setTool(DrawingTool::None);
        emit toolFinished(finished, std::move(points));
    }
    update();
}

void DrawingCanvasWidget::fitSheet() {
    if (document_ == nullptr) return;
    const double sheetWidth = document_->sheet().widthMm();
    const double sheetHeight = document_->sheet().heightMm();
    if (!(sheetWidth > 0.0) || !(sheetHeight > 0.0)) return;

    const double margin = 24.0;
    const double usableWidth = std::max(1.0, width() - 2.0 * margin);
    const double usableHeight = std::max(1.0, height() - 2.0 * margin);
    zoom_ = std::min(usableWidth / sheetWidth, usableHeight / sheetHeight);
    // Centred, which is where a piece of paper belongs in a window.
    const double drawnWidth = sheetWidth * zoom_;
    const double drawnHeight = sheetHeight * zoom_;
    originPx_ = QPointF((width() - drawnWidth) / 2.0, (height() + drawnHeight) / 2.0);
}

QRectF DrawingCanvasWidget::sheetRectForTesting() const {
    if (document_ == nullptr) return QRectF();
    return transform().paperRect(document_->sheet());
}

void DrawingCanvasWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // A WINDOW THAT GREW SHOULD NOT LEAVE THE PAPER OFF THE EDGE. Refitting on
    // every resize costs the user their zoom, so it only happens while nothing
    // has been panned or zoomed by hand -- which is exactly the freshly-opened
    // case that matters.
    if (!panning_) fitSheet();
}

void DrawingCanvasWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // THE DESK IS THE SCREEN'S, not the drawing's. A plot has no desk, which
    // is exactly why it is painted here and not in PaintDrawing.
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    const QColor desk = dark ? QColor(24, 28, 32) : QColor(96, 100, 104);
    painter.fillRect(rect(), desk);

    drawnCurves_ = 0;
    drawnHidden_ = 0;
    drawnDimensions_ = 0;
    danglingDrawn_ = 0;
    drawnFrameLines_ = 0;
    drawnTitleRows_ = 0;
    drawnBomRows_ = 0;
    drawnUncounted_ = 0;
    drawnWires_ = 0;
    drawnSymbols_ = 0;
    drawnUnknown_ = 0;
    drawnJunctions_ = 0;
    drawnSymbolExtent_ = Box2D{};
    if (document_ == nullptr) return;

    DrawingPaintOptions options;
    options.paper = dark ? QColor(238, 240, 242) : QColor(255, 255, 255);
    options.ink = QColor(24, 28, 34);
    options.draggingDimension = draggingDimension_;
    options.draggedToMm = dragToMm_;
    const DrawnTally tally = PaintDrawing(painter, *document_, transform(), options);
    drawnCurves_ = tally.curves;
    drawnHidden_ = tally.hidden;
    drawnDimensions_ = tally.dimensions;
    danglingDrawn_ = tally.dangling;
    drawnFrameLines_ = tally.frameLines;
    drawnTitleRows_ = tally.titleBlockRows;
    drawnBomRows_ = tally.bomRows;
    drawnUncounted_ = tally.bomUncounted;
    drawnWires_ = tally.wires;
    drawnSymbols_ = tally.placedSymbols;
    drawnUnknown_ = tally.unknownSymbols;
    drawnJunctions_ = tally.junctions;
    drawnSymbolExtent_ = tally.symbolExtentMm;

    // --- WHAT THE TOOL IS ABOUT TO MAKE --------------------------------------
    //
    // The rubber band. A tool that showed nothing until the last click is one
    // where the user finds out what they drew after they have drawn it.
    if (tool_ != DrawingTool::None && !picked_.empty()) {
        QPen ghost(QColor(0, 120, 215, 200), 1.2, Qt::DashLine);
        painter.setPen(ghost);
        painter.setBrush(Qt::NoBrush);
        const QPointF from = toScreen(picked_.front());
        const QPointF to = toScreen(cursorMm_);
        if (tool_ == DrawingTool::Line) {
            painter.drawLine(from, to);
        } else if (tool_ == DrawingTool::Circle) {
            const double r = std::hypot(to.x() - from.x(), to.y() - from.y());
            painter.drawEllipse(QRectF(from.x() - r, from.y() - r, 2.0 * r, 2.0 * r));
        } else if (tool_ == DrawingTool::Rectangle) {
            painter.drawRect(QRectF(from, to).normalized());
        } else if (tool_ == DrawingTool::Wire) {
            painter.drawLine(from, to);
        }
    }
    // THE SNAP MARKER, so a user can see WHERE the click will land before
    // they make it. A snap that only announces itself afterwards is a snap
    // they have to undo to disbelieve.
    if (snapHit_) {
        painter.setPen(QPen(QColor(0, 150, 90), 1.4));
        painter.setBrush(Qt::NoBrush);
        const QPointF at = toScreen(cursorMm_);
        painter.drawRect(QRectF(at.x() - 4.0, at.y() - 4.0, 8.0, 8.0));
    }
}

void DrawingCanvasWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        panFrom_ = event->position();
        QWidget::mousePressEvent(event);
        return;
    }
    if (event->button() != Qt::LeftButton || document_ == nullptr) {
        QWidget::mousePressEvent(event);
        return;
    }

    const Vec2 raw = toSheet(event->position());
    const Vec2 at = snapped(raw);
    if (tool_ != DrawingTool::None) {
        pickForTesting(at);
        event->accept();
        return;
    }

    // --- SELECT, or TAKE HOLD OF A DIMENSION --------------------------------
    //
    // A dimension is checked FIRST, because it is drawn over everything and a
    // user who clicks the number they can see expects to get the number they
    // can see.
    const double aperture = 12.0 / std::max(0.01, zoom_);
    for (const DrawingDimension* dimension : document_->dimensions()) {
        const Vec2 line = dimension->linePositionMm();
        if (std::hypot(line.x - raw.x, line.y - raw.y) > aperture) continue;
        draggingDimension_ = dimension->id();
        dragGrabbedAtMm_ = raw;
        dragStartedFromMm_ = line;
        dragToMm_ = line;
        emit objectPicked(dimension->id());
        event->accept();
        return;
    }
    const std::vector<ObjectId> under = document_->entitiesNear(raw, aperture);
    emit objectPicked(under.empty() ? kInvalidObjectId : under.front());
    event->accept();
}

void DrawingCanvasWidget::mouseMoveEvent(QMouseEvent* event) {
    if (panning_) {
        originPx_ += event->position() - panFrom_;
        panFrom_ = event->position();
        update();
        QWidget::mouseMoveEvent(event);
        return;
    }

    const Vec2 raw = toSheet(event->position());
    if (draggingDimension_ != kInvalidObjectId) {
        // MOVED BY THE POINTER'S TRAVEL, not set to the pointer. Setting it
        // would snap the dimension line to wherever the click landed, which is
        // a jump the user did not ask for.
        dragToMm_ = Vec2{dragStartedFromMm_.x + (raw.x - dragGrabbedAtMm_.x),
                         dragStartedFromMm_.y + (raw.y - dragGrabbedAtMm_.y)};
        update();
        QWidget::mouseMoveEvent(event);
        return;
    }

    const Vec2 at = snapped(raw);
    snapHit_ = std::hypot(at.x - raw.x, at.y - raw.y) > 1e-9;
    cursorMm_ = at;
    // ONLY WHILE THERE IS SOMETHING TO SHOW. Repainting a whole sheet on every
    // mouse move with no tool armed is work nobody asked for.
    if (tool_ != DrawingTool::None || snapHit_) update();
    QWidget::mouseMoveEvent(event);
}

void DrawingCanvasWidget::keyPressEvent(QKeyEvent* event) {
    // ESC BACKS OUT, in two steps: first the half-drawn thing, then the tool.
    // One step would leave a user who hit Esc mid-line still armed, which is
    // the "why did it draw another one" complaint.
    if (event->key() == Qt::Key_Escape) {
        if (!picked_.empty()) {
            picked_.clear();
            update();
        } else if (tool_ != DrawingTool::None) {
            setTool(DrawingTool::None);
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void DrawingCanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) panning_ = false;
    if (event->button() == Qt::LeftButton && draggingDimension_ != kInvalidObjectId) {
        const ObjectId dropped = draggingDimension_;
        draggingDimension_ = kInvalidObjectId;
        // Only when it actually MOVED. A plain click on a dimension is a
        // selection, and recording an undo step for it would fill the history
        // with edits that changed nothing.
        if (std::hypot(dragToMm_.x - dragStartedFromMm_.x,
                       dragToMm_.y - dragStartedFromMm_.y) > 1e-9)
            emit dimensionDropped(dropped, dragToMm_);
        update();
    }
    QWidget::mouseReleaseEvent(event);
}

void DrawingCanvasWidget::wheelEvent(QWheelEvent* event) {
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) return;
    const double factor = std::pow(1.15, steps);
    // ZOOM TO THE CURSOR, not to the centre. Zooming to the centre makes a
    // reader chase the detail they were looking at across the window.
    const QPointF at = event->position();
    originPx_ = at + (originPx_ - at) * factor;
    zoom_ *= factor;
    panning_ = true; // the view is now the user's, so a resize must not refit it
    update();
    event->accept();
}

} // namespace paramcad
