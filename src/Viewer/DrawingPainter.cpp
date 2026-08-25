#include "Viewer/DrawingPainter.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <variant>

namespace paramcad {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kPi = kTwoPi / 2.0;

} // namespace

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

DrawnTally PaintDrawing(QPainter& painter, const DrawingDocument& document,
                        const DrawingTransform& page, const DrawingPaintOptions& options) {
    DrawnTally tally;
    const QColor paper = options.paper;
    const QColor ink = options.ink;

    // --- THE PAPER -----------------------------------------------------------
    const QRectF sheet = page.paperRect(document.sheet());
    if (options.fillPaper) painter.fillRect(sheet, paper);
    if (options.drawPaperEdge) {
        painter.setPen(QPen(QColor(0, 0, 0, 60), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(sheet);
    }

    // --- THE FRAME AND ITS ZONES (M35) ---------------------------------------
    //
    // FIRST, under everything, because it is the paper's own edge and not part
    // of what is drawn on it. Nothing here comes out of the document's object
    // list: it is all a function of the sheet, asked for at paint time (see
    // SheetFrame.h).
    const SheetFrameGeometry frame = document.frame();
    if (document.isFrameVisible() && frame.ok) {
        const double lineWidth = std::max(0.6, 0.7 * page.pixelsPerMm);
        painter.setPen(QPen(ink, lineWidth));
        painter.setBrush(Qt::NoBrush);
        const QRectF border(page.toScreen(frame.innerMinMm), page.toScreen(frame.innerMaxMm));
        painter.drawRect(border.normalized());
        ++tally.frameLines;

        // The zone strip: a thin band between the paper's edge and the border,
        // ticked at every division and lettered in the middle of each.
        const double stripPx = frame.zoneStripMm * page.pixelsPerMm;
        QFont zoneFont = painter.font();
        zoneFont.setPixelSize(std::max(5, static_cast<int>(3.5 * page.pixelsPerMm)));
        painter.setFont(zoneFont);
        const QFontMetricsF zoneMetrics(zoneFont);
        painter.setPen(QPen(ink, std::max(0.5, 0.35 * page.pixelsPerMm)));

        for (const SheetZone& zone : frame.zones) {
            // THE LABEL GOES ON BOTH OPPOSITE EDGES, which is what makes a
            // zone reference findable from whichever side of the sheet the
            // reader is looking at.
            for (int side = 0; side < 2; ++side) {
                QPointF tickFrom;
                QPointF tickTo;
                QPointF middle;
                if (zone.isRow) {
                    const double y = frame.innerMinMm.y + zone.toMm;
                    const double x = side == 0 ? frame.innerMinMm.x : frame.innerMaxMm.x;
                    const double outward = side == 0 ? -stripPx : stripPx;
                    tickFrom = page.toScreen(Vec2{x, y});
                    tickTo = tickFrom + QPointF(outward, 0.0);
                    middle = page.toScreen(
                                 Vec2{x, frame.innerMinMm.y + (zone.fromMm + zone.toMm) / 2.0}) +
                             QPointF(outward / 2.0, 0.0);
                } else {
                    const double x = frame.innerMinMm.x + zone.toMm;
                    const double y = side == 0 ? frame.innerMinMm.y : frame.innerMaxMm.y;
                    const double outward = side == 0 ? stripPx : -stripPx;
                    tickFrom = page.toScreen(Vec2{x, y});
                    tickTo = tickFrom + QPointF(0.0, outward);
                    middle = page.toScreen(
                                 Vec2{frame.innerMinMm.x + (zone.fromMm + zone.toMm) / 2.0, y}) +
                             QPointF(0.0, outward / 2.0);
                }
                // NOT ON THE LAST DIVISION'S FAR EDGE: that tick is the border
                // itself, and drawing it again doubles the corner's weight.
                const double edge = zone.isRow ? frame.innerHeightMm() : frame.innerWidthMm();
                if (std::fabs(zone.toMm - edge) > 1e-9) painter.drawLine(tickFrom, tickTo);

                const QString label = QString::fromStdString(zone.label);
                const double width = zoneMetrics.horizontalAdvance(label);
                painter.drawText(
                    QPointF(middle.x() - width / 2.0, middle.y() + zoneMetrics.ascent() / 2.0),
                    label);
            }
        }
    }

    // --- THE TITLE BLOCK (M35) -----------------------------------------------
    //
    // What a field PRINTS is asked of the block, which asks the sheet -- so
    // the scale in this corner and the scale the views below are plotted at
    // cannot be two different answers (see TitleBlock.h).
    if (document.titleBlock().isVisible()) {
        const TitleBlock& block = document.titleBlock();
        const Vec2 origin = document.titleBlockOriginMm();
        const double rowMm = block.rowHeightMm();
        painter.setPen(QPen(ink, std::max(0.6, 0.5 * page.pixelsPerMm)));
        painter.setBrush(Qt::NoBrush);
        const QRectF box(page.toScreen(origin),
                         page.toScreen(Vec2{origin.x + block.widthMm(),
                                            origin.y + block.heightMm()}));
        painter.drawRect(box.normalized());

        QFont rowFont = painter.font();
        rowFont.setPixelSize(std::max(5, static_cast<int>(2.5 * page.pixelsPerMm)));
        painter.setFont(rowFont);
        const QFontMetricsF rowMetrics(rowFont);
        // The label column is a third of the width, which leaves the value the
        // two thirds it needs -- a drawing number is longer than the words
        // "Drawing No.".
        const double labelWidthMm = block.widthMm() / 3.0;

        // WHERE EACH ROW SITS is asked of the block, not worked out here --
        // see TitleBlock::rowBottomMm for why.
        const std::size_t rowCount = block.fields().size();
        for (std::size_t i = 0; i < rowCount; ++i) {
            const TitleBlockField& field = block.fields()[i];
            const double bottom = block.rowBottomMm(i, origin.y);
            const QPointF left = page.toScreen(Vec2{origin.x, bottom});
            const QPointF right =
                page.toScreen(Vec2{origin.x + block.widthMm(), bottom});
            // No rule under the bottom row: the block's own border is already
            // there, and a second line doubles its weight.
            if (i + 1 != rowCount) {
                painter.setPen(QPen(ink, std::max(0.4, 0.25 * page.pixelsPerMm)));
                painter.drawLine(left, right);
            }
            const QPointF split = page.toScreen(Vec2{origin.x + labelWidthMm, bottom});
            painter.drawLine(split, split - QPointF(0.0, rowMm * page.pixelsPerMm));

            const double baseline =
                left.y() - (rowMm * page.pixelsPerMm - rowMetrics.ascent()) / 2.0 -
                rowMetrics.descent();
            painter.setPen(QPen(QColor(ink.red(), ink.green(), ink.blue(), 170)));
            painter.drawText(QPointF(left.x() + 1.0 * page.pixelsPerMm, baseline),
                             QString::fromStdString(field.label));
            painter.setPen(QPen(ink));
            painter.drawText(
                QPointF(split.x() + 1.0 * page.pixelsPerMm, baseline),
                QString::fromStdString(block.valueOf(field, document.sheet())));
            ++tally.titleBlockRows;
        }
    }

    // --- THE PARTS LISTS (M35.6) ---------------------------------------------
    //
    // COUNTED HERE, EVERY PAINT. The rows were never stored, so there is
    // nothing to keep in step -- and a drawing whose parts list said one thing
    // while the assembly said another is exactly what that buys.
    for (const BomTable* table : document.bomTables()) {
        const BomContents contents = document.countBom(*table);
        const Vec2 origin = table->positionMm();
        const double rowMm = table->rowHeightMm();
        const double widthMm = table->widthMm();
        const double direction = table->growsUpward() ? 1.0 : -1.0;

        QFont rowFont = painter.font();
        rowFont.setPixelSize(std::max(5, static_cast<int>(2.5 * page.pixelsPerMm)));
        painter.setFont(rowFont);
        const QFontMetricsF rowMetrics(rowFont);

        if (!contents.ok) {
            // A LIST THAT COULD NOT BE COUNTED SAYS SO, in red, where the list
            // would have been. An empty box and a list of nothing look the
            // same on paper, and only one of them is a drawing somebody can
            // build from.
            painter.setPen(QPen(QColor(200, 40, 40), std::max(0.6, 0.5 * page.pixelsPerMm)));
            painter.setBrush(Qt::NoBrush);
            const QRectF box(page.toScreen(origin),
                             page.toScreen(Vec2{origin.x + widthMm,
                                                origin.y + direction * rowMm}));
            painter.drawRect(box.normalized());
            painter.drawText(QPointF(box.normalized().left() + 2.0,
                                     box.normalized().center().y() + rowMetrics.ascent() / 2.0),
                             QString::fromStdString(table->name() + ": " + contents.why));
            ++tally.bomUncounted;
            continue;
        }

        // THE HEADING IS A ROW TOO, so the box is one taller than the parts.
        const std::size_t rowCount = contents.rows.size() + 1;
        painter.setPen(QPen(ink, std::max(0.6, 0.5 * page.pixelsPerMm)));
        painter.setBrush(Qt::NoBrush);
        const QRectF box(page.toScreen(origin),
                         page.toScreen(Vec2{origin.x + widthMm,
                                            origin.y + direction * rowMm *
                                                           static_cast<double>(rowCount)}));
        painter.drawRect(box.normalized());

        for (std::size_t i = 0; i < rowCount; ++i) {
            const bool heading = (i == 0);
            // ASKED OF THE TABLE, not worked out here -- see
            // BomTable::rowBottomMm.
            const double bottom = table->rowBottomMm(i);
            const QPointF left = page.toScreen(Vec2{origin.x, bottom});
            // A RULE UNDER EVERY ROW EXCEPT THE ONE THAT IS THE TABLE'S OWN
            // BORDER -- drawing it twice doubles that edge's weight.
            //
            // WHICH row that is DEPENDS ON THE DIRECTION: growing upward, the
            // heading is at the bottom and its lower edge is the border;
            // growing downward, the last row's is. The first draft skipped the
            // last row either way, so an upward table -- which is the default,
            // and the one in every screenshot -- lost the rule between its
            // heading and its first part. Found by looking at it.
            if (!table->rowBottomIsBorder(i, rowCount)) {
                painter.setPen(QPen(ink, std::max(0.4, 0.25 * page.pixelsPerMm)));
                painter.drawLine(left, page.toScreen(Vec2{origin.x + widthMm, bottom}));
            }
            const double baseline = left.y() -
                                    (rowMm * page.pixelsPerMm - rowMetrics.ascent()) / 2.0 -
                                    rowMetrics.descent();

            double atMm = origin.x;
            for (const BomColumn column : table->columns()) {
                const double columnMm = table->columnWidthMm(column);
                if (atMm > origin.x) {
                    painter.setPen(QPen(ink, std::max(0.4, 0.25 * page.pixelsPerMm)));
                    painter.drawLine(page.toScreen(Vec2{atMm, bottom}),
                                     page.toScreen(Vec2{atMm, bottom + direction * rowMm}));
                }
                const std::string text =
                    heading ? std::string(HeadingOf(column))
                            : contents.rows[i - 1].cell(column);
                painter.setPen(QPen(ink));
                painter.drawText(QPointF(page.toScreen(Vec2{atMm, bottom}).x() +
                                             1.0 * page.pixelsPerMm,
                                         baseline),
                                 QString::fromStdString(text));
                atMm += columnMm;
            }
            if (!heading) ++tally.bomRows;
        }
    }

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
    for (const DrawingEntity* entity : document.entities()) {
        if (!document.isEntityVisible(*entity)) continue;
        const int aci = document.resolvedColorOf(*entity);
        QPen pen(ScreenColorOf(aci, ink));
        // LINEWEIGHT IS IN HUNDREDTHS OF A MILLIMETRE, which is DXF's unit --
        // turned into pixels here, at the zoom, so a 0.5 mm line looks like a
        // 0.5 mm line at every magnification.
        const int weight = document.resolvedLineweightOf(*entity);
        pen.setWidthF(weight > 0 ? std::max(0.8, (weight / 100.0) * page.pixelsPerMm) : 1.2);
        const std::string linetype = document.resolvedLinetypeOf(*entity);
        const Linetype* pattern = document.findLinetypeNamed(linetype);
        if (pattern != nullptr && !pattern->isContinuous()) {
            // THE PATTERN'S OWN LENGTHS, scaled to pixels. Qt wants them as
            // multiples of the pen width, so the conversion is here and the
            // table keeps the drawing units DXF stores.
            QList<qreal> dashes;
            for (const double segment : pattern->pattern())
                dashes << std::max(0.1, std::fabs(segment) * page.pixelsPerMm / std::max(0.1, pen.widthF()));
            if (dashes.size() % 2 == 1) dashes << dashes.first();
            pen.setDashPattern(dashes);
        }
        painter.setPen(pen);

        if (const auto* text = std::get_if<DrawText>(&entity->shape())) {
            painter.save();
            const QPointF at = page.toScreen(text->at);
            painter.translate(at);
            painter.rotate(-text->rotation * 180.0 / (kTwoPi / 2.0));
            QFont font = painter.font();
            // THE HEIGHT IS A CAP HEIGHT IN MILLIMETRES, which is how a
            // drawing specifies text (ISO 3098) -- not a point size.
            font.setPixelSize(std::max(1, static_cast<int>(text->heightMm * page.pixelsPerMm)));
            painter.setFont(font);
            painter.drawText(QPointF(0.0, 0.0), QString::fromStdString(text->text));
            painter.restore();
            ++tally.curves;
            continue;
        }
        if (const auto* line = std::get_if<DrawLine>(&entity->shape())) {
            painter.drawLine(page.toScreen(line->a), page.toScreen(line->b));
        } else if (const auto* circle = std::get_if<DrawCircle>(&entity->shape())) {
            const QPointF centre = page.toScreen(circle->centre);
            const double r = circle->radius * page.pixelsPerMm;
            painter.drawEllipse(QRectF(centre.x() - r, centre.y() - r, 2.0 * r, 2.0 * r));
        } else if (const auto* arc = std::get_if<DrawArc>(&entity->shape())) {
            const QPointF centre = page.toScreen(arc->centre);
            const double r = arc->radius * page.pixelsPerMm;
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
            const QPointF at = page.toScreen(point->at);
            painter.drawLine(at + QPointF(-3, 0), at + QPointF(3, 0));
            painter.drawLine(at + QPointF(0, -3), at + QPointF(0, 3));
        } else {
            // Ellipses and polylines, through the entity's own flattening --
            // the one place a bulge becomes geometry.
            const std::vector<Vec2> points = entity->flatten(0.05 / std::max(0.05, page.pixelsPerMm));
            QPainterPath path;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const QPointF at = page.toScreen(points[i]);
                if (i == 0)
                    path.moveTo(at);
                else
                    path.lineTo(at);
            }
            painter.drawPath(path);
        }
        ++tally.curves;
    }

    // --- THE VIEWS -----------------------------------------------------------
    for (const DrawingView* view : document.views()) {
        if (view->currentState() != ComputeState::Valid) continue;
        // MODEL MILLIMETRES TO SHEET MILLIMETRES, through the document -- the
        // curves never carry the scale (see ProjectedGeometry.h), and
        // viewPointToSheetMm is the one place it is applied.
        const double factor = document.viewScaleFactor(view->id());
        const auto place = [&](Vec2 modelMm) {
            return page.toScreen(document.viewPointToSheetMm(view->id(), modelMm));
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
                const double radiusPx = arc->radius * factor * page.pixelsPerMm;
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
            ++tally.curves;
            if (hidden) ++tally.hidden;
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
    for (const DrawingDimension* dimension : document.dimensions()) {
        const DimensionStyle* style = document.findDimensionStyle(dimension->styleId());
        if (style == nullptr) continue;
        const DimensionMeasurement measured = document.measure(*dimension);
        // WHERE THE DIMENSION LINE IS RIGHT NOW: under the pointer while it is
        // being dragged, and the document's answer otherwise. The document is
        // not told until the button comes up (see dimensionDropped).
        const Vec2 lineAtMm = dimension->id() == options.draggingDimension
                                  ? options.draggedToMm
                                  : dimension->linePositionMm();

        const int aci = document.resolvedColorOfDimension(*dimension);
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
            font.setPixelSize(std::max(6, static_cast<int>(style->scaledTextHeightMm() * page.pixelsPerMm)));
            painter.setFont(font);
            painter.drawText(page.toScreen(lineAtMm),
                             QString::fromStdString(document.dimensionText(*dimension)));
            ++tally.dimensions;
            ++tally.dangling;
            continue;
        }

        const double arrowPx = style->scaledArrowSizeMm() * page.pixelsPerMm;
        const double gapPx = style->scaledExtensionGapMm() * page.pixelsPerMm;
        const double overshootPx = style->scaledExtensionOvershootMm() * page.pixelsPerMm;
        const double textGapPx = style->scaledTextGapMm() * page.pixelsPerMm;
        const QString text = QString::fromStdString(document.dimensionText(*dimension));
        QFont font = painter.font();
        font.setPixelSize(std::max(6, static_cast<int>(style->scaledTextHeightMm() * page.pixelsPerMm)));
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
                const QPointF a = page.toScreen(from);
                const QPointF b = page.toScreen(to);
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
            const QPointF a = page.toScreen(firstOn);
            const QPointF b = page.toScreen(secondOn);
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
            const QPointF from = page.toScreen(fromMm);
            const QPointF edge = page.toScreen(rimMm);
            const QPointF at = page.toScreen(lineAtMm);
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
            const QPointF screenVertex = page.toScreen(vertex);
            const double radiusPx = radius * page.pixelsPerMm;
            const QRectF box(screenVertex.x() - radiusPx, screenVertex.y() - radiusPx,
                             2.0 * radiusPx, 2.0 * radiusPx);
            // Qt counts sixteenths of a degree counter-clockwise and its Y
            // axis runs the other way to the sheet's, so both angles are
            // negated -- the same flip the authored arcs above make.
            painter.drawArc(box, static_cast<int>(-from * 180.0 / kPi * 16.0),
                            static_cast<int>(-sweep * 180.0 / kPi * 16.0));

            const auto onArc = [&](double angle) {
                return page.toScreen(Vec2{vertex.x + std::cos(angle) * radius,
                                     vertex.y + std::sin(angle) * radius});
            };
            painter.drawLine(screenVertex, page.toScreen(measured.firstMm));
            painter.drawLine(screenVertex, page.toScreen(measured.secondMm));
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
        ++tally.dimensions;
    }

    return tally;
}

} // namespace paramcad
