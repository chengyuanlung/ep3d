#include "Viewer/SketchCommands.h"

#include "Core/Document/PartDocument.h"
#include "Core/Expression/ExpressionTypes.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Sketch/ISketchSolver.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"
#include "Viewer/PropertyEditing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <variant>

namespace paramcad {
namespace {

constexpr double kPi = 3.14159265358979323846;

double Distance(Vec2 a, Vec2 b) noexcept {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

Vec2 Midpoint(Vec2 a, Vec2 b) noexcept { return Vec2{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5}; }

std::string IdText(SketchEntityId id) {
    return "#" + std::to_string(static_cast<unsigned long long>(ToObjectId(id)));
}

// Wraps into (-pi, pi], the range AngleConstraint is defined on (ADR-M5-006).
double WrapSigned(double angle) noexcept {
    const double twoPi = 2.0 * kPi;
    double wrapped = std::fmod(angle + kPi, twoPi);
    if (wrapped <= 0.0) wrapped += twoPi;
    return wrapped - kPi;
}

const SketchLine* LineOf(const Sketch& sketch, SketchEntityId id) noexcept {
    const SketchEntity* entity = sketch.findEntity(id);
    return entity != nullptr ? std::get_if<SketchLine>(&entity->geometry) : nullptr;
}

// Centre and radius of a circle OR an arc, so callers stop enumerating the two.
bool CurveCircle(const Sketch& sketch, SketchEntityId id, Vec2* center, double* radius,
                 double* midAngle) noexcept {
    const SketchEntity* entity = sketch.findEntity(id);
    if (entity == nullptr) return false;
    if (const auto* circle = std::get_if<SketchCircle>(&entity->geometry)) {
        if (center != nullptr) *center = circle->center;
        if (radius != nullptr) *radius = circle->radiusMm;
        if (midAngle != nullptr) *midAngle = kPi * 0.25;
        return true;
    }
    if (const auto* arc = std::get_if<SketchArc>(&entity->geometry)) {
        if (center != nullptr) *center = arc->center;
        if (radius != nullptr) *radius = arc->radiusMm;
        if (midAngle != nullptr)
            *midAngle = arc->startAngleRad + WrapSigned(arc->endAngleRad - arc->startAngleRad) * 0.5;
        return true;
    }
    return false;
}

// A parameter name nothing in the document is using yet.
//
// `prefix` is 'd' for lengths and 'a' for angles, so the constraint list is
// readable without opening each row. The names must satisfy the expression
// lexer's rules (M11.4) -- a letter first, then letters, digits or underscores
// -- or the dimension it creates could never be referenced by `#name`.
std::string UnusedParameterName(const PartDocument& document, const char* prefix) {
    for (int index = 1; index < 100000; ++index) {
        std::string candidate = std::string(prefix) + std::to_string(index);
        if (document.parameters().findByName(candidate) == nullptr) return candidate;
    }
    return std::string(prefix) + "_overflow";
}

// True when `text` is a plain number and nothing else.
bool ParsesAsNumber(const std::string& text, double* value) {
    if (text.empty()) return false;
    const char* begin = text.c_str();
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end == begin) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return false;
    if (!std::isfinite(parsed)) return false;
    if (value != nullptr) *value = parsed;
    return true;
}

} // namespace

std::string FormattedDimensionText(const Sketch& sketch, SketchConstraintId constraintId,
                                   const std::string& bareValue, bool angular) {
    const Sketch::DimensionFormat* format = sketch.dimensionFormat(constraintId);
    if (format == nullptr) return bareValue;

    std::string text = format->prefix + bareValue;
    if (format->plusTolerance != 0.0 || format->minusTolerance != 0.0) {
        // Angles are STORED in radians and READ in degrees, so a tolerance on
        // one has to make the same trip as the value it qualifies -- otherwise
        // a 0.5 degree tolerance prints as 0.0087.
        const double scale = angular ? 180.0 / kPi : 1.0;
        const double plus = format->plusTolerance * scale;
        const double minus = format->minusTolerance * scale;
        if (plus == minus) {
            // Symmetric tolerances get the single +/- form a drawing uses.
            text += " +/-" + FormatNumber(plus);
        } else {
            text += " +" + FormatNumber(plus) + "/-" + FormatNumber(minus);
        }
    }
    text += format->suffix;
    return text;
}

std::string FormatNumber(double value) {
    if (!std::isfinite(value)) return "n/a";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.4f", value);
    std::string text(buffer);
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    if (text.empty() || text == "-0") text = "0";
    return text;
}

// =============================================================================
// Applying an edit
// =============================================================================

namespace {

// Resolves a PendingRef against the entities this edit has created so far.
// Returns false for an index the edit never filled -- which can only happen if
// a tool built a malformed edit, and is refused rather than silently pointing
// at whatever entity happens to sit at that position.
bool ResolvePendingRef(const PendingRef& pending, const std::vector<SketchEntityId>& created,
                       SketchElementRef* out) {
    if (!pending.isNew) {
        *out = pending.existing;
        return out->entityId != kInvalidSketchEntityId;
    }
    if (pending.newEntity >= created.size()) return false;
    *out = SketchElementRef{created[pending.newEntity], pending.subElement};
    return true;
}

} // namespace

SketchEditOutcome ApplySketchEdit(PartDocument& document, ObjectId sketchId,
                                  const SketchEdit& edit) {
    SketchEditOutcome outcome;
    if (!edit.valid()) {
        outcome.status = "Nothing to do.";
        return outcome;
    }
    const Sketch* sketch = document.findSketch(sketchId);
    if (sketch == nullptr) {
        outcome.status = "No such sketch.";
        return outcome;
    }

    document.beginTransaction(edit.label.empty() ? std::string(SketchEditKindName(edit.kind))
                                                 : edit.label);

    // Every failure path funnels through here. An aborted transaction leaves
    // the document exactly as it was, so a rectangle whose third line was
    // refused never reaches the model as an L-shape.
    const auto fail = [&](std::string status, std::string detail) {
        document.abortTransaction();
        outcome.applied = false;
        outcome.createdEntities.clear();
        outcome.createdConstraints.clear();
        outcome.createdParameter = kInvalidObjectId;
        outcome.status = std::move(status);
        outcome.detail = std::move(detail);
        return outcome;
    };

    const auto addEntity = [&](SketchGeometry geometry, bool construction = false) {
        const SketchEntityId id = document.addSketchEntity(sketchId, geometry, construction);
        if (id != kInvalidSketchEntityId) outcome.createdEntities.push_back(id);
        return id;
    };

    const auto addConstraint = [&](SketchConstraintData data) {
        const SketchConstraintId id = document.addSketchConstraint(sketchId, std::move(data));
        if (id != kInvalidSketchConstraintId) outcome.createdConstraints.push_back(id);
        return id;
    };

    // --- Geometry -----------------------------------------------------------
    switch (edit.kind) {
    case SketchEditKind::AddPoint: {
        if (edit.points.size() < 1) return fail("Point needs a position.", "");
        if (addEntity(SketchPoint{edit.points[0]}) == kInvalidSketchEntityId)
            return fail("The sketch refused that point.", "");
        break;
    }
    case SketchEditKind::AddLine: {
        if (edit.points.size() < 2) return fail("Line needs two points.", "");
        if (addEntity(SketchLine{edit.points[0], edit.points[1]}) == kInvalidSketchEntityId)
            return fail("The sketch refused that line (it is degenerate).", "");
        break;
    }
    case SketchEditKind::AddCircle: {
        if (edit.points.size() < 2) return fail("Circle needs a centre and a rim point.", "");
        const double radius = Distance(edit.points[0], edit.points[1]);
        if (addEntity(SketchCircle{edit.points[0], radius}) == kInvalidSketchEntityId)
            return fail("The sketch refused that circle (the radius is zero).", "");
        break;
    }
    case SketchEditKind::AddArc: {
        if (edit.points.size() < 3) return fail("Arc needs a centre, a start and an end.", "");
        const Vec2 center = edit.points[0];
        const double radius = Distance(center, edit.points[1]);
        const double startAngle =
            std::atan2(edit.points[1].y - center.y, edit.points[1].x - center.x);
        const double endAngle = std::atan2(edit.points[2].y - center.y, edit.points[2].x - center.x);
        if (addEntity(SketchArc{center, radius, startAngle, endAngle, true}) ==
            kInvalidSketchEntityId)
            return fail("The sketch refused that arc.", "");
        break;
    }
    case SketchEditKind::AddCenterRectangle: {
        if (edit.points.size() < 2)
            return fail("Centre rectangle needs a centre and a corner.", "");
        const Vec2 centre = edit.points[0];
        const Vec2 corner = edit.points[1];
        const double halfWidth = std::fabs(corner.x - centre.x);
        const double halfHeight = std::fabs(corner.y - centre.y);
        // Built from the HALF-EXTENTS rather than from the clicked corner, so
        // all four corners are the same distance from the centre whichever
        // quadrant was clicked in.
        const Vec2 corners[4] = {Vec2{centre.x - halfWidth, centre.y - halfHeight},
                                 Vec2{centre.x + halfWidth, centre.y - halfHeight},
                                 Vec2{centre.x + halfWidth, centre.y + halfHeight},
                                 Vec2{centre.x - halfWidth, centre.y + halfHeight}};
        for (int i = 0; i < 4; ++i) {
            if (addEntity(SketchLine{corners[i], corners[(i + 1) % 4]}) ==
                kInvalidSketchEntityId)
                return fail("The sketch refused that rectangle.", "");
        }
        break;
    }
    case SketchEditKind::AddThreePointCircle: {
        if (edit.points.size() < 3)
            return fail("A circle through 3 points needs three of them.", "");
        const Vec2 a = edit.points[0];
        const Vec2 b = edit.points[1];
        const Vec2 c = edit.points[2];
        // The circumcentre, from the perpendicular bisectors. `d` is twice the
        // signed area of the triangle, so it is zero exactly when the three
        // points are collinear -- which the tool already refuses, and which is
        // re-checked here because an edit can also arrive from a replay.
        const double d =
            2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
        if (std::fabs(d) <= kSketchToleranceMm)
            return fail("Those three points are in a line; there is no circle through them.", "");
        const double aa = a.x * a.x + a.y * a.y;
        const double bb = b.x * b.x + b.y * b.y;
        const double cc = c.x * c.x + c.y * c.y;
        const Vec2 centre{(aa * (b.y - c.y) + bb * (c.y - a.y) + cc * (a.y - b.y)) / d,
                          (aa * (c.x - b.x) + bb * (a.x - c.x) + cc * (b.x - a.x)) / d};
        const double radius = Distance(centre, a);
        if (addEntity(SketchCircle{centre, radius}) == kInvalidSketchEntityId)
            return fail("The sketch refused that circle.", "");
        break;
    }
    case SketchEditKind::AddThreePointArc: {
        if (edit.points.size() < 3) return fail("An arc through 3 points needs three.", "");
        const Vec2 start = edit.points[0];
        const Vec2 end = edit.points[1];
        const Vec2 through = edit.points[2];
        const double d = 2.0 * (start.x * (end.y - through.y) + end.x * (through.y - start.y) +
                                through.x * (start.y - end.y));
        if (std::fabs(d) <= kSketchToleranceMm)
            return fail("Those three points are in a line; there is no arc through them.", "");
        const double ss = start.x * start.x + start.y * start.y;
        const double ee = end.x * end.x + end.y * end.y;
        const double tt = through.x * through.x + through.y * through.y;
        const Vec2 centre{
            (ss * (end.y - through.y) + ee * (through.y - start.y) + tt * (start.y - end.y)) / d,
            (ss * (through.x - end.x) + ee * (start.x - through.x) + tt * (end.x - start.x)) / d};
        const double radius = Distance(centre, start);
        const double startAngle = std::atan2(start.y - centre.y, start.x - centre.x);
        const double endAngle = std::atan2(end.y - centre.y, end.x - centre.x);
        const double throughAngle = std::atan2(through.y - centre.y, through.x - centre.x);

        // WHICH WAY ROUND. Two arcs join any two points on a circle, and the
        // third click is the whole reason this tool exists: it says which. The
        // arc is stored counter-clockwise always (the rest of the model
        // assumes it), so when the through-point is NOT on the CCW sweep from
        // start to end, the tips are swapped instead -- the same curve, said
        // the way everything else here expects to hear it.
        const auto sweepFrom = [](double from, double to) {
            double sweep = to - from;
            while (sweep < 0.0) sweep += 2.0 * kPi;
            return sweep;
        };
        const bool throughIsOnTheCcwSweep =
            sweepFrom(startAngle, throughAngle) < sweepFrom(startAngle, endAngle);
        const double first = throughIsOnTheCcwSweep ? startAngle : endAngle;
        const double second = throughIsOnTheCcwSweep ? endAngle : startAngle;
        if (addEntity(SketchArc{centre, radius, first, second, true}) == kInvalidSketchEntityId)
            return fail("The sketch refused that arc.", "");
        break;
    }
    case SketchEditKind::AddTangentArc: {
        // AN ARC WITH NO CHOICES LEFT. Given a point, the direction the curve
        // there is heading, and somewhere to stop, there is exactly ONE arc --
        // so the tool takes two clicks and asks nothing else.
        //
        // The centre must be on the perpendicular at the start (a tangent's
        // radius is square to it) and equidistant from both ends. Writing the
        // centre as p0 + s*n and solving |p0 + s*n - p1| = |s| gives
        //
        //     s = (d.d) / (2 * n.d),     d = p1 - p0
        //
        // where the s^2 terms cancel, which is why there is no square root and
        // no second branch to choose between.
        if (edit.points.size() < 2)
            return fail("A tangent arc needs a start and an end.", "");
        if (edit.tangentFrom.entityId == kInvalidSketchEntityId)
            return fail("A tangent arc has to start at the END of a line or an arc. "
                        "Click one, then click where the arc should stop.",
                        "");
        const SketchEntity* host = sketch->findEntity(edit.tangentFrom.entityId);
        if (host == nullptr)
            return fail("The line or arc this tangent arc grows from is not in this sketch.", "");
        const bool fromStart = edit.tangentFrom.subElement == SketchSubElement::StartPoint;

        // WHICH WAY THE HOST IS HEADING as it LEAVES that end -- outward, not
        // along its own sweep. Get this backwards and the arc still touches and
        // still joins; it just curls back the wrong way round, which is a
        // different arc and a silently wrong one.
        Vec2 heading{};
        if (const auto* line = std::get_if<SketchLine>(&host->geometry)) {
            heading = fromStart ? Vec2{line->start.x - line->end.x, line->start.y - line->end.y}
                                : Vec2{line->end.x - line->start.x, line->end.y - line->start.y};
        } else if (const auto* arc = std::get_if<SketchArc>(&host->geometry)) {
            const Vec2 tip = fromStart ? StartPointOf(host->geometry) : EndPointOf(host->geometry);
            const Vec2 radial{tip.x - arc->center.x, tip.y - arc->center.y};
            // A counter-clockwise arc's velocity is its radius turned +90; at
            // its START that velocity points INTO the arc, so leaving means the
            // other way. Both reversals are here rather than folded into one
            // sign, because a single XOR is unreadable and this is exactly the
            // sort of sign nobody re-derives when they change it.
            const Vec2 turned{-radial.y, radial.x};
            const bool forward = arc->counterClockwise != fromStart;
            heading = forward ? turned : Vec2{-turned.x, -turned.y};
        } else {
            return fail("A tangent arc can only grow from a line or an arc.", "");
        }
        const double headingLength = std::sqrt(heading.x * heading.x + heading.y * heading.y);
        if (headingLength <= kSketchToleranceMm)
            return fail("That line or arc is too small to have a direction.", "");
        heading = Vec2{heading.x / headingLength, heading.y / headingLength};

        // The START IS THE HOST'S OWN END, read from the geometry rather than
        // from the click. They agree to within a snap, and the difference is
        // the gap the joint would have to close on its first solve.
        const Vec2 start = fromStart ? StartPointOf(host->geometry) : EndPointOf(host->geometry);
        const Vec2 stop = edit.points[1];
        const Vec2 d{stop.x - start.x, stop.y - start.y};
        const double span = std::sqrt(d.x * d.x + d.y * d.y);
        if (span <= kSketchToleranceMm) return fail("That tangent arc has no length.", "");
        const Vec2 normal{-heading.y, heading.x};
        const double nd = normal.x * d.x + normal.y * d.y;
        // STRAIGHT AHEAD has no arc through it -- the centre runs off to
        // infinity. Compared against the SPAN so the test is the sine of an
        // angle and means the same thing at any size.
        if (std::fabs(nd) <= kSketchToleranceMm * span)
            return fail("That point is straight ahead. A tangent arc there would be a line -- "
                        "use the line tool, or click to one side.",
                        "");
        const double s = (d.x * d.x + d.y * d.y) / (2.0 * nd);
        const Vec2 centre{start.x + s * normal.x, start.y + s * normal.y};
        const double radius = std::fabs(s);
        const Vec2 fromCentre{start.x - centre.x, start.y - centre.y};
        // Counter-clockwise when the outward heading agrees with the direction
        // a CCW sweep would leave in. The two arcs between these tips differ
        // only in this flag, and only one of them is tangent.
        const bool counterClockwise = fromCentre.x * heading.y - fromCentre.y * heading.x > 0.0;
        const SketchEntityId arc = addEntity(
            SketchArc{centre, radius, std::atan2(fromCentre.y, fromCentre.x),
                      std::atan2(stop.y - centre.y, stop.x - centre.x), counterClockwise});
        if (arc == kInvalidSketchEntityId) return fail("The sketch refused that arc.", "");

        // WHAT MAKES IT TANGENT rather than an arc that starts in the right
        // place today. Built here, beside the geometry, because they are one
        // idea: an unconstrained tangent arc is a lie the first drag exposes.
        if (addConstraint(CoincidentConstraint{
                SketchElementRef{arc, SketchSubElement::StartPoint}, edit.tangentFrom}) ==
            kInvalidSketchConstraintId)
            return fail("The sketch refused the tangent arc's joint.", "");
        // ONE LINE FOR BOTH HOSTS. `at` names an end of the FIRST entity, so
        // putting the host first serves a line (perpendicular to the radius)
        // and an arc (collinear radii) without this code knowing which.
        if (addConstraint(TangentConstraint{edit.tangentFrom.entityId, arc, false,
                                            edit.tangentFrom.subElement}) ==
            kInvalidSketchConstraintId)
            return fail("The sketch refused the tangent arc's tangency.", "");
        break;
    }
    case SketchEditKind::AddSpline: {
        if (edit.points.size() < kMinSplinePoints)
            return fail("A spline needs at least two points.", "");
        if (edit.splineClosed && edit.points.size() < 3)
            return fail("A closed spline needs at least three points.",
                        "Two points closed back on themselves are a line drawn twice.");
        if (addEntity(SketchSpline{edit.points, edit.splineClosed}) == kInvalidSketchEntityId)
            return fail("The sketch refused that spline.",
                        "Two of its points are on top of each other, and a span with no "
                        "length has no direction for the curve to leave in.");
        break;
    }
    case SketchEditKind::AddEllipse:
    case SketchEditKind::AddEllipticalArc: {
        const bool open = edit.kind == SketchEditKind::AddEllipticalArc;
        if (edit.points.size() < (open ? 4u : 3u))
            return fail(open ? "An elliptical arc needs a centre, the long axis, a width and "
                               "an end."
                             : "An ellipse needs a centre, the long axis and a width.",
                        "");
        const Vec2 centre = edit.points[0];
        const double du = edit.points[1].x - centre.x;
        const double dv = edit.points[1].y - centre.y;
        const double major = std::sqrt(du * du + dv * dv);
        if (major <= kSketchToleranceMm) return fail("That ellipse has no size.", "");
        // The ROTATION is where the long axis points, which is exactly what the
        // second click said -- so it is read from that click and nowhere else.
        const double rotation = std::atan2(dv, du);
        // The third click's PERPENDICULAR distance to the axis, so sliding
        // along it does not change the width.
        const double minor =
            std::fabs((edit.points[2].x - centre.x) * dv - (edit.points[2].y - centre.y) * du) /
            major;
        if (minor <= kSketchToleranceMm) return fail("That ellipse has no width.", "");
        if (minor > major)
            return fail("The second click has to be the LONG axis: this one is wider than it "
                        "is long.",
                        "An ellipse's rotation is measured to its major axis, so the two "
                        "cannot simply be swapped.");

        if (!open) {
            if (addEntity(SketchEllipse{centre, major, minor, rotation}) ==
                kInvalidSketchEntityId)
                return fail("The sketch refused that ellipse.", "");
            break;
        }

        // STARTS AT THE LONG AXIS, which is parameter zero, and runs
        // counter-clockwise to wherever the fourth click points. Saying "it
        // starts at the long axis" in the prompt beats a fifth click for a
        // start the user has already effectively given.
        const double endParam = EllipseParamOf(centre, major, minor, rotation, edit.points[3]);
        if (addEntity(SketchEllipticalArc{centre, major, minor, rotation, 0.0, endParam, true}) ==
            kInvalidSketchEntityId)
            return fail("The sketch refused that elliptical arc.",
                        "An arc that sweeps nothing, or a whole turn, is not an arc.");
        break;
    }
    case SketchEditKind::AddPolygon: {
        if (edit.points.size() < 2) return fail("Polygon needs a centre and a corner.", "");
        const int sides = edit.polygonSides < 3 ? 3 : edit.polygonSides;
        const Vec2 centre = edit.points[0];
        const Vec2 vertex = edit.points[1];
        const double radius = Distance(centre, vertex);
        if (radius <= kSketchToleranceMm) return fail("That polygon has no size.", "");
        const double start = std::atan2(vertex.y - centre.y, vertex.x - centre.x);

        // THE CONSTRUCTION CIRCLE FIRST, because it is entity 0 and every
        // PointOnObject the tool generated names it by that index.
        //
        // It is construction geometry, not an edge: the polygon's sides are
        // what a pad sweeps, and a circumscribed circle appearing in the solid
        // would be a curve the user never drew.
        if (addEntity(SketchCircle{centre, radius}, true) == kInvalidSketchEntityId)
            return fail("The sketch refused the polygon's construction circle.", "");

        for (int i = 0; i < sides; ++i) {
            const double from = start + 2.0 * kPi * i / sides;
            const double to = start + 2.0 * kPi * (i + 1) / sides;
            const Vec2 a{centre.x + radius * std::cos(from), centre.y + radius * std::sin(from)};
            const Vec2 b{centre.x + radius * std::cos(to), centre.y + radius * std::sin(to)};
            if (addEntity(SketchLine{a, b}) == kInvalidSketchEntityId)
                return fail("The sketch refused one of the polygon's sides.", "");
        }
        break;
    }
    case SketchEditKind::AddSlot: {
        if (edit.points.size() < 3)
            return fail("Slot needs two centres and a width.", "");
        const Vec2 a = edit.points[0];
        const Vec2 b = edit.points[1];
        const Vec2 across = edit.points[2];
        const double du = b.x - a.x;
        const double dv = b.y - a.y;
        const double span = std::sqrt(du * du + dv * dv);
        if (span <= kSketchToleranceMm) return fail("That slot has no length.", "");
        const double radius =
            std::fabs((across.x - a.x) * dv - (across.y - a.y) * du) / span;
        if (radius <= kSketchToleranceMm) return fail("That slot has no width.", "");

        // The centre line's direction, and the perpendicular the sides sit on.
        const Vec2 along{du / span, dv / span};
        const Vec2 side{-along.y, along.x};
        const Vec2 offset{side.x * radius, side.y * radius};

        const double toSide = std::atan2(side.y, side.x);
        const double kPiLocal = kPi;

        // ORDER MATTERS: line, arc at b, line, arc at a -- once round the
        // outline, and the same order the tool's constraint indexes assume.
        //
        // Each arc runs COUNTER-CLOCKWISE from the far side to the near side,
        // which is what makes it bulge OUTWARD past its centre. Running it the
        // other way would produce the same two endpoints joined the wrong way
        // round: a slot with its ends caved in, which still closes and still
        // pads.
        if (addEntity(SketchLine{Vec2{a.x + offset.x, a.y + offset.y},
                                 Vec2{b.x + offset.x, b.y + offset.y}}) ==
            kInvalidSketchEntityId)
            return fail("The sketch refused one of the slot's sides.", "");
        if (addEntity(SketchArc{b, radius, toSide + kPiLocal, toSide, true}) ==
            kInvalidSketchEntityId)
            return fail("The sketch refused one of the slot's ends.", "");
        if (addEntity(SketchLine{Vec2{b.x - offset.x, b.y - offset.y},
                                 Vec2{a.x - offset.x, a.y - offset.y}}) ==
            kInvalidSketchEntityId)
            return fail("The sketch refused one of the slot's sides.", "");
        if (addEntity(SketchArc{a, radius, toSide, toSide + kPiLocal, true}) ==
            kInvalidSketchEntityId)
            return fail("The sketch refused one of the slot's ends.", "");
        break;
    }
    case SketchEditKind::AddRectangle: {
        if (edit.points.size() < 2) return fail("Rectangle needs two opposite corners.", "");
        const Vec2 a = edit.points[0];
        const Vec2 b = edit.points[1];
        const Vec2 corners[4] = {Vec2{a.x, a.y}, Vec2{b.x, a.y}, Vec2{b.x, b.y}, Vec2{a.x, b.y}};
        for (int i = 0; i < 4; ++i) {
            if (addEntity(SketchLine{corners[i], corners[(i + 1) % 4]}) == kInvalidSketchEntityId)
                return fail("The sketch refused one of the rectangle's sides.", "");
        }
        break;
    }
    default:
        break;
    }

    // --- What makes an offset an OFFSET --------------------------------------
    //
    // Built here rather than as PendingConstraints because one of them binds a
    // Parameter, and because they only make sense once the copy exists. If any
    // of them is refused the WHOLE command fails: a copy that kept its shape
    // but lost its relationship is a second piece of geometry the user now has
    // to notice and delete, which is worse than the command not running.
    if (edit.offsetSource != kInvalidSketchEntityId) {
        if (outcome.createdEntities.size() != 1)
            return fail("An offset produced no geometry to constrain.", "");
        const SketchEntityId copy = outcome.createdEntities.front();
        const Sketch* afterAdd = document.findSketch(sketchId);
        if (afterAdd == nullptr) return fail("The sketch went away mid-offset.", "");
        const SketchEntity* source = afterAdd->findEntity(edit.offsetSource);
        if (source == nullptr) return fail("The offset source is no longer in this sketch.", "");

        if (std::holds_alternative<SketchLine>(source->geometry)) {
            if (addConstraint(ParallelConstraint{copy, edit.offsetSource}) ==
                kInvalidSketchConstraintId)
                return fail("The sketch refused the offset's parallel constraint.", "");
            if (addConstraint(EqualConstraint{copy, edit.offsetSource}) ==
                kInvalidSketchConstraintId)
                return fail("The sketch refused the offset's equal-length constraint.", "");
            // SEEDED FROM THE FORMULA, not from the distance that was asked
            // for. `(pu*dv - pv*du)/len` is positive to the RIGHT of
            // start->end, so a copy placed on the left measures NEGATIVE -- and
            // a parameter seeded with the requested +10 would disagree with its
            // own geometry, sending the solver to flip the copy across the line
            // on the very first solve. Measuring it the way the residual does
            // is the only way the two can never disagree.
            const SketchLine& sourceLine = std::get<SketchLine>(source->geometry);
            const SketchEntity* copyEntity = afterAdd->findEntity(copy);
            if (copyEntity == nullptr) return fail("The offset copy went missing.", "");
            const SketchLine& copyLine = std::get<SketchLine>(copyEntity->geometry);
            const double su = sourceLine.end.x - sourceLine.start.x;
            const double sv = sourceLine.end.y - sourceLine.start.y;
            const double sourceLength = std::sqrt(su * su + sv * sv);
            if (sourceLength <= kSketchToleranceMm)
                return fail("The offset source is too short to measure from.", "");
            const double qu = copyLine.start.x - sourceLine.start.x;
            const double qv = copyLine.start.y - sourceLine.start.y;
            const double measured = (qu * sv - qv * su) / sourceLength;

            const Parameter& parameter = document.addParameter(
                UnusedParameterName(document, "d"), measured, UnitType::Millimeter);
            outcome.createdParameter = parameter.id();
            if (addConstraint(PointLineDistanceConstraint{
                    SketchElementRef{copy, SketchSubElement::StartPoint}, edit.offsetSource,
                    parameter.id()}) == kInvalidSketchConstraintId)
                return fail("The sketch refused the offset's distance dimension.", "");
        } else {
            if (addConstraint(ConcentricConstraint{copy, edit.offsetSource}) ==
                kInvalidSketchConstraintId)
                return fail("The sketch refused the offset's concentric constraint.", "");
            double radius = 0.0;
            if (!CurveCircle(*afterAdd, copy, nullptr, &radius, nullptr))
                return fail("The offset copy is not a curve.", "");
            // The copy's RADIUS, not the gap: a concentric pair is pinned by
            // either, and a radius is the number a drawing states.
            const Parameter& parameter = document.addParameter(
                UnusedParameterName(document, "d"), radius, UnitType::Millimeter);
            outcome.createdParameter = parameter.id();
            if (addConstraint(RadiusConstraint{copy, parameter.id()}) ==
                kInvalidSketchConstraintId)
                return fail("The sketch refused the offset's radius dimension.", "");
        }
    }

    // --- Constraints the edit brought with it (inference, rectangle) ---------
    // Re-read: `sketch` was captured before any entity was added, and adding to
    // the sketch can move its storage.
    const Sketch* current = document.findSketch(sketchId);
    if (current == nullptr) return fail("The sketch went away mid-edit.", "");

    for (const PendingConstraint& pending : edit.autoConstraints) {
        // An inferred constraint whose EXISTING target is gone is DROPPED, not
        // failed.
        //
        // This happens for real: undo in the middle of a polyline deletes the
        // segment the chain is holding on to, and the next click still carries
        // its reference. Failing the edit would leave the user unable to draw
        // at all, with a message about a constraint they never asked for --
        // while the honest outcome is the one afterApply already takes when it
        // has no id to attach: draw the geometry, invent no reference.
        //
        // This is NOT the roadmap 4.2 case of dropping a snap the user saw. The
        // thing that was snapped to no longer exists, so there is no
        // relationship left to record, and the DOF is right either way.
        if (!pending.a.isNew && current->findEntity(pending.a.existing.entityId) == nullptr)
            continue;
        if (!pending.b.isNew && pending.b.existing.entityId != kInvalidSketchEntityId &&
            current->findEntity(pending.b.existing.entityId) == nullptr)
            continue;

        SketchElementRef a{};
        SketchElementRef b{};
        if (!ResolvePendingRef(pending.a, outcome.createdEntities, &a))
            return fail("An inferred constraint referred to geometry that was not created.", "");

        SketchConstraintId id = kInvalidSketchConstraintId;
        if (pending.kind == SketchEditKind::AddHorizontal) {
            id = addConstraint(HorizontalConstraint{a.entityId});
        } else if (pending.kind == SketchEditKind::AddVertical) {
            id = addConstraint(VerticalConstraint{a.entityId});
        } else if (pending.kind == SketchEditKind::AddCoincident) {
            if (!ResolvePendingRef(pending.b, outcome.createdEntities, &b))
                return fail("An inferred coincidence referred to geometry that was not created.",
                            "");
            id = addConstraint(CoincidentConstraint{a, b});
        } else if (pending.kind == SketchEditKind::AddFix) {
            // A point the user dropped on the origin. One-sided, unlike every
            // other inferred constraint: there is no origin entity for the
            // second half to name.
            id = addConstraint(FixConstraint{a});
        } else if (pending.kind == SketchEditKind::AddPointOnObject) {
            if (!ResolvePendingRef(pending.b, outcome.createdEntities, &b))
                return fail("An inferred point-on-object referred to geometry that was not "
                            "created.",
                            "");
            id = addConstraint(PointOnObjectConstraint{a, b.entityId});
        } else if (pending.kind == SketchEditKind::AddTangent) {
            if (!ResolvePendingRef(pending.b, outcome.createdEntities, &b))
                return fail("An inferred tangency referred to geometry that was not created.",
                            "");
            // EXTERNAL tangency: the slot's sides run along the OUTSIDE of its
            // end arcs. The internal branch is the one where a small circle
            // sits inside a larger one, which a slot never is.
            // `a` IS THE LINE and carries the end that touches; every tool
            // that asks for a tangency orders it that way, and a tangency at a
            // corner that did not say WHERE holds nothing at all.
            id = addConstraint(TangentConstraint{a.entityId, b.entityId, false, a.subElement});
        } else if (pending.kind == SketchEditKind::AddEqual) {
            if (!ResolvePendingRef(pending.b, outcome.createdEntities, &b))
                return fail("An inferred equality referred to geometry that was not created.",
                            "");
            id = addConstraint(EqualConstraint{a.entityId, b.entityId});
        } else {
            // A KIND THIS LOOP DOES NOT KNOW is a programming error, not a
            // user's, and it used to look exactly like a sketch refusing a
            // constraint. The polygon tool asked for PointOnObject and Equal,
            // fell through here, and reported "the sketch refused an
            // automatically added constraint" -- about a constraint that was
            // never offered to the sketch at all.
            return fail(std::string("This command asked for an automatic ") +
                            SketchEditKindName(pending.kind) +
                            " constraint, which is not one that can be inferred.",
                        "");
        }
        if (id == kInvalidSketchConstraintId)
            return fail(std::string("The sketch refused the automatic ") +
                            SketchEditKindName(pending.kind) + " constraint.",
                        "Roadmap 4.2: an inferred snap must become a real constraint or not "
                        "happen at all -- a snap the solver cannot see makes the DOF readout "
                        "lie, so the whole command is refused rather than half-applied.");
    }

    // --- Explicit constraints -----------------------------------------------
    switch (edit.kind) {
    case SketchEditKind::AddCoincident: {
        if (edit.refs.size() < 2) return fail("Coincident needs two points.", "");
        if (addConstraint(CoincidentConstraint{edit.refs[0], edit.refs[1]}) ==
            kInvalidSketchConstraintId)
            return fail("The sketch refused that coincident constraint.", "");
        break;
    }
    case SketchEditKind::AddHorizontal:
    case SketchEditKind::AddVertical: {
        const bool horizontal = edit.kind == SketchEditKind::AddHorizontal;
        // WHICH FORM, through the SAME function requestConstraint used to
        // decide it. Asking it a second way here is what made this apply one
        // constraint per line for a selection that meant one alignment.
        if (IsPointPairAlignment(*current, edit.refs)) {
            const SketchConstraintId id =
                horizontal ? addConstraint(PointsHorizontalConstraint{edit.refs[0], edit.refs[1]})
                           : addConstraint(PointsVerticalConstraint{edit.refs[0], edit.refs[1]});
            if (id == kInvalidSketchConstraintId)
                return fail(std::string("The sketch refused that ") +
                                SketchEditKindName(edit.kind) + " constraint.",
                            "");
            break;
        }
        for (const SketchElementRef& ref : edit.refs) {
            const SketchConstraintId id =
                horizontal ? addConstraint(HorizontalConstraint{ref.entityId})
                           : addConstraint(VerticalConstraint{ref.entityId});
            if (id == kInvalidSketchConstraintId)
                return fail(std::string("The sketch refused that ") +
                                SketchEditKindName(edit.kind) + " constraint.",
                            "");
        }
        break;
    }
    case SketchEditKind::AddFix: {
        for (const SketchElementRef& ref : edit.refs)
            if (addConstraint(FixConstraint{ref}) == kInvalidSketchConstraintId)
                return fail("The sketch refused that fix constraint.", "");
        break;
    }
    case SketchEditKind::AddSymmetric: {
        if (edit.refs.size() < 3)
            return fail("Symmetric needs 2 points and a line.", "");
        // refs[2] is the mirror; requestConstraint put it there whatever order
        // the user clicked in.
        if (addConstraint(SymmetricConstraint{edit.refs[0], edit.refs[1],
                                              edit.refs[2].entityId}) ==
            kInvalidSketchConstraintId)
            return fail("The sketch refused that symmetric constraint.", "");
        break;
    }

    // --- M13: the geometric constraints ------------------------------------
    //
    // Every one of these is a PAIR, and `requestConstraint` has already checked
    // the arity and the kinds. The size check stays anyway: this function is
    // reachable from anywhere, including a future OCCT overlay that builds its
    // own edits, and indexing refs[1] on a one-element vector is undefined
    // behaviour rather than a failed command.
    case SketchEditKind::AddParallel:
    case SketchEditKind::AddPerpendicular:
    case SketchEditKind::AddEqual:
    case SketchEditKind::AddConcentric:
    case SketchEditKind::AddMidpoint:
    case SketchEditKind::AddPointOnObject:
    case SketchEditKind::AddTangent: {
        if (edit.refs.size() < 2)
            return fail(std::string(SketchEditKindName(edit.kind)) +
                            " needs 2 selected elements.",
                        "");
        const SketchElementRef& a = edit.refs[0];
        const SketchElementRef& b = edit.refs[1];
        SketchConstraintData data{};
        switch (edit.kind) {
        case SketchEditKind::AddParallel:
            data = ParallelConstraint{a.entityId, b.entityId};
            break;
        case SketchEditKind::AddPerpendicular:
            data = PerpendicularConstraint{a.entityId, b.entityId};
            break;
        case SketchEditKind::AddEqual:
            data = EqualConstraint{a.entityId, b.entityId};
            break;
        case SketchEditKind::AddConcentric:
            data = ConcentricConstraint{a.entityId, b.entityId};
            break;
        case SketchEditKind::AddMidpoint:
            // refs[0] is the POINT and refs[1] the line, fixed by
            // requestConstraint so nothing downstream re-derives which is which.
            data = MidpointConstraint{a, b.entityId};
            break;
        case SketchEditKind::AddPointOnObject:
            data = PointOnObjectConstraint{a, b.entityId};
            break;
        case SketchEditKind::AddTangent:
            // refs[0] is the LINE when there is one, and its subElement is the
            // corner requestConstraint found -- Whole when the two do not
            // already meet at a pinned point.
            data = TangentConstraint{a.entityId, b.entityId, edit.tangentInternal, a.subElement};
            break;
        default:
            data = TangentConstraint{a.entityId, b.entityId, edit.tangentInternal};
            break;
        }
        if (addConstraint(std::move(data)) == kInvalidSketchConstraintId)
            return fail(std::string("The sketch refused that ") +
                            SketchEditKindName(edit.kind) + " constraint.",
                        "");
        break;
    }
    default:
        break;
    }

    // --- Dimensions ---------------------------------------------------------
    //
    // Each creates a Parameter AND a constraint in this ONE transaction. The
    // parameter is seeded with what the geometry MEASURES right now, so adding
    // a dimension never moves anything: the user names a quantity first and
    // changes it second (roadmap section 7's driving-dimension workflow).
    if (IsDimensionEdit(edit.kind)) {
        // ARITY FIRST. `requestDimension` guarantees these, but this function is
        // reachable from anywhere -- including a future OCCT overlay that builds
        // its own edits -- and `refs.front()` on an empty vector is undefined
        // behaviour, not a failed command.
        const bool pairKind = edit.kind == SketchEditKind::AddDistance ||
                              edit.kind == SketchEditKind::AddHorizontalDistance ||
                              edit.kind == SketchEditKind::AddVerticalDistance ||
                              edit.kind == SketchEditKind::AddHVDistance ||
                              edit.kind == SketchEditKind::AddPointLineDistance ||
                              edit.kind == SketchEditKind::AddAngle;
        const std::size_t needed = pairKind ? 2u : 1u;
        if (edit.refs.size() < needed)
            return fail(std::string(SketchEditKindName(edit.kind)) + " needs " +
                            std::to_string(needed) + " selected element" +
                            (needed == 1 ? "" : "s") + ".",
                        "");

        // ONE COMMAND, TWO LEGS (M26.5). The legs ARE the existing kinds, run
        // through the existing code -- so the seeding formula, the sign
        // convention, the zero rule and the parameter naming are the ones
        // already proven, not a second copy that could drift from them.
        //
        // Both are built inside the transaction opened above, so H&V is ONE
        // undo step: a user who presses Ctrl+Z after it expects the pair to go,
        // not to be left holding half of what they asked for.
        std::vector<SketchEditKind> legs{edit.kind};
        if (edit.kind == SketchEditKind::AddHVDistance)
            legs = {SketchEditKind::AddHorizontalDistance, SketchEditKind::AddVerticalDistance};

        std::string legStatus;
        for (const SketchEditKind leg : legs) {
            double seed = 0.0;
            UnitType unit = UnitType::Millimeter;
            const char* prefix = "d";
            SketchConstraintData data{};

            switch (leg) {
            case SketchEditKind::AddLength: {
                const SketchLine* line = LineOf(*sketch, edit.refs.front().entityId);
                if (line == nullptr) return fail("Length needs a line.", "");
                seed = Distance(line->start, line->end);
                break;
            }
            case SketchEditKind::AddDistance: {
                bool okA = false;
                bool okB = false;
                const Vec2 a = ResolveElementPoint(*sketch, edit.refs[0], &okA);
                const Vec2 b = ResolveElementPoint(*sketch, edit.refs[1], &okB);
                if (!okA || !okB) return fail("Distance needs two resolvable points.", "");
                seed = Distance(a, b);
                break;
            }
            case SketchEditKind::AddHorizontalDistance:
            case SketchEditKind::AddVerticalDistance: {
                bool okA = false;
                bool okB = false;
                const Vec2 a = ResolveElementPoint(*sketch, edit.refs[0], &okA);
                const Vec2 b = ResolveElementPoint(*sketch, edit.refs[1], &okB);
                if (!okA || !okB)
                    return fail(std::string(SketchEditKindName(leg)) +
                                    " needs two resolvable points.",
                                "");
                // SIGNED, matching the residual exactly. requestDimension has
                // already ordered the pair so this comes out positive; seeding from
                // the same formula the solver uses is what makes "adding a
                // dimension never moves anything" true for these two as well.
                seed = leg == SketchEditKind::AddHorizontalDistance ? b.x - a.x : b.y - a.y;
                break;
            }
            case SketchEditKind::AddPointLineDistance: {
                bool okPoint = false;
                const Vec2 point = ResolveElementPoint(*sketch, edit.refs[0], &okPoint);
                const SketchLine* line = LineOf(*sketch, edit.refs[1].entityId);
                if (!okPoint || line == nullptr)
                    return fail("Distance to a line needs a point and a line.", "");
                // The SAME formula the residual uses, so the seeded value is the
                // number the solver will read back. Deriving it any other way is
                // how "adding a dimension moves the geometry" starts.
                const double du = line->end.x - line->start.x;
                const double dv = line->end.y - line->start.y;
                const double length = std::sqrt(du * du + dv * dv);
                if (length <= kSketchToleranceMm)
                    return fail("That line is too short to measure a distance to.", "");
                const double pu = point.x - line->start.x;
                const double pv = point.y - line->start.y;
                seed = (pu * dv - pv * du) / length;
                break;
            }
            case SketchEditKind::AddRadius:
            case SketchEditKind::AddDiameter: {
                double radius = 0.0;
                if (!CurveCircle(*sketch, edit.refs.front().entityId, nullptr, &radius, nullptr))
                    return fail("Radius and diameter need a circle or an arc.", "");
                seed = leg == SketchEditKind::AddDiameter ? radius * 2.0 : radius;
                break;
            }
            case SketchEditKind::AddMajorAxis:
            case SketchEditKind::AddMinorAxis: {
                // MEASURED THROUGH MeasureConstraint, the one measurement site, by
                // building the constraint this is about to add and asking it what
                // it currently reads. Reaching into the geometry here would be a
                // second formula for the same number (ADR-M17-042).
                const bool minor = leg == SketchEditKind::AddMinorAxis;
                const std::optional<double> measured = MeasureConstraint(
                    *sketch,
                    EllipseAxisConstraint{edit.refs.front().entityId, kInvalidObjectId, minor});
                if (!measured) return fail("A major or minor axis dimension needs an ellipse.", "");
                seed = *measured;
                prefix = minor ? "b" : "a";
                break;
            }
            case SketchEditKind::AddEllipseRotation: {
                const std::optional<double> measured = MeasureConstraint(
                    *sketch, EllipseRotationConstraint{edit.refs.front().entityId, kInvalidObjectId});
                if (!measured) return fail("An orientation dimension needs an ellipse.", "");
                seed = *measured;
                unit = UnitType::Radian;
                prefix = "a";
                break;
            }
            case SketchEditKind::AddAngle: {
                const SketchLine* a = LineOf(*sketch, edit.refs[0].entityId);
                const SketchLine* b = LineOf(*sketch, edit.refs[1].entityId);
                if (a == nullptr || b == nullptr) return fail("Angle needs two lines.", "");
                // FROM a TO b, counter-clockwise, in radians -- ADR-M5-006's
                // convention, reproduced here so the seeded value is the angle the
                // solver will read rather than its supplement.
                const double angleA = std::atan2(a->end.y - a->start.y, a->end.x - a->start.x);
                const double angleB = std::atan2(b->end.y - b->start.y, b->end.x - b->start.x);
                seed = WrapSigned(angleB - angleA);
                unit = UnitType::Radian;
                prefix = "a";
                break;
            }
            default:
                break;
            }

            // MAGNITUDE, not value. Two of these kinds are signed -- a point on the
            // far side of a line measures negative, and refusing that would refuse
            // half the plane for being "too small".
            //
            // ...AND ZERO IS A REAL ANSWER for the signed ones (M18). A horizontal
            // distance of nought says "these two are level", a vertical one says
            // "these two are side by side", and a point-line distance of nought
            // says "this point is on that line". Those are three of the most
            // ordinary things a user asks for, and this guard refused all of them
            // for being too small -- the same magnitude assumption DimensionValueValid
            // was corrected for, living on in a second place that never heard.
            //
            // A LENGTH or a DISTANCE of nought really is degenerate: it describes
            // geometry with no extent, which the solver cannot orient. Those keep
            // the minimum.
            const bool signedSeparation = leg == SketchEditKind::AddHorizontalDistance ||
                                          leg == SketchEditKind::AddVerticalDistance ||
                                          leg == SketchEditKind::AddPointLineDistance;
            if (unit == UnitType::Millimeter && !signedSeparation &&
                !(std::abs(seed) >= kMinSketchDimensionMm))
                return fail("That geometry is too small to dimension.",
                            "The measured value is below the smallest dimension the solver "
                            "accepts (kMinSketchDimensionMm).");
            // ZERO IS ONLY MEANINGLESS FOR AN ANGLE BETWEEN TWO LINES. An ellipse
            // whose major axis happens to lie along +u has a rotation of exactly
            // zero, and that is a number worth pinning -- refusing it would have
            // told the user their ellipse was "parallel to itself".
            if (leg == SketchEditKind::AddAngle && std::abs(seed) < 1e-9)
                return fail("Those two lines are parallel; there is no angle to dimension.", "");

            const Parameter& parameter =
                document.addParameter(UnusedParameterName(document, prefix), seed, unit);
            outcome.createdParameter = parameter.id();

            switch (leg) {
            case SketchEditKind::AddLength:
                data = LengthConstraint{edit.refs.front().entityId, parameter.id()};
                break;
            case SketchEditKind::AddDistance:
                data = DistanceConstraint{edit.refs[0], edit.refs[1], parameter.id()};
                break;
            case SketchEditKind::AddHorizontalDistance:
                data = HorizontalDistanceConstraint{edit.refs[0], edit.refs[1], parameter.id()};
                break;
            case SketchEditKind::AddVerticalDistance:
                data = VerticalDistanceConstraint{edit.refs[0], edit.refs[1], parameter.id()};
                break;
            case SketchEditKind::AddPointLineDistance:
                data = PointLineDistanceConstraint{edit.refs[0], edit.refs[1].entityId,
                                                   parameter.id()};
                break;
            case SketchEditKind::AddRadius:
                data = RadiusConstraint{edit.refs.front().entityId, parameter.id()};
                break;
            case SketchEditKind::AddDiameter:
                data = DiameterConstraint{edit.refs.front().entityId, parameter.id()};
                break;
            case SketchEditKind::AddMajorAxis:
            case SketchEditKind::AddMinorAxis:
                data = EllipseAxisConstraint{edit.refs.front().entityId, parameter.id(),
                                             leg == SketchEditKind::AddMinorAxis};
                break;
            case SketchEditKind::AddEllipseRotation:
                data = EllipseRotationConstraint{edit.refs.front().entityId, parameter.id()};
                break;
            case SketchEditKind::AddAngle:
                data = AngleConstraint{edit.refs[0].entityId, edit.refs[1].entityId, parameter.id()};
                break;
            default:
                break;
            }
            if (addConstraint(std::move(data)) == kInvalidSketchConstraintId)
                return fail("The sketch refused that dimension.", "");

            if (!legStatus.empty()) legStatus += ", ";
            legStatus += parameter.name() + " = " +
                         (unit == UnitType::Radian ? FormatNumber(seed * 180.0 / kPi) + " deg"
                                                   : FormatNumber(seed) + " mm");
        }
        outcome.status = std::string(SketchEditKindName(edit.kind)) + " " + legStatus;
    }

    // --- Deletions ----------------------------------------------------------
    if (edit.kind == SketchEditKind::DeleteEntities) {
        std::size_t removed = 0;
        for (const SketchElementRef& ref : edit.refs)
            if (document.removeSketchEntity(sketchId, ref.entityId)) ++removed;
        if (removed == 0) return fail("Nothing was deleted.", "");
        outcome.status = "Deleted " + std::to_string(removed) + " sketch entit" +
                         (removed == 1 ? "y" : "ies") + " and every constraint on them.";
    }
    if (edit.kind == SketchEditKind::DeleteConstraints) {
        std::size_t removed = 0;
        for (const SketchConstraintId id : edit.constraints)
            if (document.removeSketchConstraint(sketchId, id)) ++removed;
        if (removed == 0) return fail("That constraint is already gone.", "");
        outcome.status =
            "Deleted " + std::to_string(removed) + " constraint" + (removed == 1 ? "" : "s") + ".";
    }

    // WHERE the dimension goes, inside the same transaction that made it
    // (M17.18) -- setSketchDimensionPlacement records a delta and opens no
    // transaction of its own, so it joins this one.
    if (edit.hasDimensionPlacement && !outcome.createdConstraints.empty())
        (void)document.setSketchDimensionPlacement(sketchId, outcome.createdConstraints.front(),
                                                   edit.dimensionPlacement);

    if (!document.commitTransaction()) return fail("The document refused the change.", "");

    outcome.applied = true;
    if (outcome.status.empty()) {
        outcome.status = std::string(SketchEditKindName(edit.kind)) + ": added " +
                         std::to_string(outcome.createdEntities.size()) + " entit" +
                         (outcome.createdEntities.size() == 1 ? "y" : "ies") + " and " +
                         std::to_string(outcome.createdConstraints.size()) + " constraint" +
                         (outcome.createdConstraints.size() == 1 ? "" : "s") + ".";
    }
    return outcome;
}

// =============================================================================
// What the user is shown
// =============================================================================

SketchStatusLine DescribeSketchStatus(const Sketch& sketch) {
    SketchStatusLine line;
    const int dof = sketch.degreesOfFreedom();
    const std::string dofText =
        dof == kUnknownDegreesOfFreedom ? std::string("not measured") : std::to_string(dof);

    switch (sketch.solveStatus()) {
    case SketchSolveStatus::Solved:
        line.severity = SketchStatusLine::Severity::Ok;
        line.badge = "OK";
        line.text = "Fully constrained -- DOF 0";
        break;
    case SketchSolveStatus::UnderConstrained:
        line.severity = SketchStatusLine::Severity::Info;
        line.badge = "DOF";
        line.text = "Under constrained -- DOF " + dofText;
        break;
    case SketchSolveStatus::OverConstrained:
        line.severity = SketchStatusLine::Severity::Warning;
        line.badge = "REDUNDANT";
        // Roadmap 8.2 point 3: redundant-but-consistent is a DIFFERENT
        // diagnosis from conflicting, and merging them sends the user to fix
        // the wrong thing.
        line.text = "Redundant constraints -- consistent, but more than needed";
        break;
    case SketchSolveStatus::Conflicting:
        line.severity = SketchStatusLine::Severity::Error;
        line.badge = "CONFLICT";
        line.text = "Conflicting constraints -- the sketch cannot satisfy them all";
        break;
    case SketchSolveStatus::InvalidInput:
        line.severity = SketchStatusLine::Severity::Error;
        line.badge = "INVALID";
        line.text = "Invalid constraint input -- a reference or a value is unusable";
        break;
    case SketchSolveStatus::NumericalFailure:
        line.severity = SketchStatusLine::Severity::Error;
        line.badge = "FAILED";
        // Roadmap 8.2 point 4: a convergence failure is not a model failure.
        line.text = "Solver did not converge -- numerical failure, not a contradiction";
        break;
    }

    line.detail = sketch.solveMessage();
    if (!sketch.offendingConstraints().empty()) {
        std::string ids;
        for (const SketchConstraintId id : sketch.offendingConstraints()) {
            if (!ids.empty()) ids += ", ";
            ids += "#" + std::to_string(static_cast<unsigned long long>(ToObjectId(id)));
        }
        if (!line.detail.empty()) line.detail += "\n";
        line.detail += "Constraints named by the solver: " + ids;
    }
    return line;
}

namespace {

bool IsOffending(const Sketch& sketch, SketchConstraintId id) {
    const std::vector<SketchConstraintId>& offending = sketch.offendingConstraints();
    return std::find(offending.begin(), offending.end(), id) != offending.end();
}

std::string ElementsText(const Sketch& sketch, const SketchConstraintData& data) {
    std::string text;
    if (const auto* coincident = std::get_if<CoincidentConstraint>(&data)) {
        text = DescribeElementRef(sketch, coincident->a) + ", " +
               DescribeElementRef(sketch, coincident->b);
    } else if (const auto* fix = std::get_if<FixConstraint>(&data)) {
        text = DescribeElementRef(sketch, fix->target);
    } else if (const auto* distance = std::get_if<DistanceConstraint>(&data)) {
        text = DescribeElementRef(sketch, distance->a) + ", " +
               DescribeElementRef(sketch, distance->b);
    } else if (const auto* dx = std::get_if<HorizontalDistanceConstraint>(&data)) {
        text = DescribeElementRef(sketch, dx->a) + ", " + DescribeElementRef(sketch, dx->b);
    } else if (const auto* dy = std::get_if<VerticalDistanceConstraint>(&data)) {
        text = DescribeElementRef(sketch, dy->a) + ", " + DescribeElementRef(sketch, dy->b);
    } else if (const auto* ph = std::get_if<PointsHorizontalConstraint>(&data)) {
        text = DescribeElementRef(sketch, ph->a) + ", " + DescribeElementRef(sketch, ph->b);
    } else if (const auto* pv = std::get_if<PointsVerticalConstraint>(&data)) {
        text = DescribeElementRef(sketch, pv->a) + ", " + DescribeElementRef(sketch, pv->b);
    } else if (const auto* symmetric = std::get_if<SymmetricConstraint>(&data)) {
        text = DescribeElementRef(sketch, symmetric->a) + ", " +
               DescribeElementRef(sketch, symmetric->b) + " about " +
               DescribeElementRef(sketch,
                                  SketchElementRef{symmetric->line, SketchSubElement::Whole});
    } else if (const auto* toLine = std::get_if<PointLineDistanceConstraint>(&data)) {
        text = DescribeElementRef(sketch, toLine->point) + ", " +
               DescribeElementRef(sketch, SketchElementRef{toLine->line, SketchSubElement::Whole});
    } else if (const auto* midpoint = std::get_if<MidpointConstraint>(&data)) {
        text = DescribeElementRef(sketch, midpoint->point) + ", " +
               DescribeElementRef(sketch,
                                  SketchElementRef{midpoint->line, SketchSubElement::Whole});
    } else if (const auto* on = std::get_if<PointOnObjectConstraint>(&data)) {
        text = DescribeElementRef(sketch, on->point) + ", " +
               DescribeElementRef(sketch, SketchElementRef{on->target, SketchSubElement::Whole});
    } else if (const auto* tangent = std::get_if<TangentConstraint>(&data)) {
        text = DescribeElementRef(sketch, SketchElementRef{tangent->a, SketchSubElement::Whole}) +
               ", " +
               DescribeElementRef(sketch, SketchElementRef{tangent->b, SketchSubElement::Whole}) +
               (tangent->at == SketchSubElement::StartPoint
                    ? " (at its start)"
                    : tangent->at == SketchSubElement::EndPoint
                          ? " (at its end)"
                          : (tangent->internal ? " (inner)" : " (outer)"));
    } else if (const auto* angle = std::get_if<AngleConstraint>(&data)) {
        text = DescribeElementRef(sketch, SketchElementRef{angle->lineA, SketchSubElement::Whole}) +
               ", " +
               DescribeElementRef(sketch, SketchElementRef{angle->lineB, SketchSubElement::Whole});
    } else {
        // The single-entity kinds. ReferencedEntities keeps this from becoming
        // a switch that forgets the tenth constraint type (ADR-M3-007).
        for (const SketchEntityId id : ReferencedEntities(data)) {
            if (!text.empty()) text += ", ";
            text += DescribeElementRef(sketch, SketchElementRef{id, SketchSubElement::Whole});
        }
    }
    return text.empty() ? "(no elements)" : text;
}

std::string ParameterText(const PartDocument& document, const SketchConstraintData& data) {
    const ObjectId parameterId = BoundParameterId(data);
    if (parameterId == kInvalidObjectId) return std::string();
    const Parameter* parameter = document.parameters().findById(parameterId);
    if (parameter == nullptr) return "(missing parameter)";
    std::string text = parameter->name() + " = ";
    if (parameter->unit() == UnitType::Radian)
        text += FormatNumber(parameter->value() * 180.0 / kPi) + " deg";
    else
        text += FormatNumber(parameter->value()) + " mm";
    if (!parameter->expression().empty()) text += "  [" + parameter->expression() + "]";
    return text;
}

} // namespace

std::vector<ConstraintRow> ConstraintRowsFor(const PartDocument& document, const Sketch& sketch) {
    std::vector<ConstraintRow> rows;
    rows.reserve(sketch.constraints().size());
    for (const SketchConstraint& constraint : sketch.constraints()) {
        ConstraintRow row;
        row.id = constraint.id;
        row.kind = ConstraintKindName(constraint.data);
        row.elements = ElementsText(sketch, constraint.data);
        row.parameter = ParameterText(document, constraint.data);
        row.dimensional = IsDimensional(constraint.data);
        row.offending = IsOffending(sketch, constraint.id);
        rows.push_back(std::move(row));
    }
    return rows;
}

namespace {

// --- Dimension layout -------------------------------------------------------
//
// The proportions a mechanical drawing uses, expressed against the measured
// quantity so a 4 mm hole and a 400 mm plate both look right. They are drawing
// geometry, in millimetres, and they SCALE WITH THE ZOOM -- a dimension belongs
// to the drawing, not to the screen. Only the arrowheads and the text are drawn
// at a fixed pixel size, because those are read rather than measured.

// How far the dimension line stands off the geometry it measures.
double OffsetFor(double measuredMm) noexcept {
    return std::clamp(measuredMm * 0.18, 3.0, 15.0);
}
// The gap a draughtsman leaves between the geometry and the start of its
// extension line, so the two are visibly separate things.
double ExtensionGapFor(double offsetMm) noexcept { return std::max(offsetMm * 0.12, 0.4); }
// How far the extension line runs PAST the dimension line.
double ExtensionOvershootFor(double offsetMm) noexcept { return std::max(offsetMm * 0.18, 0.6); }

Vec2 Add(Vec2 a, Vec2 b) noexcept { return Vec2{a.x + b.x, a.y + b.y}; }
Vec2 Scale(Vec2 v, double k) noexcept { return Vec2{v.x * k, v.y * k}; }

Vec2 Normalised(Vec2 v, bool* ok = nullptr) noexcept {
    const double length = std::sqrt(v.x * v.x + v.y * v.y);
    if (!(length > 1e-12) || !std::isfinite(length)) {
        if (ok != nullptr) *ok = false;
        return Vec2{1.0, 0.0};
    }
    if (ok != nullptr) *ok = true;
    return Vec2{v.x / length, v.y / length};
}

// Left-hand normal of a direction.
Vec2 LeftNormal(Vec2 direction) noexcept { return Vec2{-direction.y, direction.x}; }

// Keeps text within +/-90 degrees of upright. A value that reads upside down is
// a value nobody checks, and half of a rectangle's dimensions are drawn along
// directions that would otherwise invert.
double ReadableAngle(double radians) noexcept {
    double angle = WrapSigned(radians);
    if (angle > kPi * 0.5) angle -= kPi;
    if (angle < -kPi * 0.5) angle += kPi;
    return angle;
}

// The classic two-extension-line linear dimension.
//
// `side` chooses which way the dimension line is offset; +1 is the left-hand
// normal of a -> b.
void BuildLinearDimension(DimensionAnnotation& annotation, Vec2 a, Vec2 b, double side) {
    bool ok = false;
    const Vec2 along = Normalised(Vec2{b.x - a.x, b.y - a.y}, &ok);
    if (!ok) {
        // Two coincident points have no direction to lay a dimension along.
        // Placing the text and nothing else is honest; inventing a direction
        // would draw a dimension pointing somewhere arbitrary.
        annotation.labelMm = a;
        return;
    }
    const double measured = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    const double offset = OffsetFor(measured) * side;
    const Vec2 normal = LeftNormal(along);

    const Vec2 dimA = Add(a, Scale(normal, offset));
    const Vec2 dimB = Add(b, Scale(normal, offset));

    const double gap = ExtensionGapFor(std::abs(offset));
    const double overshoot = ExtensionOvershootFor(std::abs(offset));
    const double sign = offset >= 0.0 ? 1.0 : -1.0;
    annotation.extensionLines.push_back(
        {Add(a, Scale(normal, gap * sign)), Add(dimA, Scale(normal, overshoot * sign))});
    annotation.extensionLines.push_back(
        {Add(b, Scale(normal, gap * sign)), Add(dimB, Scale(normal, overshoot * sign))});

    annotation.dimensionLines.push_back({dimA, dimB});
    // Arrowheads point OUTWARD along the dimension line, toward the extension
    // lines -- which is what makes the pair read as "this span".
    annotation.arrows.push_back({dimA, Scale(along, -1.0)});
    annotation.arrows.push_back({dimB, along});

    annotation.labelMm = Vec2{(dimA.x + dimB.x) * 0.5, (dimA.y + dimB.y) * 0.5};
    annotation.textAngleRad = ReadableAngle(std::atan2(along.y, along.x));
}

// A dimension that measures ONE AXIS between two points: the dimension line runs
// horizontally (or vertically) clear of both, with an extension line dropped
// from each point to reach it.
//
// Not BuildLinearDimension with projected points. That would draw a line of the
// right LENGTH in the right DIRECTION, but its extension lines would rise from
// the projections rather than from the points the dimension is about -- and a
// dimension whose extension lines do not touch what it measures is a drawing
// error, not a stylistic one. This is the same shape a draughtsman draws.
void BuildAxisDimension(DimensionAnnotation& annotation, Vec2 a, Vec2 b, bool horizontal,
                        double side) {
    const double delta = horizontal ? b.x - a.x : b.y - a.y;
    if (std::abs(delta) < 1e-12) {
        // No separation on this axis: there is a length to state but no span to
        // draw one along. The value still gets placed, the geometry does not.
        annotation.labelMm = Midpoint(a, b);
        return;
    }
    const double offset = OffsetFor(std::abs(delta));
    const double sign = side >= 0.0 ? 1.0 : -1.0;

    // The dimension line sits BEYOND both points, so it never crosses the
    // geometry it is measuring.
    const double aCross = horizontal ? a.y : a.x;
    const double bCross = horizontal ? b.y : b.x;
    const double line = sign >= 0.0 ? std::max(aCross, bCross) + offset
                                    : std::min(aCross, bCross) - offset;

    const Vec2 dimA = horizontal ? Vec2{a.x, line} : Vec2{line, a.y};
    const Vec2 dimB = horizontal ? Vec2{b.x, line} : Vec2{line, b.y};

    const double gap = ExtensionGapFor(std::abs(offset));
    const double overshoot = ExtensionOvershootFor(std::abs(offset));
    // Each extension line runs from ITS OWN point to the shared dimension line,
    // so the two are generally different lengths -- which is exactly what says
    // "these two points, measured on one axis".
    const auto extension = [&](Vec2 point, Vec2 atLine) {
        const double from = horizontal ? point.y : point.x;
        const double toward = line >= from ? 1.0 : -1.0;
        const Vec2 start = horizontal ? Vec2{point.x, from + gap * toward}
                                      : Vec2{from + gap * toward, point.y};
        const Vec2 end = horizontal ? Vec2{atLine.x, line + overshoot * toward}
                                    : Vec2{line + overshoot * toward, atLine.y};
        annotation.extensionLines.push_back({start, end});
    };
    extension(a, dimA);
    extension(b, dimB);

    const Vec2 along = horizontal ? Vec2{delta >= 0.0 ? 1.0 : -1.0, 0.0}
                                  : Vec2{0.0, delta >= 0.0 ? 1.0 : -1.0};
    annotation.dimensionLines.push_back({dimA, dimB});
    annotation.arrows.push_back({dimA, Scale(along, -1.0)});
    annotation.arrows.push_back({dimB, along});

    annotation.labelMm = Midpoint(dimA, dimB);
    annotation.textAngleRad = ReadableAngle(std::atan2(along.y, along.x));
}

// Radius: one leader from the centre to the rim, arrow at the rim.
void BuildRadialDimension(DimensionAnnotation& annotation, Vec2 centre, double radius,
                          double angleRad) {
    const Vec2 radial{std::cos(angleRad), std::sin(angleRad)};
    const Vec2 rim = Add(centre, Scale(radial, radius));
    annotation.dimensionLines.push_back({centre, rim});
    annotation.arrows.push_back({rim, radial});
    annotation.labelMm = Add(centre, Scale(radial, radius * 0.55));
    annotation.textAngleRad = ReadableAngle(angleRad);
}

// Diameter: a line straight through the centre, arrows at both rim points.
void BuildDiametralDimension(DimensionAnnotation& annotation, Vec2 centre, double radius,
                             double angleRad) {
    const Vec2 radial{std::cos(angleRad), std::sin(angleRad)};
    const Vec2 near = Add(centre, Scale(radial, -radius));
    const Vec2 far = Add(centre, Scale(radial, radius));
    annotation.dimensionLines.push_back({near, far});
    annotation.arrows.push_back({near, Scale(radial, -1.0)});
    annotation.arrows.push_back({far, radial});
    annotation.labelMm = centre;
    annotation.textAngleRad = ReadableAngle(angleRad);
}

// Where two infinite lines cross. False when they are (near) parallel.
bool IntersectLines(const SketchLine& a, const SketchLine& b, Vec2* out) noexcept {
    const double ax = a.end.x - a.start.x;
    const double ay = a.end.y - a.start.y;
    const double bx = b.end.x - b.start.x;
    const double by = b.end.y - b.start.y;
    const double denominator = ax * by - ay * bx;
    const double scale = std::hypot(ax, ay) * std::hypot(bx, by);
    if (!(scale > 1e-12)) return false;
    // Compared against the SCALE, not against an absolute epsilon: two 1000 mm
    // lines a hair off parallel have a large cross product, and two 0.1 mm ones
    // that genuinely cross at 45 degrees have a tiny one.
    if (std::abs(denominator) < scale * 1e-9) return false;
    const double t = ((b.start.x - a.start.x) * by - (b.start.y - a.start.y) * bx) / denominator;
    *out = Vec2{a.start.x + ax * t, a.start.y + ay * t};
    return std::isfinite(out->x) && std::isfinite(out->y);
}

// Angular: an arc swept between the two lines about their intersection.
void BuildAngularDimension(DimensionAnnotation& annotation, const SketchLine& lineA,
                           const SketchLine& lineB) {
    Vec2 vertex{};
    if (!IntersectLines(lineA, lineB, &vertex)) {
        // Parallel lines meet at infinity. Drawing an arc of a made-up radius
        // somewhere would be worse than drawing none: the value still shows,
        // sitting between the two lines.
        annotation.labelMm = Vec2{(lineA.start.x + lineB.start.x) * 0.5,
                                  (lineA.start.y + lineB.start.y) * 0.5};
        return;
    }

    bool okA = false;
    bool okB = false;
    const Vec2 dirA = Normalised(Vec2{lineA.end.x - lineA.start.x, lineA.end.y - lineA.start.y},
                                 &okA);
    const Vec2 dirB = Normalised(Vec2{lineB.end.x - lineB.start.x, lineB.end.y - lineB.start.y},
                                 &okB);
    if (!okA || !okB) {
        annotation.labelMm = vertex;
        return;
    }

    // The arc sits comfortably inside the shorter of the two legs, so it lands
    // on the corner being measured rather than out in space.
    const auto legLength = [&](const SketchLine& line) {
        const double toStart = std::hypot(line.start.x - vertex.x, line.start.y - vertex.y);
        const double toEnd = std::hypot(line.end.x - vertex.x, line.end.y - vertex.y);
        return std::max(toStart, toEnd);
    };
    const double radius =
        std::clamp(std::min(legLength(lineA), legLength(lineB)) * 0.4, 2.0, 40.0);

    double startAngle = std::atan2(dirA.y, dirA.x);
    double endAngle = std::atan2(dirB.y, dirB.x);
    // FROM a TO b, counter-clockwise -- ADR-M5-006's convention, so the arc
    // drawn is the angle the constraint actually measures rather than its
    // supplement.
    double sweep = WrapSigned(endAngle - startAngle);
    if (sweep < 0.0) {
        std::swap(startAngle, endAngle);
        sweep = -sweep;
    }

    annotation.hasArc = true;
    annotation.arc = DimensionArc{vertex, radius, startAngle, startAngle + sweep};

    const Vec2 startPoint{vertex.x + radius * std::cos(startAngle),
                          vertex.y + radius * std::sin(startAngle)};
    const Vec2 endPoint{vertex.x + radius * std::cos(startAngle + sweep),
                        vertex.y + radius * std::sin(startAngle + sweep)};
    // Tangent to the arc at each end, pointing the way the arc runs.
    annotation.arrows.push_back({startPoint, Vec2{std::sin(startAngle), -std::cos(startAngle)}});
    annotation.arrows.push_back(
        {endPoint, Vec2{-std::sin(startAngle + sweep), std::cos(startAngle + sweep)}});

    // The value sits WELL CLEAR of the arc, on the bisector.
    //
    // 1.18 put it right on top of the arc, and the text is drawn with the
    // background knocked out behind it -- so the dimension erased its own arc
    // and only the number survived. Found by looking at a screenshot; no
    // geometry assertion could have seen it, because every position was
    // correct.
    const double middle = startAngle + sweep * 0.5;
    annotation.labelMm = Vec2{vertex.x + radius * 1.55 * std::cos(middle),
                              vertex.y + radius * 1.55 * std::sin(middle)};
    // Angular values are read UPRIGHT. Rotating them with the arc is what makes
    // an angle dimension hard to read on a drawing.
    annotation.textAngleRad = 0.0;
}

// Which side of a line the rest of the sketch is NOT on, so a dimension lands
// outside the shape instead of across it.
double OutwardSideFor(const Sketch& sketch, Vec2 a, Vec2 b) {
    bool ok = false;
    const Vec2 along = Normalised(Vec2{b.x - a.x, b.y - a.y}, &ok);
    if (!ok) return 1.0;
    const Vec2 normal = LeftNormal(along);
    const Vec2 middle{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5};

    // A crude centroid of everything in the sketch is enough: the question is
    // only which side has more geometry on it.
    double sumX = 0.0;
    double sumY = 0.0;
    int count = 0;
    const auto accumulate = [&](Vec2 p) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return;
        sumX += p.x;
        sumY += p.y;
        ++count;
    };
    for (const SketchEntity& entity : sketch.entities()) {
        if (const auto* point = std::get_if<SketchPoint>(&entity.geometry)) {
            accumulate(point->position);
        } else if (const auto* line = std::get_if<SketchLine>(&entity.geometry)) {
            accumulate(line->start);
            accumulate(line->end);
        } else if (const auto* circle = std::get_if<SketchCircle>(&entity.geometry)) {
            accumulate(circle->center);
        } else if (const auto* arc = std::get_if<SketchArc>(&entity.geometry)) {
            accumulate(arc->center);
        } else if (const auto* full = std::get_if<SketchEllipse>(&entity.geometry)) {
            accumulate(full->center);
        } else if (const auto* piece = std::get_if<SketchEllipticalArc>(&entity.geometry)) {
            accumulate(piece->center);
        } else if (const auto* spline = std::get_if<SketchSpline>(&entity.geometry)) {
            for (const Vec2& at : spline->points) accumulate(at);
        }
    }
    if (count == 0) return 1.0;
    const Vec2 centroid{sumX / count, sumY / count};
    const double towardsCentroid =
        (centroid.x - middle.x) * normal.x + (centroid.y - middle.y) * normal.y;
    // Push AWAY from the bulk of the sketch. Exactly on it (a single line,
    // dimensioned) the sign is arbitrary and +1 is as good as -1.
    return towardsCentroid > 0.0 ? -1.0 : 1.0;
}


// Room an arrowhead needs on screen, and a rough width for a value. Both are
// PIXELS, because both are drawn at a fixed screen size (ADR-M14-001) -- the
// layout needs to know how big they will be, not how big the geometry is.
constexpr double kArrowRoomPx = 16.0;

double EstimatedTextWidthPx(const std::string& text) {
    // Deliberately an ESTIMATE. The exact advance depends on the widget's font,
    // and asking for it here would drag Qt into this layer for a number that
    // only has to be right to within a character.
    return 8.0 * static_cast<double>(text.size()) + 6.0;
}

// The two points a LINEAR dimension spans, or false for the kinds that do not
// span two points.
bool MeasuredSpan(const Sketch& sketch, const SketchConstraintData& data, Vec2* a, Vec2* b) {
    if (const auto* length = std::get_if<LengthConstraint>(&data)) {
        const SketchLine* line = LineOf(sketch, length->line);
        if (line == nullptr) return false;
        *a = line->start;
        *b = line->end;
        return true;
    }
    if (const auto* distance = std::get_if<DistanceConstraint>(&data)) {
        bool okA = false;
        bool okB = false;
        *a = ResolveElementPoint(sketch, distance->a, &okA);
        *b = ResolveElementPoint(sketch, distance->b, &okB);
        return okA && okB;
    }
    // The axis kinds report the span they MEASURE, not the segment between the
    // two points: dragging a horizontal dimension has to keep it horizontal, so
    // the span it is re-laid along is the projection, not the diagonal.
    if (const auto* dx = std::get_if<HorizontalDistanceConstraint>(&data)) {
        bool okA = false;
        bool okB = false;
        const Vec2 pa = ResolveElementPoint(sketch, dx->a, &okA);
        const Vec2 pb = ResolveElementPoint(sketch, dx->b, &okB);
        *a = pa;
        *b = Vec2{pb.x, pa.y};
        return okA && okB;
    }
    if (const auto* toLine = std::get_if<PointLineDistanceConstraint>(&data)) {
        bool okPoint = false;
        const Vec2 point = ResolveElementPoint(sketch, toLine->point, &okPoint);
        const SketchLine* line = LineOf(sketch, toLine->line);
        if (!okPoint || line == nullptr) return false;
        const double du = line->end.x - line->start.x;
        const double dv = line->end.y - line->start.y;
        const double lengthSquared = du * du + dv * dv;
        if (lengthSquared <= kSketchToleranceMm) return false;
        const double t = ((point.x - line->start.x) * du + (point.y - line->start.y) * dv) /
                         lengthSquared;
        *a = Vec2{line->start.x + du * t, line->start.y + dv * t};
        *b = point;
        return true;
    }
    if (const auto* dy = std::get_if<VerticalDistanceConstraint>(&data)) {
        bool okA = false;
        bool okB = false;
        const Vec2 pa = ResolveElementPoint(sketch, dy->a, &okA);
        const Vec2 pb = ResolveElementPoint(sketch, dy->b, &okB);
        *a = pa;
        *b = Vec2{pa.x, pb.y};
        return okA && okB;
    }
    return false;
}

// Pushes apart the labels of AUTO-placed dimensions that would print on top of
// each other.
//
// User-placed ones are left exactly where they were put: a dimension somebody
// dragged somewhere is a decision, and quietly moving it would be the layout
// overruling the person using it. That asymmetry is the whole rule.
void SeparateOverlappingLabels(std::vector<DimensionAnnotation>& annotations,
                               double pixelsPerMm) {
    if (!(pixelsPerMm > 0.0) || annotations.size() < 2) return;

    for (std::size_t i = 0; i < annotations.size(); ++i) {
        DimensionAnnotation& mine = annotations[i];
        if (mine.userPlaced) continue;

        // Bounded: a sketch with many coincident dimensions must not turn this
        // into a long loop, and after a few steps the label is clear of
        // anything it started on top of.
        for (int attempt = 0; attempt < 6; ++attempt) {
            bool clashed = false;
            for (std::size_t j = 0; j < i; ++j) {
                const DimensionAnnotation& other = annotations[j];
                const double gapMm = std::hypot(other.labelMm.x - mine.labelMm.x,
                                                other.labelMm.y - mine.labelMm.y);
                const double neededMm =
                    (0.5 * (EstimatedTextWidthPx(mine.text) + EstimatedTextWidthPx(other.text)) +
                     4.0) /
                    pixelsPerMm;
                if (gapMm >= neededMm) continue;

                clashed = true;
                // Move ALONG the dimension line's normal, so the label stays
                // paired with the geometry it belongs to instead of drifting.
                Vec2 push{0.0, 1.0};
                if (!mine.dimensionLines.empty()) {
                    const DimensionSegment& span = mine.dimensionLines.front();
                    bool ok = false;
                    const Vec2 along = Normalised(
                        Vec2{span.toMm.x - span.fromMm.x, span.toMm.y - span.fromMm.y}, &ok);
                    if (ok) push = LeftNormal(along);
                }
                const double stepMm = neededMm - gapMm + 1.0 / pixelsPerMm;
                // Away from the thing it clashed with, not blindly one way.
                const double side = ((mine.labelMm.x - other.labelMm.x) * push.x +
                                     (mine.labelMm.y - other.labelMm.y) * push.y) >= 0.0
                                        ? 1.0
                                        : -1.0;
                const Vec2 shift = Scale(push, stepMm * side);
                mine.labelMm = Add(mine.labelMm, shift);
                for (DimensionSegment& segment : mine.dimensionLines) {
                    segment.fromMm = Add(segment.fromMm, shift);
                    segment.toMm = Add(segment.toMm, shift);
                }
                for (DimensionArrow& arrow : mine.arrows)
                    arrow.tipMm = Add(arrow.tipMm, shift);
                for (DimensionSegment& segment : mine.extensionLines)
                    segment.toMm = Add(segment.toMm, shift);
                break;
            }
            if (!clashed) break;
        }
    }
}

// The layout a dimension gets when nobody has moved it: the whole of M14's
// placement rule, factored out so a user-placed dimension can start from the
// same annotation and replace only the geometry.
bool BuildAutomaticLayout(const PartDocument& document, const Sketch& sketch,
                          const SketchConstraint& constraint, DimensionAnnotation& annotation) {
    annotation.id = constraint.id;
    annotation.parameterId = BoundParameterId(constraint.data);
    annotation.offending = IsOffending(sketch, constraint.id);
    const Parameter* parameter = document.parameters().findById(annotation.parameterId);
    const double value = parameter != nullptr ? parameter->value() : 0.0;

    if (const auto* length = std::get_if<LengthConstraint>(&constraint.data)) {
        const SketchLine* line = LineOf(sketch, length->line);
        if (line == nullptr) return false;
        annotation.kind = SketchEditKind::AddLength;
        annotation.text = FormatNumber(value);
        BuildLinearDimension(annotation, line->start, line->end,
                             OutwardSideFor(sketch, line->start, line->end));
    } else if (const auto* distance = std::get_if<DistanceConstraint>(&constraint.data)) {
        bool okA = false;
        bool okB = false;
        const Vec2 a = ResolveElementPoint(sketch, distance->a, &okA);
        const Vec2 b = ResolveElementPoint(sketch, distance->b, &okB);
        if (!okA || !okB) return false;
        annotation.kind = SketchEditKind::AddDistance;
        annotation.text = FormatNumber(value);
        BuildLinearDimension(annotation, a, b, OutwardSideFor(sketch, a, b));
    } else if (const auto* horizontal =
                   std::get_if<HorizontalDistanceConstraint>(&constraint.data)) {
        bool okA = false;
        bool okB = false;
        const Vec2 a = ResolveElementPoint(sketch, horizontal->a, &okA);
        const Vec2 b = ResolveElementPoint(sketch, horizontal->b, &okB);
        if (!okA || !okB) return false;
        annotation.kind = SketchEditKind::AddHorizontalDistance;
        // The stored value can be negative if the user typed one; a drawing
        // states a magnitude, and the direction is visible in the geometry.
        annotation.text = FormatNumber(std::abs(value));
        BuildAxisDimension(annotation, a, b, true, OutwardSideFor(sketch, a, b));
    } else if (const auto* vertical =
                   std::get_if<VerticalDistanceConstraint>(&constraint.data)) {
        bool okA = false;
        bool okB = false;
        const Vec2 a = ResolveElementPoint(sketch, vertical->a, &okA);
        const Vec2 b = ResolveElementPoint(sketch, vertical->b, &okB);
        if (!okA || !okB) return false;
        annotation.kind = SketchEditKind::AddVerticalDistance;
        annotation.text = FormatNumber(std::abs(value));
        BuildAxisDimension(annotation, a, b, false, OutwardSideFor(sketch, a, b));
    } else if (const auto* toLine = std::get_if<PointLineDistanceConstraint>(&constraint.data)) {
        bool okPoint = false;
        const Vec2 point = ResolveElementPoint(sketch, toLine->point, &okPoint);
        const SketchLine* line = LineOf(sketch, toLine->line);
        if (!okPoint || line == nullptr) return false;
        const double du = line->end.x - line->start.x;
        const double dv = line->end.y - line->start.y;
        const double lengthSquared = du * du + dv * dv;
        if (lengthSquared <= kSketchToleranceMm) return false;
        // Drawn from the point to its FOOT on the line, which is the segment
        // the number actually measures. Anything else would put a value next to
        // a span that is not the one it states.
        const double t = ((point.x - line->start.x) * du + (point.y - line->start.y) * dv) /
                         lengthSquared;
        const Vec2 foot{line->start.x + du * t, line->start.y + dv * t};
        annotation.kind = SketchEditKind::AddPointLineDistance;
        annotation.text = FormatNumber(std::abs(value));
        BuildLinearDimension(annotation, foot, point, OutwardSideFor(sketch, foot, point));
    } else if (const auto* radius = std::get_if<RadiusConstraint>(&constraint.data)) {
        Vec2 centre{};
        double r = 0.0;
        double midAngle = 0.0;
        if (!CurveCircle(sketch, radius->curve, &centre, &r, &midAngle)) return false;
        annotation.kind = SketchEditKind::AddRadius;
        // "R" prefix, the drawing convention -- the leader alone cannot say
        // whether a value is a radius or a diameter.
        annotation.text = "R" + FormatNumber(value);
        BuildRadialDimension(annotation, centre, r, midAngle);
    } else if (const auto* diameter = std::get_if<DiameterConstraint>(&constraint.data)) {
        Vec2 centre{};
        double r = 0.0;
        double midAngle = 0.0;
        if (!CurveCircle(sketch, diameter->curve, &centre, &r, &midAngle)) return false;
        annotation.kind = SketchEditKind::AddDiameter;
        annotation.text = "D" + FormatNumber(value);
        // A full circle gets a HORIZONTAL diameter, the drawing
        // convention, rather than the 45 degrees a radius uses. Two
        // dimensions on one circle then cross rather than overlap, and a
        // horizontal value is the easier of the two to read.
        const SketchEntity* entity = sketch.findEntity(diameter->curve);
        const bool fullCircle =
            entity != nullptr && std::holds_alternative<SketchCircle>(entity->geometry);
        BuildDiametralDimension(annotation, centre, r, fullCircle ? 0.0 : midAngle);
    } else if (const auto* angle = std::get_if<AngleConstraint>(&constraint.data)) {
        const SketchLine* a = LineOf(sketch, angle->lineA);
        const SketchLine* b = LineOf(sketch, angle->lineB);
        if (a == nullptr || b == nullptr) return false;
        annotation.kind = SketchEditKind::AddAngle;
        annotation.text = FormatNumber(value * 180.0 / kPi) + "deg";
        BuildAngularDimension(annotation, *a, *b);
    } else {
        return false;
    }
    // The prefix, suffix and tolerance go on LAST, so every kind picks them up
    // without each branch remembering to.
    annotation.text = FormattedDimensionText(sketch, constraint.id, annotation.text,
                                             annotation.kind == SketchEditKind::AddAngle);
    // A REFERENCE dimension is drawn in PARENTHESES (M17.19, ADR-M17-042).
    //
    // The drafting convention, and the one every reader of a drawing already
    // knows -- which matters more here than in most places: a number that
    // looks like it controls the part and does not is worse than no number.
    // Text, not colour, because a monochrome print still has to say it (A06).
    if (constraint.driven) annotation.text = "(" + annotation.text + ")";
    return true;
}

} // namespace

