#include "Viewer/SketchIcons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>

#include <array>
#include <cmath>
#include <vector>

namespace paramcad::ui {
namespace {

// Every icon is drawn on this grid and then scaled, so one set of coordinates
// serves 16 px and 48 px alike.
constexpr double kGrid = 24.0;

// The sizes a QIcon carries. Qt picks the nearest and scales only when it must,
// which for line art is the difference between crisp and smeared.
constexpr std::array<int, 5> kSizes{16, 20, 24, 32, 48};

struct Ink {
    QColor stroke;  // ordinary geometry
    QColor accent;  // the relationship the icon is ABOUT
    QColor subdue;  // construction and reference marks
    QColor danger;  // destructive commands only
};

// Derived from the palette, never hard-coded -- the same rule
// DesignTokens.h's presentationFor() follows, and for the same reason.
Ink InkFor(const QPalette& palette) {
    const QColor background = palette.color(QPalette::Window);
    const double luminance = (0.299 * background.red() + 0.587 * background.green() +
                              0.114 * background.blue()) /
                             255.0;
    const bool dark = luminance < 0.5;

    Ink ink;
    ink.stroke = palette.color(QPalette::ButtonText);
    // Blue on light, a lifted blue on dark: the accent has to stay legible
    // against the toolbar it sits on, not against a notional white.
    ink.accent = dark ? QColor(0x6E, 0xA8, 0xFF) : QColor(0x1B, 0x5F, 0xC8);
    ink.subdue = palette.color(QPalette::Disabled, QPalette::ButtonText);
    ink.danger = dark ? QColor(0xE8, 0x7D, 0x7D) : QColor(0xB3, 0x2A, 0x2A);
    return ink;
}

// A dot the same shape the canvas draws for a defined point.
void Dot(QPainter& p, double x, double y, const QColor& colour, double radius = 1.7) {
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(colour);
    p.drawEllipse(QPointF(x, y), radius, radius);
    p.restore();
}

void Stroke(QPainter& p, const QColor& colour, double width = 1.6,
            Qt::PenStyle style = Qt::SolidLine) {
    QPen pen(colour, width, style, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
}

// A small filled arrowhead, the same idea the dimension renderer uses.
void Arrow(QPainter& p, QPointF tip, QPointF direction, const QColor& colour,
           double length = 4.2, double halfWidth = 1.9) {
    const double norm = std::hypot(direction.x(), direction.y());
    if (!(norm > 1e-9)) return;
    const QPointF unit(direction.x() / norm, direction.y() / norm);
    const QPointF back(tip.x() - unit.x() * length, tip.y() - unit.y() * length);
    const QPointF side(-unit.y() * halfWidth, unit.x() * halfWidth);
    QPainterPath path;
    path.moveTo(tip);
    path.lineTo(back + side);
    path.lineTo(back - side);
    path.closeSubpath();
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(colour);
    p.drawPath(path);
    p.restore();
}

// The right-angle square that marks a perpendicular corner on a drawing.
void RightAngleMark(QPainter& p, QPointF corner, QPointF alongA, QPointF alongB,
                    const QColor& colour, double size = 4.0) {
    const QPointF a(corner.x() + alongA.x() * size, corner.y() + alongA.y() * size);
    const QPointF b(corner.x() + alongB.x() * size, corner.y() + alongB.y() * size);
    const QPointF c(a.x() + alongB.x() * size, a.y() + alongB.y() * size);
    Stroke(p, colour, 1.2);
    p.drawLine(a, c);
    p.drawLine(c, b);
}

void PaintIcon(QPainter& p, SketchIcon icon, const Ink& ink) {
    switch (icon) {
    // --- Drawing tools ---------------------------------------------------
    case SketchIcon::Select: {
        // The pointer, filled so it reads at 16 px where an outline would not.
        QPainterPath arrowPath;
        arrowPath.moveTo(7.0, 4.0);
        arrowPath.lineTo(7.0, 18.5);
        arrowPath.lineTo(10.6, 15.0);
        arrowPath.lineTo(13.0, 20.4);
        arrowPath.lineTo(15.3, 19.3);
        arrowPath.lineTo(12.9, 14.1);
        arrowPath.lineTo(17.6, 13.6);
        arrowPath.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(ink.stroke);
        p.drawPath(arrowPath);
        break;
    }
    case SketchIcon::Line:
        Stroke(p, ink.stroke);
        p.drawLine(QPointF(4.5, 18.5), QPointF(19.5, 5.5));
        Dot(p, 4.5, 18.5, ink.accent);
        Dot(p, 19.5, 5.5, ink.accent);
        break;
    case SketchIcon::Rectangle:
        Stroke(p, ink.stroke);
        p.drawRect(QRectF(4.5, 6.5, 15.0, 11.0));
        Dot(p, 4.5, 6.5, ink.accent);
        Dot(p, 19.5, 17.5, ink.accent);
        break;
    case SketchIcon::Circle:
        Stroke(p, ink.stroke);
        p.drawEllipse(QPointF(12.0, 12.0), 7.5, 7.5);
        Dot(p, 12.0, 12.0, ink.accent);
        break;
    case SketchIcon::Arc:
        Stroke(p, ink.stroke);
        // Centre-point arc: the tool takes centre, start, end -- so the icon
        // shows all three, not just a bent line.
        p.drawArc(QRectF(4.0, 6.0, 16.0, 16.0), 20 * 16, 140 * 16);
        Dot(p, 12.0, 14.0, ink.subdue, 1.4);
        Dot(p, 19.5, 11.3, ink.accent);
        Dot(p, 4.5, 11.3, ink.accent);
        break;
    case SketchIcon::Point:
        Stroke(p, ink.subdue, 1.2);
        p.drawLine(QPointF(12.0, 5.0), QPointF(12.0, 19.0));
        p.drawLine(QPointF(5.0, 12.0), QPointF(19.0, 12.0));
        Dot(p, 12.0, 12.0, ink.accent, 2.6);
        break;

    // --- Geometric constraints -------------------------------------------
    case SketchIcon::Coincident:
        // Two things ARRIVING at one point, shown as two heads closing on a
        // single dot.
        //
        // The first version drew two line ends meeting inside a ring, and in
        // the toolbar it read as a handbag -- the ring closed the V into a
        // shape. A screenshot is the only thing that could have said so.
        Stroke(p, ink.stroke, 1.6);
        p.drawLine(QPointF(3.0, 12.0), QPointF(7.6, 12.0));
        p.drawLine(QPointF(21.0, 12.0), QPointF(16.4, 12.0));
        Arrow(p, QPointF(9.0, 12.0), QPointF(1.0, 0.0), ink.stroke);
        Arrow(p, QPointF(15.0, 12.0), QPointF(-1.0, 0.0), ink.stroke);
        Dot(p, 12.0, 12.0, ink.accent, 3.0);
        break;
    case SketchIcon::Horizontal:
        Stroke(p, ink.accent, 2.0);
        p.drawLine(QPointF(4.0, 12.0), QPointF(20.0, 12.0));
        Dot(p, 4.0, 12.0, ink.stroke);
        Dot(p, 20.0, 12.0, ink.stroke);
        Stroke(p, ink.subdue, 1.1, Qt::DashLine);
        p.drawLine(QPointF(4.0, 18.5), QPointF(20.0, 18.5));
        break;
    case SketchIcon::Vertical:
        Stroke(p, ink.accent, 2.0);
        p.drawLine(QPointF(12.0, 4.0), QPointF(12.0, 20.0));
        Dot(p, 12.0, 4.0, ink.stroke);
        Dot(p, 12.0, 20.0, ink.stroke);
        Stroke(p, ink.subdue, 1.1, Qt::DashLine);
        p.drawLine(QPointF(5.5, 4.0), QPointF(5.5, 20.0));
        break;
    case SketchIcon::Fix: {
        // A padlock. "Pinned where it is" needs a metaphor a first-time user
        // already owns; a dot with decoration does not read as anything.
        Stroke(p, ink.stroke, 1.6);
        p.drawArc(QRectF(8.0, 4.5, 8.0, 9.0), 0, 180 * 16);
        p.setPen(Qt::NoPen);
        p.setBrush(ink.accent);
        p.drawRoundedRect(QRectF(6.5, 11.0, 11.0, 8.5), 1.6, 1.6);
        Dot(p, 12.0, 15.2, QColor(255, 255, 255, 220), 1.4);
        break;
    }
    case SketchIcon::Parallel:
        Stroke(p, ink.accent, 1.8);
        p.drawLine(QPointF(5.0, 19.0), QPointF(12.0, 5.0));
        p.drawLine(QPointF(12.0, 19.0), QPointF(19.0, 5.0));
        break;
    case SketchIcon::Perpendicular:
        Stroke(p, ink.accent, 1.8);
        p.drawLine(QPointF(6.0, 4.5), QPointF(6.0, 19.0));
        p.drawLine(QPointF(6.0, 19.0), QPointF(20.0, 19.0));
        RightAngleMark(p, QPointF(6.0, 19.0), QPointF(0.0, -1.0), QPointF(1.0, 0.0), ink.stroke);
        break;
    case SketchIcon::Equal:
        // Two segments carrying the single tick a drawing uses for "same size".
        Stroke(p, ink.stroke, 1.6);
        p.drawLine(QPointF(4.5, 8.0), QPointF(19.5, 8.0));
        p.drawLine(QPointF(4.5, 16.5), QPointF(19.5, 16.5));
        Stroke(p, ink.accent, 1.8);
        p.drawLine(QPointF(12.0, 5.4), QPointF(12.0, 10.6));
        p.drawLine(QPointF(12.0, 13.9), QPointF(12.0, 19.1));
        break;
    case SketchIcon::Concentric:
        Stroke(p, ink.stroke, 1.5);
        p.drawEllipse(QPointF(12.0, 12.0), 8.0, 8.0);
        Stroke(p, ink.accent, 1.5);
        p.drawEllipse(QPointF(12.0, 12.0), 4.0, 4.0);
        Dot(p, 12.0, 12.0, ink.accent, 1.5);
        break;
    case SketchIcon::Midpoint:
        Stroke(p, ink.stroke, 1.6);
        p.drawLine(QPointF(4.0, 12.0), QPointF(20.0, 12.0));
        // Equal-length ticks either side say MIDpoint rather than on-the-line.
        Stroke(p, ink.accent, 1.5);
        p.drawLine(QPointF(8.0, 9.0), QPointF(8.0, 15.0));
        p.drawLine(QPointF(16.0, 9.0), QPointF(16.0, 15.0));
        Dot(p, 12.0, 12.0, ink.accent, 2.7);
        break;
    case SketchIcon::PointOnObject:
        Stroke(p, ink.stroke, 1.6);
        p.drawLine(QPointF(3.5, 17.5), QPointF(20.5, 7.5));
        Dot(p, 12.0, 12.5, ink.accent, 2.9);
        break;
    case SketchIcon::Tangent:
        Stroke(p, ink.stroke, 1.5);
        p.drawEllipse(QPointF(12.0, 14.5), 6.5, 6.5);
        Stroke(p, ink.accent, 1.8);
        p.drawLine(QPointF(3.5, 8.0), QPointF(20.5, 8.0));
        Dot(p, 12.0, 8.0, ink.accent, 2.0);
        break;

    // --- Dimensions -------------------------------------------------------
    case SketchIcon::Dimension:
        // The icon is the thing itself: extension lines, a dimension line and
        // two outward arrowheads.
        Stroke(p, ink.subdue, 1.2);
        p.drawLine(QPointF(5.0, 7.0), QPointF(5.0, 19.5));
        p.drawLine(QPointF(19.0, 7.0), QPointF(19.0, 19.5));
        Stroke(p, ink.accent, 1.5);
        p.drawLine(QPointF(5.0, 13.0), QPointF(19.0, 13.0));
        Arrow(p, QPointF(5.0, 13.0), QPointF(-1.0, 0.0), ink.accent);
        Arrow(p, QPointF(19.0, 13.0), QPointF(1.0, 0.0), ink.accent);
        break;
    case SketchIcon::Radius:
        Stroke(p, ink.stroke, 1.5);
        p.drawEllipse(QPointF(12.0, 12.0), 8.0, 8.0);
        Stroke(p, ink.accent, 1.5);
        p.drawLine(QPointF(12.0, 12.0), QPointF(17.7, 6.3));
        Arrow(p, QPointF(17.7, 6.3), QPointF(1.0, -1.0), ink.accent);
        Dot(p, 12.0, 12.0, ink.accent, 1.5);
        break;
    case SketchIcon::Diameter:
        Stroke(p, ink.stroke, 1.5);
        p.drawEllipse(QPointF(12.0, 12.0), 8.0, 8.0);
        Stroke(p, ink.accent, 1.5);
        p.drawLine(QPointF(6.3, 17.7), QPointF(17.7, 6.3));
        Arrow(p, QPointF(17.7, 6.3), QPointF(1.0, -1.0), ink.accent);
        Arrow(p, QPointF(6.3, 17.7), QPointF(-1.0, 1.0), ink.accent);
        break;

    case SketchIcon::AutoPlaceDimensions:
        // A dimension line with a return arrow curling over it: put the values
        // back where the layout wants them.
        Stroke(p, ink.subdue, 1.1);
        p.drawLine(QPointF(5.0, 12.5), QPointF(5.0, 20.0));
        p.drawLine(QPointF(19.0, 12.5), QPointF(19.0, 20.0));
        Stroke(p, ink.stroke, 1.4);
        p.drawLine(QPointF(5.0, 17.0), QPointF(19.0, 17.0));
        Arrow(p, QPointF(5.0, 17.0), QPointF(-1.0, 0.0), ink.stroke, 3.6, 1.6);
        Arrow(p, QPointF(19.0, 17.0), QPointF(1.0, 0.0), ink.stroke, 3.6, 1.6);
        Stroke(p, ink.accent, 1.7);
        p.drawArc(QRectF(6.0, 3.0, 12.0, 11.0), 200 * 16, -230 * 16);
        Arrow(p, QPointF(6.6, 6.2), QPointF(-0.5, -1.0), ink.accent, 4.0, 1.8);
        break;

    case SketchIcon::HorizontalDistance:
        // A horizontal dimension line with a tick at each end, and the two
        // POINTS it spans marked -- which is what distinguishes it from Length,
        // whose subject is a line rather than a pair.
        Stroke(p, ink.accent, 1.6);
        p.drawLine(QPointF(4.5, 12.0), QPointF(19.5, 12.0));
        p.drawLine(QPointF(4.5, 9.0), QPointF(4.5, 15.0));
        p.drawLine(QPointF(19.5, 9.0), QPointF(19.5, 15.0));
        Dot(p, 4.5, 19.0, ink.stroke, 2.0);
        Dot(p, 19.5, 5.0, ink.stroke, 2.0);
        break;
    case SketchIcon::VerticalDistance:
        Stroke(p, ink.accent, 1.6);
        p.drawLine(QPointF(12.0, 4.5), QPointF(12.0, 19.5));
        p.drawLine(QPointF(9.0, 4.5), QPointF(15.0, 4.5));
        p.drawLine(QPointF(9.0, 19.5), QPointF(15.0, 19.5));
        Dot(p, 5.0, 4.5, ink.stroke, 2.0);
        Dot(p, 19.0, 19.5, ink.stroke, 2.0);
        break;

    // --- Assembly (M30.2) ---------------------------------------------------
    //
    // A PART IS A FILLED BLOCK in this group, where sketch geometry is a line.
    // That is the one visual rule the set follows, and it is what lets a user
    // glancing at the toolbar tell that these commands are about whole parts
    // without reading a single label.
    case SketchIcon::InsertInstance: {
        // A block dropping INTO a place waiting for it: the dashed outline is
        // where it goes, the solid one is what is arriving.
        Stroke(p, ink.subdue, 1.4, Qt::DashLine);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(3.5, 12.5, 10.0, 8.0));
        p.setPen(Qt::NoPen);
        p.setBrush(ink.accent);
        p.drawRect(QRectF(11.0, 4.0, 9.5, 8.0));
        Arrow(p, QPointF(10.0, 13.5), QPointF(-1.0, 1.0), ink.stroke, 4.2, 1.8);
        break;
    }
    case SketchIcon::GroundInstance: {
        // A block sitting ON hatched ground -- the drawing convention for
        // "this does not move", which is what grounding means.
        p.setPen(Qt::NoPen);
        p.setBrush(ink.accent);
        p.drawRect(QRectF(6.0, 4.5, 12.0, 9.0));
        Stroke(p, ink.stroke, 1.7);
        p.drawLine(QPointF(3.0, 14.5), QPointF(21.0, 14.5));
        Stroke(p, ink.stroke, 1.2);
        for (int i = 0; i < 5; ++i) {
            const double x = 4.5 + i * 3.6;
            p.drawLine(QPointF(x, 20.0), QPointF(x + 2.6, 14.8));
        }
        break;
    }
    case SketchIcon::AddMate: {
        // TWO blocks meeting at ONE point -- the connector they share. The dot
        // is the whole idea: a mate is two things meeting somewhere.
        p.setPen(Qt::NoPen);
        p.setBrush(ink.subdue);
        p.drawRect(QRectF(2.5, 8.0, 8.0, 8.0));
        p.setBrush(ink.accent);
        p.drawRect(QRectF(13.5, 8.0, 8.0, 8.0));
        Dot(p, 12.0, 12.0, ink.stroke, 2.6);
        break;
    }
    case SketchIcon::DriveMate: {
        // A block turned about a pinned centre: driving is a value you set, and
        // the arc is the value moving.
        Stroke(p, ink.accent, 1.7);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(4.0, 4.0, 16.0, 16.0), 20 * 16, 200 * 16);
        Arrow(p, QPointF(19.4, 9.0), QPointF(0.8, -1.0), ink.accent, 4.2, 1.8);
        p.setPen(Qt::NoPen);
        p.setBrush(ink.stroke);
        p.drawRect(QRectF(9.5, 9.5, 5.0, 5.0));
        break;
    }
    case SketchIcon::LimitMate: {
        // The same turn, STOPPED at both ends. The stops are the point, so they
        // are the heaviest strokes in it.
        Stroke(p, ink.subdue, 1.5);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(4.0, 4.0, 16.0, 16.0), 30 * 16, 120 * 16);
        Stroke(p, ink.accent, 2.4);
        p.drawLine(QPointF(4.6, 10.0), QPointF(8.4, 12.2));
        p.drawLine(QPointF(19.4, 10.0), QPointF(15.6, 12.2));
        Dot(p, 12.0, 15.5, ink.stroke, 2.2);
        break;
    }
    case SketchIcon::AssemblyPattern: {
        // One solid block plus ghosts: a pattern is an original and copies, and
        // ADR-M26-003 says the copies hang off the original.
        p.setPen(Qt::NoPen);
        p.setBrush(ink.accent);
        p.drawRect(QRectF(2.5, 9.0, 6.0, 6.0));
        Stroke(p, ink.subdue, 1.4);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(9.5, 9.0, 6.0, 6.0));
        p.drawRect(QRectF(16.5, 9.0, 6.0, 6.0));
        break;
    }
    case SketchIcon::NamedPosition: {
        // A bookmark over a block: a place you can come back to, and nothing
        // about the part changes when you do.
        p.setPen(Qt::NoPen);
        p.setBrush(ink.subdue);
        p.drawRect(QRectF(2.5, 7.0, 11.0, 12.0));
        p.setBrush(ink.accent);
        const QPointF flag[5] = {QPointF(13.5, 3.5), QPointF(21.0, 3.5), QPointF(21.0, 16.0),
                                 QPointF(17.25, 12.0), QPointF(13.5, 16.0)};
        p.drawPolygon(flag, 5);
        break;
    }
    case SketchIcon::ExplodeView: {
        // Blocks flying APART along one line, with the line drawn: §49's
        // explosion is an ORDERED set of moves, and the trail is the order.
        Stroke(p, ink.subdue, 1.2, Qt::DashLine);
        p.drawLine(QPointF(3.5, 20.0), QPointF(20.5, 4.0));
        p.setPen(Qt::NoPen);
        p.setBrush(ink.accent);
        p.drawRect(QRectF(2.0, 16.0, 6.0, 6.0));
        p.drawRect(QRectF(9.0, 9.5, 6.0, 6.0));
        p.drawRect(QRectF(16.0, 3.0, 6.0, 6.0));
        break;
    }
    case SketchIcon::Interference: {
        // Two blocks OVERLAPPING, with the overlap itself filled -- which is
        // the number the check reports.
        Stroke(p, ink.stroke, 1.5);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(3.0, 5.5, 11.0, 11.0));
        p.drawRect(QRectF(10.0, 8.5, 11.0, 11.0));
        p.setPen(Qt::NoPen);
        p.setBrush(ink.accent);
        p.drawRect(QRectF(10.0, 8.5, 4.0, 8.0));
        break;
    }
    case SketchIcon::NewDrawing: {
        // A blank sheet with a folded corner: the universal "new document",
        // and the fold is what stops it reading as a plain rectangle.
        Stroke(p, ink.stroke, 1.4);
        p.setBrush(Qt::NoBrush);
        const QPointF page[5] = {QPointF(5.0, 3.0), QPointF(15.0, 3.0), QPointF(19.0, 7.0),
                                 QPointF(19.0, 21.0), QPointF(5.0, 21.0)};
        p.drawPolygon(page, 5);
        Stroke(p, ink.accent, 1.3);
        p.drawLine(QPointF(15.0, 3.0), QPointF(15.0, 7.0));
        p.drawLine(QPointF(15.0, 7.0), QPointF(19.0, 7.0));
        break;
    }
    case SketchIcon::BaseView: {
        // A SHEET with one view on it. The sheet is the outline, the view is
        // the filled block -- the same "a part is a filled block" rule the
        // assembly set uses, so a part reads as a part in both.
        Stroke(p, ink.subdue, 1.2);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(2.5, 4.0, 19.0, 16.0));
        p.setPen(Qt::NoPen);
        p.setBrush(ink.accent);
        p.drawRect(QRectF(6.0, 8.0, 8.0, 8.0));
        break;
    }
    case SketchIcon::ProjectedView: {
        // TWO views, one above the other, with the ALIGNMENT drawn as the
        // dashed line between them -- because the alignment is the whole point
        // and a picture of two blocks side by side would not say it.
        p.setPen(Qt::NoPen);
        p.setBrush(ink.accent);
        p.drawRect(QRectF(3.0, 13.5, 8.0, 7.0));
        p.setBrush(ink.subdue);
        p.drawRect(QRectF(3.0, 3.5, 8.0, 7.0));
        Stroke(p, ink.subdue, 1.0, Qt::DashLine);
        p.drawLine(QPointF(3.0, 12.0), QPointF(19.0, 12.0));
        p.drawLine(QPointF(11.0, 2.0), QPointF(11.0, 22.0));
        break;
    }
    case SketchIcon::UpdateViews: {
        // A sheet with a REFRESH arrow over it: the model moved on and the
        // paper has not caught up.
        Stroke(p, ink.subdue, 1.2);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(2.5, 4.0, 15.0, 16.0));
        Stroke(p, ink.accent, 1.6);
        p.drawArc(QRectF(8.0, 7.0, 13.0, 13.0), 30 * 16, 260 * 16);
        p.setPen(Qt::NoPen);
        p.setBrush(ink.accent);
        const QPointF head[3] = {QPointF(21.5, 10.5), QPointF(16.5, 11.5),
                                 QPointF(19.5, 6.5)};
        p.drawPolygon(head, 3);
        break;
    }
    case SketchIcon::SheetSetup: {
        // The sheet, with its SIZE called out -- two dimension ticks on the
        // edges, which is how paper is talked about.
        Stroke(p, ink.stroke, 1.3);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(4.0, 6.0, 16.0, 12.0));
        Stroke(p, ink.accent, 1.2);
        p.drawLine(QPointF(4.0, 3.0), QPointF(20.0, 3.0));
        p.drawLine(QPointF(4.0, 1.5), QPointF(4.0, 4.5));
        p.drawLine(QPointF(20.0, 1.5), QPointF(20.0, 4.5));
        break;
    }
    case SketchIcon::DrawingLayer: {
        // STACKED sheets. A layer is not a thing on the paper, it is one of
        // several papers the drawing is spread over -- which is exactly what
        // the stack says.
        Stroke(p, ink.subdue, 1.2);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(3.0, 3.0, 15.0, 11.0));
        p.drawRect(QRectF(5.0, 6.0, 15.0, 11.0));
        Stroke(p, ink.accent, 1.4);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(7.0, 9.0, 15.0, 11.0));
        break;
    }

    case SketchIcon::LinearDimension: {
        // A DIMENSION LINE WITH AN ARROW AT EACH END, and the two witness
        // lines that make it a dimension rather than a double-headed arrow.
        Stroke(p, ink.subdue, 1.1);
        p.drawLine(QPointF(4.0, 4.0), QPointF(4.0, 14.0));
        p.drawLine(QPointF(20.0, 4.0), QPointF(20.0, 14.0));
        Stroke(p, ink.stroke, 1.4);
        p.drawLine(QPointF(4.0, 12.0), QPointF(20.0, 12.0));
        Arrow(p, QPointF(4.0, 12.0), QPointF(-1.0, 0.0), ink.accent);
        Arrow(p, QPointF(20.0, 12.0), QPointF(1.0, 0.0), ink.accent);
        break;
    }

    case SketchIcon::RadiusDimension: {
        // AN ARC WITH A LEADER FROM ITS CENTRE. The centre mark is what says
        // "radius" -- without it this is a line touching a curve.
        Stroke(p, ink.stroke, 1.4);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(3.0, 5.0, 18.0, 18.0), 30 * 16, 120 * 16);
        Stroke(p, ink.subdue, 1.0);
        p.drawLine(QPointF(10.0, 14.0), QPointF(14.0, 14.0));
        p.drawLine(QPointF(12.0, 12.0), QPointF(12.0, 16.0));
        Stroke(p, ink.accent, 1.3);
        p.drawLine(QPointF(12.0, 14.0), QPointF(18.0, 8.0));
        Arrow(p, QPointF(18.0, 8.0), QPointF(1.0, -1.0), ink.accent);
        break;
    }

    case SketchIcon::DiameterDimension: {
        // A FULL CIRCLE CROSSED BY A LEADER WITH TWO ARROWS -- the difference
        // from the radius icon is that the line goes all the way THROUGH,
        // which is the difference between the two measurements.
        Stroke(p, ink.stroke, 1.4);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12.0, 12.0), 8.0, 8.0);
        Stroke(p, ink.accent, 1.3);
        p.drawLine(QPointF(6.3, 17.7), QPointF(17.7, 6.3));
        Arrow(p, QPointF(6.3, 17.7), QPointF(-1.0, 1.0), ink.accent);
        Arrow(p, QPointF(17.7, 6.3), QPointF(1.0, -1.0), ink.accent);
        break;
    }

    case SketchIcon::AngularDimension: {
        // TWO ARMS AND THE ARC BETWEEN THEM. The arc is the whole icon: two
        // arms alone are a corner, and a corner is not a measurement.
        Stroke(p, ink.subdue, 1.2);
        p.drawLine(QPointF(4.0, 20.0), QPointF(21.0, 20.0));
        p.drawLine(QPointF(4.0, 20.0), QPointF(18.0, 6.0));
        Stroke(p, ink.accent, 1.4);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(-5.0, 11.0, 18.0, 18.0), 0 * 16, 45 * 16);
        break;
    }

    case SketchIcon::DimensionStyleIcon: {
        // THE LINEAR DIMENSION AGAIN, with a brush across it -- the same
        // "this is the settings for that" pairing the sketch set uses, so a
        // user does not have to learn a second visual idea.
        Stroke(p, ink.subdue, 1.1);
        p.drawLine(QPointF(4.0, 6.0), QPointF(4.0, 14.0));
        p.drawLine(QPointF(20.0, 6.0), QPointF(20.0, 14.0));
        Stroke(p, ink.stroke, 1.3);
        p.drawLine(QPointF(4.0, 12.0), QPointF(20.0, 12.0));
        Arrow(p, QPointF(4.0, 12.0), QPointF(-1.0, 0.0), ink.stroke);
        Arrow(p, QPointF(20.0, 12.0), QPointF(1.0, 0.0), ink.stroke);
        Stroke(p, ink.accent, 1.6);
        p.drawLine(QPointF(9.0, 21.0), QPointF(19.0, 16.0));
        p.setBrush(QBrush(ink.accent));
        p.drawEllipse(QPointF(19.5, 15.5), 2.2, 2.2);
        break;
    }

    case SketchIcon::TitleBlock: {
        // A SHEET WITH A FILLED BOX IN ITS BOTTOM-RIGHT CORNER, which is
        // exactly where a reader's eye goes and exactly where the block is.
        Stroke(p, ink.subdue, 1.2);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(3.0, 4.0, 18.0, 16.0));
        Stroke(p, ink.accent, 1.2);
        p.setBrush(QBrush(ink.accent));
        p.drawRect(QRectF(11.0, 14.0, 10.0, 6.0));
        // Two rules across it, so the box reads as a TABLE and not a blob.
        Stroke(p, ink.stroke, 0.8);
        p.drawLine(QPointF(11.0, 16.0), QPointF(21.0, 16.0));
        p.drawLine(QPointF(11.0, 18.0), QPointF(21.0, 18.0));
        break;
    }

    case SketchIcon::AddRelation: {
        // TWO TOOTHED WHEELS, meshing. The teeth are what make it a gear
        // rather than two circles, and the mesh is what makes it a relation
        // rather than two mates: turn one and the other has to turn.
        Stroke(p, ink.stroke, 1.5);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(8.0, 9.0), 5.0, 5.0);
        p.drawEllipse(QPointF(16.5, 15.5), 4.0, 4.0);
        // Four teeth each, drawn as short radial stubs -- enough to read as a
        // gear at 24 px, where a full tooth ring is a grey smudge.
        Stroke(p, ink.accent, 1.5);
        p.drawLine(QPointF(8.0, 3.0), QPointF(8.0, 5.0));
        p.drawLine(QPointF(8.0, 13.0), QPointF(8.0, 15.0));
        p.drawLine(QPointF(2.0, 9.0), QPointF(4.0, 9.0));
        p.drawLine(QPointF(12.0, 9.0), QPointF(14.0, 9.0));
        p.drawLine(QPointF(16.5, 10.5), QPointF(16.5, 12.0));
        p.drawLine(QPointF(16.5, 19.0), QPointF(16.5, 20.5));
        p.drawLine(QPointF(11.0, 15.5), QPointF(12.5, 15.5));
        p.drawLine(QPointF(20.5, 15.5), QPointF(22.0, 15.5));
        break;
    }

    case SketchIcon::HVDistance:
        // BOTH legs at once: the two points, the horizontal run and the
        // vertical rise, drawn as the right-angled pair they are. It has to
        // read as "the two of them together" rather than as either one, so
        // neither leg is centred the way the single-leg icons are -- they meet
        // at the corner the ordinate pair actually makes.
        Stroke(p, ink.accent, 1.6);
        p.drawLine(QPointF(5.0, 18.0), QPointF(19.0, 18.0));   // the u leg
        p.drawLine(QPointF(19.0, 18.0), QPointF(19.0, 5.0));   // the v leg
        p.drawLine(QPointF(5.0, 15.5), QPointF(5.0, 20.5));    // tick on the u leg
        p.drawLine(QPointF(16.5, 5.0), QPointF(21.5, 5.0));    // tick on the v leg
        Stroke(p, ink.subdue, 1.2);
        p.drawLine(QPointF(5.0, 18.0), QPointF(19.0, 5.0));    // the pair itself
        Dot(p, 5.0, 18.0, ink.stroke, 2.2);
        Dot(p, 19.0, 5.0, ink.stroke, 2.2);
        break;

    case SketchIcon::PointLineDistance:
        // A point with a PERPENDICULAR dropped to a line, and the little square
        // that says the angle is a right angle -- which is the whole meaning of
        // the dimension.
        Stroke(p, ink.subdue, 1.5);
        p.drawLine(QPointF(3.5, 19.0), QPointF(20.5, 19.0));
        Stroke(p, ink.accent, 1.7);
        p.drawLine(QPointF(12.0, 6.0), QPointF(12.0, 19.0));
        p.drawRect(QRectF(12.0, 15.5, 3.5, 3.5));
        Dot(p, 12.0, 6.0, ink.stroke, 2.2);
        break;
    case SketchIcon::Offset: {
        // Two nested outlines: the shape, and a copy of it standing off. The
        // copy is DASHED so the pair reads as "this one is derived from that
        // one" rather than as two unrelated boxes.
        Stroke(p, ink.stroke, 1.7);
        p.drawRect(QRectF(4.0, 8.0, 10.0, 10.0));
        QPen dashed(ink.accent, 1.5);
        dashed.setStyle(Qt::DashLine);
        p.setPen(dashed);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(7.5, 4.5, 13.0, 13.0));
        break;
    }

    case SketchIcon::Trim: {
        // A line crossed by another, with the cut-off piece DASHED: the picture
        // of what the command does, not a pair of scissors that could mean any
        // of five commands.
        Stroke(p, ink.subdue, 1.5);
        p.drawLine(QPointF(15.0, 3.5), QPointF(15.0, 20.5));
        Stroke(p, ink.stroke, 1.9);
        p.drawLine(QPointF(3.5, 12.0), QPointF(15.0, 12.0));
        QPen dashed(ink.danger, 1.6);
        dashed.setStyle(Qt::DashLine);
        p.setPen(dashed);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(15.0, 12.0), QPointF(21.0, 12.0));
        break;
    }

    case SketchIcon::Extend: {
        // Trim's mirror image: a line reaching a boundary, with the NEW piece
        // dashed. Drawn as the opposite of Trim on purpose -- the two are
        // opposites, and icons that look unrelated make the pair harder to
        // learn than either alone.
        Stroke(p, ink.subdue, 1.5);
        p.drawLine(QPointF(20.0, 3.5), QPointF(20.0, 20.5));
        Stroke(p, ink.stroke, 1.9);
        p.drawLine(QPointF(3.5, 12.0), QPointF(12.0, 12.0));
        QPen grown(ink.accent, 1.6);
        grown.setStyle(Qt::DashLine);
        p.setPen(grown);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(12.0, 12.0), QPointF(20.0, 12.0));
        break;
    }
    case SketchIcon::Chamfer:
        // A corner with its point cut off, and the cut drawn heavier: the
        // result of the command rather than a symbol for it.
        Stroke(p, ink.subdue, 1.6);
        p.drawLine(QPointF(4.0, 20.5), QPointF(4.0, 10.0));
        p.drawLine(QPointF(20.5, 4.0), QPointF(10.0, 4.0));
        Stroke(p, ink.accent, 2.1);
        p.drawLine(QPointF(4.0, 10.0), QPointF(10.0, 4.0));
        break;

    case SketchIcon::Fillet: {
        // Chamfer's twin: the same corner, rounded instead of cut. Drawn as a
        // pair on purpose -- the two commands answer the same question and the
        // icons should say so.
        Stroke(p, ink.subdue, 1.6);
        p.drawLine(QPointF(4.0, 20.5), QPointF(4.0, 10.0));
        p.drawLine(QPointF(20.5, 4.0), QPointF(10.0, 4.0));
        Stroke(p, ink.accent, 2.1);
        p.drawArc(QRectF(4.0, 4.0, 12.0, 12.0), 180 * 16, -90 * 16);
        break;
    }
    case SketchIcon::Symmetric:
        // Two dots either side of an axis, with the axis drawn as a
        // centreline: the relationship, not an object.
        Stroke(p, ink.accent, 1.6);
        p.drawLine(QPointF(12.0, 3.0), QPointF(12.0, 21.0));
        Dot(p, 5.0, 12.0, ink.stroke, 2.6);
        Dot(p, 19.0, 12.0, ink.stroke, 2.6);
        break;
    case SketchIcon::Mirror: {
        // A shape and its reflection across the same axis, the copy DASHED --
        // the same "this one is derived from that one" language Offset uses.
        Stroke(p, ink.accent, 1.5);
        p.drawLine(QPointF(12.0, 3.0), QPointF(12.0, 21.0));
        Stroke(p, ink.stroke, 1.7);
        p.drawLine(QPointF(4.0, 6.0), QPointF(9.5, 12.0));
        p.drawLine(QPointF(9.5, 12.0), QPointF(4.0, 18.0));
        QPen copy(ink.subdue, 1.5);
        copy.setStyle(Qt::DashLine);
        p.setPen(copy);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(20.0, 6.0), QPointF(14.5, 12.0));
        p.drawLine(QPointF(14.5, 12.0), QPointF(20.0, 18.0));
        break;
    }

    // --- Sketch-mode commands --------------------------------------------
    case SketchIcon::OriginPoint:
        // Two AXES crossing at a marked corner -- the thing a draughtsman
        // measures from. Deliberately NOT the Point icon with a decoration:
        // that pair read as the same button at toolbar size, and the smoke test
        // rejects two buttons sharing a fingerprint for exactly this reason.
        Stroke(p, ink.subdue, 1.4);
        p.drawLine(QPointF(6.0, 18.0), QPointF(20.5, 18.0));
        p.drawLine(QPointF(6.0, 18.0), QPointF(6.0, 3.5));
        Stroke(p, ink.accent, 1.7);
        p.drawRect(QRectF(3.4, 15.4, 5.2, 5.2));
        break;
    case SketchIcon::Construction: {
        // A DASHED line, which is exactly how construction geometry is drawn on
        // the canvas. The icon and the result should be the same picture.
        QPen dashed(ink.accent, 1.9);
        dashed.setStyle(Qt::DashLine);
        dashed.setCapStyle(Qt::RoundCap);
        p.setPen(dashed);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(3.5, 18.5), QPointF(20.5, 5.5));
        Dot(p, 3.5, 18.5, ink.stroke, 2.0);
        Dot(p, 20.5, 5.5, ink.stroke, 2.0);
        break;
    }
    case SketchIcon::DeleteGeometry:
        Stroke(p, ink.subdue, 1.5);
        p.drawLine(QPointF(4.0, 18.0), QPointF(14.0, 6.5));
        Stroke(p, ink.danger, 2.1);
        p.drawLine(QPointF(13.0, 13.0), QPointF(20.5, 20.5));
        p.drawLine(QPointF(20.5, 13.0), QPointF(13.0, 20.5));
        break;
    case SketchIcon::FitSketch: {
        // Four corner brackets: the universal "fit to view".
        Stroke(p, ink.stroke, 1.7);
        const double a = 4.0;
        const double b = 20.0;
        const double t = 5.0;
        p.drawLine(QPointF(a, a + t), QPointF(a, a));
        p.drawLine(QPointF(a, a), QPointF(a + t, a));
        p.drawLine(QPointF(b - t, a), QPointF(b, a));
        p.drawLine(QPointF(b, a), QPointF(b, a + t));
        p.drawLine(QPointF(b, b - t), QPointF(b, b));
        p.drawLine(QPointF(b, b), QPointF(b - t, b));
        p.drawLine(QPointF(a + t, b), QPointF(a, b));
        p.drawLine(QPointF(a, b), QPointF(a, b - t));
        Stroke(p, ink.accent, 1.4);
        p.drawRect(QRectF(9.0, 9.5, 6.0, 5.0));
        break;
    }
    case SketchIcon::NewSketch:
    case SketchIcon::EditSketch:
    case SketchIcon::FinishSketch: {
        // A sketch PLANE seen in perspective, so all three read as one family
        // and differ only by the mark on them.
        QPainterPath plane;
        plane.moveTo(3.0, 15.5);
        plane.lineTo(10.5, 8.5);
        plane.lineTo(21.0, 8.5);
        plane.lineTo(13.5, 15.5);
        plane.closeSubpath();
        Stroke(p, ink.stroke, 1.4);
        p.drawPath(plane);

        if (icon == SketchIcon::NewSketch) {
            Stroke(p, ink.accent, 2.0);
            p.drawLine(QPointF(17.0, 14.0), QPointF(17.0, 21.0));
            p.drawLine(QPointF(13.5, 17.5), QPointF(20.5, 17.5));
        } else if (icon == SketchIcon::EditSketch) {
            // A pencil across the plane.
            Stroke(p, ink.accent, 1.9);
            p.drawLine(QPointF(13.0, 21.0), QPointF(20.5, 13.5));
            Stroke(p, ink.stroke, 1.4);
            p.drawLine(QPointF(12.4, 21.6), QPointF(14.2, 20.2));
        } else {
            Stroke(p, ink.accent, 2.2);
            p.drawLine(QPointF(13.5, 18.0), QPointF(16.2, 20.8));
            p.drawLine(QPointF(16.2, 20.8), QPointF(21.0, 14.5));
        }
        break;
    }

    // --- Solid modelling ---------------------------------------------------
    //
    // All three say the same sentence in three tenses: here is a PROFILE, and
    // here is what happens to it. The profile is drawn identically in each so
    // the family reads as one idea, and only the action mark differs -- which
    // is the part a user actually has to tell apart.
    case SketchIcon::Pad: {
        // A profile, and material growing UP out of it.
        Stroke(p, ink.subdue, 1.4);
        p.drawRect(QRectF(4.0, 14.0, 11.0, 6.0));
        Stroke(p, ink.accent, 1.9);
        p.drawLine(QPointF(9.5, 13.0), QPointF(9.5, 4.0));
        p.drawLine(QPointF(9.5, 4.0), QPointF(6.5, 7.5));
        p.drawLine(QPointF(9.5, 4.0), QPointF(12.5, 7.5));
        break;
    }
    case SketchIcon::Pocket: {
        // The same profile, with material going DOWN into it. Pad's arrow
        // reversed, deliberately: the two commands are opposites and the icons
        // should be too.
        Stroke(p, ink.subdue, 1.4);
        p.drawRect(QRectF(4.0, 14.0, 11.0, 6.0));
        Stroke(p, ink.danger, 1.9);
        p.drawLine(QPointF(9.5, 4.0), QPointF(9.5, 13.0));
        p.drawLine(QPointF(9.5, 13.0), QPointF(6.5, 9.5));
        p.drawLine(QPointF(9.5, 13.0), QPointF(12.5, 9.5));
        break;
    }
    case SketchIcon::SketchOnFace: {
        // A face seen at an angle, with a sketch drawn ON it. The parallelogram
        // is the face; the accent square is the drawing lying in its plane, so
        // it is skewed the same way -- an upright square would read as a
        // sketch floating in front of the face rather than on it.
        Stroke(p, ink.subdue, 1.4);
        QPolygonF face;
        face << QPointF(3.0, 9.0) << QPointF(13.0, 4.5) << QPointF(21.0, 10.0)
             << QPointF(11.0, 15.5);
        p.drawPolygon(face);
        Stroke(p, ink.accent, 1.6);
        QPolygonF drawn;
        drawn << QPointF(8.5, 9.5) << QPointF(13.5, 7.25) << QPointF(17.0, 9.6)
              << QPointF(12.0, 11.85);
        p.drawPolygon(drawn);
        break;
    }
    case SketchIcon::CenterRectangle: {
        // The same rectangle the corner tool's icon draws, with the CENTRE
        // marked -- the difference between the two tools is where you click,
        // and the icons say exactly that much.
        Stroke(p, ink.accent, 1.8);
        p.drawRect(QRectF(4.0, 7.0, 16.0, 10.0));
        Stroke(p, ink.subdue, 1.4);
        p.drawLine(QPointF(9.0, 12.0), QPointF(15.0, 12.0));
        p.drawLine(QPointF(12.0, 9.0), QPointF(12.0, 15.0));
        break;
    }
    case SketchIcon::ThreePointCircle: {
        Stroke(p, ink.accent, 1.8);
        p.drawEllipse(QPointF(12.0, 12.0), 8.0, 8.0);
        // The three points that define it, ON the rim -- which is the whole
        // difference from the centre-and-radius circle.
        Stroke(p, ink.subdue, 1.6);
        p.setBrush(ink.subdue);
        for (const QPointF& at : {QPointF(12.0, 4.0), QPointF(19.0, 16.0), QPointF(5.0, 16.0)})
            p.drawEllipse(at, 1.6, 1.6);
        p.setBrush(Qt::NoBrush);
        break;
    }
    case SketchIcon::ThreePointArc: {
        Stroke(p, ink.accent, 1.9);
        p.drawArc(QRectF(4.0, 6.0, 16.0, 16.0), 20 * 16, 140 * 16);
        Stroke(p, ink.subdue, 1.6);
        p.setBrush(ink.subdue);
        for (const QPointF& at : {QPointF(19.0, 11.0), QPointF(12.0, 6.0), QPointF(5.0, 11.0)})
            p.drawEllipse(at, 1.6, 1.6);
        p.setBrush(Qt::NoBrush);
        break;
    }
    case SketchIcon::TangentArc: {
        // A straight run, then an arc leaving it SMOOTHLY -- which is the whole
        // idea, so the join is where the eye should land. The dot marks the end
        // you click, because that is the one thing this tool needs and the one
        // thing the picture cannot otherwise say.
        Stroke(p, ink.subdue, 1.6);
        p.drawLine(QPointF(3.0, 16.0), QPointF(12.0, 16.0));
        Stroke(p, ink.accent, 1.9);
        // Centred above the joint, so it leaves horizontally: tangent by
        // construction rather than by a hand-picked bounding box.
        p.drawArc(QRectF(4.0, 8.0, 16.0, 16.0), 180 * 16, -90 * 16);
        Stroke(p, ink.subdue, 1.6);
        p.setBrush(ink.subdue);
        p.drawEllipse(QPointF(12.0, 16.0), 1.7, 1.7);
        p.setBrush(Qt::NoBrush);
        break;
    }
    case SketchIcon::Split: {
        // One line, and a GAP where the cut is -- the gap is the whole idea, so
        // it is what the eye lands on. The crossing line is drawn faint,
        // because it is the cutter and it does not change.
        Stroke(p, ink.subdue, 1.4);
        p.drawLine(QPointF(12.0, 4.0), QPointF(12.0, 20.0));
        Stroke(p, ink.accent, 2.0);
        p.drawLine(QPointF(3.0, 12.0), QPointF(10.0, 12.0));
        p.drawLine(QPointF(14.0, 12.0), QPointF(21.0, 12.0));
        break;
    }
    case SketchIcon::Transform: {
        // A shape and the same shape moved, with the arrow between them: the
        // MOVE is the idea, and rotate and scale are the same idea with a
        // different number, so one picture serves the whole command.
        Stroke(p, ink.subdue, 1.4, Qt::DashLine);
        p.drawRect(QRectF(3.0, 12.0, 8.0, 8.0));
        Stroke(p, ink.accent, 1.8);
        p.drawRect(QRectF(13.0, 4.0, 8.0, 8.0));
        Stroke(p, ink.subdue, 1.3);
        p.drawLine(QPointF(9.0, 12.0), QPointF(14.0, 7.0));
        p.drawLine(QPointF(14.0, 7.0), QPointF(11.0, 7.5));
        p.drawLine(QPointF(14.0, 7.0), QPointF(13.5, 10.0));
        break;
    }
    case SketchIcon::Ellipse: {
        // Wider than tall, and the two axes drawn faint: the LONG one is what
        // the second click points at, and the icon is the only place that gets
        // said before the tooltip is read.
        Stroke(p, ink.accent, 1.8);
        p.drawEllipse(QPointF(12.0, 12.0), 9.0, 5.5);
        Stroke(p, ink.subdue, 1.0, Qt::DashLine);
        p.drawLine(QPointF(3.0, 12.0), QPointF(21.0, 12.0));
        p.drawLine(QPointF(12.0, 6.5), QPointF(12.0, 17.5));
        break;
    }
    case SketchIcon::EllipticalArc: {
        // The same ellipse with a piece of it: the accent is what gets drawn,
        // the faint remainder is what does not.
        Stroke(p, ink.subdue, 1.0, Qt::DotLine);
        p.drawEllipse(QPointF(12.0, 12.0), 9.0, 5.5);
        Stroke(p, ink.accent, 2.0);
        p.drawArc(QRectF(3.0, 6.5, 18.0, 11.0), 0, 140 * 16);
        break;
    }
    case SketchIcon::MajorAxisDimension: {
        // The ellipse, faint, with the LONG axis dimensioned -- arrows on the
        // wide way across. The pair with MinorAxisDimension differ in which
        // way the arrows point, which is the whole difference between the two
        // commands.
        Stroke(p, ink.subdue, 1.2);
        p.drawEllipse(QPointF(12.0, 12.0), 9.0, 5.5);
        Stroke(p, ink.accent, 1.8);
        p.drawLine(QPointF(3.0, 12.0), QPointF(21.0, 12.0));
        p.drawLine(QPointF(3.0, 12.0), QPointF(6.5, 9.5));
        p.drawLine(QPointF(3.0, 12.0), QPointF(6.5, 14.5));
        p.drawLine(QPointF(21.0, 12.0), QPointF(17.5, 9.5));
        p.drawLine(QPointF(21.0, 12.0), QPointF(17.5, 14.5));
        break;
    }
    case SketchIcon::MinorAxisDimension: {
        Stroke(p, ink.subdue, 1.2);
        p.drawEllipse(QPointF(12.0, 12.0), 9.0, 5.5);
        Stroke(p, ink.accent, 1.8);
        p.drawLine(QPointF(12.0, 6.5), QPointF(12.0, 17.5));
        p.drawLine(QPointF(12.0, 6.5), QPointF(9.5, 10.0));
        p.drawLine(QPointF(12.0, 6.5), QPointF(14.5, 10.0));
        p.drawLine(QPointF(12.0, 17.5), QPointF(9.5, 14.0));
        p.drawLine(QPointF(12.0, 17.5), QPointF(14.5, 14.0));
        break;
    }
    case SketchIcon::Spline: {
        // An S-curve through three marked points: the POINTS are what the tool
        // takes, and marking them is what tells it apart from an arc.
        Stroke(p, ink.accent, 1.9);
        QPainterPath path;
        path.moveTo(3.0, 18.0);
        path.cubicTo(7.0, 18.0, 8.0, 6.0, 12.0, 6.0);
        path.cubicTo(16.0, 6.0, 17.0, 16.0, 21.0, 16.0);
        p.drawPath(path);
        Stroke(p, ink.subdue, 1.2);
        p.setBrush(ink.subdue);
        for (const QPointF& at : {QPointF(3.0, 18.0), QPointF(12.0, 6.0), QPointF(21.0, 16.0)})
            p.drawEllipse(at, 1.5, 1.5);
        p.setBrush(Qt::NoBrush);
        break;
    }
    case SketchIcon::Polygon: {
        // A hexagon, because six is the default and an icon that showed a
        // different count than the tool draws would be teaching the wrong
        // thing.
        Stroke(p, ink.accent, 1.8);
        QPolygonF hexagon;
        for (int i = 0; i < 6; ++i) {
            const double angle = 3.14159265358979323846 * (2.0 * i / 6.0);
            hexagon << QPointF(12.0 + 8.0 * std::cos(angle), 12.0 + 8.0 * std::sin(angle));
        }
        p.drawPolygon(hexagon);
        break;
    }
    case SketchIcon::Slot: {
        // The outline itself: two sides and two round ends. The centre line is
        // shown because that is what the first two clicks are.
        Stroke(p, ink.accent, 1.8);
        p.drawLine(QPointF(8.0, 8.0), QPointF(16.0, 8.0));
        p.drawLine(QPointF(8.0, 16.0), QPointF(16.0, 16.0));
        p.drawArc(QRectF(12.0, 8.0, 8.0, 8.0), 270 * 16, 180 * 16);
        p.drawArc(QRectF(4.0, 8.0, 8.0, 8.0), 90 * 16, 180 * 16);
        Stroke(p, ink.subdue, 1.0, Qt::DashLine);
        p.drawLine(QPointF(8.0, 12.0), QPointF(16.0, 12.0));
        break;
    }
    case SketchIcon::ReferenceDimension: {
        // The same dimension line, with the BRACKETS the canvas draws around a
        // reference value -- the icon teaches the convention rather than
        // inventing a second symbol for it.
        Stroke(p, ink.subdue, 1.2);
        p.drawLine(QPointF(5.0, 7.0), QPointF(5.0, 17.0));
        p.drawLine(QPointF(19.0, 7.0), QPointF(19.0, 17.0));
        Stroke(p, ink.accent, 1.6);
        p.drawLine(QPointF(5.0, 15.0), QPointF(19.0, 15.0));
        // Brackets, big enough to read at 24 pixels.
        p.drawArc(QRectF(4.0, 4.0, 6.0, 9.0), 90 * 16, 180 * 16);
        p.drawArc(QRectF(14.0, 4.0, 6.0, 9.0), 270 * 16, 180 * 16);
        break;
    }
    case SketchIcon::DimensionTool: {
        // A dimension line with its two witness lines and both arrowheads --
        // the thing the tool places, drawn as it is drawn on the canvas.
        Stroke(p, ink.subdue, 1.2);
        p.drawLine(QPointF(5.0, 6.0), QPointF(5.0, 17.0));
        p.drawLine(QPointF(19.0, 6.0), QPointF(19.0, 17.0));
        Stroke(p, ink.accent, 1.6);
        p.drawLine(QPointF(5.0, 15.0), QPointF(19.0, 15.0));
        p.drawLine(QPointF(5.0, 15.0), QPointF(8.0, 13.0));
        p.drawLine(QPointF(5.0, 15.0), QPointF(8.0, 17.0));
        p.drawLine(QPointF(19.0, 15.0), QPointF(16.0, 13.0));
        p.drawLine(QPointF(19.0, 15.0), QPointF(16.0, 17.0));
        break;
    }
    case SketchIcon::UseReference: {
        // A dot-dashed reference edge, and a solid one being lifted off it.
        // The stroke styles are the SAME two the canvas uses for reference and
        // real geometry, so the icon teaches the convention rather than
        // inventing a third symbol for it.
        Stroke(p, ink.subdue, 1.4, Qt::DashDotLine);
        p.drawLine(QPointF(3.0, 16.5), QPointF(21.0, 16.5));
        Stroke(p, ink.accent, 2.0);
        p.drawLine(QPointF(3.0, 8.5), QPointF(21.0, 8.5));
        // The lift: a short arrow from the reference up to the copy.
        Stroke(p, ink.accent, 1.5);
        p.drawLine(QPointF(12.0, 15.0), QPointF(12.0, 10.5));
        p.drawLine(QPointF(12.0, 10.5), QPointF(10.0, 12.5));
        p.drawLine(QPointF(12.0, 10.5), QPointF(14.0, 12.5));
        break;
    }
    case SketchIcon::Revolve: {
        // A profile beside an AXIS, with a sweep arrow round it. The axis is
        // dashed because that is how a centreline is drawn -- and, since
        // ADR-M17-021, how the command finds it.
        QPen axis(ink.subdue, 1.4);
        axis.setStyle(Qt::DashLine);
        p.setPen(axis);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(5.0, 3.0), QPointF(5.0, 21.0));
        Stroke(p, ink.stroke, 1.5);
        p.drawRect(QRectF(9.0, 9.0, 6.0, 10.0));
        Stroke(p, ink.accent, 1.7);
        p.drawArc(QRectF(5.0, 2.5, 14.0, 9.0), 200 * 16, -170 * 16);
        p.drawLine(QPointF(18.0, 8.0), QPointF(19.5, 4.5));
        p.drawLine(QPointF(18.0, 8.0), QPointF(14.6, 6.6));
        break;
    }

    // --- Document-level commands -----------------------------------------
    // --- M19-M22 solid modelling -----------------------------------------
    //
    // The visual rule for this group: the PROFILE or the material that goes in
    // is subdued, and what the command DOES to it is accent -- or danger when
    // it removes material. That is the same grammar Pad and Pocket already
    // use, so a user who has learned those two can read these eleven.
    //
    // Every one has to be visually distinct from every other, and that is not
    // a preference: --selftest fingerprints each toolbar icon and fails the
    // build if two match. An icon that "reads like Pad but for sweeps" would
    // be caught there rather than by somebody clicking the wrong button.
    case SketchIcon::Sweep: {
        // A profile, dragged along a path. The path is the point, so the path
        // is the accent and the profile is where it starts.
        Stroke(p, ink.subdue, 1.4);
        p.drawEllipse(QRectF(3.2, 13.6, 5.2, 5.2));
        QPainterPath path;
        path.moveTo(5.8, 13.2);
        path.cubicTo(6.6, 6.0, 13.0, 4.4, 19.0, 6.6);
        Stroke(p, ink.accent, 1.9);
        p.drawPath(path);
        Arrow(p, QPointF(19.4, 6.8), QPointF(2.6, 1.0), ink.accent);
        break;
    }
    case SketchIcon::Loft: {
        // TWO profiles of different sizes, and the skin between them. Sweep
        // has one profile and a path; loft has two profiles and no path, and
        // the icons say exactly that difference.
        Stroke(p, ink.subdue, 1.4);
        p.drawLine(QPointF(3.6, 4.4), QPointF(11.4, 4.4));
        p.drawLine(QPointF(6.2, 19.6), QPointF(19.4, 19.6));
        Stroke(p, ink.accent, 1.8);
        p.drawLine(QPointF(3.6, 4.4), QPointF(6.2, 19.6));
        p.drawLine(QPointF(11.4, 4.4), QPointF(19.4, 19.6));
        Stroke(p, ink.accent, 1.2, Qt::DashLine);
        p.drawLine(QPointF(7.5, 4.4), QPointF(12.8, 19.6));
        break;
    }
    case SketchIcon::Shell: {
        // A solid with its inside taken out and its top open. The WALL is what
        // the command is about, so the wall is the accent and the opening is
        // the gap in it.
        Stroke(p, ink.subdue, 1.4);
        p.drawRect(QRectF(4.0, 5.0, 16.0, 14.5));
        Stroke(p, ink.accent, 1.8);
        p.drawLine(QPointF(7.0, 5.0), QPointF(7.0, 16.5));
        p.drawLine(QPointF(7.0, 16.5), QPointF(17.0, 16.5));
        p.drawLine(QPointF(17.0, 16.5), QPointF(17.0, 5.0));
        break;
    }
    case SketchIcon::Hole: {
        // The bore seen from ABOVE -- a circle on a face, with the wall going
        // down behind it. Pocket is the same operation from the side; a hole
        // knows its diameter and its depth, and the round mouth is what tells
        // them apart at 16 px.
        Stroke(p, ink.subdue, 1.4);
        p.drawRect(QRectF(3.4, 7.0, 17.2, 12.0));
        Stroke(p, ink.danger, 1.8);
        p.drawEllipse(QRectF(8.4, 8.0, 7.2, 3.6));
        Stroke(p, ink.danger, 1.4, Qt::DashLine);
        p.drawLine(QPointF(8.4, 9.8), QPointF(8.4, 16.0));
        p.drawLine(QPointF(15.6, 9.8), QPointF(15.6, 16.0));
        Stroke(p, ink.danger, 1.4);
        QPainterPath bottom;
        bottom.moveTo(8.4, 16.0);
        bottom.cubicTo(9.6, 18.0, 14.4, 18.0, 15.6, 16.0);
        p.drawPath(bottom);
        break;
    }
    case SketchIcon::Union: {
        // Two circles, and the OUTLINE of the pair. The three booleans differ
        // only in which part is emphasised, which is the honest way to draw
        // them: they are the same two inputs with three answers.
        Stroke(p, ink.subdue, 1.3);
        p.drawEllipse(QRectF(3.4, 6.6, 11.0, 11.0));
        p.drawEllipse(QRectF(9.6, 6.6, 11.0, 11.0));
        Stroke(p, ink.accent, 2.0);
        QPainterPath outline;
        outline.arcMoveTo(QRectF(3.4, 6.6, 11.0, 11.0), 60.0);
        outline.arcTo(QRectF(3.4, 6.6, 11.0, 11.0), 60.0, 240.0);
        outline.arcTo(QRectF(9.6, 6.6, 11.0, 11.0), 240.0, 240.0);
        p.drawPath(outline);
        break;
    }
    case SketchIcon::Subtract: {
        // The FIRST solid, with the second taken out of it. The removed part is
        // danger, as Pocket's arrow is.
        Stroke(p, ink.subdue, 1.3);
        p.drawEllipse(QRectF(9.6, 6.6, 11.0, 11.0));
        Stroke(p, ink.accent, 2.0);
        QPainterPath keep;
        keep.arcMoveTo(QRectF(3.4, 6.6, 11.0, 11.0), 60.0);
        keep.arcTo(QRectF(3.4, 6.6, 11.0, 11.0), 60.0, 240.0);
        p.drawPath(keep);
        Stroke(p, ink.danger, 1.6);
        QPainterPath cut;
        cut.arcMoveTo(QRectF(9.6, 6.6, 11.0, 11.0), 120.0);
        cut.arcTo(QRectF(9.6, 6.6, 11.0, 11.0), 120.0, 120.0);
        p.drawPath(cut);
        break;
    }
    case SketchIcon::Intersect: {
        // ONLY the lens. Both inputs subdued, because neither of them survives
        // -- what comes out is the overlap and nothing else.
        Stroke(p, ink.subdue, 1.3);
        p.drawEllipse(QRectF(3.4, 6.6, 11.0, 11.0));
        p.drawEllipse(QRectF(9.6, 6.6, 11.0, 11.0));
        QPainterPath lens;
        lens.arcMoveTo(QRectF(3.4, 6.6, 11.0, 11.0), 60.0);
        lens.arcTo(QRectF(3.4, 6.6, 11.0, 11.0), 60.0, -120.0);
        lens.arcTo(QRectF(9.6, 6.6, 11.0, 11.0), 240.0, 120.0);
        lens.closeSubpath();
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(ink.accent);
        p.drawPath(lens);
        p.restore();
        break;
    }
    case SketchIcon::CircularPattern: {
        // Copies AROUND a centre. The centre dot is what makes it circular
        // rather than a row, so the centre is drawn even though nothing is
        // there in the model.
        Dot(p, 12.0, 12.6, ink.subdue, 1.4);
        Stroke(p, ink.subdue, 1.2, Qt::DashLine);
        p.drawEllipse(QRectF(4.6, 5.2, 14.8, 14.8));
        Stroke(p, ink.accent, 1.6);
        p.drawRect(QRectF(10.0, 3.4, 4.0, 3.6));
        p.drawRect(QRectF(17.0, 10.6, 4.0, 3.6));
        p.drawRect(QRectF(3.0, 10.6, 4.0, 3.6));
        break;
    }
    case SketchIcon::CurvePattern: {
        // Copies ALONG a curve. The same squares as the circular pattern, on a
        // path instead of a ring -- and the path is the accent because it is
        // the input the user has to supply.
        QPainterPath spine;
        spine.moveTo(2.6, 17.4);
        spine.cubicTo(7.0, 8.0, 15.0, 17.0, 21.4, 7.0);
        Stroke(p, ink.accent, 1.7);
        p.drawPath(spine);
        Stroke(p, ink.subdue, 1.5);
        p.drawRect(QRectF(2.4, 17.4, 3.4, 3.2));
        p.drawRect(QRectF(10.2, 12.0, 3.4, 3.2));
        p.drawRect(QRectF(18.2, 6.4, 3.4, 3.2));
        break;
    }
    case SketchIcon::ExportModel: {
        // The part, and an arrow LEAVING it. Import is the same drawing with
        // the arrow reversed, for the same reason Pad and Pocket are.
        Stroke(p, ink.subdue, 1.4);
        p.drawRect(QRectF(3.4, 6.4, 9.4, 11.2));
        Stroke(p, ink.accent, 1.9);
        p.drawLine(QPointF(11.4, 12.0), QPointF(19.6, 12.0));
        Arrow(p, QPointF(20.4, 12.0), QPointF(1.0, 0.0), ink.accent);
        break;
    }
    case SketchIcon::ImportModel: {
        Stroke(p, ink.subdue, 1.4);
        p.drawRect(QRectF(11.2, 6.4, 9.4, 11.2));
        Stroke(p, ink.accent, 1.9);
        p.drawLine(QPointF(12.6, 12.0), QPointF(4.4, 12.0));
        Arrow(p, QPointF(3.6, 12.0), QPointF(-1.0, 0.0), ink.accent);
        break;
    }
    case SketchIcon::Undo:
    case SketchIcon::Redo: {
        // A curved arrow, mirrored for Redo. The pair has to be legible as a
        // PAIR and still tell which is which at 20 px -- so the two are exact
        // mirrors rather than two different drawings, and the arrowhead is what
        // carries the direction.
        const bool undo = icon == SketchIcon::Undo;
        const double dir = undo ? 1.0 : -1.0;
        const double cx = 12.0;
        QPainterPath arc;
        // A three-quarter loop, open at the bottom on the side the arrow leaves.
        arc.arcMoveTo(QRectF(5.0, 6.0, 14.0, 12.0), undo ? 200.0 : -20.0);
        arc.arcTo(QRectF(5.0, 6.0, 14.0, 12.0), undo ? 200.0 : -20.0, undo ? -250.0 : 250.0);
        Stroke(p, ink.stroke, 1.9);
        p.drawPath(arc);

        // The head, at the tail end of the sweep, pointing back down and in.
        const double hx = cx - dir * 6.4;
        const double hy = 15.6;
        Stroke(p, ink.accent, 1.9);
        p.drawLine(QPointF(hx, hy), QPointF(hx + dir * 0.4, hy - 5.4));
        p.drawLine(QPointF(hx, hy), QPointF(hx + dir * 5.2, hy - 2.2));
        break;
    }
    case SketchIcon::Recompute: {
        // A closed loop of two arcs: "run it again". Deliberately a RING rather
        // than the single curve Undo uses, so the two never read as each other.
        Stroke(p, ink.stroke, 1.9);
        p.drawArc(QRectF(5.0, 5.0, 14.0, 14.0), 30 * 16, 150 * 16);
        p.drawArc(QRectF(5.0, 5.0, 14.0, 14.0), 210 * 16, 150 * 16);
        Stroke(p, ink.accent, 1.9);
        p.drawLine(QPointF(18.4, 9.4), QPointF(18.4, 14.0));
        p.drawLine(QPointF(18.4, 9.4), QPointF(14.0, 9.4));
        p.drawLine(QPointF(5.6, 14.6), QPointF(5.6, 10.0));
        p.drawLine(QPointF(5.6, 14.6), QPointF(10.0, 14.6));
        break;
    }
    case SketchIcon::Count:
        // Not an icon. Painting nothing is right, and being in the switch at
        // all is what keeps it exhaustive -- so a new icon is a compile error
        // rather than a blank button.
        break;
    case SketchIcon::Visibility: {
        // An eye. Show/Hide is a visibility toggle, and no amount of geometry
        // says "visible" the way an eye does.
        QPainterPath eye;
        eye.moveTo(3.5, 12.0);
        eye.quadTo(12.0, 4.5, 20.5, 12.0);
        eye.quadTo(12.0, 19.5, 3.5, 12.0);
        Stroke(p, ink.stroke, 1.6);
        p.drawPath(eye);
        Dot(p, 12.0, 12.0, ink.accent, 2.6);
        break;
    }
    }
}

} // namespace

