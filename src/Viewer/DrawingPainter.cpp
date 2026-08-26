#include "Viewer/DrawingPainter.h"

#include "Core/Drawing/Hatch.h"
#include "Core/Electrical/SymbolLibrary.h"

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
                // THROUGH THE DOCUMENT (M48). It is the only thing that knows
                // every fact a block derives -- which page, how many, and what
                // issue the drawing is at -- so it is the only caller.
                QString::fromStdString(document.titleBlockValue(field)));
            ++tally.titleBlockRows;
        }
    }

    // --- THE SCHEMATIC (M36) --------------------------------------------------
    //
    // WIRES FIRST, then symbols on top: a symbol's body should hide the wire
    // that runs under it, not the other way round.
    {
        QPen wirePen(ScreenColorOf(4, ink)); // cyan by convention, over ByLayer
        wirePen.setWidthF(std::max(0.8, 0.35 * page.pixelsPerMm));
        QFont labelFont = painter.font();
        labelFont.setPixelSize(std::max(5, static_cast<int>(2.0 * page.pixelsPerMm)));
        const QFontMetricsF labelMetrics(labelFont);

        for (const WireEntity* wire : document.wires()) {
            if (!wire->isDrawable()) continue;
            painter.setPen(wirePen);
            painter.setBrush(Qt::NoBrush);
            QPainterPath path;
            for (std::size_t i = 0; i < wire->pointsMm().size(); ++i) {
                const QPointF at = page.toScreen(wire->pointsMm()[i]);
                if (i == 0)
                    path.moveTo(at);
                else
                    path.lineTo(at);
            }
            painter.drawPath(path);
            ++tally.wires;

            // THE WIRE NUMBER, along the run. A schematic whose wires are
            // numbered only in a list is one an electrician has to hold beside
            // the drawing to use.
            if (!wire->label().empty()) {
                const Vec2 a = wire->pointsMm().front();
                const Vec2 b = wire->pointsMm()[1];
                const QPointF at = page.toScreen(Vec2{(a.x + b.x) / 2.0, (a.y + b.y) / 2.0});
                painter.setFont(labelFont);
                painter.setPen(QPen(ScreenColorOf(4, ink)));
                painter.drawText(at + QPointF(2.0, -2.0),
                                 QString::fromStdString(wire->label()));
            }
        }

        // --- JUNCTION DOTS ---------------------------------------------------
        //
        // WHERE THREE OR MORE WIRE ENDS MEET. Their ABSENCE is what misleads:
        // a reader who sees no dot reads a crossing, and the netlist would
        // disagree with the drawing about the circuit. Two ends meeting need
        // no dot -- that is just a corner.
        {
            std::vector<Vec2> ends;
            for (const WireEntity* wire : document.wires()) {
                if (!wire->isDrawable()) continue;
                ends.push_back(wire->pointsMm().front());
                ends.push_back(wire->pointsMm().back());
            }
            std::vector<Vec2> drawn;
            for (const Vec2 point : ends) {
                std::size_t meeting = 0;
                for (const Vec2 other : ends)
                    if (std::hypot(other.x - point.x, other.y - point.y) <= kNetToleranceMm)
                        ++meeting;
                if (meeting < 3) continue;
                bool already = false;
                for (const Vec2 done : drawn)
                    if (std::hypot(done.x - point.x, done.y - point.y) <= kNetToleranceMm)
                        already = true;
                if (already) continue;
                drawn.push_back(point);
                const double radius = std::max(1.2, 0.7 * page.pixelsPerMm);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QBrush(ScreenColorOf(4, ink)));
                painter.drawEllipse(page.toScreen(point), radius, radius);
                ++tally.junctions;
            }
            painter.setBrush(Qt::NoBrush);
        }

        // --- THE COMPONENTS ---------------------------------------------------
        const SymbolLibrary& library = BuiltInSymbols();
        for (const SymbolPlacement* placed : document.symbols()) {
            const ElectricalSymbol* symbol = library.find(placed->symbolName());
            const QPointF at = page.toScreen(placed->positionMm());
            if (symbol == nullptr) {
                // A COMPONENT DRAWN AS NOTHING LOOKS EXACTLY LIKE ONE THAT WAS
                // NEVER PLACED. So an unknown symbol is a red box with its tag
                // and the name it wanted -- enough for a user to go and find
                // the library it came from.
                const QColor alarm(200, 40, 40);
                painter.setPen(QPen(alarm, 1.2));
                painter.setBrush(Qt::NoBrush);
                const double size = 5.0 * page.pixelsPerMm;
                painter.drawRect(QRectF(at.x() - size / 2.0, at.y() - size / 2.0, size, size));
                painter.setFont(labelFont);
                painter.drawText(at + QPointF(size / 2.0 + 2.0, 0.0),
                                 QString::fromStdString(placed->tag() + " ? " +
                                                        placed->symbolName()));
                ++tally.unknownSymbols;
                ++tally.placedSymbols;
                continue;
            }

            const Matrix2D transform = SymbolPlacementTransform(
                placed->positionMm(), placed->rotationRad(), placed->isMirrored());
            const int aci = document.resolvedColorOnLayerForTesting(placed->layerId());
            QPen pen(ScreenColorOf(aci, ink));
            pen.setWidthF(std::max(0.8, 0.35 * page.pixelsPerMm));
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);

            // THROUGH THE SAME TRANSFORM THE PINS USE. A symbol whose geometry
            // and pins were placed by two different calculations would draw a
            // resistor here and connect it there.
            for (const DrawShape& shape : symbol->shapes()) {
                const DrawShape moved = TransformShape(shape, transform);
                // RECORDED IN SHEET MILLIMETRES, from the shape that is about
                // to be drawn -- so what this reports is where the ink went,
                // not where the placement says it should have.
                tally.symbolExtentMm.grow(BoundsOf(moved));
                if (const auto* line = std::get_if<DrawLine>(&moved)) {
                    painter.drawLine(page.toScreen(line->a), page.toScreen(line->b));
                } else if (const auto* circle = std::get_if<DrawCircle>(&moved)) {
                    const double r = circle->radius * page.pixelsPerMm;
                    const QPointF centre = page.toScreen(circle->centre);
                    painter.drawEllipse(QRectF(centre.x() - r, centre.y() - r, 2.0 * r,
                                               2.0 * r));
                } else if (const auto* arc = std::get_if<DrawArc>(&moved)) {
                    const double r = arc->radius * page.pixelsPerMm;
                    const QPointF centre = page.toScreen(arc->centre);
                    const double startDeg = -arc->startAngle * 180.0 / kPi;
                    double sweep = arc->endAngle - arc->startAngle;
                    while (sweep <= 0.0) sweep += kTwoPi;
                    painter.drawArc(QRectF(centre.x() - r, centre.y() - r, 2.0 * r, 2.0 * r),
                                    static_cast<int>(startDeg * 16.0),
                                    static_cast<int>(-sweep * 180.0 / kPi * 16.0));
                }
            }

            // THE TAG, beside the part. A schematic where the components are
            // unlabelled is one nobody can cross-reference to a wiring list.
            painter.setFont(labelFont);
            const Box2D box = symbol->bounds();
            const QPointF tagAt =
                page.toScreen(transform.apply(Vec2{box.max.x, box.max.y}));
            painter.drawText(tagAt + QPointF(2.0, -2.0),
                             QString::fromStdString(placed->tag()));
            ++tally.placedSymbols;
        }
    }

    // --- THE PARTS LISTS (M35.6) ---------------------------------------------
    //
    // COUNTED HERE, EVERY PAINT. The rows were never stored, so there is
    // nothing to keep in step -- and a drawing whose parts list said one thing
    // while the assembly said another is exactly what that buys.
    for (const BomTable* table : document.bomTables()) {
        if (!document.isOnCurrentSheet(table->sheetId())) continue;
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

    // --- THE GENERAL TOLERANCE NOTE (M37) ------------------------------------
    //
    // WHAT EVERY UNMARKED SIZE MEANS, printed ONCE, above the title block. A
    // drawing that stated it per dimension would let two dimensions answer to
    // two different classes; a drawing that stated it nowhere leaves every
    // unmarked size undefined rather than loose.
    if (!document.generalToleranceNote().empty() && document.titleBlock().isVisible()) {
        const Vec2 origin = document.titleBlockOriginMm();
        QFont noteFont = painter.font();
        noteFont.setPixelSize(std::max(5, static_cast<int>(3.0 * page.pixelsPerMm)));
        painter.setFont(noteFont);
        painter.setPen(QPen(ink));
        const QFontMetricsF noteMetrics(noteFont);
        painter.drawText(page.toScreen(Vec2{origin.x, origin.y + document.titleBlock()
                                                                     .heightMm() +
                                                          2.0}) +
                             QPointF(0.0, -noteMetrics.descent()),
                         QStringLiteral("General tolerances %1")
                             .arg(QString::fromStdString(document.generalToleranceNote())));
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
        if (!document.isOnCurrentSheet(entity->sheetId())) continue;
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

    // --- THE SYMBOLS (M41) ---------------------------------------------------
    //
    // Drawn from the document's answers, not from the specifications: what a
    // frame SAYS is asked for, letters and all, so the box on the paper cannot
    // carry a letter the datum does not.
    for (const Annotation* annotation : document.annotations()) {
        if (!document.isOnCurrentSheet(annotation->sheetId())) continue;
        const QString said = QString::fromStdString(document.annotationText(annotation->id()));
        const std::optional<Vec2> tip = document.annotationLeaderTipMm(annotation->id());
        const Vec2 at = annotation->positionMm();

        QFont symbolFont = painter.font();
        symbolFont.setPixelSize(std::max(6, static_cast<int>(3.0 * page.pixelsPerMm)));
        painter.setFont(symbolFont);
        const QFontMetricsF metrics(symbolFont);

        // A DANGLING SYMBOL IS DRAWN IN THE ALARM COLOUR AND COUNTED. It still
        // draws -- deleting it would throw away what the drafter said -- but
        // it must not look like one that is attached.
        const QColor ink2 = tip.has_value() ? ink : QColor(190, 60, 40);
        painter.setPen(QPen(ink2, 1.0));
        if (!tip.has_value()) ++tally.danglingSymbols;

        // THE LEADER, from the symbol to the thing it is about.
        if (tip.has_value())
            painter.drawLine(page.toScreen(at), page.toScreen(*tip));

        if (said.isEmpty()) {
            // The specification cannot be written. Said out loud rather than
            // drawn as an empty box, which is what it would otherwise be.
            ++tally.unreadableSymbols;
            painter.setPen(QPen(QColor(190, 60, 40)));
            painter.drawText(page.toScreen(at), QStringLiteral("?"));
            continue;
        }

        const double width = metrics.horizontalAdvance(said);
        const double height = metrics.height();
        const QPointF corner = page.toScreen(at);
        if (annotation->isFrame()) {
            // A FEATURE CONTROL FRAME IS A BOX. Without the box it is a line
            // of symbols a reader has no reason to read as a specification.
            // The padding is 5 either side rather than 3: at 3 the box closed
            // on the last datum letter, which on the screenshot read as a
            // letter clipped by the frame rather than one inside it.
            painter.drawRect(QRectF(corner.x(), corner.y() - height, width + 10.0,
                                    height + 2.0));
            painter.drawText(QPointF(corner.x() + 5.0, corner.y() - 2.0), said);
        } else if (annotation->isBalloon()) {
            // A CIRCLE WITH THE ROW NUMBER IN IT. Round on purpose: a reader
            // scanning an assembly drawing finds balloons by their shape, and
            // a boxed number is a feature control frame.
            //
            // THE SHAPE IS CHECKED BY EYE, not by an assertion, and that is
            // recorded here rather than pretended otherwise: a mutation
            // turning this ellipse into a rectangle survives every test in the
            // suite, because no tally can see a shape. What sees it is the
            // screenshot the self test writes beside the parts list, which is
            // what ADR-M26-009 says a visual fact needs.
            const double radius = std::max(height * 0.75, width * 0.7);
            painter.drawEllipse(QPointF(corner.x() + radius, corner.y() - height * 0.4),
                                radius, radius);
            painter.drawText(QRectF(corner.x(), corner.y() - height * 0.4 - radius,
                                    radius * 2.0, radius * 2.0),
                             Qt::AlignCenter, said);
        } else if (annotation->isDatum()) {
            // A DATUM IS A LETTER IN A BOX with a filled triangle at the end
            // of its leader -- the triangle is what makes it a datum rather
            // than a note.
            const double box = std::max(width + 6.0, height + 2.0);
            painter.drawRect(QRectF(corner.x(), corner.y() - height, box, height + 2.0));
            painter.drawText(QPointF(corner.x() + 3.0, corner.y() - 2.0), said);
            if (tip.has_value()) {
                const QPointF point = page.toScreen(*tip);
                const double size = std::max(3.0, 1.5 * page.pixelsPerMm);
                QPolygonF triangle;
                triangle << point << QPointF(point.x() - size, point.y() - size)
                         << QPointF(point.x() + size, point.y() - size);
                painter.setBrush(QBrush(ink2));
                painter.drawPolygon(triangle);
                painter.setBrush(Qt::NoBrush);
            }
        } else if (annotation->isWeld()) {
            // ISO 2553 SYSTEM A: A SOLID LINE AND A DASHED ONE.
            //
            // The screenshot after the first cut of M47 showed the two sides
            // drawn IDENTICALLY, distinguished only by the words "arrow" and
            // "other" in the text -- which is the exact ambiguity the type was
            // built to remove, put straight back on the paper. A welder does
            // not read the word; they read which line the symbol sits on.
            //
            // System A is used rather than the above/below convention because
            // it is the one that cannot be misread: the ARROW side is always
            // on the SOLID reference line and the OTHER side is always on the
            // DASHED identification line, whichever way round the two lines
            // are drawn. The same property the struct has, on paper.
            const auto* weld = std::get_if<WeldSymbolSpec>(&annotation->body());
            const QString arrowText =
                weld != nullptr && weld->arrowSide.has_value()
                    ? QString::fromStdString(WeldBeadText(*weld->arrowSide))
                    : QString();
            const QString otherText =
                weld != nullptr && weld->otherSide.has_value()
                    ? QString::fromStdString(WeldBeadText(*weld->otherSide))
                    : QString();
            const double tick = std::max(4.0, 2.5 * page.pixelsPerMm);
            const double run = std::max({metrics.horizontalAdvance(arrowText),
                                         metrics.horizontalAdvance(otherText), 6.0 * tick}) +
                               8.0;
            const QPointF root(corner.x(), corner.y());
            const QPointF end(corner.x() + run, corner.y());

            // THE REFERENCE LINE IS ALWAYS THERE, even when the weld is only
            // on the far side: an identification line with nothing to identify
            // against is not a weld symbol.
            painter.drawLine(root, end);
            if (!arrowText.isEmpty())
                painter.drawText(QPointF(root.x() + 4.0, root.y() - 3.0), arrowText);

            if (!otherText.isEmpty()) {
                const double drop = height + 1.0;
                painter.setPen(QPen(ink2, 1.0, Qt::DashLine));
                painter.drawLine(QPointF(root.x(), root.y() + drop),
                                 QPointF(end.x(), end.y() + drop));
                painter.setPen(QPen(ink2, 1.0));
                // BELOW the identification line, not through it. The first
                // cut struck the dashes straight across the glyphs, which on
                // the screenshot read as a crossed-out weld.
                painter.drawText(QPointF(root.x() + 4.0, root.y() + drop + height),
                                 otherText);
            }

            // THE CIRCLE AND THE FLAG SIT AT THE ELBOW, because they are about
            // the whole instruction and not about one side of the joint.
            if (weld != nullptr && weld->allAround)
                painter.drawEllipse(root, tick * 0.35, tick * 0.35);
            if (weld != nullptr && weld->fieldWeld) {
                QPolygonF flag;
                flag << root << QPointF(root.x(), root.y() - tick * 1.6)
                     << QPointF(root.x() + tick * 0.9, root.y() - tick * 1.2)
                     << QPointF(root.x(), root.y() - tick * 0.8);
                painter.setBrush(QBrush(ink2));
                painter.drawPolygon(flag);
                painter.setBrush(Qt::NoBrush);
            }
            // THE TAIL: a fork at the far end, with the process in it.
            if (weld != nullptr && !weld->tail.empty()) {
                painter.drawLine(end, QPointF(end.x() + tick * 0.8, end.y() - tick * 0.6));
                painter.drawLine(end, QPointF(end.x() + tick * 0.8, end.y() + tick * 0.6));
                painter.drawText(QPointF(end.x() + tick * 1.1, end.y() + 3.0),
                                 QString::fromStdString(weld->tail));
            }
        } else {
            // THE ISO 1302 TICK. The bar across the top says material MUST be
            // removed and the circle in the vee says it must NOT -- which is
            // the whole difference between a machined face and a cast one.
            const auto* finish = std::get_if<SurfaceFinishSpec>(&annotation->body());
            const double tick = std::max(4.0, 2.5 * page.pixelsPerMm);
            const QPointF root(corner.x(), corner.y());
            const QPointF dip(corner.x() + tick * 0.5, corner.y() + tick * 0.5);
            const QPointF peak(corner.x() + tick * 1.5, corner.y() - tick);
            painter.drawLine(root, dip);
            painter.drawLine(dip, peak);
            if (finish != nullptr && finish->symbol == SurfaceSymbol::Machined)
                painter.drawLine(peak, QPointF(peak.x() + width + 6.0, peak.y()));
            if (finish != nullptr && finish->symbol == SurfaceSymbol::AsCast)
                painter.drawEllipse(QPointF(corner.x() + tick * 0.75, corner.y() - tick * 0.1),
                                    tick * 0.35, tick * 0.35);
            if (finish != nullptr && finish->allAround)
                painter.drawEllipse(root, tick * 0.3, tick * 0.3);
            painter.drawText(QPointF(peak.x() + 2.0, peak.y() - 2.0), said);
        }
        ++tally.symbols;
    }

    // --- THE HOLE TABLES, AND THE TAGS IN THE VIEW (M39.4) -------------------
    //
    // Drawn from ONE reading of the part. The row says A1 at (10, 40) and the
    // tag is placed at exactly that point -- worked out from the same numbers,
    // so a tag cannot end up beside a hole other than the one its row
    // describes. Two readings is precisely the shape of defect this project
    // keeps closing.
    for (const HoleTable* table : document.holeTables()) {
        if (!document.isOnCurrentSheet(table->sheetId())) continue;
        const HoleTableContents holes = document.holesOf(*table);
        if (!holes.ok) {
            // THE ALARM HAS TO REACH THE SCREEN. An empty table is what a part
            // with no holes gives, and the reader cannot tell the two apart.
            ++tally.uncountedHoleTables;
            painter.setPen(QPen(QColor(190, 60, 40)));
            painter.drawText(page.toScreen(table->positionMm()),
                             QStringLiteral("%1: %2")
                                 .arg(QString::fromStdString(table->name()),
                                      QString::fromStdString(holes.why)));
            continue;
        }

        const Vec2 corner = table->positionMm();
        const double width = table->widthMm();
        const std::size_t lines = holes.rows.size() + 1; // the heading is a row
        QFont cell = painter.font();
        cell.setPixelSize(std::max(5, static_cast<int>(2.2 * page.pixelsPerMm)));
        painter.setFont(cell);

        for (std::size_t line = 0; line < lines; ++line) {
            const double top = corner.y + table->rowBottomMm(line, lines) +
                               table->rowHeightMm();
            const double bottom = corner.y + table->rowBottomMm(line, lines);
            painter.setPen(QPen(ink, 0.9));
            painter.drawLine(page.toScreen(Vec2{corner.x, bottom}),
                             page.toScreen(Vec2{corner.x + width, bottom}));

            double x = corner.x;
            for (const HoleColumn column : table->columns()) {
                const double columnWidth = table->columnWidthMm(column);
                painter.drawLine(page.toScreen(Vec2{x, top}),
                                 page.toScreen(Vec2{x, bottom}));
                const QString text =
                    line == 0 ? QString::fromStdString(std::string(HeadingOf(column)))
                              : QString::fromStdString(CellOf(holes.rows[line - 1], column));
                const QPointF at = page.toScreen(Vec2{x + 1.0, bottom + 1.5});
                painter.drawText(at, text);
                x += columnWidth;
            }
            painter.drawLine(page.toScreen(Vec2{x, top}), page.toScreen(Vec2{x, bottom}));
            if (line > 0) ++tally.holeTableRows;
        }
        // ...and the top rule, which the loop above draws only bottoms of.
        painter.drawLine(page.toScreen(Vec2{corner.x, corner.y}),
                         page.toScreen(Vec2{corner.x + width, corner.y}));

        // THE TAG GOES WHERE THE ROW SAYS THE HOLE IS -- datum plus offset,
        // through the view's own scale, which is the one conversion (M35).
        for (const HoleTableRow& row : holes.rows) {
            const Vec2 inView{table->datumMm().x + row.atMm.x,
                              table->datumMm().y + row.atMm.y};
            const QPointF at =
                page.toScreen(document.viewPointToSheetMm(table->viewId(), inView));
            painter.setPen(QPen(ink));
            painter.drawText(at + QPointF(3.0, -3.0), QString::fromStdString(row.tag));
            ++tally.holeTags;
        }
    }

    // --- THE REVISION TABLE (M48) --------------------------------------------
    //
    // Holding none of its rows: they are the drawing's history, asked for
    // here. So a table cannot show an issue this drawing does not have, and
    // cannot miss one it does -- which is the same reason the parts list does
    // not keep its own quantities.
    for (const RevisionTable* table : document.revisionTables()) {
        if (!document.isOnCurrentSheet(table->sheetId())) continue;
        const std::vector<Revision>& history = document.revisions();
        const Vec2 corner = table->positionMm();
        const double width = table->widthMm();
        const std::size_t lines = history.size() + 1;   // the heading is a row

        QFont cell = painter.font();
        cell.setPixelSize(std::max(5, static_cast<int>(2.2 * page.pixelsPerMm)));
        painter.setFont(cell);
        painter.setPen(QPen(ink, 0.9));

        // A REVISION TABLE GROWS UPWARDS: the heading sits on the table's own
        // corner and the newest issue is at the top, which is where a reader
        // looks for it. rowBottomMm owns that arithmetic (the title block
        // taught this the hard way).
        const std::vector<RevisionColumn>& columns = RevisionColumns();
        for (std::size_t line = 0; line < lines; ++line) {
            const double bottom = corner.y + table->rowBottomMm(line);
            const double top = bottom + table->rowHeightMm();
            painter.drawLine(page.toScreen(Vec2{corner.x, bottom}),
                             page.toScreen(Vec2{corner.x + width, bottom}));

            double x = corner.x;
            for (std::size_t c = 0; c < columns.size(); ++c) {
                // The description gets the room: it is the only cell with a
                // sentence in it, and the other three are a letter, a date and
                // a pair of initials.
                const double columnWidth =
                    columns[c] == RevisionColumn::Description ? width * 0.55 : width * 0.15;
                painter.drawLine(page.toScreen(Vec2{x, top}), page.toScreen(Vec2{x, bottom}));
                const QString text =
                    line == 0
                        ? QString::fromStdString(std::string(toString(columns[c])))
                        : QString::fromStdString(CellOf(history[line - 1], columns[c]));
                painter.drawText(page.toScreen(Vec2{x + 1.0, bottom + 1.5}), text);
                x += columnWidth;
            }
            painter.drawLine(page.toScreen(Vec2{x, top}), page.toScreen(Vec2{x, bottom}));
            if (line > 0) ++tally.revisionRows;
        }
        // ...and the rule over the newest row, which the loop draws only the
        // bottoms of.
        const double capMm = corner.y + table->rowBottomMm(lines - 1) + table->rowHeightMm();
        painter.drawLine(page.toScreen(Vec2{corner.x, capMm}),
                         page.toScreen(Vec2{corner.x + width, capMm}));
        ++tally.revisionTables;
    }

    // --- THE VIEWS -----------------------------------------------------------
    for (const DrawingView* view : document.views()) {
        // ANOTHER PAGE OF THE SAME DRAWING. Not hidden, not deleted --
        // just not this sheet, which is the whole of what a page means.
        if (!document.isOnCurrentSheet(view->sheetId())) continue;
        if (view->currentState() != ComputeState::Valid) continue;
        // MODEL MILLIMETRES TO SHEET MILLIMETRES, through the document -- the
        // curves never carry the scale (see ProjectedGeometry.h), and
        // viewPointToSheetMm is the one place it is applied.
        const double factor = document.viewScaleFactor(view->id());
        const auto place = [&](Vec2 modelMm) {
            return page.toScreen(document.viewPointToSheetMm(view->id(), modelMm));
        };

        // --- THE CUT FACE, HATCHED (M38) -------------------------------------
        //
        // FIRST, under the curves: hatch is a fill and the outline belongs on
        // top of it. Drawn at the SHEET's pitch rather than the model's, so a
        // 1:10 view is not filled solid -- a hatch is annotation, like a
        // dimension, and does not scale with what it annotates.
        if (!view->projected().cutLoops.empty()) {
            // THE REGION AND THE PATTERN BOTH COME FROM THE DOCUMENT.
            //
            // They were worked out here, which put the conversion out of model
            // millimetres -- the one line that decides whether a 1:10 section
            // fills solid -- in the one place a test cannot look. The painter
            // draws; what to draw is a question the document answers.
            const HatchRegion region = document.sectionHatchRegionMm(view->id());
            const HatchStyle hatchStyle = document.sectionHatchStyle(view->id());
            const HatchLines hatch = HatchTheRegion(region, hatchStyle);
            if (!hatch.ok) {
                ++tally.unhatchedSections;
            } else {
                QPen hatchPen(QColor(ink.red(), ink.green(), ink.blue(), 170));
                hatchPen.setWidthF(std::max(0.5, 0.25 * page.pixelsPerMm));
                painter.setPen(hatchPen);
                for (const auto& segment : hatch.segments) {
                    painter.drawLine(page.toScreen(segment.first),
                                     page.toScreen(segment.second));
                    ++tally.hatchLines;
                }
            }
        }

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
        // --- THE SECTION LINE ON THE PARENT (M38) ----------------------------
        //
        // Drawn on the view the cut was taken FROM, not on the section itself,
        // with the letter at each end and arrows showing which way the reader
        // is looking. The letter comes from the document, so the line and the
        // section's own title cannot end up carrying different ones.
        for (const DrawingView* child : document.views()) {
            if (child->parentViewId() != view->id() || !child->isSection()) continue;
            const std::string letter = document.sectionLetterOf(child->id());
            const Vec2 from =
                document.viewPointToSheetMm(view->id(), child->sectionCut().fromMm);
            const Vec2 to =
                document.viewPointToSheetMm(view->id(), child->sectionCut().toMm);

            QPen cutPen(ScreenColorOf(1, ink)); // red by convention
            cutPen.setWidthF(std::max(1.0, 0.7 * page.pixelsPerMm));
            // A CHAIN-DASHED LINE is what a cutting plane is drawn with, and a
            // solid one would read as an edge of the part.
            QList<qreal> dashes;
            dashes << 8.0 << 3.0 << 1.0 << 3.0;
            cutPen.setDashPattern(dashes);
            painter.setPen(cutPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawLine(page.toScreen(from), page.toScreen(to));

            // The arrows, at each end, pointing the way the reader looks.
            const double dx = to.x - from.x;
            const double dy = to.y - from.y;
            const double run = std::hypot(dx, dy);
            if (run > 1e-9) {
                const double side = child->sectionCut().arrowSide >= 0 ? 1.0 : -1.0;
                // Across the line, on the arrow side -- the same cross product
                // the cut plane is built from, in two dimensions.
                const Vec2 across{dy / run * side, -dx / run * side};
                const double lengthMm = 6.0;
                QFont letterFont = painter.font();
                letterFont.setPixelSize(std::max(6, static_cast<int>(4.0 * page.pixelsPerMm)));
                painter.setFont(letterFont);
                for (const Vec2 end : {from, to}) {
                    const Vec2 tip{end.x + across.x * lengthMm, end.y + across.y * lengthMm};
                    QPen solid(ScreenColorOf(1, ink));
                    solid.setWidthF(std::max(1.0, 0.7 * page.pixelsPerMm));
                    painter.setPen(solid);
                    painter.drawLine(page.toScreen(end), page.toScreen(tip));
                    painter.drawText(page.toScreen(Vec2{tip.x + across.x * 3.0,
                                                        tip.y + across.y * 3.0}),
                                     QString::fromStdString(letter));
                    ++tally.sectionArrows;
                }
            }
        }

        // --- THE DETAIL CIRCLE ON THE PARENT (M49) ---------------------------
        //
        // Drawn on the view the detail was taken FROM, with its letter beside
        // it. The letter comes from the document -- the same call the caption
        // under the detail makes -- so the circle here and the title there
        // cannot end up carrying different ones.
        for (const DrawingView* child : document.views()) {
            if (child->parentViewId() != view->id() || !child->isDetail()) continue;
            const std::string letter = document.viewLetterOf(child->id());
            const Vec2 centre =
                document.viewPointToSheetMm(view->id(), child->detailFrame().centreMm);
            // THE RADIUS GOES THROUGH THE SAME SCALE THE POINT DID. Drawn at
            // its model size on a view at 1:2, the circle would be twice as
            // wide as the region it stands for -- and a reader would look for
            // the detail's contents in the wrong place.
            const Vec2 edge = document.viewPointToSheetMm(
                view->id(), Vec2{child->detailFrame().centreMm.x + child->detailFrame().radiusMm,
                                 child->detailFrame().centreMm.y});
            const double radiusMm = std::hypot(edge.x - centre.x, edge.y - centre.y);

            QPen ringPen(ScreenColorOf(1, ink));   // red, as the cut line is
            ringPen.setWidthF(std::max(1.0, 0.5 * page.pixelsPerMm));
            QList<qreal> ringDashes;
            ringDashes << 6.0 << 3.0;
            ringPen.setDashPattern(ringDashes);
            painter.setPen(ringPen);
            painter.setBrush(Qt::NoBrush);
            const QPointF at = page.toScreen(centre);
            const double radiusPx = radiusMm * page.pixelsPerMm;
            painter.drawEllipse(at, radiusPx, radiusPx);

            QFont letterFont = painter.font();
            letterFont.setPixelSize(std::max(6, static_cast<int>(4.0 * page.pixelsPerMm)));
            painter.setFont(letterFont);
            QPen solid(ScreenColorOf(1, ink));
            solid.setWidthF(std::max(1.0, 0.7 * page.pixelsPerMm));
            painter.setPen(solid);
            painter.drawText(at + QPointF(radiusPx + 3.0, -radiusPx),
                             QString::fromStdString(letter));
            ++tally.detailCircles;
        }

        // --- AND THE DETAIL'S OWN BOUNDARY -----------------------------------
        //
        // Drawn on the detail itself, round the same region at the enlarged
        // size. Without it the crop's edge is wherever the geometry happened to
        // stop, and a reader has no way to tell "the part ends here" from "the
        // detail ends here" -- which are opposite statements about the part.
        if (view->isDetail() && view->detailFrame().usable()) {
            const Vec2 centre =
                document.viewPointToSheetMm(view->id(), view->detailFrame().centreMm);
            const Vec2 edge = document.viewPointToSheetMm(
                view->id(), Vec2{view->detailFrame().centreMm.x + view->detailFrame().radiusMm,
                                 view->detailFrame().centreMm.y});
            const double radiusPx =
                std::hypot(edge.x - centre.x, edge.y - centre.y) * page.pixelsPerMm;
            QPen edgePen(QColor(ink.red(), ink.green(), ink.blue(), 150));
            edgePen.setWidthF(std::max(1.0, 0.35 * page.pixelsPerMm));
            painter.setPen(edgePen);
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(page.toScreen(centre), radiusPx, radiusPx);
        }

        // WHAT GOES UNDER A VIEW is the document's answer, not one composed
        // here: the letter has to match the cut line drawn on the parent, and
        // a caption typed in the renderer is a caption no test can read.
        const QString label = QString::fromStdString(document.viewLabelText(view->id()));
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
        if (!document.isOnCurrentSheet(dimension->sheetId())) continue;
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

        // THE TOLERANCE, SET SMALLER, and whether the size is boxed. Asked of
        // the document rather than worked out here: dimensionText is built
        // FROM dimensionToleranceText, so the canvas cannot end up printing a
        // tolerance the plot and the DXF do not.
        const QString toleranceText =
            QString::fromStdString(document.dimensionToleranceText(*dimension));
        const bool boxed = document.dimensionIsBasic(*dimension);
        // The tolerance is drawn at 70% of the size's height, which is what
        // ISO 129-1 asks for -- big enough to read, small enough that the size
        // is still the thing the eye lands on.
        QFont toleranceFont = font;
        toleranceFont.setPixelSize(std::max(
            4, static_cast<int>(style->scaledTextHeightMm() * 0.7 * page.pixelsPerMm)));

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
            // THE SIZE ALONE, without the tolerance -- which is drawn beside
            // it in smaller type below. A dimension that ran them together in
            // one size would set the tolerance as loud as the size, and the
            // size is what a reader is looking for.
            QString sizeOnly = text;
            if (!toleranceText.isEmpty() && sizeOnly.endsWith(toleranceText))
                sizeOnly = sizeOnly.left(sizeOnly.size() - toleranceText.size()).trimmed();

            const QFontMetricsF metrics(painter.font());
            const double width = metrics.horizontalAdvance(sizeOnly);
            painter.setPen(pen);
            painter.drawText(QPointF(-width / 2.0, -textGapPx), sizeOnly);

            // A BASIC DIMENSION IS BOXED. The box IS the specification -- it
            // says the size is theoretically exact and its tolerance lives in
            // a geometric control somewhere else, and without it the number
            // reads as an ordinary untoleranced size.
            if (boxed) {
                const double pad = 0.8 * page.pixelsPerMm;
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(QRectF(-width / 2.0 - pad,
                                        -textGapPx - metrics.ascent() - pad / 2.0,
                                        width + 2.0 * pad,
                                        metrics.ascent() + metrics.descent() + pad));
            }

            if (!toleranceText.isEmpty()) {
                painter.setFont(toleranceFont);
                const QFontMetricsF small(toleranceFont);
                // AFTER the size, on the same line, at the same baseline --
                // which is where a single-line tolerance goes. Stacked
                // deviations are a separate treatment and not this one.
                painter.drawText(QPointF(width / 2.0 + 1.0 * page.pixelsPerMm, -textGapPx),
                                 toleranceText);
                painter.setFont(font);
            }
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