namespace {

// Re-lays a dimension around a label the USER put somewhere.
//
// The stored placement is the ONE thing a drag produces -- the point the value
// was dragged to -- and every kind derives its own geometry from it. Storing a
// per-kind bundle of offsets instead would need a different drag interaction
// for each, and the user is doing the same thing every time: moving the number.
void ReplaceWithUserPlacement(DimensionAnnotation& annotation, Vec2 label, Vec2 a, Vec2 b,
                              bool linear) {
    annotation.extensionLines.clear();
    annotation.dimensionLines.clear();
    annotation.arrows.clear();
    annotation.hasArc = false;
    annotation.userPlaced = true;

    if (!linear) {
        annotation.labelMm = label;
        return;
    }
    bool ok = false;
    const Vec2 along = Normalised(Vec2{b.x - a.x, b.y - a.y}, &ok);
    if (!ok) {
        annotation.labelMm = label;
        return;
    }
    const Vec2 normal = LeftNormal(along);
    // The label's PERPENDICULAR distance is the offset; its position along the
    // measured direction is ignored, so the dimension line always spans what it
    // measures. Letting the label slide sideways as well would let a user
    // produce a dimension line that starts and ends nowhere near its geometry.
    const double offset = (label.x - a.x) * normal.x + (label.y - a.y) * normal.y;

    const Vec2 dimA = Add(a, Scale(normal, offset));
    const Vec2 dimB = Add(b, Scale(normal, offset));
    const double magnitude = std::abs(offset);
    const double gap = ExtensionGapFor(magnitude);
    const double overshoot = ExtensionOvershootFor(magnitude);
    const double sign = offset >= 0.0 ? 1.0 : -1.0;
    annotation.extensionLines.push_back(
        {Add(a, Scale(normal, gap * sign)), Add(dimA, Scale(normal, overshoot * sign))});
    annotation.extensionLines.push_back(
        {Add(b, Scale(normal, gap * sign)), Add(dimB, Scale(normal, overshoot * sign))});
    annotation.dimensionLines.push_back({dimA, dimB});
    annotation.arrows.push_back({dimA, Scale(along, -1.0)});
    annotation.arrows.push_back({dimB, along});
    annotation.labelMm = Vec2{(dimA.x + dimB.x) * 0.5, (dimA.y + dimB.y) * 0.5};
    annotation.textAngleRad = ReadableAngle(std::atan2(along.y, along.x));
}

// Turns the arrowheads round when the span is too tight to hold them.
//
// The drawing convention: when the value and its two heads cannot fit between
// the extension lines, the heads go OUTSIDE pointing in, the dimension line
// grows a stub past each one, and the value moves out beyond the right-hand
// end. Leaving them inside produces two heads overlapping a number, which is
// the one thing on a drawing that must never be ambiguous.
//
// It is decided in PIXELS because that is what "fits" means for a value and an
// arrowhead -- both are drawn at a fixed screen size (ADR-M14-001).
void FlipArrowsIfCramped(DimensionAnnotation& annotation, double pixelsPerMm,
                         double neededPx) {
    if (!(pixelsPerMm > 0.0)) return;
    if (annotation.dimensionLines.size() != 1 || annotation.arrows.size() != 2) return;

    const DimensionSegment span = annotation.dimensionLines.front();
    const double lengthMm = std::hypot(span.toMm.x - span.fromMm.x, span.toMm.y - span.fromMm.y);
    if (lengthMm * pixelsPerMm >= neededPx) return;

    bool ok = false;
    const Vec2 along =
        Normalised(Vec2{span.toMm.x - span.fromMm.x, span.toMm.y - span.fromMm.y}, &ok);
    if (!ok) return;

    // Heads outside, pointing back in at the extension lines.
    const double stubMm = neededPx * 0.45 / pixelsPerMm;
    annotation.arrows.clear();
    annotation.arrows.push_back({span.fromMm, along});
    annotation.arrows.push_back({span.toMm, Scale(along, -1.0)});
    // Stubs for the heads to sit on, so they are attached to something.
    annotation.dimensionLines.push_back(
        {span.fromMm, Add(span.fromMm, Scale(along, -stubMm))});
    annotation.dimensionLines.push_back({span.toMm, Add(span.toMm, Scale(along, stubMm))});
    // ...and the value clears the whole thing.
    annotation.labelMm = Add(span.toMm, Scale(along, stubMm * 1.9));
}

} // namespace

