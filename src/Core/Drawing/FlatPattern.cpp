#include "Core/Drawing/FlatPattern.h"

namespace paramcad {

namespace {

constexpr double kTiny = 1e-9;

} // namespace

FlatPatternResultGeometry FlatPatternOf(const SheetContour& contour, SheetMaterial material,
                                        double thicknessMm, double widthMm) {
    FlatPatternResultGeometry out;
    if (!(widthMm > kTiny)) {
        out.why = "a blank with no width is not a blank";
        return out;
    }
    // THE SAME REFUSALS THE FOLD MAKES, asked of the same function. A part
    // this program will not fold is a part it must not hand a blank for --
    // the blank would be cut, and then nothing would bend it.
    out.why = WhyContourRefused(contour, material, thicknessMm);
    if (!out.why.empty()) return out;

    // ONE WALK. The running total that places each bend band is the running
    // total that ends as the blank's length -- so a blank of the right size
    // with its folds in the wrong places is not a state this can be in.
    double along = 0.0;
    for (const ContourStep& step : contour.steps) {
        along += step.flangeMm;
        BendLine line;
        line.fromMm = along;
        const double k = KFactorFor(material, step.bend.innerRadiusMm, thicknessMm);
        along += BendAllowanceMm(step.bend, thicknessMm, k);
        line.toMm = along;
        // WHICH WAY, AND HOW FAR. A flat pattern with unmarked folds is one
        // the operator has to guess at, and the guess is fifty-fifty per bend.
        line.angleDeg = step.bend.angleDeg;
        line.turnsLeft = step.turnsLeft;
        line.innerRadiusMm = step.bend.innerRadiusMm;
        out.bendLines.push_back(line);
    }
    along += contour.lastFlangeMm;

    out.lengthMm = along;
    out.widthMm = widthMm;
    out.ok = true;
    return out;
}

ProjectedDrawing FlatPatternDrawing(const FlatPatternResultGeometry& pattern) {
    ProjectedDrawing drawing;
    if (!pattern.ok) return drawing;

    const auto line = [&](Vec2 a, Vec2 b, ProjectedEdgeKind kind) {
        ProjectedCurve curve;
        curve.shape = ProjectedLine{a, b};
        curve.kind = kind;
        curve.visibility = ProjectedVisibility::Visible;
        drawing.curves.push_back(curve);
        GrowExtent(drawing.extent, curve);
    };

    const double L = pattern.lengthMm;
    const double W = pattern.widthMm;
    // The outline, as four sharp edges -- the cut path.
    line(Vec2{0.0, 0.0}, Vec2{L, 0.0}, ProjectedEdgeKind::Sharp);
    line(Vec2{L, 0.0}, Vec2{L, W}, ProjectedEdgeKind::Sharp);
    line(Vec2{L, W}, Vec2{0.0, W}, ProjectedEdgeKind::Sharp);
    line(Vec2{0.0, W}, Vec2{0.0, 0.0}, ProjectedEdgeKind::Sharp);

    for (const BendLine& bend : pattern.bendLines) {
        // BOTH EDGES OF THE BAND. A bend is as wide as its allowance, and one
        // line in the middle leaves the operator to decide which edge the
        // press meets -- which on a tight radius is most of a millimetre.
        //
        // SMOOTH, not sharp: they are not edges of the part. Drawn as sharp
        // they would read as a cut, and a flat pattern is a thing that goes to
        // a laser.
        line(Vec2{bend.fromMm, 0.0}, Vec2{bend.fromMm, W}, ProjectedEdgeKind::Smooth);
        line(Vec2{bend.toMm, 0.0}, Vec2{bend.toMm, W}, ProjectedEdgeKind::Smooth);
    }
    return drawing;
}

} // namespace paramcad
