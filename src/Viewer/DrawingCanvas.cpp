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

QPointF DrawingCanvasWidget::toScreen(Vec2 sheetMm) const {
    // Y IS UP ON PAPER AND DOWN ON SCREEN. Flipped here, once -- a drawing
    // whose views were placed in one convention and drawn in the other is
    // upside down in a way that looks almost plausible.
    return QPointF(originPx_.x() + sheetMm.x * zoom_, originPx_.y() - sheetMm.y * zoom_);
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
    drawnDimensions_ = 0;
    danglingDrawn_ = 0;
    if (document_ == nullptr) return;

    // --- THE PAPER -----------------------------------------------------------
    const QRectF sheet = sheetRectForTesting();
    painter.fillRect(sheet, paper);
    painter.setPen(QPen(QColor(0, 0, 0, 60), 1.0));
    painter.drawRect(sheet);

    // --- WHAT THE USER DREW (M33) --------------------------------------------
    //
    // BEFORE THE VIEWS, so a projected edge is never hidden by a centreline
    // drawn over it. What a user drew is annotation ON the drawing, and the
    // drawing is what it annotates.
    //
    // Colour and linetype come through the DOCUMENT's resolver, never read off
    // the entity: ByLayer is the default, and an entity that carried its own
    // colour would make changing a layer's colour change nothing -- which is
    // the whole reason layers exist.
    for (const DrawingEntity* entity : document_->entities()) {
        if (!document_->isEntityVisible(*entity)) continue;
        const int aci = document_->resolvedColorOf(*entity);
        QPen pen(ScreenColorOf(aci, ink));
        // LINEWEIGHT IS IN HUNDREDTHS OF A MILLIMETRE, which is DXF's unit --
        // turned into pixels here, at the zoom, so a 0.5 mm line looks like a
        // 0.5 mm line at every magnification.
        const int weight = document_->resolvedLineweightOf(*entity);
        pen.setWidthF(weight > 0 ? std::max(0.8, (weight / 100.0) * zoom_) : 1.2);
        const std::string linetype = document_->resolvedLinetypeOf(*entity);
        const Linetype* pattern = document_->findLinetypeNamed(linetype);
        if (pattern != nullptr && !pattern->isContinuous()) {
            // THE PATTERN'S OWN LENGTHS, scaled to pixels. Qt wants them as
            // multiples of the pen width, so the conversion is here and the
            // table keeps the drawing units DXF stores.
            QList<qreal> dashes;
            for (const double segment : pattern->pattern())
                dashes << std::max(0.1, std::fabs(segment) * zoom_ / std::max(0.1, pen.widthF()));
            if (dashes.size() % 2 == 1) dashes << dashes.first();
            pen.setDashPattern(dashes);
        }
        painter.setPen(pen);

        if (const auto* text = std::get_if<DrawText>(&entity->shape())) {
            painter.save();
            const QPointF at = toScreen(text->at);
            painter.translate(at);
            painter.rotate(-text->rotation * 180.0 / (kTwoPi / 2.0));
            QFont font = painter.font();
            // THE HEIGHT IS A CAP HEIGHT IN MILLIMETRES, which is how a
            // drawing specifies text (ISO 3098) -- not a point size.
            font.setPixelSize(std::max(1, static_cast<int>(text->heightMm * zoom_)));
            painter.setFont(font);
            painter.drawText(QPointF(0.0, 0.0), QString::fromStdString(text->text));
            painter.restore();
            ++drawnCurves_;
            continue;
        }
        if (const auto* line = std::get_if<DrawLine>(&entity->shape())) {
            painter.drawLine(toScreen(line->a), toScreen(line->b));
        } else if (const auto* circle = std::get_if<DrawCircle>(&entity->shape())) {
            const QPointF centre = toScreen(circle->centre);
            const double r = circle->radius * zoom_;
            painter.drawEllipse(QRectF(centre.x() - r, centre.y() - r, 2.0 * r, 2.0 * r));
        } else if (const auto* arc = std::get_if<DrawArc>(&entity->shape())) {
            const QPointF centre = toScreen(arc->centre);
            const double r = arc->radius * zoom_;
            const double startDeg = -arc->startAngle * 180.0 / (kTwoPi / 2.0);
            double sweep = arc->endAngle - arc->startAngle;
            while (sweep <= 0.0) sweep += kTwoPi;
            painter.drawArc(QRectF(centre.x() - r, centre.y() - r, 2.0 * r, 2.0 * r),
                            static_cast<int>(startDeg * 16.0),
                            static_cast<int>(-sweep * 180.0 / (kTwoPi / 2.0) * 16.0));
        } else if (const auto* point = std::get_if<DrawPoint>(&entity->shape())) {
            // A POINT IS DRAWN AS A CROSS at a FIXED PIXEL SIZE. A point has
            // no size in the model, so scaling its marker with the zoom would
            // make it vanish or swallow the drawing.
            const QPointF at = toScreen(point->at);
            painter.drawLine(at + QPointF(-3, 0), at + QPointF(3, 0));
            painter.drawLine(at + QPointF(0, -3), at + QPointF(0, 3));
        } else {
            // Ellipses and polylines, through the entity's own flattening --
            // the one place a bulge becomes geometry.
            const std::vector<Vec2> points = entity->flatten(0.05 / std::max(0.05, zoom_));
            QPainterPath path;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const QPointF at = toScreen(points[i]);
                if (i == 0)
                    path.moveTo(at);
                else
                    path.lineTo(at);
            }
            painter.drawPath(path);
        }
        ++drawnCurves_;
    }

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

    // --- THE DIMENSIONS (M34) ------------------------------------------------
    //
    // LAST, over everything, because a dimension is what the drawing is FOR
    // and a curve crossing its text is the reader's problem to move, not the
    // painter's to hide.
    //
    // Every length in here is PAPER millimetres from the style; every position
    // is SHEET millimetres from the document. The measured number came out of
    // `measure` in MODEL millimetres and is never recomputed here -- the whole
    // reason it is asked for rather than derived is that the canvas, a plot
    // and a DXF write must not be able to disagree about what the drawing
    // says.
    for (const DrawingDimension* dimension : document_->dimensions()) {
        const DimensionStyle* style = document_->findDimensionStyle(dimension->styleId());
        if (style == nullptr) continue;
        const DimensionMeasurement measured = document_->measure(*dimension);
        // WHERE THE DIMENSION LINE IS RIGHT NOW: under the pointer while it is
        // being dragged, and the document's answer otherwise. The document is
        // not told until the button comes up (see dimensionDropped).
        const Vec2 lineAtMm = dimension->id() == draggingDimension_
                                  ? dragToMm_
                                  : dimension->linePositionMm();

        const int aci = document_->resolvedColorOfDimension(*dimension);
        QColor colour = ScreenColorOf(aci, ink);
        QPen pen(colour);
        pen.setWidthF(1.0);

        // A DANGLING DIMENSION IS DRAWN IN RED WITH "<?>" WHERE ITS NUMBER
        // WAS. Hiding it would leave a drawing that has quietly stopped
        // measuring the part; drawing the last number it read would be worse.
        if (!measured.ok) {
            const QColor alarm(200, 40, 40);
            painter.setPen(QPen(alarm, 1.2));
            QFont font = painter.font();
            font.setPixelSize(std::max(6, static_cast<int>(style->scaledTextHeightMm() * zoom_)));
            painter.setFont(font);
            painter.drawText(toScreen(lineAtMm),
                             QString::fromStdString(document_->dimensionText(*dimension)));
            ++drawnDimensions_;
            ++danglingDrawn_;
            continue;
        }

        const double arrowPx = style->scaledArrowSizeMm() * zoom_;
        const double gapPx = style->scaledExtensionGapMm() * zoom_;
        const double overshootPx = style->scaledExtensionOvershootMm() * zoom_;
        const double textGapPx = style->scaledTextGapMm() * zoom_;
        const QString text = QString::fromStdString(document_->dimensionText(*dimension));
        QFont font = painter.font();
        font.setPixelSize(std::max(6, static_cast<int>(style->scaledTextHeightMm() * zoom_)));
        painter.setFont(font);
        painter.setPen(pen);

        // A FILLED CLOSED ARROWHEAD (ISO 129-1's default), pointing along
        // `along` with its tip at `tip`.
        const auto arrowhead = [&](QPointF tip, QPointF along) {
            const double length = std::hypot(along.x(), along.y());
            if (!(length > 1e-9) || !(arrowPx > 0.5)) return;
            const QPointF unit = along / length;
            const QPointF normal(-unit.y(), unit.x());
            const QPointF back = tip - unit * arrowPx;
            QPainterPath head;
            head.moveTo(tip);
            head.lineTo(back + normal * (arrowPx / 6.0));
            head.lineTo(back - normal * (arrowPx / 6.0));
            head.closeSubpath();
            painter.fillPath(head, colour);
        };

        // The text, centred on `at` and rotated with the dimension line, sat
        // ABOVE it by the text gap -- which is where ISO 129-1 puts it, and
        // why the gap is a style field rather than a constant in here.
        const auto placeText = [&](QPointF at, double radians) {
            painter.save();
            painter.translate(at);
            // Never upside down: past a quarter turn the text flips to stay
            // readable from the bottom or the right of the sheet, which is
            // what every drawing standard asks for.
            double degrees = -radians * 180.0 / kPi;
            while (degrees <= -90.0) degrees += 180.0;
            while (degrees > 90.0) degrees -= 180.0;
            painter.rotate(degrees);
            const QFontMetricsF metrics(painter.font());
            const double width = metrics.horizontalAdvance(text);
            painter.setPen(pen);
            painter.drawText(QPointF(-width / 2.0, -textGapPx), text);
            painter.restore();
        };

        if (dimension->kind() == DimensionKind::Linear) {
            // THE MEASURING DIRECTION decides everything else. Horizontal and
            // vertical are the aligned case with the direction replaced, which
            // is why there is one path here and not three.
            Vec2 axis{measured.secondMm.x - measured.firstMm.x,
                      measured.secondMm.y - measured.firstMm.y};
            if (dimension->direction() == LinearDirection::Horizontal) axis = Vec2{1.0, 0.0};
            if (dimension->direction() == LinearDirection::Vertical) axis = Vec2{0.0, 1.0};
            const double length = std::hypot(axis.x, axis.y);
            if (!(length > 1e-9)) continue;
            const Vec2 unit{axis.x / length, axis.y / length};

            // The dimension line runs through the position the user dragged
            // it to, along the measuring direction; each witness point lands
            // on it at its own projection.
            const Vec2 origin = lineAtMm;
            const auto onLine = [&](Vec2 point) {
                const double t = (point.x - origin.x) * unit.x + (point.y - origin.y) * unit.y;
                return Vec2{origin.x + unit.x * t, origin.y + unit.y * t};
            };
            const Vec2 firstOn = onLine(measured.firstMm);
            const Vec2 secondOn = onLine(measured.secondMm);

            // --- the two extension lines ---
            //
            // They start a GAP away from the feature, so a witness line never
            // touches the part it is measuring -- a drawing where they meet is
            // one where the reader cannot tell the annotation from the shape.
            const auto extension = [&](Vec2 from, Vec2 to) {
                const QPointF a = toScreen(from);
                const QPointF b = toScreen(to);
                const QPointF delta = b - a;
                const double run = std::hypot(delta.x(), delta.y());
                if (!(run > gapPx + 1.0)) return;
                const QPointF step = delta / run;
                painter.setPen(pen);
                painter.drawLine(a + step * gapPx, b + step * overshootPx);
            };
            extension(measured.firstMm, firstOn);
            extension(measured.secondMm, secondOn);

            // --- the dimension line and its arrows ---
            const QPointF a = toScreen(firstOn);
            const QPointF b = toScreen(secondOn);
            painter.setPen(pen);
            painter.drawLine(a, b);
            arrowhead(a, a - b);
            arrowhead(b, b - a);
            placeText((a + b) / 2.0, std::atan2(secondOn.y - firstOn.y, secondOn.x - firstOn.x));
        } else if (dimension->kind() == DimensionKind::Radius ||
                   dimension->kind() == DimensionKind::Diameter) {
            // A RADIUS RUNS FROM THE CENTRE TO THE RIM. A DIAMETER RUNS ALL
            // THE WAY ACROSS.
            //
            // Not a cosmetic difference: drawn as a radius, a diameter is a
            // line half the circle long with "40" beside it, and a reader who
            // trusts what they see reads the wrong size off the drawing. The
            // number and the line have to agree, so the line changes with the
            // kind.
            const Vec2 centreMm = measured.firstMm;
            const Vec2 rimMm = measured.secondMm;
            const bool across = dimension->kind() == DimensionKind::Diameter;
            const Vec2 fromMm = across
                                    ? Vec2{2.0 * centreMm.x - rimMm.x, 2.0 * centreMm.y - rimMm.y}
                                    : centreMm;
            const QPointF from = toScreen(fromMm);
            const QPointF edge = toScreen(rimMm);
            const QPointF at = toScreen(lineAtMm);
            painter.setPen(pen);
            painter.drawLine(from, edge);
            arrowhead(edge, edge - from);
            // A diameter has an arrow at BOTH rim points; a radius has one,
            // and its tail at the centre is where the centre mark goes.
            if (across) {
                arrowhead(from, from - edge);
            } else {
                const double markPx = std::max(2.0, arrowPx / 2.0);
                painter.drawLine(from + QPointF(-markPx, 0.0), from + QPointF(markPx, 0.0));
                painter.drawLine(from + QPointF(0.0, -markPx), from + QPointF(0.0, markPx));
            }
            painter.drawLine(edge, at);
            placeText(at, std::atan2(rimMm.y - centreMm.y, rimMm.x - centreMm.x));
        } else {
            // ANGULAR: an arc about the vertex, at the radius of the nearer
            // arm, with an arrow at each end and an arm line out to each
            // anchor.
            const Vec2 vertex = lineAtMm;
            const double firstRadius = std::hypot(measured.firstMm.x - vertex.x,
                                                  measured.firstMm.y - vertex.y);
            const double secondRadius = std::hypot(measured.secondMm.x - vertex.x,
                                                   measured.secondMm.y - vertex.y);
            const double radius = std::min(firstRadius, secondRadius);
            if (!(radius > 1e-9)) continue;
            const double from = std::atan2(measured.firstMm.y - vertex.y,
                                           measured.firstMm.x - vertex.x);
            const double to = std::atan2(measured.secondMm.y - vertex.y,
                                         measured.secondMm.x - vertex.x);
            // THE SHORT WAY ROUND, which is the angle `measure` reported --
            // drawing the long way would show an arc that does not match its
            // own number.
            double sweep = to - from;
            while (sweep <= -kPi) sweep += kTwoPi;
            while (sweep > kPi) sweep -= kTwoPi;

            painter.setPen(pen);
            const QPointF screenVertex = toScreen(vertex);
            const double radiusPx = radius * zoom_;
            const QRectF box(screenVertex.x() - radiusPx, screenVertex.y() - radiusPx,
                             2.0 * radiusPx, 2.0 * radiusPx);
            // Qt counts sixteenths of a degree counter-clockwise and its Y
            // axis runs the other way to the sheet's, so both angles are
            // negated -- the same flip the authored arcs above make.
            painter.drawArc(box, static_cast<int>(-from * 180.0 / kPi * 16.0),
                            static_cast<int>(-sweep * 180.0 / kPi * 16.0));

            const auto onArc = [&](double angle) {
                return toScreen(Vec2{vertex.x + std::cos(angle) * radius,
                                     vertex.y + std::sin(angle) * radius});
            };
            painter.drawLine(screenVertex, toScreen(measured.firstMm));
            painter.drawLine(screenVertex, toScreen(measured.secondMm));
            // The arrows sit ALONG the arc, so their direction is the tangent,
            // not the radius -- and the tangent flips with the turn, which is
            // why both use the sweep's sign.
            const double turn = sweep > 0.0 ? 1.0 : -1.0;
            arrowhead(onArc(from), QPointF(std::sin(from) * turn, std::cos(from) * turn));
            arrowhead(onArc(from + sweep),
                      QPointF(-std::sin(from + sweep) * turn, -std::cos(from + sweep) * turn));
            const double middle = from + sweep / 2.0;
            placeText(onArc(middle), middle + kPi / 2.0);
        }
        ++drawnDimensions_;
    }

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