Vec2 AutomaticDimensionLabel(const PartDocument& document, const Sketch& sketch,
                             SketchConstraintId constraintId, bool* ok) {
    if (ok != nullptr) *ok = false;
    const SketchConstraint* constraint = sketch.findConstraint(constraintId);
    if (constraint == nullptr || !IsDimensional(constraint->data)) return Vec2{};

    DimensionAnnotation annotation;
    if (!BuildAutomaticLayout(document, sketch, *constraint, annotation)) return Vec2{};
    if (ok != nullptr) *ok = true;
    return annotation.labelMm;
}

namespace {

// A short glyph for each constraint kind. Text, not an icon set: it is legible
// at any DPI, it survives a monochrome screenshot, and A06 wants the meaning
// carried by something other than colour.
//
// ASCII on purpose for the M13 kinds -- a glyph font with a hole in it would
// put the meaning back into the colour.
const char* GlyphFor(const SketchConstraintData& data) {
    if (std::holds_alternative<HorizontalConstraint>(data)) return "H";
    if (std::holds_alternative<VerticalConstraint>(data)) return "V";
    // THE SAME GLYPHS: what the badge says is what the constraint MEANS, and
    // both forms mean the same thing. A separate glyph would ask the user to
    // learn a distinction that only exists in the storage.
    if (std::holds_alternative<PointsHorizontalConstraint>(data)) return "H";
    if (std::holds_alternative<PointsVerticalConstraint>(data)) return "V";
    if (std::holds_alternative<CoincidentConstraint>(data)) return "o";
    if (std::holds_alternative<FixConstraint>(data)) return "X";
    if (std::holds_alternative<ParallelConstraint>(data)) return "//";
    if (std::holds_alternative<PerpendicularConstraint>(data)) return "|_";
    if (std::holds_alternative<EqualConstraint>(data)) return "=";
    if (std::holds_alternative<ConcentricConstraint>(data)) return "()";
    if (std::holds_alternative<MidpointConstraint>(data)) return "M";
    if (std::holds_alternative<PointOnObjectConstraint>(data)) return "-o";
    if (std::holds_alternative<TangentConstraint>(data)) return "T";
    return nullptr; // dimensional kinds draw as dimensions, not badges
}

} // namespace

