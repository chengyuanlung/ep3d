#include "Core/Measure/SketchMeasure.h"

#include "Core/Sketch/Sketch.h"

#include <cmath>
#include <variant>

namespace paramcad {

const char* MeasureUnitSuffix(MeasureUnit unit) noexcept {
    switch (unit) {
    case MeasureUnit::Millimetre: return "mm";
    case MeasureUnit::SquareMillimetre: return "mm^2";
    case MeasureUnit::Radian: return "rad";
    case MeasureUnit::Count: break;
    }
    return "";
}

namespace {

constexpr double kPi = 3.14159265358979323846;

// How finely a curve without a closed-form length is walked.
//
// A chord underestimates the arc it spans by about (dtheta)^2/24 of its length,
// so 256 chords per span leaves a spline's length short by roughly six parts in
// ten million -- under a tenth of a micrometre on 100 mm, and below the
// tolerance every other number in this program is compared at.
//
// The same count over a WHOLE ellipse is 256 chords for the entire 2*pi, and
// that is only accurate to two parts in a hundred thousand -- 7 micrometres on
// a 60 x 25 ellipse, which a second, independent approximation caught (see
// M18_MEA_005). Ellipses are integrated instead, below.
//
// Both remain APPROXIMATE, and are reported as such. Being more accurate than
// the reader needs is not the same as being exact.
constexpr int kLengthSamplesPerSpan = 256;

double PolylineLength(const std::vector<Vec2>& points) {
    double total = 0.0;
    for (std::size_t i = 1; i < points.size(); ++i)
        total += std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
    return total;
}

// An ellipse's arc length has no elementary closed form -- it is the elliptic
// integral the function is named after -- so it is INTEGRATED rather than
// walked.
//
// Simpson's rule on the speed |dP/dt| = sqrt(a^2 sin^2 t + b^2 cos^2 t), which
// is smooth and periodic, so the error falls off as the fourth power of the
// step: 256 intervals put it near the limit of what a double can express.
// Chording the curve instead was only accurate to two parts in a hundred
// thousand, because a chord always cuts the corner and never overshoots -- the
// errors accumulate in one direction rather than cancelling.
//
// The ROTATION does not appear: turning an ellipse does not change how long it
// is. It is still taken as an argument so that every caller passes the same
// four numbers that describe an ellipse everywhere else in this program, and
// nobody has to remember that this one function is the exception.
double EllipticalArcLength(Vec2 centre, double major, double minor, double rotation, double from,
                           double to) {
    (void)centre;
    (void)rotation;
    const int steps = kLengthSamplesPerSpan; // even, which Simpson requires
    const double step = (to - from) / steps;
    const auto speed = [&](double t) {
        const double sinT = std::sin(t);
        const double cosT = std::cos(t);
        return std::sqrt(major * major * sinT * sinT + minor * minor * cosT * cosT);
    };
    double total = speed(from) + speed(to);
    for (int i = 1; i < steps; ++i)
        total += (i % 2 == 0 ? 2.0 : 4.0) * speed(from + step * i);
    return std::fabs(total * step / 3.0);
}

double SweptAngle(double from, double to, bool counterClockwise) {
    double sweep = counterClockwise ? to - from : from - to;
    while (sweep < 0.0) sweep += 2.0 * kPi;
    while (sweep > 2.0 * kPi) sweep -= 2.0 * kPi;
    return sweep;
}

} // namespace

double SketchGeometryLength(const SketchGeometry& geometry, bool* approximate) {
    const auto exact = [&](double value) {
        if (approximate != nullptr) *approximate = false;
        return value;
    };
    const auto sampled = [&](double value) {
        if (approximate != nullptr) *approximate = true;
        return value;
    };

    if (const auto* line = std::get_if<SketchLine>(&geometry))
        return exact(std::hypot(line->end.x - line->start.x, line->end.y - line->start.y));
    if (const auto* circle = std::get_if<SketchCircle>(&geometry))
        return exact(2.0 * kPi * circle->radiusMm);
    if (const auto* arc = std::get_if<SketchArc>(&geometry))
        return exact(arc->radiusMm *
                     SweptAngle(arc->startAngleRad, arc->endAngleRad, arc->counterClockwise));
    if (const auto* full = std::get_if<SketchEllipse>(&geometry))
        return sampled(EllipticalArcLength(full->center, full->majorRadiusMm, full->minorRadiusMm,
                                           full->rotationRad, 0.0, 2.0 * kPi));
    if (const auto* piece = std::get_if<SketchEllipticalArc>(&geometry)) {
        // ALONG THE DIRECTION IT IS DRAWN. Walking from the smaller parameter
        // to the larger would measure the other piece of the same ellipse for
        // every clockwise arc -- the right number for a shape nobody drew.
        double from = piece->startParamRad;
        double to = piece->endParamRad;
        if (!piece->counterClockwise) std::swap(from, to);
        while (to < from) to += 2.0 * kPi;
        return sampled(EllipticalArcLength(piece->center, piece->majorRadiusMm,
                                           piece->minorRadiusMm, piece->rotationRad, from, to));
    }
    if (const auto* spline = std::get_if<SketchSpline>(&geometry)) {
        std::vector<Vec2> walk = SampleSpline(*spline, kLengthSamplesPerSpan);
        // A CLOSED spline's sampler stops one step short of coming back round,
        // because the first point IS the last. The closing step is part of the
        // length even though it is not part of the sample list.
        if (spline->closed && walk.size() >= 2) walk.push_back(walk.front());
        return sampled(PolylineLength(walk));
    }
    return exact(0.0);
}

namespace {

// Everything worth saying about ONE entity. Each kind answers what it has and
// stays silent about what it has not -- an ellipse reports two radii and no
// "radius", because the single number a caller would read from that label does
// not exist (the same reason IsCurveRef refuses one).
std::vector<MeasureItem> ItemsForOne(const SketchGeometry& geometry) {
    std::vector<MeasureItem> items;
    const auto add = [&](std::string label, double value, MeasureUnit unit,
                         bool approximate) {
        items.push_back(MeasureItem{std::move(label), value, unit, approximate});
    };

    if (const auto* point = std::get_if<SketchPoint>(&geometry)) {
        add("u", point->position.x, MeasureUnit::Millimetre, false);
        add("v", point->position.y, MeasureUnit::Millimetre, false);
        return items;
    }
    if (const auto* line = std::get_if<SketchLine>(&geometry)) {
        add("length", SketchGeometryLength(geometry, nullptr), MeasureUnit::Millimetre, false);
        add("angle", std::atan2(line->end.y - line->start.y, line->end.x - line->start.x),
            MeasureUnit::Radian, false);
        add("du", line->end.x - line->start.x, MeasureUnit::Millimetre, false);
        add("dv", line->end.y - line->start.y, MeasureUnit::Millimetre, false);
        return items;
    }
    if (const auto* circle = std::get_if<SketchCircle>(&geometry)) {
        add("radius", circle->radiusMm, MeasureUnit::Millimetre, false);
        add("diameter", 2.0 * circle->radiusMm, MeasureUnit::Millimetre, false);
        add("perimeter", SketchGeometryLength(geometry, nullptr), MeasureUnit::Millimetre, false);
        add("area", kPi * circle->radiusMm * circle->radiusMm, MeasureUnit::SquareMillimetre,
            false);
        return items;
    }
    if (const auto* arc = std::get_if<SketchArc>(&geometry)) {
        add("radius", arc->radiusMm, MeasureUnit::Millimetre, false);
        add("length", SketchGeometryLength(geometry, nullptr), MeasureUnit::Millimetre, false);
        add("sweep", SweptAngle(arc->startAngleRad, arc->endAngleRad, arc->counterClockwise),
            MeasureUnit::Radian, false);
        return items;
    }
    if (const auto* full = std::get_if<SketchEllipse>(&geometry)) {
        bool approximate = false;
        const double perimeter = SketchGeometryLength(geometry, &approximate);
        add("major", full->majorRadiusMm, MeasureUnit::Millimetre, false);
        add("minor", full->minorRadiusMm, MeasureUnit::Millimetre, false);
        add("rotation", full->rotationRad, MeasureUnit::Radian, false);
        add("perimeter", perimeter, MeasureUnit::Millimetre, approximate);
        add("area", kPi * full->majorRadiusMm * full->minorRadiusMm,
            MeasureUnit::SquareMillimetre, false);
        return items;
    }
    if (const auto* piece = std::get_if<SketchEllipticalArc>(&geometry)) {
        bool approximate = false;
        const double length = SketchGeometryLength(geometry, &approximate);
        add("major", piece->majorRadiusMm, MeasureUnit::Millimetre, false);
        add("minor", piece->minorRadiusMm, MeasureUnit::Millimetre, false);
        add("rotation", piece->rotationRad, MeasureUnit::Radian, false);
        add("length", length, MeasureUnit::Millimetre, approximate);
        return items;
    }
    if (const auto* spline = std::get_if<SketchSpline>(&geometry)) {
        bool approximate = false;
        const double length = SketchGeometryLength(geometry, &approximate);
        add("points", static_cast<double>(spline->points.size()), MeasureUnit::Count, false);
        add("length", length, MeasureUnit::Millimetre, approximate);
        return items;
    }
    return items;
}

} // namespace

MeasureResult MeasureSketch(const Sketch& sketch,
                            const std::vector<SketchElementRef>& selection) {
    const auto refuse = [](std::string why) {
        MeasureResult out;
        out.message = std::move(why);
        return out;
    };
    if (selection.empty()) return refuse("Select something to measure.");
    if (selection.size() > 2)
        // Answering about the first two would report a number about geometry
        // the caller did not ask about, and it would be a plausible number.
        return refuse("Measure takes one entity or two; " + std::to_string(selection.size()) +
                      " selected.");

    const auto entityOf = [&](const SketchElementRef& ref) { return sketch.findEntity(ref.entityId); };
    for (const SketchElementRef& ref : selection)
        if (entityOf(ref) == nullptr) return refuse("That is not in this sketch any more.");

    MeasureResult out;
    if (selection.size() == 1) {
        const SketchEntity* entity = entityOf(selection.front());
        // A SUB-ELEMENT is a point, and a point's measurement is where it is.
        if (selection.front().subElement != SketchSubElement::Whole) {
            const std::optional<Vec2> at = PointOfSubElement(entity->geometry,
                                                             selection.front().subElement,
                                                             selection.front().index);
            if (!at) return refuse("That entity has no such point.");
            out.items.push_back(MeasureItem{"u", at->x, MeasureUnit::Millimetre, false});
            out.items.push_back(MeasureItem{"v", at->y, MeasureUnit::Millimetre, false});
            out.ok = true;
            return out;
        }
        out.items = ItemsForOne(entity->geometry);
        if (out.items.empty()) return refuse("There is nothing to measure about that.");
        out.ok = true;
        return out;
    }

    // TWO. Between two points the answer is a separation; anything else has no
    // single distance that is not a claim about which parts are nearest, and
    // that is the minimum-distance query roadmap 50.2 lists and this does not
    // do yet.
    const SketchElementRef& a = selection[0];
    const SketchElementRef& b = selection[1];
    const std::optional<Vec2> pa =
        PointOfSubElement(entityOf(a)->geometry, a.subElement, a.index);
    const std::optional<Vec2> pb =
        PointOfSubElement(entityOf(b)->geometry, b.subElement, b.index);
    if (pa && pb) {
        out.items.push_back(
            MeasureItem{"distance", std::hypot(pb->x - pa->x, pb->y - pa->y),
                        MeasureUnit::Millimetre, false});
        out.items.push_back(MeasureItem{"du", pb->x - pa->x, MeasureUnit::Millimetre, false});
        out.items.push_back(MeasureItem{"dv", pb->y - pa->y, MeasureUnit::Millimetre, false});
        out.ok = true;
        return out;
    }

    const auto* lineA = std::get_if<SketchLine>(&entityOf(a)->geometry);
    const auto* lineB = std::get_if<SketchLine>(&entityOf(b)->geometry);
    if (lineA != nullptr && lineB != nullptr && a.subElement == SketchSubElement::Whole &&
        b.subElement == SketchSubElement::Whole) {
        const double angleA = std::atan2(lineA->end.y - lineA->start.y,
                                         lineA->end.x - lineA->start.x);
        const double angleB = std::atan2(lineB->end.y - lineB->start.y,
                                         lineB->end.x - lineB->start.x);
        double between = angleB - angleA;
        while (between <= -kPi) between += 2.0 * kPi;
        while (between > kPi) between -= 2.0 * kPi;
        out.items.push_back(MeasureItem{"angle", between, MeasureUnit::Radian, false});
        out.ok = true;
        return out;
    }

    return refuse("Measuring between those two is not supported yet: two points give a "
                  "separation and two lines give an angle.");
}

} // namespace paramcad
