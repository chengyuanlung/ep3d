#include "Viewer/DrawingPlot.h"

#include "Viewer/DrawingPainter.h"

#include <QFileInfo>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSizeF>

namespace paramcad {

DrawingTransform PageTransformFor(double sheetHeightMm, int resolutionDpi) noexcept {
    // ONE MILLIMETRE IS ONE MILLIMETRE.
    //
    // The device is `resolutionDpi` dots to the inch, so a millimetre is
    // dpi/25.4 of them. Sheet Y runs up and the page's runs down, which
    // DrawingTransform already flips -- so the origin is the page's
    // BOTTOM-left, which is `sheetHeightMm` down the page.
    constexpr double kMmPerInch = 25.4;
    DrawingTransform page;
    page.pixelsPerMm = static_cast<double>(resolutionDpi) / kMmPerInch;
    page.originPx = QPointF(0.0, sheetHeightMm * page.pixelsPerMm);
    return page;
}

PlotResult PlotDrawingToPdf(const DrawingDocument& document, const QString& path,
                            int resolutionDpi) {
    PlotResult out;
    const auto refuse = [&out](std::string why) {
        out.why = std::move(why);
        return out;
    };
    if (path.isEmpty()) return refuse("a plot needs somewhere to go");
    if (resolutionDpi < 72) return refuse("a plot below 72 dpi is not readable");

    const double widthMm = document.sheet().widthMm();
    const double heightMm = document.sheet().heightMm();
    if (!(widthMm > 0.0) || !(heightMm > 0.0))
        return refuse("this sheet has no area to plot");
    out.widthMm = widthMm;
    out.heightMm = heightMm;

    QPdfWriter writer(path);
    writer.setResolution(resolutionDpi);
    // THE PAPER IS THE SHEET'S OWN SIZE, given in millimetres rather than as
    // QPageSize::A3 -- a custom sheet has no named size, and routing the named
    // ones through the same call means there is one path and not two.
    writer.setPageSize(QPageSize(QSizeF(widthMm, heightMm), QPageSize::Millimeter));
    // NO MARGINS AT THE PAGE LEVEL. The drawing has its own frame, whose
    // margins the user set; a second set here would inset the first and the
    // border would no longer be where the sheet says it is.
    writer.setPageMargins(QMarginsF(0.0, 0.0, 0.0, 0.0));
    writer.setTitle(QStringLiteral("EP3D drawing"));

    QPainter painter;
    if (!painter.begin(&writer))
        return refuse("that file could not be opened for writing");
    painter.setRenderHint(QPainter::Antialiasing, true);

    const DrawingTransform page = PageTransformFor(heightMm, resolutionDpi);

    DrawingPaintOptions options;
    options.ink = QColor(0, 0, 0);
    // A PAGE IS ALREADY PAPER. Filling it wastes toner, and on a printer that
    // cannot lay down white it comes out grey.
    options.fillPaper = false;
    // ...and the reader can see where the paper ends by holding it.
    options.drawPaperEdge = false;

    const DrawnTally tally = PaintDrawing(painter, document, page, options);
    painter.end();

    if (!QFileInfo::exists(path)) return refuse("the plot was written and the file is not there");
    // A PLOT OF NOTHING IS REPORTED AS SUCH, rather than handed over as a
    // finished drawing. An empty page that a program said it wrote is the
    // worst of both: nobody looks at it until it matters.
    if (tally.curves == 0 && tally.dimensions == 0 && tally.frameLines == 0 &&
        tally.titleBlockRows == 0)
        return refuse("there is nothing on this sheet to plot");

    out.ok = true;
    return out;
}

} // namespace paramcad