// --- TRIM --------------------------------------------------------------------

namespace {

// Normalises an angle into [0, 2pi).
double WrapPositiveAngle(double angle) noexcept {
    const double twoPi = 2.0 * kPi;
    double wrapped = std::fmod(angle, twoPi);
    if (wrapped < 0.0) wrapped += twoPi;
    return wrapped;
}

// Parameter along `line` (0 at start, 1 at end) of the point nearest `p`.
double ParamAlong(const SketchLine& line, Vec2 p) noexcept {
    const double du = line.end.x - line.start.x;
    const double dv = line.end.y - line.start.y;
    const double lengthSquared = du * du + dv * dv;
    if (lengthSquared <= kSketchToleranceMm) return 0.0;
    return ((p.x - line.start.x) * du + (p.y - line.start.y) * dv) / lengthSquared;
}

Vec2 PointAt(const SketchLine& line, double t) noexcept {
    return Vec2{line.start.x + (line.end.x - line.start.x) * t,
                line.start.y + (line.end.y - line.start.y) * t};
}

// Whether `t` is on the segment, with a tolerance that keeps a crossing exactly
// at an endpoint from being dropped by rounding.
bool OnSegment(double t) noexcept { return t > -1e-9 && t < 1.0 + 1e-9; }

} // namespace