const char* SketchIconName(SketchIcon icon) noexcept {
    switch (icon) {
    case SketchIcon::Select: return "Select";
    case SketchIcon::Line: return "Line";
    case SketchIcon::Rectangle: return "Rectangle";
    case SketchIcon::Circle: return "Circle";
    case SketchIcon::Arc: return "Arc";
    case SketchIcon::Point: return "Point";
    case SketchIcon::Coincident: return "Coincident";
    case SketchIcon::Horizontal: return "Horizontal";
    case SketchIcon::Vertical: return "Vertical";
    case SketchIcon::Fix: return "Fix";
    case SketchIcon::Parallel: return "Parallel";
    case SketchIcon::Perpendicular: return "Perpendicular";
    case SketchIcon::Equal: return "Equal";
    case SketchIcon::Concentric: return "Concentric";
    case SketchIcon::Midpoint: return "Midpoint";
    case SketchIcon::PointOnObject: return "PointOnObject";
    case SketchIcon::Tangent: return "Tangent";
    case SketchIcon::Dimension: return "Dimension";
    case SketchIcon::Radius: return "Radius";
    case SketchIcon::Diameter: return "Diameter";
    case SketchIcon::AutoPlaceDimensions: return "AutoPlaceDimensions";
    case SketchIcon::HorizontalDistance: return "HorizontalDistance";
    case SketchIcon::VerticalDistance: return "VerticalDistance";
    case SketchIcon::InsertInstance: return "InsertInstance";
    case SketchIcon::GroundInstance: return "GroundInstance";
    case SketchIcon::AddMate: return "AddMate";
    case SketchIcon::DriveMate: return "DriveMate";
    case SketchIcon::LimitMate: return "LimitMate";
    case SketchIcon::AssemblyPattern: return "AssemblyPattern";
    case SketchIcon::NamedPosition: return "NamedPosition";
    case SketchIcon::ExplodeView: return "ExplodeView";
    case SketchIcon::Interference: return "Interference";
    case SketchIcon::AddRelation: return "AddRelation";
    case SketchIcon::NewDrawing: return "NewDrawing";
    case SketchIcon::BaseView: return "BaseView";
    case SketchIcon::ProjectedView: return "ProjectedView";
    case SketchIcon::UpdateViews: return "UpdateViews";
    case SketchIcon::SheetSetup: return "SheetSetup";
    case SketchIcon::DrawingLayer: return "DrawingLayer";
    case SketchIcon::LinearDimension: return "LinearDimension";
    case SketchIcon::RadiusDimension: return "RadiusDimension";
    case SketchIcon::DiameterDimension: return "DiameterDimension";
    case SketchIcon::AngularDimension: return "AngularDimension";
    case SketchIcon::DimensionStyleIcon: return "DimensionStyleIcon";
    case SketchIcon::TitleBlock: return "TitleBlock";
    case SketchIcon::HVDistance: return "HVDistance";
    case SketchIcon::PointLineDistance: return "PointLineDistance";
    case SketchIcon::Offset: return "Offset";
    case SketchIcon::Trim: return "Trim";
    case SketchIcon::Extend: return "Extend";
    case SketchIcon::Chamfer: return "Chamfer";
    case SketchIcon::Fillet: return "Fillet";
    case SketchIcon::Symmetric: return "Symmetric";
    case SketchIcon::Mirror: return "Mirror";
    case SketchIcon::OriginPoint: return "OriginPoint";
    case SketchIcon::Construction: return "Construction";
    case SketchIcon::Pad: return "Pad";
    case SketchIcon::Pocket: return "Pocket";
    case SketchIcon::Revolve: return "Revolve";
    case SketchIcon::SketchOnFace: return "SketchOnFace";
    case SketchIcon::UseReference: return "UseReference";
    case SketchIcon::CenterRectangle: return "CenterRectangle";
    case SketchIcon::ThreePointCircle: return "ThreePointCircle";
    case SketchIcon::ThreePointArc: return "ThreePointArc";
    case SketchIcon::TangentArc: return "TangentArc";
    case SketchIcon::Split: return "Split";
    case SketchIcon::Transform: return "Transform";
    case SketchIcon::Ellipse: return "Ellipse";
    case SketchIcon::EllipticalArc: return "EllipticalArc";
    case SketchIcon::MajorAxisDimension: return "MajorAxisDimension";
    case SketchIcon::MinorAxisDimension: return "MinorAxisDimension";
    case SketchIcon::Spline: return "Spline";
    case SketchIcon::Polygon: return "Polygon";
    case SketchIcon::DimensionTool: return "DimensionTool";
    case SketchIcon::ReferenceDimension: return "ReferenceDimension";
    case SketchIcon::Slot: return "Slot";
    case SketchIcon::Sweep: return "Sweep";
    case SketchIcon::Loft: return "Loft";
    case SketchIcon::Shell: return "Shell";
    case SketchIcon::Hole: return "Hole";
    case SketchIcon::Union: return "Union";
    case SketchIcon::Subtract: return "Subtract";
    case SketchIcon::Intersect: return "Intersect";
    case SketchIcon::CircularPattern: return "CircularPattern";
    case SketchIcon::CurvePattern: return "CurvePattern";
    case SketchIcon::ExportModel: return "ExportModel";
    case SketchIcon::ImportModel: return "ImportModel";
    case SketchIcon::Undo: return "Undo";
    case SketchIcon::Redo: return "Redo";
    case SketchIcon::Recompute: return "Recompute";
    case SketchIcon::Visibility: return "Visibility";
    case SketchIcon::Count: return "Count";
    case SketchIcon::DeleteGeometry: return "DeleteGeometry";
    case SketchIcon::FitSketch: return "FitSketch";
    case SketchIcon::NewSketch: return "NewSketch";
    case SketchIcon::EditSketch: return "EditSketch";
    case SketchIcon::FinishSketch: return "FinishSketch";
    }
    return "Unknown";
}

const SketchIcon* AllSketchIcons(int* count) noexcept {
    // Built once, from the enum's own extent. Adding an icon needs no change
    // here, which is the whole point.
    static const std::vector<SketchIcon> kAll = [] {
        std::vector<SketchIcon> all;
        all.reserve(static_cast<std::size_t>(SketchIcon::Count));
        for (int i = 0; i < static_cast<int>(SketchIcon::Count); ++i)
            all.push_back(static_cast<SketchIcon>(i));
        return all;
    }();
    if (count != nullptr) *count = static_cast<int>(kAll.size());
    return kAll.data();
}

QIcon MakeSketchIcon(SketchIcon icon, const QPalette& palette) {
    const Ink ink = InkFor(palette);
    QIcon result;
    for (const int size : kSizes) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // ONE coordinate system for every size: the drawing is authored on a
        // 24-unit grid and the scale does the rest, so a 48 px icon is the same
        // artwork rather than a second one that drifted.
        painter.scale(size / kGrid, size / kGrid);
        PaintIcon(painter, icon, ink);
        painter.end();
        result.addPixmap(pixmap);
    }
    return result;
}

} // namespace paramcad::ui
