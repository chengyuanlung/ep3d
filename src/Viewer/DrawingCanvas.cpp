#include "Viewer/DrawingCanvas.h"

#include "Viewer/DesignTokens.h"

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace paramcad {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

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
}

void DrawingCanvasWidget::setDocument(DrawingDocument* document) {
    document_ = document;
    fitSheet();
    update();
}

QPointF DrawingCanvasWidget::toScreen(Vec2 sheetMm) const {
    // Y IS UP ON PAPER AND DOWN ON SCREEN. Flipped here, once -- a drawing
    // whose views were placed in one convention and drawn in the other is
    // upside down in a way that looks almost plausible.
    return QPointF(originPx_.x() + sheetMm.x * zoom_, originPx_.y() - sheetMm.y * zoom_);
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
    const QPointF bottomLeft = toScreen(Vec2{0.0, 0.0});
    const QPointF topRight =
        toScreen(Vec2{document_->sheet().widthMm(), document_->sheet().heightMm()});
    return QRectF(bottomLeft, topRight).normalized();
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

    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    const QColor desk = dark ? QColor(24, 28, 32) : QColor(96, 100, 104);
    const QColor paper = dark ? QColor(238, 240, 242) : QColor(255, 255, 255);
    const QColor ink = QColor(24, 28, 34);
    painter.fillRect(rect(), desk);

    drawnCurves_ = 0;
    drawnHidden_ = 0;
    if (document_ == nullptr) return;

    // --- THE PAPER -----------------------------------------------------------
    const QRectF sheet = sheetRectForTesting();
    painter.fillRect(sheet, paper);
    painter.setPen(QPen(QColor(0, 0, 0, 60), 1.0));
    painter.drawRect(sheet);

    // --- THE VIEWS -----------------------------------------------------------
    for (const DrawingView* view : document_->views()) {
        if (view->currentState() != ComputeState::Valid) continue;
        const Vec2 at = document_->viewPositionMm(view->id());
        // MODEL MILLIMETRES TIMES THE SCALE, here and only here. The curves
        // never carry the scale (see ProjectedGeometry.h), so this is the one
        // multiplication -- and it is also why a dimension can read the true
        // size straight off the curve.
        const double factor = view->effectiveScale(document_->sheet().scale()).factor();
        const auto place = [&](Vec2 modelMm) {
            return toScreen(Vec2{at.x + modelMm.x * factor, at.y + modelMm.y * factor});
        };

        for (const ProjectedCurve& curve : view->projected().curves) {
            // A LAYER IS NOT CONSULTED YET: projected curves are derived and
            // belong to no layer (M33 gives authored geometry layers). What
            // decides how they are drawn is the DRAWING CONVENTION -- solid
            // for visible, dashed for hidden -- which is a property of the
            // curve itself.
            const bool hidden = curve.visibility == ProjectedVisibility::Hidden;
            QPen pen(hidden ? QColor(ink.red(), ink.green(), ink.blue(), 150) : ink);
            pen.setWidthF(hidden ? 0.9 : 1.4);
            if (hidden) pen.setStyle(Qt::DashLine);
            if (curve.kind == ProjectedEdgeKind::Smooth) pen.setWidthF(0.7);
            painter.setPen(pen);

            if (const auto* line = std::get_if<ProjectedLine>(&curve.shape)) {
                painter.drawLine(place(line->a), place(line->b));
            } else if (const auto* arc = std::get_if<ProjectedArc>(&curve.shape)) {
                // DRAWN AS AN ARC, not as the polygon a tessellation would
                // give: the whole reason the exact projector is used is that a
                // circle stays a circle, and drawing it as segments here would
                // throw that away at the last step.
                const double radiusPx = arc->radius * factor * zoom_;
                const QPointF centre = place(arc->centre);
                const QRectF box(centre.x() - radiusPx, centre.y() - radiusPx, 2.0 * radiusPx,
                                 2.0 * radiusPx);
                if (arc->isFullCircle) {
                    painter.drawEllipse(box);
                } else {
                    // Qt counts sixteenths of a degree, counter-clockwise, and
                    // its Y axis runs the other way -- so the angles are
                    // negated along with the page flip.
                    const double startDeg = -arc->startAngle * 180.0 / (kTwoPi / 2.0);
                    double sweep = arc->endAngle - arc->startAngle;
                    while (sweep < 0.0) sweep += kTwoPi;
                    const double sweepDeg = -sweep * 180.0 / (kTwoPi / 2.0);
                    painter.drawArc(box, static_cast<int>(startDeg * 16.0),
                                    static_cast<int>(sweepDeg * 16.0));
                }
            } else if (const auto* polyline = std::get_if<ProjectedPolyline>(&curve.shape)) {
                QPainterPath path;
                for (std::size_t i = 0; i < polyline->points.size(); ++i) {
                    const QPointF point = place(polyline->points[i]);
                    if (i == 0)
                        path.moveTo(point);
                    else
                        path.lineTo(point);
                }
                painter.drawPath(path);
            }
            ++drawnCurves_;
            if (hidden) ++drawnHidden_;
        }

        // THE VIEW'S NAME AND SCALE, under it. A drawing with three unlabelled
        // views is one a reader has to work out, and the scale belongs beside
        // the view whose scale it is -- not only in the title block, which
        // carries the SHEET's.
        const QRectF box(place(Vec2{view->projected().extent.min.x,
                                    view->projected().extent.min.y}),
                         place(Vec2{view->projected().extent.max.x,
                                    view->projected().extent.max.y}));
        QString label = QString::fromStdString(view->name());
        if (view->hasOwnScale())
            label += QStringLiteral("  (%1)")
                         .arg(QString::fromStdString(view->scale().toString()));
        painter.setPen(QPen(QColor(ink.red(), ink.green(), ink.blue(), 190)));
        QFont small = painter.font();
        small.setPointSizeF(std::max(6.0, small.pointSizeF() - 1.0));
        painter.setFont(small);
        painter.drawText(QPointF(box.normalized().left(), box.normalized().bottom() + 14.0),
                         label);
    }
}

void DrawingCanvasWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        panFrom_ = event->position();
    }
    QWidget::mousePressEvent(event);
}

void DrawingCanvasWidget::mouseMoveEvent(QMouseEvent* event) {
    if (panning_) {
        originPx_ += event->position() - panFrom_;
        panFrom_ = event->position();
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void DrawingCanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) panning_ = false;
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