// Where two circles cross. Empty when they miss, nest, or are the same circle.
//
// The concentric case is excluded deliberately: two circles with the same
// centre and radius agree everywhere, and "everywhere" is not a set of cutting
// points -- it is a modelling mistake the user should see rather than a trim
// that silently picks somewhere.
std::vector<Vec2> CircleCircleCrossings(Vec2 c1, double r1, Vec2 c2, double r2) {
    std::vector<Vec2> points;
    const double dx = c2.x - c1.x;
    const double dy = c2.y - c1.y;
    const double distance = std::sqrt(dx * dx + dy * dy);
    if (distance <= kSketchToleranceMm) return points;              // concentric
    if (distance > r1 + r2 + kSketchToleranceMm) return points;     // too far apart
    if (distance < std::abs(r1 - r2) - kSketchToleranceMm) return points; // one inside

    const double a = (r1 * r1 - r2 * r2 + distance * distance) / (2.0 * distance);
    const double hSquared = r1 * r1 - a * a;
    const double h = hSquared > 0.0 ? std::sqrt(hSquared) : 0.0;
    const Vec2 mid{c1.x + a * dx / distance, c1.y + a * dy / distance};
    if (h <= kSketchToleranceMm) {
        points.push_back(mid); // tangent: one point
        return points;
    }
    points.push_back(Vec2{mid.x + h * dy / distance, mid.y - h * dx / distance});
    points.push_back(Vec2{mid.x - h * dy / distance, mid.y + h * dx / distance});
    return points;
}

// Where `cutterId` meets the CIRCLE of `centre` and `radius`, as points.
//
// The circle, not the arc: whether those points are on the piece being asked
// about is the caller's question, and asking it here would make this unusable
// for a full circle. The CUTTER's own extent IS applied, because a crossing off
// the end of a line, or off an arc's sweep, is not a place the two can be seen
// to meet.
std::vector<Vec2> CircleHits(const Sketch& sketch, Vec2 centre, double radius,
                             SketchEntityId cutterId) {
    std::vector<Vec2> hits;
    const SketchEntity* cutter = sketch.findEntity(cutterId);
    if (cutter == nullptr || radius <= kSketchToleranceMm) return hits;

    if (const auto* line = std::get_if<SketchLine>(&cutter->geometry)) {
        const double du = line->end.x - line->start.x;
        const double dv = line->end.y - line->start.y;
        const double lengthSquared = du * du + dv * dv;
        if (lengthSquared <= kSketchToleranceMm) return hits;
        const double fx = line->start.x - centre.x;
        const double fy = line->start.y - centre.y;
        const double b = 2.0 * (fx * du + fy * dv);
        const double c = fx * fx + fy * fy - radius * radius;
        const double discriminant = b * b - 4.0 * lengthSquared * c;
        if (discriminant < 0.0) return hits;
        const double root = std::sqrt(discriminant);
        for (const double t : {(-b - root) / (2.0 * lengthSquared),
                               (-b + root) / (2.0 * lengthSquared)}) {
            if (t < -1e-9 || t > 1.0 + 1e-9) continue;
            hits.push_back(Vec2{line->start.x + du * t, line->start.y + dv * t});
        }
        // A TANGENT LINE gives the same point twice. One touch is one place.
        if (hits.size() == 2 &&
            std::hypot(hits[0].x - hits[1].x, hits[0].y - hits[1].y) <= kSketchToleranceMm)
            hits.pop_back();
        return hits;
    }

    Vec2 cutterCentre{};
    double cutterRadius = 0.0;
    double midAngle = 0.0;
    if (!CurveCircle(sketch, cutterId, &cutterCentre, &cutterRadius, &midAngle)) return hits;
    hits = CircleCircleCrossings(centre, radius, cutterCentre, cutterRadius);
    if (const auto* cutterArc = std::get_if<SketchArc>(&cutter->geometry)) {
        hits.erase(std::remove_if(hits.begin(), hits.end(),
                                  [&](Vec2 p) {
                                      return !AngleOnArcSweep(
                                          *cutterArc, std::atan2(p.y - cutterCentre.y,
                                                                 p.x - cutterCentre.x));
                                  }),
                   hits.end());
    }
    return hits;
}

// Where `cutterId` crosses the ARC `arc`, as positions along its sweep in
// [0,1] from its start angle to its end angle.
std::vector<double> TrimCutsAlongArc(const Sketch& sketch, const SketchArc& arc,
                                     SketchEntityId targetId, SketchEntityId cutterId) {
    std::vector<double> cuts;
    if (targetId == cutterId) return cuts;
    const SketchEntity* cutter = sketch.findEntity(cutterId);
    if (cutter == nullptr) return cuts;

    const double sweep = arc.counterClockwise ? WrapPositiveAngle(arc.endAngleRad - arc.startAngleRad)
                                              : WrapPositiveAngle(arc.startAngleRad - arc.endAngleRad);
    if (sweep <= 1e-9) return cuts;

    const std::vector<Vec2> hits = CircleHits(sketch, arc.center, arc.radiusMm, cutterId);

    for (const Vec2& p : hits) {
        const double angle = std::atan2(p.y - arc.center.y, p.x - arc.center.x);
        const double along = arc.counterClockwise ? WrapPositiveAngle(angle - arc.startAngleRad)
                                                  : WrapPositiveAngle(arc.startAngleRad - angle);
        if (along > sweep + 1e-9) continue; // past the end of the sweep
        cuts.push_back(along / sweep);
    }
    return cuts;
}

std::vector<double> TrimCutsAlongLine(const Sketch& sketch, SketchEntityId targetId,
                                      SketchEntityId cutterId) {
    std::vector<double> cuts;
    if (targetId == cutterId) return cuts;
    const SketchLine* target = LineOf(sketch, targetId);
    const SketchEntity* cutter = sketch.findEntity(cutterId);
    if (target == nullptr || cutter == nullptr) return cuts;

    const double du = target->end.x - target->start.x;
    const double dv = target->end.y - target->start.y;
    const double lengthSquared = du * du + dv * dv;
    if (lengthSquared <= kSketchToleranceMm) return cuts;

    if (const auto* line = std::get_if<SketchLine>(&cutter->geometry)) {
        Vec2 at{};
        if (!IntersectLines(*target, *line, &at)) return cuts; // parallel
        const double t = ParamAlong(*target, at);
        // On BOTH segments: an infinite-line crossing off the end of the cutter
        // is not a place the user can see the two meet.
        if (OnSegment(t) && OnSegment(ParamAlong(*line, at))) cuts.push_back(t);
        return cuts;
    }

    Vec2 centre{};
    double radius = 0.0;
    double midAngle = 0.0;
    if (!CurveCircle(sketch, cutterId, &centre, &radius, &midAngle)) return cuts;

    // Line-circle: solve |start + t*d - centre|^2 = r^2 for t.
    const double fx = target->start.x - centre.x;
    const double fy = target->start.y - centre.y;
    const double b = 2.0 * (fx * du + fy * dv);
    const double c = fx * fx + fy * fy - radius * radius;
    const double discriminant = b * b - 4.0 * lengthSquared * c;
    if (discriminant < 0.0) return cuts; // misses entirely
    const double root = std::sqrt(discriminant);
    const double candidates[2] = {(-b - root) / (2.0 * lengthSquared),
                                  (-b + root) / (2.0 * lengthSquared)};
    const auto* arc = std::get_if<SketchArc>(&cutter->geometry);
    for (const double t : candidates) {
        if (!OnSegment(t)) continue;
        if (arc != nullptr) {
            // An ARC only cuts where the arc actually is. The circle through it
            // reaches further, and cutting there would trim at a point with
            // nothing drawn at it.
            const Vec2 at = PointAt(*target, t);
            const double angle = std::atan2(at.y - centre.y, at.x - centre.x);
            if (!AngleOnArcSweep(*arc, angle)) continue;
        }
        cuts.push_back(t);
    }
    return cuts;
}

namespace {

// Where the INFINITE line through `targetId` meets `cutterId`, as t along the
// target. The crossing must still lie on the CUTTER: extending to a place the
// boundary does not actually occupy would stop at nothing.
std::vector<double> InfiniteLineCrossings(const Sketch& sketch, SketchEntityId targetId,
                                          SketchEntityId cutterId) {
    std::vector<double> crossings;
    if (targetId == cutterId) return crossings;
    const SketchLine* target = LineOf(sketch, targetId);
    const SketchEntity* cutter = sketch.findEntity(cutterId);
    if (target == nullptr || cutter == nullptr) return crossings;
    const double du = target->end.x - target->start.x;
    const double dv = target->end.y - target->start.y;
    const double lengthSquared = du * du + dv * dv;
    if (lengthSquared <= kSketchToleranceMm) return crossings;

    if (const auto* line = std::get_if<SketchLine>(&cutter->geometry)) {
        Vec2 at{};
        if (!IntersectLines(*target, *line, &at)) return crossings;
        if (OnSegment(ParamAlong(*line, at))) crossings.push_back(ParamAlong(*target, at));
        return crossings;
    }
    Vec2 centre{};
    double radius = 0.0;
    double midAngle = 0.0;
    if (!CurveCircle(sketch, cutterId, &centre, &radius, &midAngle)) return crossings;
    const double fx = target->start.x - centre.x;
    const double fy = target->start.y - centre.y;
    const double b = 2.0 * (fx * du + fy * dv);
    const double c = fx * fx + fy * fy - radius * radius;
    const double discriminant = b * b - 4.0 * lengthSquared * c;
    if (discriminant < 0.0) return crossings;
    const double root = std::sqrt(discriminant);
    const double candidates[2] = {(-b - root) / (2.0 * lengthSquared),
                                  (-b + root) / (2.0 * lengthSquared)};
    const auto* arc = std::get_if<SketchArc>(&cutter->geometry);
    for (const double t : candidates) {
        if (arc != nullptr) {
            const Vec2 at = PointAt(*target, t);
            if (!AngleOnArcSweep(*arc, std::atan2(at.y - centre.y, at.x - centre.x))) continue;
        }
        crossings.push_back(t);
    }
    return crossings;
}

} // namespace

TrimPlan PlanTrim(const Sketch& sketch, SketchEntityId targetId,
                  const std::vector<SketchEntityId>& cutterIds, Vec2 pickMm) {
    TrimPlan plan;
    const auto refuse = [&](std::string why) {
        plan.ok = false;
        plan.why = std::move(why);
        return plan;
    };
    const SketchEntity* entity = sketch.findEntity(targetId);
    if (entity == nullptr) return refuse("That entity is not in this sketch.");
    if (std::holds_alternative<SketchCircle>(entity->geometry))
        return refuse("A circle cannot be trimmed: cutting one does not shorten it, it turns it "
                      "into an arc -- and every constraint naming the circle would suddenly be "
                      "naming something else.");
    if (std::holds_alternative<SketchEllipse>(entity->geometry) ||
        std::holds_alternative<SketchEllipticalArc>(entity->geometry))
        return refuse("Trimming an ellipse is not supported yet: finding exactly where "
                      "something crosses one needs geometry EP3D does not have.");
    if (std::holds_alternative<SketchSpline>(entity->geometry))
        return refuse("A spline cannot be trimmed: shortening one means moving the points it "
                      "goes through, and which points those should be is not something a "
                      "click can say.");
    const SketchLine* line = std::get_if<SketchLine>(&entity->geometry);
    const SketchArc* arc = std::get_if<SketchArc>(&entity->geometry);
    if (line == nullptr && arc == nullptr) return refuse("Only lines and arcs can be trimmed.");

    // WHERE the cuts are, as a fraction along the target -- 0 at its start, 1
    // at its end. One vocabulary for both kinds, so everything below is about
    // which PIECE was picked rather than about lines versus arcs.
    std::vector<double> cuts;
    for (const SketchEntityId cutterId : cutterIds) {
        const std::vector<double> found =
            line != nullptr ? TrimCutsAlongLine(sketch, targetId, cutterId)
                            : TrimCutsAlongArc(sketch, *arc, targetId, cutterId);
        cuts.insert(cuts.end(), found.begin(), found.end());
    }
    // Crossings AT an end cut nothing off; dropping them keeps them from being
    // chosen as a boundary that leaves the target exactly as it was.
    cuts.erase(std::remove_if(cuts.begin(), cuts.end(),
                              [](double t) { return t < 1e-6 || t > 1.0 - 1e-6; }),
               cuts.end());
    if (cuts.empty())
        return refuse("Nothing crosses that, so there is no piece to trim off.");
    std::sort(cuts.begin(), cuts.end());

    // WHICH PIECE the user pointed at, in the same fraction.
    double pick = 0.0;
    if (line != nullptr) {
        pick = ParamAlong(*line, pickMm);
    } else {
        const double sweep =
            arc->counterClockwise ? WrapPositiveAngle(arc->endAngleRad - arc->startAngleRad)
                                  : WrapPositiveAngle(arc->startAngleRad - arc->endAngleRad);
        if (sweep <= 1e-9) return refuse("That arc has no sweep to trim.");
        const double angle = std::atan2(pickMm.y - arc->center.y, pickMm.x - arc->center.x);
        const double along = arc->counterClockwise
                                 ? WrapPositiveAngle(angle - arc->startAngleRad)
                                 : WrapPositiveAngle(arc->startAngleRad - angle);
        pick = along / sweep;
        if (pick > 1.0 + 1e-9)
            return refuse("Click the part of the arc you want to remove.");
    }
    if (pick < -1e-9 || pick > 1.0 + 1e-9)
        return refuse("Click the part you want to remove.");

    const auto above = std::lower_bound(cuts.begin(), cuts.end(), pick);
    const bool hasBefore = above != cuts.begin();
    const bool hasAfter = above != cuts.end();
    if (hasBefore && hasAfter)
        return refuse("That piece is in the middle, between two crossings. Removing it would "
                      "split it in two, and which half keeps the constraints is not something "
                      "to decide for you.");

    plan.target = targetId;
    plan.trimmedStart = hasAfter;
    const double at = hasAfter ? *above : *(above - 1);

    if (line != nullptr) {
        SketchLine trimmed = *line;
        (hasAfter ? trimmed.start : trimmed.end) = PointAt(*line, at);
        if (Distance(trimmed.start, trimmed.end) <= kMinSketchDimensionMm)
            return refuse("Trimming there would leave nothing of the line.");
        plan.result = trimmed;
    } else {
        // TRIMMING AN ARC MOVES AN ANGLE. The tips are where the angles put
        // them (ADR-M17-018), so this is the same edit as moving a line's
        // endpoint, expressed in the arc's own state.
        const double sweep =
            arc->counterClockwise ? WrapPositiveAngle(arc->endAngleRad - arc->startAngleRad)
                                  : WrapPositiveAngle(arc->startAngleRad - arc->endAngleRad);
        const double direction = arc->counterClockwise ? 1.0 : -1.0;
        const double cutAngle = arc->startAngleRad + direction * at * sweep;
        SketchArc trimmed = *arc;
        (hasAfter ? trimmed.startAngleRad : trimmed.endAngleRad) = cutAngle;
        const double remaining =
            trimmed.counterClockwise
                ? WrapPositiveAngle(trimmed.endAngleRad - trimmed.startAngleRad)
                : WrapPositiveAngle(trimmed.startAngleRad - trimmed.endAngleRad);
        if (remaining <= 1e-6) return refuse("Trimming there would leave nothing of the arc.");
        plan.result = trimmed;
    }
    plan.ok = true;
    return plan;
}

TrimPlan PlanExtend(const Sketch& sketch, SketchEntityId targetId,
                    const std::vector<SketchEntityId>& boundaryIds, Vec2 pickMm) {
    TrimPlan plan;
    const auto refuse = [&](std::string why) {
        plan.ok = false;
        plan.why = std::move(why);
        return plan;
    };
    const SketchEntity* entity = sketch.findEntity(targetId);
    if (entity == nullptr) return refuse("That entity is not in this sketch.");
    const SketchLine* target = std::get_if<SketchLine>(&entity->geometry);
    if (target == nullptr)
        return refuse("Only lines can be extended yet: an arc's ends carry no solver variables, "
                      "so a stretched curve would be held in place by nothing.");
    if (Distance(target->start, target->end) <= kSketchToleranceMm)
        return refuse("That line is too short to have an end to extend.");

    // WHICH END the user pointed at. Nearer half means that end -- the same
    // "point at what you mean" rule trim uses, so the two commands do not need
    // separate explanations.
    const double pick = ParamAlong(*target, pickMm);
    const bool extendStart = pick < 0.5;

    // Crossings of the INFINITE line, since the point of extending is to reach
    // something the segment does not currently touch.
    double best = 0.0;
    bool found = false;
    for (const SketchEntityId boundaryId : boundaryIds) {
        for (const double t : InfiniteLineCrossings(sketch, targetId, boundaryId)) {
            // Ahead of the end being grown, never behind it.
            if (extendStart ? !(t < -1e-9) : !(t > 1.0 + 1e-9)) continue;
            // NEAREST first: extending past the first thing in the way would
            // cross geometry the user can see it should have stopped at.
            const bool better = !found || (extendStart ? t > best : t < best);
            if (better) {
                best = t;
                found = true;
            }
        }
    }
    if (!found)
        return refuse("Nothing lies beyond that end, so there is nothing to extend to.");

    plan.target = targetId;
    plan.trimmedStart = extendStart;
    SketchLine stretched = *target;
    (extendStart ? stretched.start : stretched.end) = PointAt(*target, best);
    plan.result = stretched;
    plan.ok = true;
    return plan;
}

SketchEdit MakeOffsetEdit(const Sketch& sketch, SketchEntityId sourceId, double distanceMm,
                          double side, std::string* whyNot) {
    SketchEdit edit;
    const auto refuse = [&](std::string why) {
        if (whyNot != nullptr) *whyNot = std::move(why);
        return SketchEdit{};
    };
    const SketchEntity* source = sketch.findEntity(sourceId);
    if (source == nullptr) return refuse("That entity is not in this sketch.");
    if (!(std::abs(distanceMm) >= kMinSketchDimensionMm))
        return refuse("An offset distance of zero would put the copy on top of the original.");
    const double signedDistance = side >= 0.0 ? std::abs(distanceMm) : -std::abs(distanceMm);

    if (const auto* line = std::get_if<SketchLine>(&source->geometry)) {
        const double du = line->end.x - line->start.x;
        const double dv = line->end.y - line->start.y;
        const double length = std::sqrt(du * du + dv * dv);
        if (length <= kSketchToleranceMm) return refuse("That line is too short to offset.");
        // LEFT of start->end, the same side PointLineDistance calls positive.
        // One convention, shared with the residual, so the constraint the edit
        // creates measures the number the geometry was placed at.
        const Vec2 normal{-dv / length, du / length};
        const Vec2 start{line->start.x + normal.x * signedDistance,
                         line->start.y + normal.y * signedDistance};
        const Vec2 end{line->end.x + normal.x * signedDistance,
                       line->end.y + normal.y * signedDistance};

        edit.kind = SketchEditKind::AddLine;
        edit.points = {start, end};
        edit.label = "Offset";
        edit.offsetSource = sourceId;
        edit.offsetDistanceMm = signedDistance;
        return edit;
    }
    if (std::holds_alternative<SketchCircle>(source->geometry) ||
        std::holds_alternative<SketchArc>(source->geometry)) {
        Vec2 centre{};
        double radius = 0.0;
        double midAngle = 0.0;
        if (!CurveCircle(sketch, sourceId, &centre, &radius, &midAngle))
            return refuse("That curve cannot be offset.");
        const double offsetRadius = radius + signedDistance;
        // A concentric copy smaller than nothing is not a curve. Refused with
        // the number, because "invalid" leaves the user to work out by how
        // much.
        if (!(offsetRadius >= kMinSketchDimensionMm))
            return refuse("Offsetting inward by " + FormatNumber(std::abs(distanceMm)) +
                          " would leave a radius of " + FormatNumber(offsetRadius) + ".");

        if (const auto* arc = std::get_if<SketchArc>(&source->geometry)) {
            edit.kind = SketchEditKind::AddArc;
            // Centre, then the two tips at the SAME angles: an offset arc
            // spans the same sweep, it is only further out.
            edit.points = {centre,
                           Vec2{centre.x + offsetRadius * std::cos(arc->startAngleRad),
                                centre.y + offsetRadius * std::sin(arc->startAngleRad)},
                           Vec2{centre.x + offsetRadius * std::cos(arc->endAngleRad),
                                centre.y + offsetRadius * std::sin(arc->endAngleRad)}};
        } else {
            edit.kind = SketchEditKind::AddCircle;
            edit.points = {centre, Vec2{centre.x + offsetRadius, centre.y}};
        }
        edit.label = "Offset";
        edit.offsetSource = sourceId;
        edit.offsetDistanceMm = signedDistance;
        return edit;
    }
    if (std::holds_alternative<SketchSpline>(source->geometry))
        return refuse("A spline cannot be offset: the curve a fixed distance from one is not "
                      "a spline through any set of points, and EP3D has no shape for it.");
    if (std::holds_alternative<SketchEllipse>(source->geometry) ||
        std::holds_alternative<SketchEllipticalArc>(source->geometry))
        // NOT A SHAPE THIS MODEL HAS. The curve a fixed distance from an
        // ellipse is not another ellipse -- it is a degree-8 offset curve --
        // so there is nothing to create. Saying so beats producing a
        // near-enough ellipse that is wrong by a fraction of a millimetre in
        // the middle of each quadrant.
        return refuse("An ellipse cannot be offset: the curve a fixed distance from one is "
                      "not an ellipse, and EP3D has no shape for it.");
    return refuse("Only lines, circles and arcs can be offset.");
}

// --- TRANSFORM ---------------------------------------------------------------

SketchGeometry TransformedGeometry(const SketchGeometry& geometry,
                                   const SketchTransform& transform, Vec2 anchor) {
    const auto place = [&](Vec2 p) -> Vec2 {
        switch (transform.kind) {
        case SketchTransformKind::Move:
            return Vec2{p.x + transform.deltaMm.x, p.y + transform.deltaMm.y};
        case SketchTransformKind::Rotate: {
            const double du = p.x - anchor.x;
            const double dv = p.y - anchor.y;
            const double c = std::cos(transform.angleRad);
            const double sn = std::sin(transform.angleRad);
            return Vec2{anchor.x + du * c - dv * sn, anchor.y + du * sn + dv * c};
        }
        case SketchTransformKind::Scale:
            return Vec2{anchor.x + (p.x - anchor.x) * transform.factor,
                        anchor.y + (p.y - anchor.y) * transform.factor};
        }
        return p;
    };

    if (const auto* point = std::get_if<SketchPoint>(&geometry))
        return SketchPoint{place(point->position)};
    if (const auto* line = std::get_if<SketchLine>(&geometry))
        return SketchLine{place(line->start), place(line->end)};
    if (const auto* circle = std::get_if<SketchCircle>(&geometry)) {
        SketchCircle moved = *circle;
        moved.center = place(circle->center);
        if (transform.kind == SketchTransformKind::Scale)
            moved.radiusMm = circle->radiusMm * transform.factor;
        return moved;
    }
    if (const auto* arc = std::get_if<SketchArc>(&geometry)) {
        SketchArc moved = *arc;
        moved.center = place(arc->center);
        if (transform.kind == SketchTransformKind::Scale)
            moved.radiusMm = arc->radiusMm * transform.factor;
        // ANGLES TURN, and only for a rotation. A scale about a point leaves
        // every direction where it was, and a move certainly does -- adding the
        // angle in either case would spin the arc's sweep round its own centre
        // while the centre sat still.
        if (transform.kind == SketchTransformKind::Rotate) {
            moved.startAngleRad = arc->startAngleRad + transform.angleRad;
            moved.endAngleRad = arc->endAngleRad + transform.angleRad;
        }
        return moved;
    }
    if (const auto* spline = std::get_if<SketchSpline>(&geometry)) {
        // EVERY POINT, through the same `place`. A spline has no centre and no
        // radius: moving, turning and resizing it are all the same operation on
        // its points, which is the one case where this function has nothing
        // special to say.
        SketchSpline moved = *spline;
        for (Vec2& at : moved.points) at = place(at);
        return moved;
    }
    if (const auto* full = std::get_if<SketchEllipse>(&geometry)) {
        SketchEllipse moved = *full;
        moved.center = place(full->center);
        if (transform.kind == SketchTransformKind::Scale) {
            moved.majorRadiusMm = full->majorRadiusMm * transform.factor;
            moved.minorRadiusMm = full->minorRadiusMm * transform.factor;
        }
        // THE ROTATION turns, and the PARAMETERS do not. The parameter says how
        // far round the ellipse's own frame a point is, and turning the frame
        // carries the point with it -- adding the angle to both would turn it
        // twice.
        if (transform.kind == SketchTransformKind::Rotate)
            moved.rotationRad = full->rotationRad + transform.angleRad;
        return moved;
    }
    const auto& piece = std::get<SketchEllipticalArc>(geometry);
    SketchEllipticalArc moved = piece;
    moved.center = place(piece.center);
    if (transform.kind == SketchTransformKind::Scale) {
        moved.majorRadiusMm = piece.majorRadiusMm * transform.factor;
        moved.minorRadiusMm = piece.minorRadiusMm * transform.factor;
    }
    if (transform.kind == SketchTransformKind::Rotate)
        moved.rotationRad = piece.rotationRad + transform.angleRad;
    return moved;
}

Vec2 TransformAnchor(const Sketch& sketch, const std::vector<SketchEntityId>& entities,
                     bool* fromSelectedPoint) {
    if (fromSelectedPoint != nullptr) *fromSelectedPoint = false;

    // ONE SELECTED POINT is a user saying where. Two is not -- it is ambiguous,
    // and picking the first would be a rule nobody could guess.
    const SketchPoint* only = nullptr;
    int points = 0;
    for (const SketchEntityId id : entities) {
        const SketchEntity* entity = sketch.findEntity(id);
        if (entity == nullptr) continue;
        if (const auto* point = std::get_if<SketchPoint>(&entity->geometry)) {
            ++points;
            only = point;
        }
    }
    if (points == 1 && only != nullptr) {
        if (fromSelectedPoint != nullptr) *fromSelectedPoint = true;
        return only->position;
    }

    // Otherwise the centre of what is selected, from the box round its extremes.
    bool any = false;
    double minU = 0.0, minV = 0.0, maxU = 0.0, maxV = 0.0;
    const auto include = [&](Vec2 p) {
        if (!any) {
            minU = maxU = p.x;
            minV = maxV = p.y;
            any = true;
            return;
        }
        minU = std::min(minU, p.x);
        maxU = std::max(maxU, p.x);
        minV = std::min(minV, p.y);
        maxV = std::max(maxV, p.y);
    };
    for (const SketchEntityId id : entities) {
        const SketchEntity* entity = sketch.findEntity(id);
        if (entity == nullptr) continue;
        if (const auto* circle = std::get_if<SketchCircle>(&entity->geometry)) {
            include(Vec2{circle->center.x - circle->radiusMm, circle->center.y - circle->radiusMm});
            include(Vec2{circle->center.x + circle->radiusMm, circle->center.y + circle->radiusMm});
            continue;
        }
        if (std::holds_alternative<SketchArc>(entity->geometry)) {
            // THE ARC ITSELF, not the circle it is part of: its ends and the
            // point half way round are where it actually is, and a full-circle
            // box would drag the anchor off towards geometry that is not there.
            include(StartPointOf(entity->geometry));
            include(EndPointOf(entity->geometry));
            include(MidPointOf(entity->geometry));
            continue;
        }
        if (const auto* point = std::get_if<SketchPoint>(&entity->geometry)) {
            include(point->position);
            continue;
        }
        if (const auto* line = std::get_if<SketchLine>(&entity->geometry)) {
            include(line->start);
            include(line->end);
            continue;
        }
        if (const auto* spline = std::get_if<SketchSpline>(&entity->geometry)) {
            // THE SAMPLED CURVE, so the anchor is the middle of the shape
            // rather than the middle of the points it was drawn through.
            for (const Vec2& at : SampleSpline(*spline, 8)) include(at);
            continue;
        }
        // AN ELLIPSE, whichever kind. The MAJOR radius both ways: a box that
        // certainly contains it whatever its rotation, and one rule rather than
        // a second place for the rotation convention to live.
        //
        // This used to be an unguarded `std::get<SketchLine>` on the assumption
        // that a line was all that could be left. When the variant grew, that
        // assumption became a bad_variant_access on any transform of a
        // selection containing an ellipse.
        double major = 0.0;
        Vec2 centre{};
        if (const auto* full = std::get_if<SketchEllipse>(&entity->geometry)) {
            major = full->majorRadiusMm;
            centre = full->center;
        } else if (const auto* piece =
                       std::get_if<SketchEllipticalArc>(&entity->geometry)) {
            // The ARC ITSELF, like the circular case above: its ends and its
            // middle are where it actually is.
            include(StartPointOf(entity->geometry));
            include(EndPointOf(entity->geometry));
            include(MidPointOf(entity->geometry));
            continue;
        } else {
            continue;
        }
        include(Vec2{centre.x - major, centre.y - major});
        include(Vec2{centre.x + major, centre.y + major});
    }
    if (!any) return Vec2{0.0, 0.0};
    return Vec2{(minU + maxU) * 0.5, (minV + maxV) * 0.5};
}

TransformOutcome ApplyTransform(PartDocument& document, ObjectId sketchId,
                                const std::vector<SketchEntityId>& entities,
                                const SketchTransform& transform) {
    TransformOutcome outcome;
    const Sketch* sketch = document.findSketch(sketchId);
    if (sketch == nullptr) {
        outcome.status = "No such sketch.";
        return outcome;
    }
    if (entities.empty()) {
        outcome.status = "Select what to transform first.";
        return outcome;
    }
    for (const SketchEntityId id : entities)
        if (sketch->findEntity(id) == nullptr) {
            outcome.status = "Something selected is no longer in this sketch.";
            return outcome;
        }

    // A TRANSFORM THAT MOVES NOTHING is refused rather than run. In place it
    // would be a no-op with a success message; as a copy it would drop a
    // duplicate exactly on top of the original, where the only way to find it
    // is to notice the DOF went up.
    const char* nothing = nullptr;
    switch (transform.kind) {
    case SketchTransformKind::Move:
        if (std::hypot(transform.deltaMm.x, transform.deltaMm.y) < kMinSketchDimensionMm)
            nothing = "That move is zero. Give it a distance.";
        break;
    case SketchTransformKind::Rotate:
        if (std::fabs(transform.angleRad) < 1e-9) nothing = "That rotation is zero.";
        break;
    case SketchTransformKind::Scale:
        if (!(transform.factor > 0.0))
            nothing = "A scale has to be positive. To flip it over, use Mirror.";
        else if (std::fabs(transform.factor - 1.0) < 1e-9)
            nothing = "A scale of 1 changes nothing.";
        break;
    }
    if (nothing != nullptr) {
        outcome.status = nothing;
        return outcome;
    }

    bool fromPoint = false;
    outcome.anchor = TransformAnchor(*sketch, entities, &fromPoint);

    const auto inSet = [&entities](SketchEntityId id) {
        return std::find(entities.begin(), entities.end(), id) != entities.end();
    };

    document.beginTransaction(transform.keepACopy ? "Copy" : "Transform");
    const auto fail = [&](std::string status) {
        document.abortTransaction();
        outcome.applied = false;
        outcome.created.clear();
        outcome.status = std::move(status);
        return outcome;
    };

    if (!transform.keepACopy) {
        // READ ALL, THEN WRITE ALL, for the reason the copy branch below
        // records at length: `sketch` points into the document, and every write
        // is a write to it.
        std::vector<SketchGeometry> moved;
        for (const SketchEntityId id : entities)
            moved.push_back(
                TransformedGeometry(sketch->findEntity(id)->geometry, transform, outcome.anchor));
        for (std::size_t i = 0; i < entities.size(); ++i)
            if (!document.setSketchEntityGeometry(sketchId, entities[i], moved[i]))
                return fail("The sketch refused to move one of those.");
        if (!document.commitTransaction()) return fail("The document refused the transform.");
        outcome.applied = true;
        outcome.status = "Moved " + std::to_string(entities.size()) + " item(s).";
        return outcome;
    }

    // --- The copy ------------------------------------------------------------
    //
    // EVERYTHING IS READ FIRST. The loop below ADDS constraints to the sketch,
    // and the first version of it walked `sketch->constraints()` while doing so
    // -- iterating a vector that was being appended to. It copied two of eight
    // and reported success, which is precisely the shape of failure this
    // project keeps writing tests to catch: a plausible number, no error, and
    // geometry that comes apart on the next drag.
    std::vector<SketchConstraintData> existing;
    for (const SketchConstraint& constraint : sketch->constraints())
        existing.push_back(constraint.data);

    std::vector<std::pair<SketchEntityId, SketchEntityId>> map;
    for (const SketchEntityId id : entities) {
        const SketchEntity* entity = sketch->findEntity(id);
        const SketchGeometry moved =
            TransformedGeometry(entity->geometry, transform, outcome.anchor);
        const bool construction = entity->construction;
        // ...and `sketch` is not read after this point either: adding an entity
        // is a write to the very object it points into.
        const SketchEntityId copy = document.addSketchEntity(sketchId, moved, construction);
        if (copy == kInvalidSketchEntityId) return fail("The sketch refused one of the copies.");
        map.emplace_back(id, copy);
        outcome.created.push_back(copy);
    }
    const auto copyOf = [&map](SketchEntityId id) {
        for (const auto& pair : map)
            if (pair.first == id) return pair.second;
        return kInvalidSketchEntityId;
    };

    for (const SketchConstraintData& data : existing) {
        const std::vector<SketchElementRef> refs = ReferencedElements(data);
        bool touches = false;
        bool wholly = true;
        for (const SketchElementRef& ref : refs) {
            if (inSet(ref.entityId)) touches = true;
            else wholly = false;
        }
        if (!touches) continue;
        // REACHES OUT of the selection. Copying it would tie the copy to the
        // original's neighbours -- a rectangle copied off a wall would still be
        // stuck to that wall -- so it is not copied, and it is COUNTED. The
        // original keeps it, so nothing the user typed has gone.
        if (!wholly) {
            ++outcome.constraintsLeftBehind;
            continue;
        }

        SketchConstraintData copy = data;
        VisitConstraintElements(copy, [&](SketchEntityId& id, SketchSubElement) {
            id = copyOf(id);
        });

        // ITS OWN PARAMETER, seeded with the same value -- the rule Offset
        // already follows. Sharing the original's would look right and then
        // refuse to let the copy be resized, which is a relationship the user
        // never asked for and cannot see.
        bool rebound = true;
        VisitBoundParameter(copy, [&](ObjectId& bound) {
            const Parameter* source = document.parameters().findById(bound);
            if (source == nullptr) {
                rebound = false;
                return;
            }
            const Parameter& fresh = document.addParameter(
                UnusedParameterName(document, "d"), source->value(), source->unit());
            bound = fresh.id();
        });
        if (!rebound) return fail("A copied dimension had no parameter to copy.");

        if (document.addSketchConstraint(sketchId, copy) == kInvalidSketchConstraintId)
            return fail("The sketch refused a copied constraint.");
        ++outcome.constraintsCopied;
    }

    if (!document.commitTransaction()) return fail("The document refused the copy.");
    outcome.applied = true;
    outcome.status = "Copied " + std::to_string(outcome.created.size()) + " item(s) with " +
                     std::to_string(outcome.constraintsCopied) + " constraint(s).";
    if (outcome.constraintsLeftBehind > 0)
        outcome.status += " " + std::to_string(outcome.constraintsLeftBehind) +
                          " constraint(s) reached outside the selection and were not copied.";
    if (transform.kind != SketchTransformKind::Move)
        outcome.status += fromPoint ? " Turned about the selected point."
                                    : " Turned about the centre of the selection.";
    return outcome;
}

// --- SPLIT -------------------------------------------------------------------

SplitSurvival SurvivesSplit(const SketchConstraintData& data, SketchEntityId target,
                            const SketchGeometry& targetGeometry) {
    // HOW the target is named decides most of it. A constraint that only ever
    // touches an end point goes with that end; one that names the whole thing
    // has to be judged on what it says about it.
    bool namesStart = false;
    bool namesEnd = false;
    bool namesCentre = false;
    bool namesWhole = false;
    bool namesSplinePoint = false;
    for (const SketchElementRef& ref : ReferencedElements(data)) {
        if (ref.entityId != target) continue;
        switch (ref.subElement) {
        case SketchSubElement::StartPoint: namesStart = true; break;
        case SketchSubElement::EndPoint: namesEnd = true; break;
        case SketchSubElement::CenterPoint: namesCentre = true; break;
        case SketchSubElement::SplinePoint: namesSplinePoint = true; break;
        case SketchSubElement::Whole: namesWhole = true; break;
        }
    }
    // Not about the target at all. Callers filter these out; answering
    // EveryPiece would be a claim, and there is nothing to claim.
    if (!namesStart && !namesEnd && !namesCentre && !namesWhole && !namesSplinePoint)
        return SplitSurvival::EveryPiece;

    // A SPLINE POINT IS NAMED BY ITS NUMBER, and cutting the spline renumbers
    // them: point 4 of the original is point 1 of the second piece, or gone.
    // Carrying the constraint over would leave it pointing confidently at a
    // different point than the one the user picked, so it cannot survive.
    // Splitting a spline is refused outright (PlanSplit) so nothing reaches
    // this today -- it is here so that lifting that refusal cannot silently
    // move somebody's constraint.
    if (namesSplinePoint) return SplitSurvival::Refuse;

    if (!namesWhole) {
        // BOTH ENDS, and no piece has both any more. A distance from one end of
        // a line to the other is its length by another name.
        if (namesStart && namesEnd) return SplitSurvival::Refuse;
        // The CENTRE survives on every piece: sub-arcs of one arc share it, and
        // so do the arcs a circle is cut into.
        if (namesCentre && !namesStart && !namesEnd) return SplitSurvival::EveryPiece;
        return SplitSurvival::OwningPiece;
    }

    // Named as a WHOLE. What survives depends on whether the property is one
    // every piece inherits -- direction for a line, centre and radius for a
    // curve -- or one that is about its extent.
    // A SPLINE IS NEITHER. It has no direction a piece inherits and no centre
    // or radius a piece shares, so every whole-entity constraint on one is
    // about its extent -- which is the Refuse branch, and correct. Splitting a
    // spline is refused outright anyway (PlanSplit), so this is belt and
    // braces rather than the load-bearing check.
    const bool curve = std::holds_alternative<SketchArc>(targetGeometry) ||
                       std::holds_alternative<SketchCircle>(targetGeometry) ||
                       std::holds_alternative<SketchEllipse>(targetGeometry) ||
                       std::holds_alternative<SketchEllipticalArc>(targetGeometry);
    return std::visit(
        [curve](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            (void)c;
            if constexpr (std::is_same_v<T, HorizontalConstraint> ||
                          std::is_same_v<T, VerticalConstraint> ||
                          std::is_same_v<T, ParallelConstraint> ||
                          std::is_same_v<T, PerpendicularConstraint> ||
                          std::is_same_v<T, AngleConstraint>) {
                // DIRECTION, which every sub-segment of a line has too.
                return curve ? SplitSurvival::Refuse : SplitSurvival::EveryPiece;
            } else if constexpr (std::is_same_v<T, RadiusConstraint> ||
                                 std::is_same_v<T, DiameterConstraint> ||
                                 std::is_same_v<T, EllipseAxisConstraint> ||
                                 std::is_same_v<T, EllipseRotationConstraint> ||
                                 std::is_same_v<T, ConcentricConstraint>) {
                // CENTRE AND RADIUS, which every sub-arc has too -- and an
                // ellipse's two semi-axes likewise.
                return curve ? SplitSurvival::EveryPiece : SplitSurvival::Refuse;
            } else {
                // EXTENT, or a place along it. Length, Equal, Tangent,
                // Point-on-object, a point's distance to this line, a midpoint,
                // symmetry about it -- every one of them says something about
                // the thing as a whole that stops being true of any piece.
                return SplitSurvival::Refuse;
            }
        },
        data);
}

namespace {

// The cut positions along `targetId`, sorted, deduplicated, and with the ends
// dropped. A crossing AT an end cuts off nothing.
std::vector<double> SplitCuts(const Sketch& sketch, SketchEntityId targetId,
                              const SketchEntity& target,
                              const std::vector<SketchEntityId>& cutterIds) {
    std::vector<double> cuts;
    const auto* arc = std::get_if<SketchArc>(&target.geometry);
    for (const SketchEntityId cutter : cutterIds) {
        if (cutter == targetId) continue;
        std::vector<double> found;
        if (std::holds_alternative<SketchLine>(target.geometry)) {
            found = TrimCutsAlongLine(sketch, targetId, cutter);
        } else if (arc != nullptr) {
            found = TrimCutsAlongArc(sketch, *arc, targetId, cutter);
        } else if (const auto* circle = std::get_if<SketchCircle>(&target.geometry)) {
            // A CIRCLE has no start to measure from, so its cuts are absolute
            // angles turned into fractions of a full turn. It cannot go through
            // TrimCutsAlongArc: a whole turn wraps to a sweep of zero, and that
            // function -- rightly -- refuses to divide by it.
            for (const Vec2& hit : CircleHits(sketch, circle->center, circle->radiusMm, cutter))
                found.push_back(WrapPositiveAngle(std::atan2(hit.y - circle->center.y,
                                                             hit.x - circle->center.x)) /
                                (2.0 * kPi));
        }
        cuts.insert(cuts.end(), found.begin(), found.end());
    }
    const bool closed = std::holds_alternative<SketchCircle>(target.geometry);
    std::sort(cuts.begin(), cuts.end());
    std::vector<double> kept;
    for (const double t : cuts) {
        // AT AN END is not a cut on an open curve: it takes nothing off, and
        // the zero-length piece it would make is refused by the sketch anyway.
        // On a CLOSED one there are no ends -- angle zero is an ordinary place.
        if (!closed && (t <= 1e-6 || t >= 1.0 - 1e-6)) continue;
        if (!kept.empty() && t - kept.back() <= 1e-6) continue; // the same place twice
        kept.push_back(t);
    }
    // ...and on a circle the first and last can still be the same place, once
    // round.
    if (closed && kept.size() > 1 && kept.front() + 1.0 - kept.back() <= 1e-6) kept.pop_back();
    return kept;
}

// The angle `fraction` of the way along an arc's own sweep.
double AngleAlong(const SketchArc& arc, double fraction) noexcept {
    const double sweep = arc.counterClockwise
                             ? WrapPositiveAngle(arc.endAngleRad - arc.startAngleRad)
                             : WrapPositiveAngle(arc.startAngleRad - arc.endAngleRad);
    return arc.counterClockwise ? arc.startAngleRad + sweep * fraction
                                : arc.startAngleRad - sweep * fraction;
}

} // namespace

SplitPlan PlanSplit(const Sketch& sketch, SketchEntityId targetId,
                    const std::vector<SketchEntityId>& cutterIds) {
    SplitPlan plan;
    plan.target = targetId;
    const auto refuse = [&plan](std::string why) {
        plan.ok = false;
        plan.why = std::move(why);
        plan.pieces.clear();
        return plan;
    };

    const SketchEntity* target = sketch.findEntity(targetId);
    if (target == nullptr) return refuse("That entity is not in this sketch.");
    if (std::holds_alternative<SketchPoint>(target->geometry))
        return refuse("A point has nothing to cut.");
    if (cutterIds.empty())
        return refuse("Select what to split, then the things that cross it.");

    const std::vector<double> cuts = SplitCuts(sketch, targetId, *target, cutterIds);
    const bool closed = std::holds_alternative<SketchCircle>(target->geometry);
    if (cuts.empty())
        return refuse("Nothing selected crosses it, so there is nowhere to cut.");
    if (closed && cuts.size() < 2)
        return refuse("One crossing does not open a circle. Two are needed to split it.");

    // THE CONSTRAINTS, before the geometry: a plan that reports pieces and then
    // has to be thrown away is a plan that already told the caller something
    // that is not going to happen.
    std::vector<std::string> blocked;
    for (const SketchConstraint& constraint : sketch.constraints()) {
        bool touches = false;
        for (const SketchElementRef& ref : ReferencedElements(constraint.data))
            touches = touches || ref.entityId == targetId;
        if (!touches) continue;
        if (SurvivesSplit(constraint.data, targetId, target->geometry) != SplitSurvival::Refuse)
            continue;
        blocked.push_back(ConstraintKindName(constraint.data));
    }
    if (!blocked.empty()) {
        std::string names;
        for (const std::string& name : blocked) {
            if (!names.empty()) names += ", ";
            names += name;
        }
        return refuse("Splitting it would change what these constraints say: " + names +
                      ". Delete them first, or split something else.");
    }

    if (const auto* line = std::get_if<SketchLine>(&target->geometry)) {
        double from = 0.0;
        for (std::size_t i = 0; i <= cuts.size(); ++i) {
            const double to = i < cuts.size() ? cuts[i] : 1.0;
            SplitPiece piece;
            piece.geometry = SketchLine{PointAt(*line, from), PointAt(*line, to)};
            piece.keepsStart = i == 0;
            piece.keepsEnd = i == cuts.size();
            plan.pieces.push_back(std::move(piece));
            from = to;
        }
    } else if (const auto* arc = std::get_if<SketchArc>(&target->geometry)) {
        double from = 0.0;
        for (std::size_t i = 0; i <= cuts.size(); ++i) {
            const double to = i < cuts.size() ? cuts[i] : 1.0;
            SplitPiece piece;
            piece.geometry = SketchArc{arc->center, arc->radiusMm, AngleAlong(*arc, from),
                                       AngleAlong(*arc, to), arc->counterClockwise};
            piece.keepsStart = i == 0;
            piece.keepsEnd = i == cuts.size();
            plan.pieces.push_back(std::move(piece));
            from = to;
        }
    } else if (std::holds_alternative<SketchSpline>(target->geometry)) {
        return refuse("Splitting a spline is not supported yet: the pieces would have to be "
                      "splines through points nobody clicked.");
    } else if (std::holds_alternative<SketchEllipse>(target->geometry) ||
               std::holds_alternative<SketchEllipticalArc>(target->geometry)) {
        // NOT YET, and said rather than attempted. Cutting an ellipse is
        // straightforward geometry -- the pieces are elliptical arcs of the
        // same ellipse -- but finding WHERE a line crosses one is a quartic,
        // and SplitCuts has no answer for it. Guessing with the sampler the
        // canvas picks with would put the cut a fraction off the crossing, and
        // the joint would then be visibly not on the cutter.
        return refuse("Splitting an ellipse is not supported yet: finding exactly where "
                      "something crosses one needs geometry EP3D does not have.");
    } else {
        const auto& circle = std::get<SketchCircle>(target->geometry);
        // ONE ARC PER GAP, all the way round -- so the last cut runs back to the
        // first and there are as many pieces as cuts, not one more. A circle has
        // no start to keep, which is why neither flag is ever set here.
        for (std::size_t i = 0; i < cuts.size(); ++i) {
            const double from = cuts[i] * 2.0 * kPi;
            const double to = cuts[(i + 1) % cuts.size()] * 2.0 * kPi;
            SplitPiece piece;
            piece.geometry = SketchArc{circle.center, circle.radiusMm, from, to, true};
            plan.pieces.push_back(std::move(piece));
        }
        plan.closesTheLoop = true;
    }

    plan.ok = true;
    plan.why = "Split into " + std::to_string(plan.pieces.size()) + " pieces.";
    return plan;
}

SplitOutcome ApplySplit(PartDocument& document, ObjectId sketchId, SketchEntityId targetId,
                        const std::vector<SketchEntityId>& cutterIds) {
    SplitOutcome outcome;
    const Sketch* sketch = document.findSketch(sketchId);
    if (sketch == nullptr) {
        outcome.status = "No such sketch.";
        return outcome;
    }
    const SplitPlan plan = PlanSplit(*sketch, targetId, cutterIds);
    if (!plan.ok) {
        outcome.status = plan.why;
        return outcome;
    }

    // WHAT HAS TO BE REBUILT, read before anything moves. Once the original is
    // gone its constraints are gone with it, so the list of what to re-hang has
    // to exist first.
    struct Rehang {
        SketchConstraintData data;
        SplitSurvival survival;
    };
    std::vector<Rehang> rehang;
    for (const SketchConstraint& constraint : sketch->constraints()) {
        bool touches = false;
        for (const SketchElementRef& ref : ReferencedElements(constraint.data))
            touches = touches || ref.entityId == targetId;
        if (!touches) continue;
        rehang.push_back(Rehang{constraint.data,
                                SurvivesSplit(constraint.data, targetId, sketch->findEntity(targetId)->geometry)});
    }
    const bool construction = sketch->findEntity(targetId)->construction;

    document.beginTransaction("Split");
    const auto fail = [&](std::string status) {
        document.abortTransaction();
        outcome.applied = false;
        outcome.created.clear();
        outcome.status = std::move(status);
        return outcome;
    };

    std::vector<SketchEntityId> pieces;
    for (const SplitPiece& piece : plan.pieces) {
        const SketchEntityId id = document.addSketchEntity(sketchId, piece.geometry, construction);
        if (id == kInvalidSketchEntityId) return fail("The sketch refused one of the pieces.");
        pieces.push_back(id);
    }

    // THE JOINTS, which are what make this a split. Added before the original
    // goes, so a refusal leaves nothing half-done.
    const std::size_t joints = plan.closesTheLoop ? pieces.size() : pieces.size() - 1;
    for (std::size_t i = 0; i < joints; ++i) {
        const SketchEntityId a = pieces[i];
        const SketchEntityId b = pieces[(i + 1) % pieces.size()];
        if (document.addSketchConstraint(
                sketchId, CoincidentConstraint{SketchElementRef{a, SketchSubElement::EndPoint},
                                               SketchElementRef{b, SketchSubElement::StartPoint}}) ==
            kInvalidSketchConstraintId)
            return fail("The sketch refused one of the split's joints.");
    }

    if (!document.removeSketchEntity(sketchId, targetId))
        return fail("The sketch refused to remove the entity that was split.");

    // RE-HUNG LAST, because removing the original takes its own constraints
    // with it -- putting these back first would just delete them again.
    for (const Rehang& item : rehang) {
        // Which pieces this constraint now belongs to, and with what sub-element
        // for the target's own reference.
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            SketchConstraintData copy = item.data;
            bool wanted = item.survival == SplitSurvival::EveryPiece;
            VisitConstraintElements(copy, [&](SketchEntityId& id, SketchSubElement part) {
                if (id != targetId) return;
                if (item.survival == SplitSurvival::OwningPiece) {
                    // SurvivesSplit only says OwningPiece when the target is
                    // named at exactly one of its ends, so those two are the
                    // real cases. Anything else copies rather than vanishes: a
                    // redundant constraint is visible in the list and in the
                    // DOF, and a missing one is not.
                    const bool mine = part == SketchSubElement::StartPoint
                                          ? plan.pieces[i].keepsStart
                                          : part == SketchSubElement::EndPoint
                                                ? plan.pieces[i].keepsEnd
                                                : true;
                    if (!mine) return;
                    wanted = true;
                }
                id = pieces[i];
            });
            if (!wanted) continue;
            if (document.addSketchConstraint(sketchId, copy) == kInvalidSketchConstraintId)
                return fail("The sketch refused to move a constraint onto one of the pieces.");
        }
    }

    if (!document.commitTransaction()) return fail("The document refused the split.");
    outcome.applied = true;
    outcome.created = pieces;
    outcome.status = plan.why;
    return outcome;
}

// --- CHAMFER -----------------------------------------------------------------

namespace {

// Which ends of two lines form their corner: the closest pair. Shared by
// Chamfer and Fillet -- they ask the same question and must get the same answer. Returns false
// when the nearest pair is further apart than `toleranceMm`, because two lines
// that do not meet have no corner to cut.
bool FindCorner(const SketchLine& a, const SketchLine& b, double toleranceMm, bool* aAtStart,
                bool* bAtStart) noexcept {
    const Vec2 aEnds[2] = {a.start, a.end};
    const Vec2 bEnds[2] = {b.start, b.end};
    double best = toleranceMm;
    bool found = false;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            const double distance = Distance(aEnds[i], bEnds[j]);
            if (distance > best) continue;
            best = distance;
            found = true;
            *aAtStart = i == 0;
            *bAtStart = j == 0;
        }
    }
    return found;
}

} // namespace

ChamferOutcome ApplyChamfer(PartDocument& document, ObjectId sketchId, SketchEntityId lineAId,
                            SketchEntityId lineBId, double distanceA, double distanceB) {
    ChamferOutcome outcome;
    const auto refuse = [&](std::string status) {
        outcome.applied = false;
        outcome.status = std::move(status);
        return outcome;
    };
    if (lineAId == lineBId) return refuse("A chamfer needs two different lines.");
    const Sketch* sketch = document.findSketch(sketchId);
    if (sketch == nullptr) return refuse("No such sketch.");
    const SketchLine* lineA = LineOf(*sketch, lineAId);
    const SketchLine* lineB = LineOf(*sketch, lineBId);
    if (lineA == nullptr || lineB == nullptr) return refuse("A chamfer needs two lines.");
    if (!(distanceA >= kMinSketchDimensionMm) || !(distanceB >= kMinSketchDimensionMm))
        return refuse("Both chamfer distances have to be greater than zero.");

    bool aAtStart = false;
    bool bAtStart = false;
    // The corner has to be a real meeting, not two ends that happen to be
    // nearby: a chamfer across a gap would invent geometry between two things
    // the user never joined.
    if (!FindCorner(*lineA, *lineB, kProfileConnectivityToleranceMm, &aAtStart, &bAtStart))
        return refuse("Those two lines do not meet, so there is no corner to chamfer.");

    const double lengthA = Distance(lineA->start, lineA->end);
    const double lengthB = Distance(lineB->start, lineB->end);
    if (distanceA >= lengthA || distanceB >= lengthB)
        return refuse("The chamfer is deeper than one of the lines is long.");

    // Setbacks measured FROM the corner, along each line, toward its far end.
    const Vec2 cornerA = aAtStart ? lineA->start : lineA->end;
    const Vec2 farA = aAtStart ? lineA->end : lineA->start;
    const Vec2 cornerB = bAtStart ? lineB->start : lineB->end;
    const Vec2 farB = bAtStart ? lineB->end : lineB->start;
    const Vec2 pointA{cornerA.x + (farA.x - cornerA.x) * (distanceA / lengthA),
                      cornerA.y + (farA.y - cornerA.y) * (distanceA / lengthA)};
    const Vec2 pointB{cornerB.x + (farB.x - cornerB.x) * (distanceB / lengthB),
                      cornerB.y + (farB.y - cornerB.y) * (distanceB / lengthB)};
    if (Distance(pointA, pointB) <= kMinSketchDimensionMm)
        return refuse("Those distances would leave no chamfer to draw.");

    SketchLine reshapedA = *lineA;
    (aAtStart ? reshapedA.start : reshapedA.end) = pointA;
    SketchLine reshapedB = *lineB;
    (bAtStart ? reshapedB.start : reshapedB.end) = pointB;

    // The corner's OWN constraint, found before anything moves.
    std::vector<SketchConstraintId> cornerJoints;
    for (const SketchConstraint& constraint : sketch->constraints()) {
        const auto* coincident = std::get_if<CoincidentConstraint>(&constraint.data);
        if (coincident == nullptr) continue;
        const auto names = [&](const SketchElementRef& ref, SketchEntityId id, bool atStart) {
            return ref.entityId == id && ref.subElement == (atStart ? SketchSubElement::StartPoint
                                                                    : SketchSubElement::EndPoint);
        };
        const bool joinsCorner =
            (names(coincident->a, lineAId, aAtStart) && names(coincident->b, lineBId, bAtStart)) ||
            (names(coincident->b, lineAId, aAtStart) && names(coincident->a, lineBId, bAtStart));
        if (joinsCorner) cornerJoints.push_back(constraint.id);
    }

    document.beginTransaction("Chamfer");
    const auto fail = [&](std::string status) {
        document.abortTransaction();
        outcome.applied = false;
        outcome.created = kInvalidSketchEntityId;
        outcome.status = std::move(status);
        return outcome;
    };

    // DELETED FIRST. It says the two ends are one point, and the whole purpose
    // of the command is that they no longer are; leaving it would hand the user
    // a conflict they did not create.
    for (const SketchConstraintId id : cornerJoints)
        if (!document.removeSketchConstraint(sketchId, id))
            return fail("Could not release the corner's coincidence.");

    if (!document.setSketchEntityGeometry(sketchId, lineAId, reshapedA) ||
        !document.setSketchEntityGeometry(sketchId, lineBId, reshapedB))
        return fail("The sketch refused the chamfer's setbacks.");

    const SketchEntityId chamfer = document.addSketchEntity(sketchId, SketchLine{pointA, pointB});
    if (chamfer == kInvalidSketchEntityId) return fail("The sketch refused the chamfer line.");

    // ...and joined at both ends, so the three pieces stay one run.
    const auto join = [&](SketchEntityId lineId, bool atStart, SketchSubElement chamferEnd) {
        return document.addSketchConstraint(
                   sketchId,
                   CoincidentConstraint{
                       SketchElementRef{lineId, atStart ? SketchSubElement::StartPoint
                                                        : SketchSubElement::EndPoint},
                       SketchElementRef{chamfer, chamferEnd}}) != kInvalidSketchConstraintId;
    };
    if (!join(lineAId, aAtStart, SketchSubElement::StartPoint) ||
        !join(lineBId, bAtStart, SketchSubElement::EndPoint))
        return fail("The sketch refused a chamfer joint.");

    if (!document.commitTransaction()) return fail("The document refused the chamfer.");
    outcome.applied = true;
    outcome.created = chamfer;
    outcome.status = "Chamfered the corner.";
    return outcome;
}

// --- FILLET ------------------------------------------------------------------

ChamferOutcome ApplyFillet(PartDocument& document, ObjectId sketchId, SketchEntityId lineAId,
                           SketchEntityId lineBId, double radiusMm) {
    ChamferOutcome outcome;
    const auto refuse = [&](std::string status) {
        outcome.applied = false;
        outcome.status = std::move(status);
        return outcome;
    };
    if (lineAId == lineBId) return refuse("A fillet needs two different lines.");
    const Sketch* sketch = document.findSketch(sketchId);
    if (sketch == nullptr) return refuse("No such sketch.");
    const SketchLine* lineA = LineOf(*sketch, lineAId);
    const SketchLine* lineB = LineOf(*sketch, lineBId);
    if (lineA == nullptr || lineB == nullptr) return refuse("A fillet needs two lines.");
    if (!(radiusMm >= kMinSketchDimensionMm))
        return refuse("A fillet radius has to be greater than zero.");

    bool aAtStart = false;
    bool bAtStart = false;
    if (!FindCorner(*lineA, *lineB, kProfileConnectivityToleranceMm, &aAtStart, &bAtStart))
        return refuse("Those two lines do not meet, so there is no corner to fillet.");

    const Vec2 corner = aAtStart ? lineA->start : lineA->end;
    const Vec2 farA = aAtStart ? lineA->end : lineA->start;
    const Vec2 farB = bAtStart ? lineB->end : lineB->start;
    const double lengthA = Distance(corner, farA);
    const double lengthB = Distance(corner, farB);
    if (lengthA <= kSketchToleranceMm || lengthB <= kSketchToleranceMm)
        return refuse("One of those lines is too short to fillet.");

    // Unit vectors pointing AWAY from the corner along each line. The setback
    // is r / tan(theta/2), the standard tangent-length of a circle inscribed in
    // the corner.
    const Vec2 dirA{(farA.x - corner.x) / lengthA, (farA.y - corner.y) / lengthA};
    const Vec2 dirB{(farB.x - corner.x) / lengthB, (farB.y - corner.y) / lengthB};
    const double cosTheta = std::clamp(dirA.x * dirB.x + dirA.y * dirB.y, -1.0, 1.0);
    const double theta = std::acos(cosTheta);
    // Parallel or anti-parallel lines have no corner to inscribe a circle in --
    // the tangent length is infinite one way and undefined the other.
    if (theta <= 1e-6 || theta >= kPi - 1e-6)
        return refuse("Those two lines are in line with each other; there is no corner to round.");
    const double setback = radiusMm / std::tan(theta * 0.5);
    if (setback >= lengthA || setback >= lengthB)
        return refuse("That radius is too big for the corner: it would eat past the end of one "
                      "of the lines.");

    const Vec2 tangentA{corner.x + dirA.x * setback, corner.y + dirA.y * setback};
    const Vec2 tangentB{corner.x + dirB.x * setback, corner.y + dirB.y * setback};
    // The centre lies along the corner's bisector, at r / sin(theta/2).
    const double bisectorLength = std::sqrt((dirA.x + dirB.x) * (dirA.x + dirB.x) +
                                            (dirA.y + dirB.y) * (dirA.y + dirB.y));
    if (bisectorLength <= kSketchToleranceMm)
        return refuse("Those two lines double back on each other; there is no corner to round.");
    const Vec2 bisector{(dirA.x + dirB.x) / bisectorLength, (dirA.y + dirB.y) / bisectorLength};
    const double centreDistance = radiusMm / std::sin(theta * 0.5);
    const Vec2 centre{corner.x + bisector.x * centreDistance,
                      corner.y + bisector.y * centreDistance};

    // The arc runs from tangentA to tangentB the SHORT way round -- the long
    // way would sweep back across the corner it is supposed to be rounding.
    const double angleA = std::atan2(tangentA.y - centre.y, tangentA.x - centre.x);
    const double angleB = std::atan2(tangentB.y - centre.y, tangentB.x - centre.x);
    const bool counterClockwise = WrapSigned(angleB - angleA) > 0.0;

    SketchLine reshapedA = *lineA;
    (aAtStart ? reshapedA.start : reshapedA.end) = tangentA;
    SketchLine reshapedB = *lineB;
    (bAtStart ? reshapedB.start : reshapedB.end) = tangentB;

    // The corner's own coincidence, released for the same reason a chamfer
    // releases it: the two ends are no longer one point.
    std::vector<SketchConstraintId> cornerJoints;
    for (const SketchConstraint& constraint : sketch->constraints()) {
        const auto* coincident = std::get_if<CoincidentConstraint>(&constraint.data);
        if (coincident == nullptr) continue;
        const auto names = [&](const SketchElementRef& ref, SketchEntityId id, bool atStart) {
            return ref.entityId == id && ref.subElement == (atStart ? SketchSubElement::StartPoint
                                                                    : SketchSubElement::EndPoint);
        };
        if ((names(coincident->a, lineAId, aAtStart) && names(coincident->b, lineBId, bAtStart)) ||
            (names(coincident->b, lineAId, aAtStart) && names(coincident->a, lineBId, bAtStart)))
            cornerJoints.push_back(constraint.id);
    }

    document.beginTransaction("Fillet");
    const auto fail = [&](std::string status) {
        document.abortTransaction();
        outcome.applied = false;
        outcome.created = kInvalidSketchEntityId;
        outcome.status = std::move(status);
        return outcome;
    };
    for (const SketchConstraintId id : cornerJoints)
        if (!document.removeSketchConstraint(sketchId, id))
            return fail("Could not release the corner's coincidence.");
    if (!document.setSketchEntityGeometry(sketchId, lineAId, reshapedA) ||
        !document.setSketchEntityGeometry(sketchId, lineBId, reshapedB))
        return fail("The sketch refused the fillet's setbacks.");

    const SketchEntityId arc = document.addSketchEntity(
        sketchId, SketchArc{centre, radiusMm, counterClockwise ? angleA : angleB,
                            counterClockwise ? angleB : angleA, true});
    if (arc == kInvalidSketchEntityId) return fail("The sketch refused the fillet arc.");

    // WHICH TIP goes to which line depends on the direction the arc was built
    // in: reversing the sweep swaps its start and its end.
    const SketchSubElement tipForA =
        counterClockwise ? SketchSubElement::StartPoint : SketchSubElement::EndPoint;
    const SketchSubElement tipForB =
        counterClockwise ? SketchSubElement::EndPoint : SketchSubElement::StartPoint;
    const auto join = [&](SketchEntityId lineId, bool atStart, SketchSubElement tip) {
        return document.addSketchConstraint(
                   sketchId,
                   CoincidentConstraint{
                       SketchElementRef{lineId, atStart ? SketchSubElement::StartPoint
                                                        : SketchSubElement::EndPoint},
                       SketchElementRef{arc, tip}}) != kInvalidSketchConstraintId;
    };
    if (!join(lineAId, aAtStart, tipForA) || !join(lineBId, bAtStart, tipForB))
        return fail("The sketch refused a fillet joint.");

    // TANGENT is what makes it a fillet rather than an arc that happens to
    // touch. Without it the corner is smooth today and kinked after the first
    // parameter change.
    //
    // AT THE SETBACK, which is the whole point: the corner is where the arc
    // meets each line, and tangency asked without saying where is satisfied by
    // a line that merely grazes the circle somewhere -- a residual sitting at a
    // maximum with no gradient, holding the corner smooth today and letting it
    // kink after the first parameter change. Exactly what this comment used to
    // claim it prevented.
    const auto touch = [](bool atStart) {
        return atStart ? SketchSubElement::StartPoint : SketchSubElement::EndPoint;
    };
    if (document.addSketchConstraint(
            sketchId, TangentConstraint{lineAId, arc, false, touch(aAtStart)}) ==
            kInvalidSketchConstraintId ||
        document.addSketchConstraint(
            sketchId, TangentConstraint{lineBId, arc, false, touch(bAtStart)}) ==
            kInvalidSketchConstraintId)
        return fail("The sketch refused a fillet tangency.");

    if (!document.commitTransaction()) return fail("The document refused the fillet.");
    outcome.applied = true;
    outcome.created = arc;
    outcome.status = "Rounded the corner.";
    return outcome;
}

// --- MIRROR ------------------------------------------------------------------

namespace {

// `p` reflected across the infinite line through `line`.
Vec2 ReflectAcross(const SketchLine& line, Vec2 p) noexcept {
    const double du = line.end.x - line.start.x;
    const double dv = line.end.y - line.start.y;
    const double lengthSquared = du * du + dv * dv;
    if (lengthSquared <= kSketchToleranceMm) return p;
    const double t = ((p.x - line.start.x) * du + (p.y - line.start.y) * dv) / lengthSquared;
    const Vec2 foot{line.start.x + du * t, line.start.y + dv * t};
    return Vec2{2.0 * foot.x - p.x, 2.0 * foot.y - p.y};
}

} // namespace

MirrorOutcome ApplyMirror(PartDocument& document, ObjectId sketchId,
                          const std::vector<SketchEntityId>& sourceIds, SketchEntityId mirrorId) {
    MirrorOutcome outcome;
    const auto refuse = [&](std::string status) {
        outcome.applied = false;
        outcome.status = std::move(status);
        return outcome;
    };
    const Sketch* sketch = document.findSketch(sketchId);
    if (sketch == nullptr) return refuse("No such sketch.");
    const SketchLine* mirror = LineOf(*sketch, mirrorId);
    if (mirror == nullptr) return refuse("The mirror has to be a line.");
    if (Distance(mirror->start, mirror->end) <= kSketchToleranceMm)
        return refuse("That mirror line is too short to have a direction.");
    if (sourceIds.empty()) return refuse("Select the geometry to mirror.");

    // CHECKED BEFORE ANYTHING IS CREATED, so a refusal leaves no half-mirrored
    // sketch behind.
    for (const SketchEntityId id : sourceIds) {
        if (id == mirrorId) return refuse("The mirror line cannot mirror itself.");
        const SketchEntity* entity = sketch->findEntity(id);
        if (entity == nullptr) return refuse("Something selected is not in this sketch.");
        if (std::holds_alternative<SketchPoint>(entity->geometry)) continue;
        if (std::holds_alternative<SketchLine>(entity->geometry)) continue;
        if (std::holds_alternative<SketchCircle>(entity->geometry)) continue;
        if (std::holds_alternative<SketchArc>(entity->geometry)) continue;
        // AN ELLIPSE IS REFUSED, and the reason is the constraints rather than
        // the geometry. Reflecting one is easy -- the centre reflects and the
        // rotation becomes 2*phi - rotation -- but a mirror in this project
        // TIES the copy to the original, and there is no equality between two
        // ellipses to tie their shapes with. A copy that moved with the
        // original in position and drifted in shape is worse than not having
        // the command.
        if (std::holds_alternative<SketchEllipse>(entity->geometry) ||
            std::holds_alternative<SketchEllipticalArc>(entity->geometry))
            return refuse("An ellipse cannot be mirrored yet: there is no constraint that "
                          "would keep the copy the same shape as the original.");
        // A SPLINE IS FINE NOW (M18). It was refused because only its two ends
        // could be named by a constraint, so the copy's middle could not be
        // tied to the original's -- M17.30 gave every point a name, and this
        // refusal outlived its reason.
        if (std::holds_alternative<SketchSpline>(entity->geometry)) continue;
        return refuse("Only points, lines, circles, arcs and splines can be mirrored.");
    }

    document.beginTransaction("Mirror");
    const auto fail = [&](std::string status) {
        document.abortTransaction();
        outcome.applied = false;
        outcome.created.clear();
        outcome.status = std::move(status);
        return outcome;
    };
    // Re-read after each add: the sketch's storage can move.
    const auto tie = [&](SketchElementRef a, SketchElementRef b) {
        return document.addSketchConstraint(sketchId, SymmetricConstraint{a, b, mirrorId}) !=
               kInvalidSketchConstraintId;
    };

    for (const SketchEntityId id : sourceIds) {
        const Sketch* current = document.findSketch(sketchId);
        if (current == nullptr) return fail("The sketch went away mid-mirror.");
        const SketchEntity* source = current->findEntity(id);
        if (source == nullptr) return fail("A selected entity went away mid-mirror.");

        if (const auto* line = std::get_if<SketchLine>(&source->geometry)) {
            const SketchEntityId copy = document.addSketchEntity(
                sketchId, SketchLine{ReflectAcross(*mirror, line->start),
                                     ReflectAcross(*mirror, line->end)});
            if (copy == kInvalidSketchEntityId) return fail("The sketch refused a mirrored line.");
            outcome.created.push_back(copy);
            // BOTH ends. One would leave the copy free to swing about the other.
            if (!tie(SketchElementRef{id, SketchSubElement::StartPoint},
                     SketchElementRef{copy, SketchSubElement::StartPoint}) ||
                !tie(SketchElementRef{id, SketchSubElement::EndPoint},
                     SketchElementRef{copy, SketchSubElement::EndPoint}))
                return fail("The sketch refused a mirror symmetry.");
        } else if (const auto* circle = std::get_if<SketchCircle>(&source->geometry)) {
            const SketchEntityId copy = document.addSketchEntity(
                sketchId, SketchCircle{ReflectAcross(*mirror, circle->center), circle->radiusMm});
            if (copy == kInvalidSketchEntityId)
                return fail("The sketch refused a mirrored circle.");
            outcome.created.push_back(copy);
            if (!tie(SketchElementRef{id, SketchSubElement::CenterPoint},
                     SketchElementRef{copy, SketchSubElement::CenterPoint}))
                return fail("The sketch refused a mirror symmetry.");
            // Symmetry places the centre; EQUAL is what keeps the size in step.
            if (document.addSketchConstraint(sketchId, EqualConstraint{id, copy}) ==
                kInvalidSketchConstraintId)
                return fail("The sketch refused the mirrored circle's equal radius.");
        } else if (const auto* arc = std::get_if<SketchArc>(&source->geometry)) {
            // A REFLECTION REVERSES the sweep. Mirroring an angle theta about a
            // line of direction phi gives 2*phi - theta, and traversing the
            // image counter-clockwise runs from the reflected END to the
            // reflected START -- so the two are swapped. Keeping them in order
            // would draw the complementary arc: the same circle, the piece the
            // user did not mirror.
            const double phi = std::atan2(mirror->end.y - mirror->start.y,
                                          mirror->end.x - mirror->start.x);
            const Vec2 centre = ReflectAcross(*mirror, arc->center);
            const double start = 2.0 * phi - arc->endAngleRad;
            const double end = 2.0 * phi - arc->startAngleRad;
            const SketchEntityId copy = document.addSketchEntity(
                sketchId, SketchArc{centre, arc->radiusMm, start, end, true});
            if (copy == kInvalidSketchEntityId) return fail("The sketch refused a mirrored arc.");
            outcome.created.push_back(copy);

            // CROSSED TIPS PLUS EQUAL, and NOT the centre.
            //
            // The copy has five freedoms (centre 2, radius 1, angles 2). Two
            // tip symmetries are four equations and Equal is the fifth --
            // exactly determined. Adding a centre symmetry as well would be two
            // more equations for freedoms already spent: consistent, but the
            // solver would rightly call every mirrored arc over-constrained,
            // and roadmap 8.2 asks that reading to mean something.
            //
            // Crossed, because the reflection swapped them.
            if (!tie(SketchElementRef{id, SketchSubElement::StartPoint},
                     SketchElementRef{copy, SketchSubElement::EndPoint}) ||
                !tie(SketchElementRef{id, SketchSubElement::EndPoint},
                     SketchElementRef{copy, SketchSubElement::StartPoint}))
                return fail("The sketch refused a mirror symmetry.");
            if (document.addSketchConstraint(sketchId, EqualConstraint{id, copy}) ==
                kInvalidSketchConstraintId)
                return fail("The sketch refused the mirrored arc's equal radius.");
        } else if (const auto* spline = std::get_if<SketchSpline>(&source->geometry)) {
            // EVERY POINT REFLECTED, IN THE SAME ORDER.
            //
            // Exact, not an approximation: Catmull-Rom is a weighted sum of the
            // points whose weights add to one, so reflecting the points and
            // interpolating gives the same curve as interpolating and then
            // reflecting. An arc had to swap its two ends because its sweep
            // direction is stored as a pair of angles; a spline's direction is
            // the order of its points, and the mirror image traversed in that
            // same order IS the reflection.
            std::vector<Vec2> reflected;
            reflected.reserve(spline->points.size());
            for (const Vec2& point : spline->points)
                reflected.push_back(ReflectAcross(*mirror, point));
            const SketchEntityId copy =
                document.addSketchEntity(sketchId, SketchSpline{reflected, spline->closed});
            if (copy == kInvalidSketchEntityId)
                return fail("The sketch refused a mirrored spline.");
            outcome.created.push_back(copy);

            // ONE SYMMETRY PER POINT, which is exactly determined: the copy has
            // two freedoms per point and each symmetry is two equations. There
            // is no Equal to add on top, the way a circle needs one for its
            // radius -- a spline has no size apart from where its points are.
            //
            // The refs come from SplineRefFor, the one place that turns "point
            // i" into a reference, so the ends are named `.start` and `.end`
            // and the rest by index. Spelling them here by hand would be a
            // second copy of that rule.
            const Sketch* afterCopy = document.findSketch(sketchId);
            if (afterCopy == nullptr) return fail("The sketch went away mid-mirror.");
            const SketchEntity* copied = afterCopy->findEntity(copy);
            const SketchEntity* original = afterCopy->findEntity(id);
            if (copied == nullptr || original == nullptr)
                return fail("A mirrored spline went away mid-mirror.");
            const auto* copiedSpline = std::get_if<SketchSpline>(&copied->geometry);
            const auto* originalSpline = std::get_if<SketchSpline>(&original->geometry);
            if (copiedSpline == nullptr || originalSpline == nullptr ||
                copiedSpline->points.size() != originalSpline->points.size())
                return fail("The mirrored spline does not match the original.");
            for (int i = 0; i < static_cast<int>(originalSpline->points.size()); ++i)
                if (!tie(SplineRefFor(id, *originalSpline, i),
                         SplineRefFor(copy, *copiedSpline, i)))
                    return fail("The sketch refused a mirror symmetry.");
        } else {
            const auto* point = std::get_if<SketchPoint>(&source->geometry);
            if (point == nullptr)
                return fail("Only points, lines, circles, arcs and splines can be mirrored.");
            const SketchEntityId copy =
                document.addSketchEntity(sketchId, SketchPoint{ReflectAcross(*mirror, point->position)});
            if (copy == kInvalidSketchEntityId)
                return fail("The sketch refused a mirrored point.");
            outcome.created.push_back(copy);
            if (!tie(SketchElementRef{id, SketchSubElement::Whole},
                     SketchElementRef{copy, SketchSubElement::Whole}))
                return fail("The sketch refused a mirror symmetry.");
        }
    }

    if (!document.commitTransaction()) return fail("The document refused the mirror.");
    outcome.applied = true;
    outcome.status = "Mirrored " + std::to_string(outcome.created.size()) +
                     (outcome.created.size() == 1 ? " entity." : " entities.");
    return outcome;
}

SketchEdit MakeOriginPointEdit() {
    SketchEdit edit;
    edit.kind = SketchEditKind::AddPoint;
    edit.points = {Vec2{0.0, 0.0}};
    edit.label = "Add origin point";
    // Fixed, so it anchors rather than drifts. Built as an autoConstraint on
    // the not-yet-created entity, which is the same machinery the Point tool
    // uses when a click lands on the origin -- one path, not two.
    PendingConstraint fix;
    fix.kind = SketchEditKind::AddFix;
    fix.a = PendingRef{true, 0, SketchElementRef{}, SketchSubElement::Whole};
    edit.autoConstraints.push_back(fix);
    return edit;
}

SketchEntityId FindSketchOrigin(const Sketch& sketch) {
    for (const SketchEntity& entity : sketch.entities()) {
        const auto* point = std::get_if<SketchPoint>(&entity.geometry);
        if (point == nullptr) continue;
        if (std::abs(point->position.x) > kSketchToleranceMm) continue;
        if (std::abs(point->position.y) > kSketchToleranceMm) continue;
        return entity.id;
    }
    return kInvalidSketchEntityId;
}

std::vector<ConstraintBadge> ConstraintBadgesFor(const Sketch& sketch) {
    std::vector<ConstraintBadge> badges;
    // How many badges are already hanging off each entity, so the next one is
    // stacked below rather than printed on top.
    std::vector<std::pair<SketchEntityId, int>> stack;
    const auto slotFor = [&](SketchEntityId id) {
        for (auto& entry : stack)
            if (entry.first == id) return entry.second++;
        stack.push_back({id, 1});
        return 0;
    };

    for (const SketchConstraint& constraint : sketch.constraints()) {
        const char* glyph = GlyphFor(constraint.data);
        if (glyph == nullptr) continue;
        const std::vector<SketchEntityId> entities = ReferencedEntities(constraint.data);
        if (entities.empty()) continue;

        bool ok = false;
        Vec2 anchor = ResolveElementPoint(
            sketch, SketchElementRef{entities.front(), SketchSubElement::Whole}, &ok);
        if (!ok) continue;

        // A QUARTER of the way along a line, not its midpoint.
        //
        // ResolveElementPoint gives a line its midpoint -- which is exactly
        // where a dimension puts its value, so every dimensioned line had its
        // number and its constraint badges printed on top of each other.
        if (const SketchEntity* owner = sketch.findEntity(entities.front())) {
            if (const auto* line = std::get_if<SketchLine>(&owner->geometry))
                anchor = Vec2{line->start.x + (line->end.x - line->start.x) * 0.25,
                              line->start.y + (line->end.y - line->start.y) * 0.25};
        }

        ConstraintBadge badge;
        badge.id = constraint.id;
        badge.glyph = glyph;
        badge.anchorMm = anchor;
        badge.slot = slotFor(entities.front());
        badge.offending = std::find(sketch.offendingConstraints().begin(),
                                    sketch.offendingConstraints().end(),
                                    constraint.id) != sketch.offendingConstraints().end();
        badges.push_back(std::move(badge));
    }
    return badges;
}

std::vector<DimensionAnnotation> DimensionAnnotationsFor(const PartDocument& document,
                                                         const Sketch& sketch,
                                                         double pixelsPerMm) {
    std::vector<DimensionAnnotation> annotations;
    for (const SketchConstraint& constraint : sketch.constraints()) {
        if (!IsDimensional(constraint.data)) continue;

        DimensionAnnotation annotation;
        if (!BuildAutomaticLayout(document, sketch, constraint, annotation)) continue;

        // A user placement REPLACES the computed layout rather than nudging it.
        if (const Vec2* placed = sketch.dimensionPlacement(constraint.id)) {
            Vec2 a{};
            Vec2 b{};
            const bool linear = MeasuredSpan(sketch, constraint.data, &a, &b);
            ReplaceWithUserPlacement(annotation, *placed, a, b, linear);
        }

        // Arrowheads that will not fit turn round. Done AFTER placement, so a
        // dimension dragged in close still behaves.
        if (annotation.kind == SketchEditKind::AddLength ||
            annotation.kind == SketchEditKind::AddDistance ||
            annotation.kind == SketchEditKind::AddHorizontalDistance ||
            annotation.kind == SketchEditKind::AddVerticalDistance ||
            annotation.kind == SketchEditKind::AddPointLineDistance ||
            annotation.kind == SketchEditKind::AddDiameter) {
            FlipArrowsIfCramped(annotation, pixelsPerMm,
                                EstimatedTextWidthPx(annotation.text) + 2.0 * kArrowRoomPx);
        }

        annotations.push_back(std::move(annotation));
    }

    SeparateOverlappingLabels(annotations, pixelsPerMm);
    return annotations;
}

std::string DimensionEditText(const PartDocument& document, const Sketch& sketch,
                              SketchConstraintId constraintId) {
    const SketchConstraint* constraint = sketch.findConstraint(constraintId);
    if (constraint == nullptr || !IsDimensional(constraint->data)) return std::string();
    const Parameter* parameter = document.parameters().findById(BoundParameterId(constraint->data));
    if (parameter == nullptr) return std::string();
    // An expression driving the value is what the user should see and edit --
    // showing the evaluated number would make the first keystroke destroy the
    // formula (the M11.3 lesson about not discarding what the user typed).
    if (!parameter->expression().empty()) return parameter->expression();
    if (parameter->unit() == UnitType::Radian)
        return FormatNumber(parameter->value() * 180.0 / kPi);
    return FormatNumber(parameter->value());
}

SketchEditOutcome CommitDimensionValue(PartDocument& document, const Sketch& sketch,
                                       SketchConstraintId constraintId, const std::string& text) {
    SketchEditOutcome outcome;
    const SketchConstraint* constraint = sketch.findConstraint(constraintId);
    if (constraint == nullptr || !IsDimensional(constraint->data)) {
        outcome.status = "That is not a dimension.";
        return outcome;
    }
    const ObjectId parameterId = BoundParameterId(constraint->data);
    const Parameter* parameter = document.parameters().findById(parameterId);
    if (parameter == nullptr) {
        outcome.status = "That dimension has no parameter.";
        return outcome;
    }
    const bool isAngle = parameter->unit() == UnitType::Radian;

    std::string trimmed = text;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
        trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
        trimmed.pop_back();
    // A leading '=' is the conventional "this is a formula" marker and is not
    // part of the expression itself.
    const bool forcedExpression = !trimmed.empty() && trimmed.front() == '=';
    if (forcedExpression) trimmed.erase(trimmed.begin());

    double number = 0.0;
    if (!forcedExpression && ParsesAsNumber(trimmed, &number)) {
        // A typed number REPLACES a formula (ADR-M11-006). Clearing first keeps
        // that rule in one place rather than depending on the order two facade
        // calls happen to run in.
        document.beginTransaction("Change dimension");
        if (!parameter->expression().empty() &&
            !document.setParameterExpression(parameterId, std::string())) {
            document.abortTransaction();
            outcome.status = "Could not clear the existing expression.";
            return outcome;
        }
        // DEGREES IN, RADIANS STORED. The one conversion site for dimension
        // editing -- roadmap section 7 stores angles in radians and shows
        // degrees, and a second conversion anywhere would double-apply.
        const double stored = isAngle ? number * kPi / 180.0 : number;
        if (!document.setParameterValue(parameterId, stored)) {
            document.abortTransaction();
            outcome.status = "The document refused that value.";
            return outcome;
        }
        if (!document.commitTransaction()) {
            outcome.status = "The document refused that value.";
            return outcome;
        }
        outcome.applied = true;
        outcome.status = parameter->name() + " = " + FormatNumber(number) +
                         (isAngle ? " deg" : " mm");
        return outcome;
    }

    if (trimmed.empty()) {
        outcome.status = "A dimension needs a value or an expression.";
        return outcome;
    }

    ExpressionError error;
    if (!document.setParameterExpression(parameterId, trimmed, &error)) {
        outcome.status = DescribeExpressionError(error);
        outcome.detail = RenderExpressionError(trimmed, error);
        return outcome;
    }
    outcome.applied = true;
    outcome.status = parameter->name() + " = " + trimmed;
    return outcome;
}

// --- Using projected reference geometry (M17.6, ADR-M17-029) ----------------

ConvertReferencePlan PlanConvertReference(const Sketch& sketch, SketchReferenceId referenceId) {
    ConvertReferencePlan plan;

    const SketchReference* reference = sketch.findReference(referenceId);
    if (reference == nullptr) {
        plan.message = "Click a projected reference edge to use it";
        return plan;
    }

    // ALREADY THERE is a refusal, not a duplicate. Two identical curves lying
    // exactly on top of each other are invisible on screen and make the profile
    // ambiguous -- the loop walker finds two ways out of every vertex.
    // IsSameCurve is the sketch model's own comparison, so a line converted
    // once and then clicked again from the other end is still recognised.
    for (const SketchEntity& entity : sketch.entities()) {
        if (!IsSameCurve(entity.geometry, reference->geometry, kSketchToleranceMm)) continue;
        plan.message = "That edge is already in this sketch";
        return plan;
    }

    // A Fix on the geometry this edit is about to create. `newEntity` is 0
    // because each of these edits creates exactly one entity, and PendingRef
    // resolves the index to a real id the moment that entity exists.
    const auto fixNew = [](SketchSubElement part) {
        PendingConstraint fix;
        fix.kind = SketchEditKind::AddFix;
        fix.a.isNew = true;
        fix.a.newEntity = 0;
        fix.a.subElement = part;
        return fix;
    };

    SketchEdit& edit = plan.edit;
    if (const auto* point = std::get_if<SketchPoint>(&reference->geometry)) {
        edit.kind = SketchEditKind::AddPoint;
        edit.points = {point->position};
        edit.autoConstraints = {fixNew(SketchSubElement::Whole)};
        edit.label = "Use reference point";
        plan.message = "Reference point used, and fixed where the face put it";
    } else if (const auto* line = std::get_if<SketchLine>(&reference->geometry)) {
        edit.kind = SketchEditKind::AddLine;
        edit.points = {line->start, line->end};
        // BOTH ends: four residuals against a line's four degrees of freedom.
        // Exactly determined, never redundant.
        edit.autoConstraints = {fixNew(SketchSubElement::StartPoint),
                                fixNew(SketchSubElement::EndPoint)};
        edit.label = "Use reference line";
        plan.message = "Reference line used, and fixed where the face put it";
    } else if (const auto* circle = std::get_if<SketchCircle>(&reference->geometry)) {
        edit.kind = SketchEditKind::AddCircle;
        edit.points = {circle->center,
                       Vec2{circle->center.x + circle->radiusMm, circle->center.y}};
        // CENTRE ONLY. There is no Fix for a radius, and pinning the rim as a
        // point would be a residual the solver has no variable for. The radius
        // stays free, and the message says so -- a user who wants it held adds
        // a Radius dimension, which is the tool that exists for exactly that.
        edit.autoConstraints = {fixNew(SketchSubElement::CenterPoint)};
        edit.label = "Use reference circle";
        plan.message = "Reference circle used. Its centre is fixed; the radius is free -- "
                       "add a Radius dimension to hold it";
    } else if (const auto* arc = std::get_if<SketchArc>(&reference->geometry)) {
        edit.kind = SketchEditKind::AddArc;
        const Vec2 start{arc->center.x + arc->radiusMm * std::cos(arc->startAngleRad),
                         arc->center.y + arc->radiusMm * std::sin(arc->startAngleRad)};
        const Vec2 end{arc->center.x + arc->radiusMm * std::cos(arc->endAngleRad),
                       arc->center.y + arc->radiusMm * std::sin(arc->endAngleRad)};
        edit.points = {arc->center, start, end};
        // Centre only, for the same reason as a circle -- and one more: an arc
        // has five degrees of freedom, so pinning its centre AND both tips
        // would be six residuals for five unknowns. The solver would report the
        // sketch over-constrained and name a constraint the user never added.
        edit.autoConstraints = {fixNew(SketchSubElement::CenterPoint)};
        edit.label = "Use reference arc";
        plan.message = "Reference arc used. Its centre is fixed; the radius and sweep are free";
    } else {
        plan.message = "That reference has no shape this sketch can hold";
        return plan;
    }

    plan.ok = true;
    return plan;
}

} // namespace paramcad
